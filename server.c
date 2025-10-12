// server.c
#define _GNU_SOURCE  // for mkstemps
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdbool.h>

#include "common.h"

// --- Server State ---
typedef struct {
    StyledChar content[MAX_LINES][MAX_LINE_LEN];
    int        line_lengths[MAX_LINES];
    int        num_lines;
} Document;

static Document global_doc;
static int      client_sockets[MAX_CLIENTS] = {0};
static Cursor   client_cursors[MAX_CLIENTS];
static SelectionState client_selections[MAX_CLIENTS];

static int next_user_id = 0;

static pthread_mutex_t doc_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Robust send/recv ---
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

// --- Helpers ---
static void init_document(void) {
    memset(&global_doc, 0, sizeof(Document));
    global_doc.num_lines = 1;
    global_doc.line_lengths[0] = 0;

    memset(client_cursors, 0, sizeof(client_cursors));
    memset(client_selections, 0, sizeof(client_selections));
}

static void broadcast_message(NetMessage *msg) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < next_user_id; i++) {
        if (client_sockets[i] != 0) {
            (void)send_all(client_sockets[i], msg, sizeof(NetMessage));
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

static void *handle_client(void *client_socket_ptr);
static void apply_operation(NetMessage *msg);
static void run_code_block(NetMessage *msg);

// --- Main ---
int main(void) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    init_document();

    // Create server socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket failed"); exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt"); exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed"); exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 16) < 0) {
        perror("listen"); exit(EXIT_FAILURE);
    }

    // Info banner
    printf("\n--- C Collaborative Editor Server ---\n");
    printf("WARNING: Code execution is ENABLED. Run only on a trusted LAN.\n");
    printf("Listening on port %d\n", PORT);

    // Best-effort to print IP
    char hostbuffer[256];
    if (gethostname(hostbuffer, sizeof(hostbuffer)) == 0) {
        struct hostent *host_entry = gethostbyname(hostbuffer);
        if (host_entry && host_entry->h_addr_list[0]) {
            char *IPbuffer = inet_ntoa(*(struct in_addr*)host_entry->h_addr_list[0]);
            if (IPbuffer) printf("Server IP address: %s\n", IPbuffer);
        }
    }

    while (true) {
        new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (new_socket < 0) {
            perror("accept"); continue;
        }

        pthread_mutex_lock(&clients_mutex);
        if (next_user_id >= MAX_CLIENTS) {
            printf("Max clients reached. Connection rejected.\n");
            close(new_socket);
        } else {
            int user_id = next_user_id++;
            client_sockets[user_id] = new_socket;
            client_cursors[user_id].user_id = user_id;
            client_cursors[user_id].active = false;

            pthread_t thread_id;
            int *p_sock = (int *)malloc(sizeof(int));
            *p_sock = new_socket;
            pthread_create(&thread_id, NULL, handle_client, p_sock);
            pthread_detach(thread_id);
        }
        pthread_mutex_unlock(&clients_mutex);
    }
    return 0;
}

// --- Client thread ---
static void *handle_client(void *client_socket_ptr) {
    int sock = *(int *)client_socket_ptr;
    free(client_socket_ptr);

    int user_id = -1;

    // Find user_id for this socket
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < next_user_id; i++) {
        if (client_sockets[i] == sock) { user_id = i; break; }
    }
    pthread_mutex_unlock(&clients_mutex);

    // 1) Assign User ID
    NetMessage id_msg = {0};
    id_msg.type = S2C_USER_ID_ASSIGN;
    id_msg.user_id = user_id;
    send_all(sock, &id_msg, sizeof(NetMessage));

    // 2) Send initial document state (as a sequence of inserts/newlines)
    pthread_mutex_lock(&doc_mutex);
    for (int y = 0; y < global_doc.num_lines; y++) {
        for (int x = 0; x < global_doc.line_lengths[y]; x++) {
            NetMessage state_msg = {0};
            state_msg.type = S2C_INSERT;
            state_msg.user_id = -1; // system
            state_msg.payload.insert_op.y = y;
            state_msg.payload.insert_op.x = x;
            state_msg.payload.insert_op.s_char = global_doc.content[y][x];
            send_all(sock, &state_msg, sizeof(NetMessage));
        }
        if (y < global_doc.num_lines - 1) {
            NetMessage newline_msg = {0};
            newline_msg.type = S2C_NEWLINE;
            newline_msg.user_id = -1;
            newline_msg.payload.newline_op.y = y;
            newline_msg.payload.newline_op.x = global_doc.line_lengths[y];
            send_all(sock, &newline_msg, sizeof(NetMessage));
        }
    }
    pthread_mutex_unlock(&doc_mutex);

    // 3) Send currently active cursors and selections (nice-to-have)
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < next_user_id; ++i) {
        if (i == user_id) continue;
        if (client_sockets[i] != 0) {
            if (client_cursors[i].active) {
                NetMessage c = {0}; c.type = S2C_CURSOR_UPDATE; c.user_id = i;
                c.payload.cursor_op = client_cursors[i];
                send_all(sock, &c, sizeof(NetMessage));
            }
            NetMessage s = {0}; s.type = S2C_SELECTION_UPDATE; s.user_id = i;
            s.payload.selection_op = client_selections[i];
            send_all(sock, &s, sizeof(NetMessage));
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    // 4) Read loop
    NetMessage client_msg;
    while (recv_all(sock, &client_msg, sizeof(NetMessage)) > 0) {
        client_msg.user_id = user_id; // tag correctly
        apply_operation(&client_msg);
    }

    // Disconnected
    printf("Client %d disconnected.\n", user_id);
    pthread_mutex_lock(&clients_mutex);
    client_sockets[user_id] = 0;
    client_cursors[user_id].active = false;

    NetMessage cursor_msg = {0};
    cursor_msg.type = S2C_CURSOR_UPDATE;
    cursor_msg.payload.cursor_op = client_cursors[user_id];
    broadcast_message(&cursor_msg);

    client_selections[user_id].active = false;
    NetMessage sel_msg = {0};
    sel_msg.type = S2C_SELECTION_UPDATE;
    sel_msg.user_id = user_id;
    sel_msg.payload.selection_op = client_selections[user_id];
    broadcast_message(&sel_msg);

    pthread_mutex_unlock(&clients_mutex);
    close(sock);
    return NULL;
}

// --- Range helpers (server) ---
static void normalize_range(Range *r) {
    if (r->y1 > r->y2 || (r->y1 == r->y2 && r->x1 > r->x2)) {
        int ty = r->y1, tx = r->x1; r->y1 = r->y2; r->x1 = r->x2; r->y2 = ty; r->x2 = tx;
    }
}
static bool in_doc_bounds_point(int y, int x) {
    return y >= 0 && y < global_doc.num_lines && x >= 0 && x <= global_doc.line_lengths[y];
}
static bool clamp_range_to_doc(Range *r) {
    return in_doc_bounds_point(r->y1, r->x1) && in_doc_bounds_point(r->y2, r->x2);
}
static void apply_format_range(FormatRangeOp *op) {
    Range r = op->range; normalize_range(&r);
    if (!clamp_range_to_doc(&r)) return;
    for (int y = r.y1; y <= r.y2; ++y) {
        int sx = (y == r.y1) ? r.x1 : 0;
        int ex = (y == r.y2) ? r.x2 : global_doc.line_lengths[y];
        for (int x = sx; x < ex; ++x) {
            global_doc.content[y][x].format ^= op->format_toggle_bits;
        }
    }
}
static bool apply_delete_range(DeleteRangeOp *op, int *merge_y, int *merge_x) {
    Range r = op->range; normalize_range(&r);
    if (!clamp_range_to_doc(&r)) return false;
    if (r.y1 == r.y2 && r.x1 == r.x2) return false; // empty

    if (r.y1 == r.y2) {
        int y = r.y1, x1 = r.x1, x2 = r.x2;
        int tail = global_doc.line_lengths[y] - x2;
        memmove(&global_doc.content[y][x1], &global_doc.content[y][x2], tail * sizeof(StyledChar));
        global_doc.line_lengths[y] -= (x2 - x1);
        *merge_y = y; *merge_x = x1;
        return true;
    } else {
        int y1 = r.y1, x1 = r.x1, y2 = r.y2, x2 = r.x2;
        int keep_len   = x1;
        int suffix_len = global_doc.line_lengths[y2] - x2;

        if (keep_len + suffix_len > MAX_LINE_LEN) return false; // simple overflow guard

        // merge into y1
        memcpy(&global_doc.content[y1][x1], &global_doc.content[y2][x2], suffix_len * sizeof(StyledChar));
        global_doc.line_lengths[y1] = keep_len + suffix_len;

        // remove lines y1+1..y2
        int lines_to_remove = y2 - y1;
        memmove(&global_doc.content[y1 + 1], &global_doc.content[y2 + 1],
                (global_doc.num_lines - y2 - 1) * sizeof(global_doc.content[0]));
        memmove(&global_doc.line_lengths[y1 + 1], &global_doc.line_lengths[y2 + 1],
                (global_doc.num_lines - y2 - 1) * sizeof(int));
        global_doc.num_lines -= lines_to_remove;

        *merge_y = y1; *merge_x = x1;
        return true;
    }
}

// --- Apply operation ---
static void apply_operation(NetMessage *msg) {
    pthread_mutex_lock(&doc_mutex);

    NetMessage bmsg = *msg; // copy for broadcast
    int y, x;

    switch ((C2S_OpType)msg->type) {
        case C2S_INSERT:
            y = msg->payload.insert_op.y;
            x = msg->payload.insert_op.x;
            if (y < global_doc.num_lines && x <= global_doc.line_lengths[y] && global_doc.line_lengths[y] < MAX_LINE_LEN) {
                memmove(&global_doc.content[y][x+1], &global_doc.content[y][x],
                        (global_doc.line_lengths[y] - x) * sizeof(StyledChar));
                global_doc.content[y][x] = msg->payload.insert_op.s_char;
                global_doc.line_lengths[y]++;
                bmsg.type = S2C_INSERT; broadcast_message(&bmsg);
            }
            break;

        case C2S_DELETE:
            y = msg->payload.delete_op.y; x = msg->payload.delete_op.x;
            if (y < global_doc.num_lines && x > 0 && x <= global_doc.line_lengths[y]) {
                memmove(&global_doc.content[y][x-1], &global_doc.content[y][x],
                        (global_doc.line_lengths[y] - x + 1) * sizeof(StyledChar));
                global_doc.line_lengths[y]--;
                bmsg.type = S2C_DELETE; broadcast_message(&bmsg);
            }
            break;

        case C2S_DELETE_FORWARD:
            y = msg->payload.delete_op.y; x = msg->payload.delete_op.x;
            if (y < global_doc.num_lines && x < global_doc.line_lengths[y]) {
                memmove(&global_doc.content[y][x], &global_doc.content[y][x+1],
                        (global_doc.line_lengths[y] - x) * sizeof(StyledChar));
                global_doc.line_lengths[y]--;
                bmsg.type = S2C_DELETE_FORWARD; broadcast_message(&bmsg);
            }
            break;

        case C2S_MERGE_LINE:
            y = msg->payload.delete_op.y;
            if (y > 0 && y < global_doc.num_lines) {
                int dest_y = y - 1;
                int dest_x = global_doc.line_lengths[dest_y];
                int len_to_move = global_doc.line_lengths[y];
                if (dest_x + len_to_move < MAX_LINE_LEN) {
                    memcpy(&global_doc.content[dest_y][dest_x], &global_doc.content[y][0],
                           len_to_move * sizeof(StyledChar));
                    global_doc.line_lengths[dest_y] += len_to_move;
                    memmove(&global_doc.content[y], &global_doc.content[y+1],
                            (global_doc.num_lines - y - 1) * sizeof(global_doc.content[0]));
                    memmove(&global_doc.line_lengths[y], &global_doc.line_lengths[y+1],
                            (global_doc.num_lines - y - 1) * sizeof(int));
                    global_doc.num_lines--;
                    bmsg.type = S2C_MERGE_LINE;
                    bmsg.payload.delete_op.y = y;
                    bmsg.payload.delete_op.x = dest_x;
                    broadcast_message(&bmsg);
                }
            }
            break;

        case C2S_NEWLINE:
            y = msg->payload.newline_op.y; x = msg->payload.newline_op.x;
            if (y < global_doc.num_lines && x <= global_doc.line_lengths[y] && global_doc.num_lines < MAX_LINES) {
                int len_to_move = global_doc.line_lengths[y] - x;
                StyledChar buffer[MAX_LINE_LEN];
                memcpy(buffer, &global_doc.content[y][x], len_to_move * sizeof(StyledChar));

                memmove(&global_doc.content[y + 2], &global_doc.content[y + 1],
                        (global_doc.num_lines - y - 1) * sizeof(global_doc.content[0]));
                memmove(&global_doc.line_lengths[y + 2], &global_doc.line_lengths[y + 1],
                        (global_doc.num_lines - y - 1) * sizeof(int));

                global_doc.line_lengths[y] = x;
                global_doc.line_lengths[y + 1] = len_to_move;
                memcpy(&global_doc.content[y + 1][0], buffer, len_to_move * sizeof(StyledChar));
                global_doc.num_lines++;

                bmsg.type = S2C_NEWLINE; broadcast_message(&bmsg);
            }
            break;

        case C2S_FORMAT_CHANGE:
            y = msg->payload.format_op.y; x = msg->payload.format_op.x;
            if (y < global_doc.num_lines && x < global_doc.line_lengths[y]) {
                global_doc.content[y][x].format = msg->payload.format_op.format;
                bmsg.type = S2C_FORMAT_UPDATE; broadcast_message(&bmsg);
            }
            break;

        case C2S_CURSOR_MOVE:
            client_cursors[msg->user_id] = msg->payload.cursor_op;
            bmsg.type = S2C_CURSOR_UPDATE; broadcast_message(&bmsg);
            break;

        case C2S_RUN_CODE:
            pthread_mutex_unlock(&doc_mutex); // run without doc lock
            run_code_block(msg);
            return;

        case C2S_FORMAT_RANGE:
            apply_format_range(&msg->payload.format_range_op);
            bmsg.type = S2C_FORMAT_RANGE; broadcast_message(&bmsg);
            break;

        case C2S_DELETE_RANGE: {
            int my = 0, mx = 0;
            if (apply_delete_range(&msg->payload.delete_range_op, &my, &mx)) {
                bmsg.type = S2C_DELETE_RANGE;
                // include merge target in DeleteOp for cursor placement if desired
                bmsg.payload.delete_op.y = my;
                bmsg.payload.delete_op.x = mx;
                broadcast_message(&bmsg);
            }
            break;
        }

        case C2S_SELECTION_UPDATE:
            client_selections[msg->user_id] = msg->payload.selection_op;
            bmsg.type = S2C_SELECTION_UPDATE; broadcast_message(&bmsg);
            break;
    }

    pthread_mutex_unlock(&doc_mutex);
}

// --- Run code blocks (``` ... ```) ---
static void run_code_block(NetMessage *msg) {
    char filename[] = "/tmp/collab_code_XXXXXX.c";
    char exename[]  = "/tmp/collab_code_XXXXXX";
    char command[256];
    char output[1024] = {0};
    FILE *fp;

    int c_fd  = mkstemps(filename, 2);
    int exe_fd = mkstemp(exename);
    if (c_fd == -1 || exe_fd == -1) {
        perror("mkstemp");
        goto send_out;
    }
    close(c_fd); close(exe_fd);

    // Find the first ``` code block in the document
    pthread_mutex_lock(&doc_mutex);
    int start_y = -1, start_x = -1, end_y = -1, end_x = -1;
    bool in_block = false;

    for (int y = 0; y < global_doc.num_lines; y++) {
        for (int x = 0; x <= global_doc.line_lengths[y] - 3; x++) {
            if (global_doc.content[y][x].ch == '`' &&
                global_doc.content[y][x+1].ch == '`' &&
                global_doc.content[y][x+2].ch == '`') {
                if (!in_block) { start_y = y; start_x = x + 3; in_block = true; }
                else { end_y = y; end_x = x; goto found; }
            }
        }
    }
found:;
    if (start_y != -1 && end_y != -1) {
        fp = fopen(filename, "w");
        if (fp) {
            for (int y = start_y; y <= end_y; y++) {
                int sx = (y == start_y) ? start_x : 0;
                int ex = (y == end_y) ? end_x : global_doc.line_lengths[y];
                for (int x = sx; x < ex; x++) fputc(global_doc.content[y][x].ch, fp);
                fputc('\n', fp);
            }
            fclose(fp);
        }
    }
    pthread_mutex_unlock(&doc_mutex);

    if (start_y == -1 || end_y == -1) {
        snprintf(output, sizeof(output), "Error: Code block ```...``` not found.");
    } else {
        snprintf(command, sizeof(command), "gcc -std=c11 -Wall %s -o %s 2>&1 && %s 2>&1",
                 filename, exename, exename);
        fp = popen(command, "r");
        if (fp) {
            fread(output, 1, sizeof(output) - 1, fp);
            pclose(fp);
        } else {
            snprintf(output, sizeof(output), "Error: Failed to execute code.");
        }
    }

    unlink(filename); unlink(exename);

send_out:;
    NetMessage out_msg = {0};
    out_msg.type = S2C_CODE_OUTPUT;
    out_msg.user_id = msg->user_id;
    strncpy(out_msg.payload.code_output, output, sizeof(out_msg.payload.code_output) - 1);
    broadcast_message(&out_msg);
}
