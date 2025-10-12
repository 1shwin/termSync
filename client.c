// client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ncurses.h>

#include "common.h"

// --- Client State ---
typedef struct {
    StyledChar content[MAX_LINES][MAX_LINE_LEN];
    int        line_lengths[MAX_LINES];
    int        num_lines;
} Document;

static Document local_doc;

static Cursor user_cursor;
static Cursor other_cursors[MAX_CLIENTS];

static SelectionState other_selections[MAX_CLIENTS]; // remote selections

static int     my_user_id   = -1;
static int     server_sock  = -1;
static char    status_message[100] = "";
static uint8_t current_format      = ATTR_NONE;

static pthread_mutex_t doc_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ncurses_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Selection (local-only state) ---
typedef struct {
    bool active;
    int  ay, ax; // anchor
    int  hy, hx; // head
} Selection;

static Selection selection = (Selection){0};

// --- Prototypes ---
static void init_tui(void);
static void cleanup_tui(void);
static void redraw(void);
static void *receive_handler(void *socket_desc);
static void send_op(NetMessage *msg);
static void apply_server_message(NetMessage *msg);
static void toggle_format(uint8_t format_to_toggle);

// Selection helpers
static void selection_clear(void);
static void selection_start_from_cursor(void);
static void selection_update_head_to_cursor(void);
static void selection_get_normalized(int *y1, int *x1, int *y2, int *x2);
static void send_selection_update(void);

// Robust send/recv
static ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf; size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, 0);
        if (n <= 0) return n;
        off += (size_t)n;
    }
    return (ssize_t)off;
}
static ssize_t recv_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf; size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, p + off, len - off, 0);
        if (n <= 0) return n;
        off += (size_t)n;
    }
    return (ssize_t)off;
}

// --- Main ---
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
    serv_addr.sin_port   = htons(PORT);
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
    memset(other_cursors,    0, sizeof(other_cursors));
    memset(other_selections, 0, sizeof(other_selections));
    user_cursor.x = user_cursor.y = 0;
    user_cursor.active = true;

    init_tui();

    if (pthread_create(&recv_thread, NULL, receive_handler, &server_sock) < 0) {
        perror("could not create thread");
        cleanup_tui();
        return 1;
    }
    pthread_detach(recv_thread);

    // --- Main Input Loop ---
    int ch;
    while ((ch = getch()) != KEY_F(1)) {
        NetMessage msg = (NetMessage){0};
        bool queued_send = false; // send msg after switch

        pthread_mutex_lock(&doc_mutex);

        switch (ch) {
            // --- Shift selection (Left/Right) ---
            case KEY_SLEFT: {
                if (!selection.active) selection_start_from_cursor();
                if (user_cursor.x > 0) user_cursor.x--;
                else if (user_cursor.y > 0) {
                    user_cursor.y--;
                    user_cursor.x = local_doc.line_lengths[user_cursor.y];
                }
                selection_update_head_to_cursor();
                send_selection_update();
                break;
            }
            case KEY_SRIGHT: {
                if (!selection.active) selection_start_from_cursor();
                if (user_cursor.x < local_doc.line_lengths[user_cursor.y]) user_cursor.x++;
                else if (user_cursor.y < local_doc.num_lines - 1) {
                    user_cursor.y++;
                    user_cursor.x = 0;
                }
                selection_update_head_to_cursor();
                send_selection_update();
                break;
            }

            // --- Plain arrows: move & clear selection ---
            case KEY_UP:
                if (user_cursor.y > 0) {
                    user_cursor.y--;
                    if (user_cursor.x > local_doc.line_lengths[user_cursor.y])
                        user_cursor.x = local_doc.line_lengths[user_cursor.y];
                }
                selection_clear();
                send_selection_update();
                break;

            case KEY_DOWN:
                if (user_cursor.y < local_doc.num_lines - 1) {
                    user_cursor.y++;
                    if (user_cursor.x > local_doc.line_lengths[user_cursor.y])
                        user_cursor.x = local_doc.line_lengths[user_cursor.y];
                }
                selection_clear();
                send_selection_update();
                break;

            case KEY_LEFT:
                if (user_cursor.x > 0) {
                    user_cursor.x--;
                } else if (user_cursor.y > 0) {
                    user_cursor.y--;
                    user_cursor.x = local_doc.line_lengths[user_cursor.y];
                }
                selection_clear();
                send_selection_update();
                break;

            case KEY_RIGHT:
                if (user_cursor.x < local_doc.line_lengths[user_cursor.y]) {
                    user_cursor.x++;
                } else if (user_cursor.y < local_doc.num_lines - 1) {
                    user_cursor.y++;
                    user_cursor.x = 0;
                }
                selection_clear();
                send_selection_update();
                break;

            // --- Backspace (with range delete) ---
            case KEY_BACKSPACE:
            case 127:
                if (selection.active) {
                    int y1,x1,y2,x2; selection_get_normalized(&y1,&x1,&y2,&x2);
                    NetMessage dmsg = (NetMessage){0};
                    dmsg.type = C2S_DELETE_RANGE;
                    dmsg.payload.delete_range_op.range = (Range){y1,x1,y2,x2};
                    send_op(&dmsg);
                    selection_clear();
                    send_selection_update();
                } else {
                    if (user_cursor.x > 0) {
                        msg.type = C2S_DELETE;
                        msg.payload.delete_op.y = user_cursor.y;
                        msg.payload.delete_op.x = user_cursor.x;
                        queued_send = true;
                    } else if (user_cursor.y > 0) {
                        msg.type = C2S_MERGE_LINE;
                        msg.payload.delete_op.y = user_cursor.y;
                        queued_send = true;
                    }
                }
                break;

            // --- Delete forward ---
            case KEY_DC:
                if (user_cursor.x < local_doc.line_lengths[user_cursor.y]) {
                    msg.type = C2S_DELETE_FORWARD;
                    msg.payload.delete_op.y = user_cursor.y;
                    msg.payload.delete_op.x = user_cursor.x;
                    queued_send = true;
                }
                break;

            // --- Enter (newline) ---
            case 10:
                msg.type = C2S_NEWLINE;
                msg.payload.newline_op.y = user_cursor.y;
                msg.payload.newline_op.x = user_cursor.x;
                selection_clear();
                send_selection_update();
                queued_send = true;
                break;

            // --- Ctrl-B / Ctrl-U ---
            case ('b' & 0x1F): {
                if (selection.active) {
                    int y1,x1,y2,x2; selection_get_normalized(&y1,&x1,&y2,&x2);
                    NetMessage fmsg = (NetMessage){0};
                    fmsg.type = C2S_FORMAT_RANGE;
                    fmsg.payload.format_range_op.range = (Range){y1,x1,y2,x2};
                    fmsg.payload.format_range_op.format_toggle_bits = ATTR_BOLD;
                    send_op(&fmsg);
                    selection_clear();
                    send_selection_update();
                } else {
                    toggle_format(ATTR_BOLD);
                }
                break;
            }
            case ('u' & 0x1F): {
                if (selection.active) {
                    int y1,x1,y2,x2; selection_get_normalized(&y1,&x1,&y2,&x2);
                    NetMessage fmsg = (NetMessage){0};
                    fmsg.type = C2S_FORMAT_RANGE;
                    fmsg.payload.format_range_op.range = (Range){y1,x1,y2,x2};
                    fmsg.payload.format_range_op.format_toggle_bits = ATTR_UNDERLINE;
                    send_op(&fmsg);
                    selection_clear();
                    send_selection_update();
                } else {
                    toggle_format(ATTR_UNDERLINE);
                }
                break;
            }

            // --- Ctrl-R: run code block ---
            case ('r' & 0x1F):
                msg.type = C2S_RUN_CODE;
                snprintf(status_message, sizeof(status_message), "Sent run command...");
                queued_send = true;
                break;

            // --- Printable: overwrite selection or insert normally ---
            default:
                if (ch >= 32 && ch <= 126) {
                    if (selection.active) {
                        int y1,x1,y2,x2; selection_get_normalized(&y1,&x1,&y2,&x2);

                        NetMessage dmsg = (NetMessage){0};
                        dmsg.type = C2S_DELETE_RANGE;
                        dmsg.payload.delete_range_op.range = (Range){y1,x1,y2,x2};
                        send_op(&dmsg);

                        NetMessage imsg = (NetMessage){0};
                        imsg.type = C2S_INSERT;
                        imsg.payload.insert_op.y = y1;
                        imsg.payload.insert_op.x = x1;
                        imsg.payload.insert_op.s_char.ch = ch;
                        imsg.payload.insert_op.s_char.format = current_format;
                        send_op(&imsg);

                        selection_clear();
                        send_selection_update();
                    } else {
                        msg.type = C2S_INSERT;
                        msg.payload.insert_op.y = user_cursor.y;
                        msg.payload.insert_op.x = user_cursor.x;
                        msg.payload.insert_op.s_char.ch = ch;
                        msg.payload.insert_op.s_char.format = current_format;
                        queued_send = true;
                    }
                }
                break;
        }

        if (user_cursor.x > local_doc.line_lengths[user_cursor.y]) {
            user_cursor.x = local_doc.line_lengths[user_cursor.y];
        }

        pthread_mutex_unlock(&doc_mutex);

        // Send queued message (if any)
        if (queued_send) send_op(&msg);

        // Always send cursor update
        NetMessage cursor_msg = (NetMessage){0};
        cursor_msg.type = C2S_CURSOR_MOVE;
        user_cursor.user_id = my_user_id;
        user_cursor.active  = true;
        cursor_msg.payload.cursor_op = user_cursor;
        send_op(&cursor_msg);

        redraw();
    }

    cleanup_tui();
    close(server_sock);
    return 0;
}

// --- Implementation ---
static void *receive_handler(void *socket_desc) {
    int sock = *(int *)socket_desc;
    NetMessage msg;

    while (recv_all(sock, &msg, sizeof(NetMessage)) > 0) {
        pthread_mutex_lock(&doc_mutex);
        apply_server_message(&msg);
        pthread_mutex_unlock(&doc_mutex);
        redraw();
    }
    return NULL;
}

static void apply_server_message(NetMessage *msg) {
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

            memmove(&local_doc.content[y][x + 1], &local_doc.content[y][x],
                    (local_doc.line_lengths[y] - x) * sizeof(StyledChar));
            local_doc.content[y][x] = msg->payload.insert_op.s_char;
            local_doc.line_lengths[y]++;

            if (msg->user_id == my_user_id) user_cursor.x++;
            else if (y == user_cursor.y && x < user_cursor.x) user_cursor.x++;
            break;

        case S2C_DELETE:
            y = msg->payload.delete_op.y;
            x = msg->payload.delete_op.x;
            if (y >= local_doc.num_lines || x <= 0 || x > local_doc.line_lengths[y]) return;

            memmove(&local_doc.content[y][x - 1], &local_doc.content[y][x],
                    (local_doc.line_lengths[y] - x + 1) * sizeof(StyledChar));
            local_doc.line_lengths[y]--;

            if (msg->user_id == my_user_id) user_cursor.x--;
            else if (y == user_cursor.y && x <= user_cursor.x) user_cursor.x--;
            break;

        case S2C_DELETE_FORWARD:
            y = msg->payload.delete_op.y;
            x = msg->payload.delete_op.x;
            if (y >= local_doc.num_lines || x >= local_doc.line_lengths[y]) return;

            memmove(&local_doc.content[y][x], &local_doc.content[y][x + 1],
                    (local_doc.line_lengths[y] - x) * sizeof(StyledChar));
            local_doc.line_lengths[y]--;

            if (msg->user_id != my_user_id && y == user_cursor.y && x < user_cursor.x) user_cursor.x--;
            break;

        case S2C_MERGE_LINE: {
            y = msg->payload.delete_op.y; // removed line index
            x = msg->payload.delete_op.x; // merge point in dest line
            if (y <= 0 || y >= local_doc.num_lines) return;

            int dest_y = y - 1;
            int len_to_move = local_doc.line_lengths[y];

            memcpy(&local_doc.content[dest_y][x], &local_doc.content[y][0],
                   len_to_move * sizeof(StyledChar));
            local_doc.line_lengths[dest_y] += len_to_move;

            memmove(&local_doc.content[y], &local_doc.content[y + 1],
                    (local_doc.num_lines - y - 1) * sizeof(local_doc.content[0]));
            memmove(&local_doc.line_lengths[y], &local_doc.line_lengths[y + 1],
                    (local_doc.num_lines - y - 1) * sizeof(int));
            local_doc.num_lines--;

            if (msg->user_id == my_user_id) { user_cursor.y = dest_y; user_cursor.x = x; }
            else {
                if (user_cursor.y == y)      { user_cursor.y = dest_y; user_cursor.x += x; }
                else if (user_cursor.y > y)  { user_cursor.y--; }
            }
            break;
        }

        case S2C_NEWLINE: {
            y = msg->payload.newline_op.y;
            x = msg->payload.newline_op.x;
            if (y >= local_doc.num_lines || x > local_doc.line_lengths[y] || local_doc.num_lines >= MAX_LINES) return;

            int len_to_move = local_doc.line_lengths[y] - x;
            StyledChar buffer[MAX_LINE_LEN];
            memcpy(buffer, &local_doc.content[y][x], len_to_move * sizeof(StyledChar));

            memmove(&local_doc.content[y + 2], &local_doc.content[y + 1],
                    (local_doc.num_lines - y - 1) * sizeof(local_doc.content[0]));
            memmove(&local_doc.line_lengths[y + 2], &local_doc.line_lengths[y + 1],
                    (local_doc.num_lines - y - 1) * sizeof(int));

            local_doc.line_lengths[y]     = x;
            local_doc.line_lengths[y + 1] = len_to_move;
            memcpy(&local_doc.content[y + 1][0], buffer, len_to_move * sizeof(StyledChar));
            local_doc.num_lines++;

            if (msg->user_id == my_user_id) { user_cursor.y++; user_cursor.x = 0; }
            else if (y < user_cursor.y)     { user_cursor.y++; }
            break;
        }

        case S2C_FORMAT_UPDATE:
            y = msg->payload.format_op.y;
            x = msg->payload.format_op.x;
            if (y < local_doc.num_lines && x < local_doc.line_lengths[y]) {
                local_doc.content[y][x].format = msg->payload.format_op.format;
            }
            break;

        case S2C_CURSOR_UPDATE:
            if (msg->payload.cursor_op.user_id < MAX_CLIENTS &&
                msg->payload.cursor_op.user_id != my_user_id) {
                other_cursors[msg->payload.cursor_op.user_id] = msg->payload.cursor_op;
            }
            break;

        case S2C_FORMAT_RANGE: {
            Range r = msg->payload.format_range_op.range;
            uint8_t bits = msg->payload.format_range_op.format_toggle_bits;
            if (r.y2 < r.y1 || (r.y2 == r.y1 && r.x2 < r.x1)) {
                int ty=r.y1, tx=r.x1; r.y1=r.y2; r.x1=r.x2; r.y2=ty; r.x2=tx;
            }
            for (int yy = r.y1; yy <= r.y2; ++yy) {
                int sx = (yy == r.y1) ? r.x1 : 0;
                int ex = (yy == r.y2) ? r.x2 : local_doc.line_lengths[yy];
                if (sx < 0) sx = 0;
                if (ex > local_doc.line_lengths[yy]) ex = local_doc.line_lengths[yy];
                for (int xx = sx; xx < ex; ++xx) {
                    local_doc.content[yy][xx].format ^= bits;
                }
            }
            break;
        }

        case S2C_DELETE_RANGE: {
            Range r = msg->payload.delete_range_op.range;
            if (r.y2 < r.y1 || (r.y2 == r.y1 && r.x2 < r.x1)) {
                int ty=r.y1, tx=r.x1; r.y1=r.y2; r.x1=r.x2; r.y2=ty; r.x2=tx;
            }

            if (r.y1 == r.y2) {
                int yy = r.y1, x1 = r.x1, x2 = r.x2;
                if (yy < 0 || yy >= local_doc.num_lines) break;
                if (x1 < 0 || x2 > local_doc.line_lengths[yy]) break;
                int tail = local_doc.line_lengths[yy] - x2;
                memmove(&local_doc.content[yy][x1], &local_doc.content[yy][x2],
                        tail * sizeof(StyledChar));
                local_doc.line_lengths[yy] -= (x2 - x1);

                if (msg->user_id == my_user_id) { user_cursor.y = yy; user_cursor.x = x1; }
                else if (user_cursor.y == yy) {
                    if (user_cursor.x >= x2)     user_cursor.x -= (x2 - x1);
                    else if (user_cursor.x > x1) user_cursor.x = x1;
                }
            } else {
                int y1 = r.y1, x1 = r.x1, y2 = r.y2, x2 = r.x2;
                if (y1 < 0 || y2 >= local_doc.num_lines) break;
                if (x1 < 0 || x1 > local_doc.line_lengths[y1]) break;
                if (x2 < 0 || x2 > local_doc.line_lengths[y2]) break;

                int keep_len   = x1;
                int suffix_len = local_doc.line_lengths[y2] - x2;

                memcpy(&local_doc.content[y1][x1], &local_doc.content[y2][x2],
                       suffix_len * sizeof(StyledChar));
                local_doc.line_lengths[y1] = keep_len + suffix_len;

                int lines_to_remove = y2 - y1;
                memmove(&local_doc.content[y1 + 1], &local_doc.content[y2 + 1],
                        (local_doc.num_lines - y2 - 1) * sizeof(local_doc.content[0]));
                memmove(&local_doc.line_lengths[y1 + 1], &local_doc.line_lengths[y2 + 1],
                        (local_doc.num_lines - y2 - 1) * sizeof(int));
                local_doc.num_lines -= lines_to_remove;

                if (msg->user_id == my_user_id) { user_cursor.y = y1; user_cursor.x = x1; }
                else {
                    bool inside =
                        (user_cursor.y > y1 && user_cursor.y < y2) ||
                        (user_cursor.y == y1 && user_cursor.x >= x1) ||
                        (user_cursor.y == y2 && user_cursor.x <  x2);
                    if (inside) { user_cursor.y = y1; user_cursor.x = x1; }
                    else if (user_cursor.y > y2) { user_cursor.y -= (y2 - y1); }
                }
            }
            break;
        }

        case S2C_SELECTION_UPDATE: {
            int uid = msg->user_id;
            if (uid >= 0 && uid < MAX_CLIENTS && uid != my_user_id) {
                other_selections[uid] = msg->payload.selection_op;
            }
            break;
        }

        case S2C_CODE_OUTPUT:
            snprintf(status_message, sizeof(status_message), "%.95s", msg->payload.code_output);
            break;
    }
}

static void redraw(void) {
    pthread_mutex_lock(&ncurses_mutex);
    pthread_mutex_lock(&doc_mutex);

    erase();

    // Compute normalized local selection
    bool sel = selection.active;
    int sy=0,sx=0,ey=0,ex=0;
    if (sel) selection_get_normalized(&sy,&sx,&ey,&ex);

    // Draw document with local selection + remote selection overlay
    for (int y = 0; y < local_doc.num_lines; y++) {
        for (int x = 0; x < local_doc.line_lengths[y]; x++) {
            StyledChar s_char = local_doc.content[y][x];
            int attrs = 0;
            if (s_char.format & ATTR_BOLD)      attrs |= A_BOLD;
            if (s_char.format & ATTR_UNDERLINE) attrs |= A_UNDERLINE;

            // Local selection (reverse)
            bool inSelfSel = false;
            if (sel) {
                if (y > sy && y < ey) inSelfSel = true;
                else if (y == sy && y == ey) inSelfSel = (x >= sx && x < ex);
                else if (y == sy) inSelfSel = (x >= sx);
                else if (y == ey) inSelfSel = (x < ex);
            }
            if (inSelfSel) attrs |= A_REVERSE;

            // Remote selection tint (use user color), only if not in our selection
            int remote_pair = 0;
            if (!inSelfSel) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (i == my_user_id) continue;
                    if (!other_selections[i].active) continue;
                    Range r = other_selections[i].range;
                    if (r.y2 < r.y1 || (r.y2 == r.y1 && r.x2 < r.x1)) {
                        int ty=r.y1, tx=r.x1; r.y1=r.y2; r.x1=r.x2; r.y2=ty; r.x2=tx;
                    }
                    bool inRemote = false;
                    if (y > r.y1 && y < r.y2) inRemote = true;
                    else if (y == r.y1 && y == r.y2) inRemote = (x >= r.x1 && x < r.x2);
                    else if (y == r.y1) inRemote = (x >= r.x1);
                    else if (y == r.y2) inRemote = (x < r.x2);

                    if (inRemote) { remote_pair = (i % 7) + 1; break; }
                }
            }

            if (remote_pair) attron(COLOR_PAIR(remote_pair));
            attron(attrs);
            mvaddch(y, x, s_char.ch);
            attroff(attrs);
            if (remote_pair) attroff(COLOR_PAIR(remote_pair));
        }
    }

    // Render other cursors (clamped to screen)
    int max_y, max_x; getmaxyx(stdscr, max_y, max_x);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (other_cursors[i].active && i != my_user_id) {
            int cy = other_cursors[i].y;
            int cx = other_cursors[i].x;
            if (cy < 0) cy = 0;
            if (cx < 0) cx = 0;
            if (cy >= max_y || cx >= max_x) continue;

            chtype ch_under_cursor = mvinch(cy, cx);
            attron(COLOR_PAIR((i % 7) + 1));
            mvaddch(cy, cx, ch_under_cursor ? ch_under_cursor : ' ');
            attroff(COLOR_PAIR((i % 7) + 1));
        }
    }

    // Footer/status
    attron(A_REVERSE);
    mvprintw(max_y - 2, 0,
             " F1: Exit  C-b: Bold  C-u: Underline  C-r: Run Code  Del: Fwd-Del  Shift-Left/Right: Select ");
    clrtoeol();

    char format_str[20] = {0};
    if (current_format & ATTR_BOLD)      strcat(format_str, "B ");
    if (current_format & ATTR_UNDERLINE) strcat(format_str, "U");

    // selection count
    int sel_count = 0;
    if (sel) {
        for (int y2 = sy; y2 <= ey; ++y2) {
            int sx2 = (y2 == sy) ? sx : 0;
            int ex2 = (y2 == ey) ? ex : local_doc.line_lengths[y2];
            if (ex2 > sx2) sel_count += (ex2 - sx2);
        }
    }
    mvprintw(max_y - 2, max_x - 45, "Ln %d, Col %d  Format: %s  Sel: %d",
             user_cursor.y + 1, user_cursor.x + 1, format_str, sel ? sel_count : 0);

    mvprintw(max_y - 1, 0, "%s", status_message);
    clrtoeol();
    attroff(A_REVERSE);

    move(user_cursor.y, user_cursor.x);
    refresh();

    pthread_mutex_unlock(&doc_mutex);
    pthread_mutex_unlock(&ncurses_mutex);
}

static void init_tui(void) {
    initscr();
    raw();
    keypad(stdscr, TRUE);
    noecho();
    start_color();

    init_pair(1, COLOR_WHITE,  COLOR_RED);
    init_pair(2, COLOR_WHITE,  COLOR_GREEN);
    init_pair(3, COLOR_BLACK,  COLOR_YELLOW);
    init_pair(4, COLOR_WHITE,  COLOR_BLUE);
    init_pair(5, COLOR_WHITE,  COLOR_MAGENTA);
    init_pair(6, COLOR_BLACK,  COLOR_CYAN);
    init_pair(7, COLOR_BLACK,  COLOR_WHITE);
}

static void cleanup_tui(void) { endwin(); }

static void send_op(NetMessage *msg) {
    msg->user_id = my_user_id;
    (void)send_all(server_sock, msg, sizeof(NetMessage));
}

static void toggle_format(uint8_t format_to_toggle) {
    current_format ^= format_to_toggle;
    int y = user_cursor.y, x = user_cursor.x;
    if (x > 0 && x <= local_doc.line_lengths[y]) {
        NetMessage msg = (NetMessage){0};
        msg.type = C2S_FORMAT_CHANGE;
        msg.payload.format_op.y = y;
        msg.payload.format_op.x = x - 1;
        msg.payload.format_op.format = local_doc.content[y][x - 1].format ^ format_to_toggle;
        send_op(&msg);
    }
}

// --- Selection helpers ---
static void selection_clear(void) {
    selection.active = false;
}
static void selection_start_from_cursor(void) {
    selection.active = true;
    selection.ay = user_cursor.y; selection.ax = user_cursor.x;
    selection.hy = user_cursor.y; selection.hx = user_cursor.x;
}
static void selection_update_head_to_cursor(void) {
    selection.hy = user_cursor.y; selection.hx = user_cursor.x;
}
static void selection_get_normalized(int *y1, int *x1, int *y2, int *x2) {
    int ay = selection.ay, ax = selection.ax;
    int hy = selection.hy, hx = selection.hx;
    if (hy < ay || (hy == ay && hx < ax)) {
        *y1 = hy; *x1 = hx; *y2 = ay; *x2 = ax;
    } else {
        *y1 = ay; *x1 = ax; *y2 = hy; *x2 = hx;
    }
}
static void send_selection_update(void) {
    NetMessage msg = (NetMessage){0};
    msg.type = C2S_SELECTION_UPDATE;
    msg.payload.selection_op.active = selection.active;
    if (selection.active) {
        int y1,x1,y2,x2; selection_get_normalized(&y1,&x1,&y2,&x2);
        msg.payload.selection_op.range = (Range){y1,x1,y2,x2};
    } else {
        msg.payload.selection_op.range = (Range){0,0,0,0};
    }
    send_op(&msg);
}