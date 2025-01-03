/* main.c - Dim text editor implementation */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>

/* Defines and constants */
#define true 1
#define false 0
#define command_size 128
#define CTRL_KEY(k) ((k) & 0x1f)
#define DIM_VERSION "0.0.1"
#define DIM_TAB_STOP 8

/* Editor modes */
enum DimModes {
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

/* Special key codes */
enum EditorKeys {
    ESCAPE_KEY = 27,
    RETURN_KEY = 13,  // Change to 10 for Linux
    BACKSPACE = 127,
    TIMEOUT_KEY = 0,
    LEFT_ARROW = 1000,
    RIGHT_ARROW,
    DOWN_ARROW,
    UP_ARROW,
    PAGE_UP,
    PAGE_DOWN
};

/* Data structures */
typedef struct erow {
    int size;
    int r_size;
    char *chars;
    char *render;
} erow;

struct EditorConfig {
    int cx, cy;
    int rx;
    int screen_rows, screen_cols;
    int row_off, col_off;
    int num_rows;
    char mode;
    erow *row;
    char *file_name;
    struct termios og_termios;
};

struct ABuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

/* Global editor state */
struct EditorConfig E;

/* Function implementations */

/* Terminal handling */
void die(const char *c) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(c);
    exit(1);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.og_termios) == -1)
        die("tcsetattr");
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &E.og_termios) == -1)
        die("tcgetattr");
    atexit(disable_raw_mode);

    struct termios raw = E.og_termios;
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

/* Buffer operations */
void ab_append(struct ABuf *abuf, char *s, int len) {
    char *new = realloc(abuf->b, abuf->len + len);
    if (new == NULL) return;
    memcpy(&new[abuf->len], s, len);
    abuf->b = new;
    abuf->len += len;
}

void ab_free(struct ABuf *ab) {
    free(ab->b);
}

/* Input handling */
int editor_read_key() {
    int nread;
    char c = 0;
    while ((nread = read(STDIN_FILENO, &c, 1)) == -1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    if (c == ESCAPE_KEY) {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) == -1) return ESCAPE_KEY;
        if (read(STDIN_FILENO, &seq[1], 1) == -1) return ESCAPE_KEY;

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) == -1) return ESCAPE_KEY;
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                    }
                }
            }
            switch (seq[1]) {
                case 'A': return UP_ARROW;
                case 'B': return DOWN_ARROW;
                case 'C': return RIGHT_ARROW;
                case 'D': return LEFT_ARROW;
            }
        }
        return ESCAPE_KEY;
    }
    return c;
}

/* Window size handling */
int get_cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/* Row operations */
int editor_row_cx_to_rx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (DIM_TAB_STOP - 1) - (rx % DIM_TAB_STOP);
        rx++;
    }
    return rx;
}

void editor_update_row(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (DIM_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % DIM_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->r_size = idx;
}

void editor_insert_row(int at, char *s, size_t len) {
    if (at < 0 || at > E.num_rows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.num_rows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].r_size = 0;
    E.row[at].render = NULL;
    editor_update_row(&E.row[at]);
    E.num_rows++;
}

void editor_free_row(erow *row) {
    free(row->render);
    free(row->chars);
}

void editor_del_row(int at) {
    if (at < 0 || at >= E.num_rows) return;
    editor_free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.num_rows - at - 1));
    E.num_rows--;
}

void editor_row_insert_char(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
}

void editor_row_append_string(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
}

void editor_row_del_char(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
}

/* Editor operations */
void editor_insert_char(int c) {
    if (E.cy == E.num_rows) {
        editor_insert_row(E.num_rows, "", 0);
    }
    editor_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editor_del_char() {
    if (E.cy == E.num_rows) return;
    if (E.cx == 0 && E.cy == 0) return;
    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editor_row_del_char(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editor_row_append_string(&E.row[E.cy - 1], row->chars, row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
}

void editorInsertNewline() {
    if (E.cx == 0) {
        editor_insert_row(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    E.cy++;
    E.cx = 0;
}

char* editor_rows_to_string(int *buf_len) {
    int tot_len = 0;
    for (int j = 0; j < E.num_rows; j++) {
        tot_len += E.row[j].size + 1;
    }
    *buf_len = tot_len;

    char *buf = malloc(tot_len);
    char *p = buf;
    for (int j = 0; j < E.num_rows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editor_open(char *file_name) {
    free(E.file_name);
    E.file_name = strdup(file_name);

    FILE *fp = fopen(file_name, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t line_cap = 8;
    ssize_t line_len;

    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line_len--;
        editor_insert_row(E.num_rows, line, line_len);
    }
    free(line);
    fclose(fp);
}

void editor_save() {
    if (E.file_name == NULL) return;

    int len;
    char *buf = editor_rows_to_string(&len);

    int fd = open(E.file_name, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                printf("saved file %d bytes written", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    printf("not saved file");
}

/* Rendering */
void editor_scroll() {
    E.rx = 0;
    if (E.cy < E.num_rows) {
        E.rx = editor_row_cx_to_rx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.row_off) {
        E.row_off = E.cy;
    }
    if (E.cy >= E.row_off + E.screen_rows) {
        E.row_off = E.cy - E.screen_rows + 1;
    }
    if (E.rx < E.col_off) {
        E.col_off = E.rx;
    }
    if (E.rx >= E.col_off + E.screen_cols) {
        E.col_off = E.rx - E.screen_cols + 1;
    }
}

void editor_draw_rows(struct ABuf *ab) {
    for (int y = 0; y < E.screen_rows; y++) {
        int filerow = y + E.row_off;
        if (filerow >= E.num_rows) {
            if (E.num_rows == 0 && y == E.screen_rows / 3) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                                       "dim editor --version %s", DIM_VERSION);
                if (welcome_len > E.screen_cols) welcome_len = E.screen_cols;
                int padding = (E.screen_cols - welcome_len) / 2;
                if (padding) {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding--) ab_append(ab, " ", 1);
                ab_append(ab, welcome, welcome_len);
            } else {
                ab_append(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].r_size - E.col_off;
            if (len < 0) len = 0;
            if (len > E.screen_cols) len = E.screen_cols;
            ab_append(ab, &E.row[filerow].render[E.col_off], len);
        }

        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
    }
}

void editor_draw_status_bar(struct ABuf *ab) {
    ab_append(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                     E.file_name ? E.file_name : "[No Name]", E.num_rows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
                      E.cy + 1, E.num_rows);
    if (len > E.screen_cols) len = E.screen_cols;
    ab_append(ab, status, len);
    while (len < E.screen_cols) {
        if (E.screen_cols - len == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        } else {
            ab_append(ab, " ", 1);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 3);
}

void editor_refresh_screen() {
    editor_scroll();

    struct ABuf ab = ABUF_INIT;
    
    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.row_off) + 1, (E.rx - E.col_off) + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/* Mode handlers */
void handle_normal() {
    int key = editor_read_key();
    erow *row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
    
    switch (key) {
        case TIMEOUT_KEY:
            break;
        case LEFT_ARROW:
        case 'h':
            if (E.cx != 0) {
                --E.cx;
            }
            break;
        case RIGHT_ARROW:
        case 'l':
            if (row && E.cx < row->size) {
                ++E.cx;
            }
            break;
        case UP_ARROW:
        case 'k':
            if (E.cy != 0) {
                --E.cy;
            }
            break;
        case RETURN_KEY:
        case DOWN_ARROW:
        case 'j':
            if (E.cy < E.num_rows) {
                ++E.cy;
            }
            break;
        case ':':
            E.mode = CLMode;
            break;
        case 'i':
            E.mode = InsertMode;
            break;
        default:
            break;
    }
}

void handle_insert() {
    int c = editor_read_key();
    erow *row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
    
    switch (c) {    
        case '\r':
            editorInsertNewline();
            break;
        case TIMEOUT_KEY:
            break;
        case CTRL_KEY('s'):
            editor_save();
            break;
        case LEFT_ARROW:
            if (E.cx != 0) {
                --E.cx;
            }
            break;
        case RIGHT_ARROW:
            if (row && E.cx < row->size) {
                ++E.cx;
            }
            break;
        case UP_ARROW:
            if (E.cy != 0) {
                --E.cy;
            }
            break;
        case DOWN_ARROW:
            if (E.cy < E.num_rows) {
                ++E.cy;
            }
            break;
        case PAGE_DOWN:
            {
                E.cy = E.row_off + E.screen_rows - 1;
                if (E.cy > E.num_rows) E.cy = E.num_rows;

                int times = E.screen_cols;
                while (times--) {
                    if (E.cy != E.screen_rows - 1) {
                        ++E.cy;
                    } else {
                        break;
                    }
                }
                break;
            }
        case PAGE_UP:
            {
                E.cy = E.row_off;
                int times = E.screen_cols;
                while (times--) {
                    if (E.cy != 0) {
                        --E.cy;
                    } else {
                        break;
                    }
                }
                break;
            }
        case ESCAPE_KEY:
            E.mode = NormalMode;
            break;
        case BACKSPACE:
            editor_del_char();
            break;
        default:
            editor_insert_char(c);
            break;
    }
    
    row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void handle_cl() {
    char command[command_size] = {0};
    unsigned char index = 0;
    int key = 0;
    
    while (index < command_size) {
        key = editor_read_key();
        switch (key) {
            case UP_ARROW:
            case DOWN_ARROW:
            case 0:
                continue;
            case BACKSPACE:
                if (index > 0) {
                    --index;
                }
                continue;
            case ESCAPE_KEY:
                E.mode = NormalMode;
                return;
            case RETURN_KEY:
                command[index] = '\0';
                if (strcmp(command, "q") == 0) {
                    write(STDOUT_FILENO, "\x1b[2J", 4);
                    write(STDOUT_FILENO, "\x1b[H", 3);
                    exit(0);
                    return;
                }
                if (strcmp(command, "wq") == 0) {
                    editor_save();
                    write(STDOUT_FILENO, "\x1b[2J", 4);
                    write(STDOUT_FILENO, "\x1b[H", 3);
                    exit(0);
                    return;
                }
                if (strcmp(command, "w") == 0) {
                    editor_save();
                    return;
                }
                return;
            default:
                command[index] = (char)key;
                break;
        }
        ++index;
        key = 0;
    }
}

void editor_process_keypress() {
    switch (E.mode) {
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

/* Editor initialization */
void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.num_rows = 0;
    E.row_off = 0;
    E.col_off = 0;
    E.mode = NormalMode;
    E.row = NULL;
    E.file_name = NULL;

    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1)
        die("get_window_size");
    E.screen_rows -= 1;
}

/* Main entry point */
int main(int argc, char *argv[]) {
    enable_raw_mode();
    init_editor();
    
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    while (true) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
