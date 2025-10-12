#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ncurses.h>

#include "common.h"

#define MAX_LINES 1024
#define MAX_LINE_LEN 1024

// --- Client State ---
typedef struct {
    StyledChar content[MAX_LINES][MAX_LINE_LEN];
    int line_lengths[MAX_LINES];
    int num_lines;
} Document;

Document local_doc;
Cursor user_cursor;
Cursor other_cursors[MAX_CLIENTS];
int my_user_id = -1;
int server_sock;
char status_message[100] = "";
uint8_t current_format = ATTR_NONE;

pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ncurses_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Function Prototypes ---
void init_tui();
void cleanup_tui();
void redraw();
void *receive_handler(void *socket_desc);
void send_op(NetMessage *msg);
void apply_server_message(NetMessage *msg);
void toggle_format(uint8_t format);

// --- Main Function ---
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        exit(1);
    }

    struct sockaddr_in serv_addr;
    pthread_t recv_thread;

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return -1;
    }
    if (connect(server_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        return -1;
    }

    memset(&local_doc, 0, sizeof(Document));
    local_doc.num_lines = 1;
    memset(other_cursors, 0, sizeof(other_cursors));

    init_tui();
    
    if (pthread_create(&recv_thread, NULL, receive_handler, &server_sock) < 0) {
        perror("could not create thread");
        cleanup_tui();
        return 1;
    }

    // --- Main Input Loop ---
    int ch;
    while ((ch = getch()) != KEY_F(1)) {
        NetMessage msg;
        bool op_sent = false;

        pthread_mutex_lock(&doc_mutex);
        
        switch (ch) {
            case KEY_UP: if (user_cursor.y > 0) user_cursor.y--; break;
            case KEY_DOWN: if (user_cursor.y < local_doc.num_lines - 1) user_cursor.y++; break;
            case KEY_LEFT: if (user_cursor.x > 0) user_cursor.x--; break;
            case KEY_RIGHT: if (user_cursor.x < local_doc.line_lengths[user_cursor.y]) user_cursor.x++; break;
            
            case KEY_BACKSPACE:
            case 127:
                if (user_cursor.x > 0) {
                    msg.type = C2S_DELETE;
                    msg.payload.delete_op.y = user_cursor.y;
                    msg.payload.delete_op.x = user_cursor.x;
                    // Local cursor update happens after server confirmation for correctness
                    op_sent = true;
                } else if (user_cursor.y > 0) {
                    // Backspace at the beginning of a line
                    msg.type = C2S_MERGE_LINE;
                    msg.payload.delete_op.y = user_cursor.y;
                    op_sent = true;
                }
                break;
            
            case KEY_DC: // Delete key
                if (user_cursor.x < local_doc.line_lengths[user_cursor.y]) {
                    msg.type = C2S_DELETE_FORWARD;
                    msg.payload.delete_op.y = user_cursor.y;
                    msg.payload.delete_op.x = user_cursor.x;
                    op_sent = true;
                }
                break;

            case 10: // Enter key
                msg.type = C2S_NEWLINE;
                msg.payload.newline_op.y = user_cursor.y;
                msg.payload.newline_op.x = user_cursor.x;
                op_sent = true;
                break;
                
            case 'b' & 0x1F: toggle_format(ATTR_BOLD); break;
            case 'u' & 0x1F: toggle_format(ATTR_UNDERLINE); break;
            
            case 'r' & 0x1F:
                 msg.type = C2S_RUN_CODE;
                 op_sent = true;
                 snprintf(status_message, sizeof(status_message), "Sent run command...");
                 break;
                 
            default:
                if (ch >= 32 && ch <= 126) {
                    msg.type = C2S_INSERT;
                    msg.payload.insert_op.y = user_cursor.y;
                    msg.payload.insert_op.x = user_cursor.x;
                    msg.payload.insert_op.s_char.ch = ch;
                    msg.payload.insert_op.s_char.format = current_format;
                    op_sent = true;
                }
                break;
        }

        if (user_cursor.x > local_doc.line_lengths[user_cursor.y]) {
            user_cursor.x = local_doc.line_lengths[user_cursor.y];
        }

        pthread_mutex_unlock(&doc_mutex);
        
        if (op_sent) {
            send_op(&msg);
        }

        // Always send cursor update after any action
        NetMessage cursor_msg;
        cursor_msg.type = C2S_CURSOR_MOVE;
        user_cursor.user_id = my_user_id;
        user_cursor.active = true;
        cursor_msg.payload.cursor_op = user_cursor;
        send_op(&cursor_msg);

        redraw();
    }

    cleanup_tui();
    close(server_sock);
    return 0;
}

// --- Implementation ---

void *receive_handler(void *socket_desc) {
    int sock = *(int*)socket_desc;
    NetMessage msg;

    while (recv(sock, &msg, sizeof(NetMessage), 0) > 0) {
        pthread_mutex_lock(&doc_mutex);
        apply_server_message(&msg);
        pthread_mutex_unlock(&doc_mutex);
        redraw();
    }
    return NULL;
}

void apply_server_message(NetMessage *msg) {
    int y, x;
    switch ((S2C_MsgType)msg->type) {
        case S2C_INITIAL_STATE:
            break;

        case S2C_USER_ID_ASSIGN:
            my_user_id = msg->user_id;
            user_cursor.user_id = my_user_id;
            break;

        case S2C_INSERT:
            y = msg->payload.insert_op.y;
            x = msg->payload.insert_op.x;
            if (y >= MAX_LINES || x > local_doc.line_lengths[y] || x >= MAX_LINE_LEN) return;
            if (y >= local_doc.num_lines) local_doc.num_lines = y + 1;

            memmove(&local_doc.content[y][x + 1], &local_doc.content[y][x], (local_doc.line_lengths[y] - x) * sizeof(StyledChar));
            local_doc.content[y][x] = msg->payload.insert_op.s_char;
            local_doc.line_lengths[y]++;
            
            if (msg->user_id == my_user_id) user_cursor.x++;
            else if (y == user_cursor.y && x < user_cursor.x) user_cursor.x++;
            break;

        case S2C_DELETE:
            y = msg->payload.delete_op.y;
            x = msg->payload.delete_op.x;
            if (y >= local_doc.num_lines || x <= 0 || x > local_doc.line_lengths[y]) return;
            
            memmove(&local_doc.content[y][x - 1], &local_doc.content[y][x], (local_doc.line_lengths[y] - x + 1) * sizeof(StyledChar));
            local_doc.line_lengths[y]--;

            if (msg->user_id == my_user_id) user_cursor.x--;
            else if (y == user_cursor.y && x <= user_cursor.x) user_cursor.x--;
            break;
        
        case S2C_DELETE_FORWARD:
            y = msg->payload.delete_op.y;
            x = msg->payload.delete_op.x;
            if (y >= local_doc.num_lines || x >= local_doc.line_lengths[y]) return;

            memmove(&local_doc.content[y][x], &local_doc.content[y][x + 1], (local_doc.line_lengths[y] - x) * sizeof(StyledChar));
            local_doc.line_lengths[y]--;

            if (msg->user_id != my_user_id && y == user_cursor.y && x < user_cursor.x) user_cursor.x--;
            break;

        case S2C_MERGE_LINE:
            y = msg->payload.delete_op.y; // line that was merged up and removed
            x = msg->payload.delete_op.x; // merge point x-coordinate
            if (y <= 0 || y >= local_doc.num_lines) return;

            int dest_y = y - 1;
            int len_to_move = local_doc.line_lengths[y];
            memcpy(&local_doc.content[dest_y][x], &local_doc.content[y][0], len_to_move * sizeof(StyledChar));
            local_doc.line_lengths[dest_y] += len_to_move;

            memmove(&local_doc.content[y], &local_doc.content[y + 1], (local_doc.num_lines - y - 1) * sizeof(local_doc.content[0]));
            memmove(&local_doc.line_lengths[y], &local_doc.line_lengths[y + 1], (local_doc.num_lines - y - 1) * sizeof(int));
            local_doc.num_lines--;

            // Update cursors
            if (msg->user_id == my_user_id) {
                user_cursor.y = dest_y;
                user_cursor.x = x;
            } else {
                if (user_cursor.y == y) { user_cursor.y = dest_y; user_cursor.x += x; } 
                else if (user_cursor.y > y) { user_cursor.y--; }
            }
            break;
            
        case S2C_NEWLINE:
            y = msg->payload.newline_op.y;
            x = msg->payload.newline_op.x;
            if (y >= local_doc.num_lines || x > local_doc.line_lengths[y] || local_doc.num_lines >= MAX_LINES) return;

            len_to_move = local_doc.line_lengths[y] - x;
            StyledChar buffer[MAX_LINE_LEN];
            memcpy(buffer, &local_doc.content[y][x], len_to_move * sizeof(StyledChar));

            memmove(&local_doc.content[y + 2], &local_doc.content[y + 1], (local_doc.num_lines - y - 1) * sizeof(local_doc.content[0]));
            memmove(&local_doc.line_lengths[y + 2], &local_doc.line_lengths[y + 1], (local_doc.num_lines - y - 1) * sizeof(int));
            
            local_doc.line_lengths[y] = x;
            local_doc.line_lengths[y + 1] = len_to_move;
            memcpy(&local_doc.content[y + 1][0], buffer, len_to_move * sizeof(StyledChar));
            local_doc.num_lines++;

            if (msg->user_id == my_user_id) { user_cursor.y++; user_cursor.x = 0; }
            else if (y < user_cursor.y) { user_cursor.y++; }
            break;

        case S2C_FORMAT_UPDATE:
             y = msg->payload.format_op.y;
             x = msg->payload.format_op.x;
             if (y < local_doc.num_lines && x < local_doc.line_lengths[y]) {
                 local_doc.content[y][x].format = msg->payload.format_op.format;
             }
            break;

        case S2C_CURSOR_UPDATE:
            if (msg->payload.cursor_op.user_id < MAX_CLIENTS && msg->payload.cursor_op.user_id != my_user_id) {
                other_cursors[msg->payload.cursor_op.user_id] = msg->payload.cursor_op;
            }
            break;
        
        case S2C_CODE_OUTPUT:
            snprintf(status_message, sizeof(status_message), "%.95s", msg->payload.code_output);
            break;
    }
}


void redraw() {
    pthread_mutex_lock(&ncurses_mutex);
    pthread_mutex_lock(&doc_mutex);
    
    erase();

    for (int y = 0; y < local_doc.num_lines; y++) {
        for (int x = 0; x < local_doc.line_lengths[y]; x++) {
            StyledChar s_char = local_doc.content[y][x];
            int attrs = 0;
            if (s_char.format & ATTR_BOLD) attrs |= A_BOLD;
            if (s_char.format & ATTR_UNDERLINE) attrs |= A_UNDERLINE;
            
            attron(attrs);
            mvaddch(y, x, s_char.ch);
            attroff(attrs);
        }
    }
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (other_cursors[i].active && i != my_user_id) {
            chtype ch_under_cursor = mvinch(other_cursors[i].y, other_cursors[i].x);
            attron(COLOR_PAIR((i % 7) + 1));
            mvaddch(other_cursors[i].y, other_cursors[i].x, ch_under_cursor ? ch_under_cursor : ' ');
            attroff(COLOR_PAIR((i % 7) + 1));
        }
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    attron(A_REVERSE);
    mvprintw(max_y - 2, 0, " F1: Exit | C-b: Bold | C-u: Underline | C-r: Run Code | Del: Fwd-Del");
    clrtoeol();
    char format_str[20] = {0};
    if(current_format & ATTR_BOLD) strcat(format_str, "B ");
    if(current_format & ATTR_UNDERLINE) strcat(format_str, "U");
    mvprintw(max_y - 2, max_x - 30, "Ln %d, Col %d | Format: %s", user_cursor.y + 1, user_cursor.x + 1, format_str);
    mvprintw(max_y - 1, 0, "%s", status_message);
    clrtoeol();
    attroff(A_REVERSE);

    move(user_cursor.y, user_cursor.x);
    refresh();

    pthread_mutex_unlock(&doc_mutex);
    pthread_mutex_unlock(&ncurses_mutex);
}

void init_tui() {
    initscr(); raw(); keypad(stdscr, TRUE); noecho(); start_color();
    init_pair(1, COLOR_WHITE, COLOR_RED);
    init_pair(2, COLOR_WHITE, COLOR_GREEN);
    init_pair(3, COLOR_BLACK, COLOR_YELLOW);
    init_pair(4, COLOR_WHITE, COLOR_BLUE);
    init_pair(5, COLOR_WHITE, COLOR_MAGENTA);
    init_pair(6, COLOR_BLACK, COLOR_CYAN);
    init_pair(7, COLOR_BLACK, COLOR_WHITE);
}

void cleanup_tui() { endwin(); }

void send_op(NetMessage *msg) {
    msg->user_id = my_user_id;
    send(server_sock, msg, sizeof(NetMessage), 0);
}

void toggle_format(uint8_t format_to_toggle) {
    current_format ^= format_to_toggle;
    int y = user_cursor.y;
    int x = user_cursor.x;
    if (x > 0 && x <= local_doc.line_lengths[y]) {
        NetMessage msg;
        msg.type = C2S_FORMAT_CHANGE;
        msg.payload.format_op.y = y;
        msg.payload.format_op.x = x - 1; // Format character to the left of cursor
        msg.payload.format_op.format = local_doc.content[y][x-1].format ^ format_to_toggle;
        send_op(&msg);
    }
}

