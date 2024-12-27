#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>

#define true 1
#define false 0

#define CTRL_KEY(k) ((k)& 0x1f)
#define ESCAPE_KEY 27
//because of macos change to 10 for linux
#define RETURN_KEY 13
#define DELETE_KEY 127
#define command_size 128
#define TIMEOUT_KEY 0

enum DimModes{
    NormalMode = 0,
    VisualMode,
    InsertMode,
    SelectMode,
    CLMode,
    ReplaceMode,
    VirtualReplaceMode,
    OperatorPendingMode,
    ExMode,
    TerminalMode
};

enum EditorKeys{
    LEFT_ARROW = 1000,
    RIGHT_ARROW,
    DOWN_ARROW,
    UP_ARROW,
    PAGE_UP,
    PAGE_DOWN,
};

typedef struct erow{
    int size;
    char *chars;
} erow;

struct EditorConfig{
    int cx, cy;
    int screen_rows;
    int screen_cols;
    char mode;
    int num_rows;
    erow *row;
    struct termios og_termios;
};

struct EditorConfig E;

void
die(const char *c){
    //clears screen
    write(STDOUT_FILENO, "\x1b[2J", 4);
    //sets cursor on top left
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(c);
    exit(1);
}

void
disable_raw_mode(){
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.og_termios) == -1)
        die("tcsetattr");
}

void
enable_raw_mode(){
    if (tcgetattr(STDIN_FILENO, &E.og_termios) == -1)
        die("tcgetattr");
    atexit(disable_raw_mode);

    struct termios raw = E.og_termios;
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    //sets how long it takes for read to not recive input and return 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int
editor_read_key(){
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) == -1){
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    if (c == ESCAPE_KEY){
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) == -1) return ESCAPE_KEY;
        if (read(STDIN_FILENO, &seq[1], 1) == -1) return ESCAPE_KEY;

        if (seq[0] == '['){
            if (seq[1] >= 0 && seq[1] <= 9){
                if (read(STDIN_FILENO, &seq[2], 1) == -1) return ESCAPE_KEY;
                if (seq[2] == '~'){
                    switch (seq[1]) {
                        case '5': return PAGE_UP;
                        case '6': return PAGE_UP;
                    }
                }
            }
            switch (seq[1]){
                case 'A': return UP_ARROW;
                case 'B': return DOWN_ARROW;
                case 'C': return RIGHT_ARROW;
                case 'D': return LEFT_ARROW;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int
get_cursor_position(int *rows, int *cols){
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1){
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int
get_window_size(int *rows, int *cols){
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return get_cursor_position(rows, cols);
    } else{
        *cols = ws.ws_col;
        *rows= ws.ws_row;
        return 0;
    }
}

void
editor_append_row(char *s, size_t len){
    E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));

    int at = E.num_rows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    ++E.num_rows;
}

void
editor_open(char *file_name){
    FILE *fp = fopen(file_name, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t line_cap = 8;
    ssize_t line_len;

    line_len = getline(&line, &line_cap, fp);
    while ((line_len = getline(&line, &line_cap, fp)) != -1){
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1]  == '\r'))
            --line_len;
        editor_append_row(line, line_len);
    }
    free(line);
    fclose(fp);
}


struct ABuf{
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void
ab_append(struct ABuf *abuf, char *s, int len){
    char *new = realloc(abuf->b, abuf->len + len);

    if (new == NULL) return;
    memcpy(&new[abuf->len], s, len);
    abuf->b = new;
    abuf->len += len;
}

void
ab_free(struct ABuf *ab){
    free(ab->b);
}

void
init_editor(){
    E.cx = 0;
    E.cy = 0;
    E.num_rows = 0;
    E.mode = NormalMode;
    E.row = NULL;

    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) die("init");
}

#define DIM_VERSION "0.0.1"

void
editor_draw_rows(struct ABuf *ab){
    for(int y = 0; y < E.screen_rows; ++y){
        if (y >= E.num_rows){
            if (E.num_rows == 0 && y == E.screen_rows / 3) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                                           "dim editor --version %s", DIM_VERSION);
                if (welcome_len > E.screen_cols) welcome_len = E.screen_cols;
                int padding = (E.screen_cols - welcome_len) / 2;
                if (padding){
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while(padding--) ab_append(ab, " ", 1);
                ab_append(ab, welcome, welcome_len);
            } else {
                ab_append(ab, "~", 1);
            }
        } else {
            int len = E.row[y].size;
            if (len > E.screen_cols) len = E.screen_cols;
            ab_append(ab, E.row[y].chars, len);
        }

        ab_append(ab, "\x1b[K", 3);
        if (y < E.screen_rows - 1){
            ab_append(ab, "\r\n", 2);
        }
    }
}

void
editor_refresh_screen(){
    struct ABuf ab = ABUF_INIT;
    
    //hides cursor to prevent fickering while rendering
    ab_append(&ab, "\x1b[?25l", 6);
    //sets cursor on top left
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    
    //places cursor at where it is supposed to be
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    ab_append(&ab, buf, strlen(buf));

    //shows cursor
    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);

    ab_free(&ab);
}

void
handle_normal(){
    int c = editor_read_key();
    //moves curosr
    switch (c){
        //left
        case LEFT_ARROW:
        case 'h':
            if (E.cx != 0){
                --E.cx;
            }
            break;
        //right
        case RIGHT_ARROW:
        case 'l':
            if (E.cx != E.screen_cols - 1){
                ++E.cx;
            }
            break;
        //up
        case UP_ARROW:
        case 'k':
            if (E.cy != 0){
                --E.cy;
            }
            break;
        //down
        case RETURN_KEY:
        case DOWN_ARROW:
        case 'j':
            if (E.cy != E.screen_rows - 1){
                ++E.cy;
            }
            break;
        case ':':
            E.mode = CLMode;
            break;
        case 'i':
            E.mode = InsertMode;
            break;
    }
}

void
handle_insert(){
    int c = editor_read_key();
    switch (c){
        //left
        case LEFT_ARROW:
            if (E.cx != 0){
                --E.cx;
            }
            break;
        //right
        case RIGHT_ARROW:
            if (E.cx != E.screen_cols - 1){
                ++E.cx;
            }
            break;
        //up
        case UP_ARROW:
            if (E.cy != 0){
                --E.cy;
            }
            break;
        //down
        case DOWN_ARROW:
            if (E.cy != E.screen_rows - 1){
                ++E.cy;
            }
            break;
        case PAGE_DOWN:
            {
                int times = E.screen_cols;
                while(times--){
                    if (E.cy != E.screen_rows - 1){
                        ++E.cy;
                    }
                    else{
                        break;
                    }
                }
                break;
            }
        case PAGE_UP:
            {
                int times = E.screen_cols;
                while(times--){
                    if (E.cy != 0){
                        --E.cy;
                    }
                    else{
                        break;
                    }
                }
                break;
            }
        case ESCAPE_KEY:
            E.mode = NormalMode;
            break;
    }
}

void
handle_cl(){
    char command[command_size] = {0};
    unsigned char index = 0;
    while(index < command_size){
        int result = editor_read_key();
        switch (result){
            case UP_ARROW:
            case DOWN_ARROW:
            case TIMEOUT_KEY:
            case DELETE_KEY:
                continue;
            case ESCAPE_KEY:
                E.mode = NormalMode;
                return;
            case RETURN_KEY:
                command[index] = '\0';
                if (strcmp(command, "q") == 0){
                    write(STDOUT_FILENO, "\x1b[2J", 4);
                    write(STDOUT_FILENO, "\x1b[H", 3);
                    exit(0);
                    return;
                }
                return;
            default:
                command[index] = (char)result;
                break;
        }
        ++index;
    }
}

void
editor_process_keypress(){
    switch(E.mode){
        case NormalMode:
            handle_normal();
            break;
        case VisualMode:
        case InsertMode:
            handle_insert();
            break;
        case SelectMode:
        case CLMode:
            handle_cl();
            break;
        case ReplaceMode:
        case VirtualReplaceMode:
        case OperatorPendingMode:
        case ExMode:
        case TerminalMode:
            break;
    }
}

int
main (int argc, char *argv[]){
    enable_raw_mode();
    init_editor();
    if (argc >= 2){
        editor_open(argv[1]);
    }

    while (true){
        editor_refresh_screen();
        editor_process_keypress();
    }
    printf("row %d, col %d", E.screen_rows, E.screen_cols);
    return 0;
}
