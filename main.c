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
#define ESCAPE 27

enum EditorKey{
    LEFT_ARROW = 'h',
    RIGHT_ARROW = 'l',
    UP_ARROW = 'k',
    DOWN_ARROW ='j'
};

//Flags
//Visual mode = 1
#define VisualMode 0xfe
#define InsertMode 1
struct EditorConfig{
    int cx, cy;
    int screen_rows;
    int screen_cols;
    char flags;
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
    raw.c_cc[VTIME] = 100000;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

char
editor_read_key(){
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) == -1){
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    return c;
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

struct ABuf{
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void ab_append(struct ABuf *abuf, char *s, int len){
    char *new = realloc(abuf->b, abuf->len + len);

    if (new == NULL) return;
    memcpy(&new[abuf->len], s, len);
    abuf->b = new;
    abuf->len += len;
}

void ab_free(struct ABuf *ab){
    free(ab->b);
}

void
init_editor(){
    E.cx = 0;
    E.cy = 0;
    E.flags = 0;

    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) die("init");
}

#define DIM_VERSION "0.0.1"

void
editor_draw_rows(struct ABuf *ab){
    int y;
    for(y = 0; y < E.screen_rows; ++y){
        if (y == E.screen_rows / 3){
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
editor_move_curosr(char key){
    if ((E.flags & VisualMode) == E.flags){
        switch(key){
            case LEFT_ARROW:
                if (E.cx != 0){
                    --E.cx;
                }
                break;
            case RIGHT_ARROW:
                if (E.cx != E.screen_cols - 1){
                    ++E.cx;
                }
                break;
            case UP_ARROW:
                if (E.cy != 0){
                    --E.cy;
                }
                break;
            case DOWN_ARROW:
                if (E.cy != E.screen_rows - 1){
                    ++E.cy;
                }
                break;
        }
    }
}

void
editor_process_keypress(){
    char c = editor_read_key();

    switch (c){
        case 'q':
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 3);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case LEFT_ARROW:
        case RIGHT_ARROW:
        case UP_ARROW:
        case DOWN_ARROW:
            editor_move_curosr(c);
            break;
        case ESCAPE:
            E.flags &= VisualMode;
            break;
        case 'i':
            E.flags |= InsertMode;
            break;
    }
}

int
main (){
    enable_raw_mode();
    init_editor();

    while (true){
        editor_refresh_screen();
        editor_process_keypress();
    }
    printf("row %d, col %d", E.screen_rows, E.screen_cols);
    return 0;
}
