#define _GNU_SOURCE // Required for mkstemps
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

#define MAX_LINES 1024
#define MAX_LINE_LEN 1024

// --- Server State ---
typedef struct {
    StyledChar content[MAX_LINES][MAX_LINE_LEN];
    int line_lengths[MAX_LINES];
    int num_lines;
} Document;

Document global_doc;
int client_sockets[MAX_CLIENTS];
Cursor client_cursors[MAX_CLIENTS];
int next_user_id = 0;

pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Function Prototypes ---
void init_document();
void broadcast_message(NetMessage *msg);
void *handle_client(void *client_socket_ptr);
void apply_operation(NetMessage *msg);
void run_code_block(NetMessage *msg);

// --- Main Function ---
int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    init_document();

    // Create server socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    // DANGEROUS: The code execution feature is a major security risk.
    printf("\n--- C Collaborative Editor Server ---\n");
    printf("WARNING: Code execution is ENABLED. Do not expose this server to the internet.\n");
    printf("Listening on port %d\n", PORT);

    char hostbuffer[256];
    char *IPbuffer;
    struct hostent *host_entry;

    if (gethostname(hostbuffer, sizeof(hostbuffer)) == -1) {
        perror("gethostname");
    } else {
        host_entry = gethostbyname(hostbuffer);
        if (host_entry == NULL) {
            perror("gethostbyname");
        } else {
            IPbuffer = inet_ntoa(*((struct in_addr*)host_entry->h_addr_list[0]));
            printf("Server IP address: %s\n", IPbuffer);
        }
    }


    while (true) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen))<0) {
            perror("accept");
            continue;
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
            int *p_sock = malloc(sizeof(int));
            *p_sock = new_socket;
            pthread_create(&thread_id, NULL, handle_client, p_sock);
        }
        pthread_mutex_unlock(&clients_mutex);
    }

    return 0;
}

// --- Implementation ---

void init_document() {
    memset(&global_doc, 0, sizeof(Document));
    global_doc.num_lines = 1;
    global_doc.line_lengths[0] = 0;
}

void broadcast_message(NetMessage *msg) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < next_user_id; i++) {
        if(client_sockets[i] != 0) {
             send(client_sockets[i], msg, sizeof(NetMessage), 0);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void *handle_client(void *client_socket_ptr) {
    int sock = *(int*)client_socket_ptr;
    free(client_socket_ptr);
    int user_id;

    // Find user_id for this socket
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < next_user_id; i++) {
        if (client_sockets[i] == sock) {
            user_id = i;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    // 1. Assign User ID
    NetMessage id_msg;
    id_msg.type = S2C_USER_ID_ASSIGN;
    id_msg.user_id = user_id;
    send(sock, &id_msg, sizeof(NetMessage), 0);

    // 2. Send initial document state
    pthread_mutex_lock(&doc_mutex);
    for (int y = 0; y < global_doc.num_lines; y++) {
        for (int x = 0; x < global_doc.line_lengths[y]; x++) {
            NetMessage state_msg;
            state_msg.type = S2C_INSERT;
            state_msg.user_id = -1; // System message
            state_msg.payload.insert_op.y = y;
            state_msg.payload.insert_op.x = x;
            state_msg.payload.insert_op.s_char = global_doc.content[y][x];
            send(sock, &state_msg, sizeof(NetMessage), 0);
        }
        if (y < global_doc.num_lines - 1) {
            NetMessage newline_msg;
            newline_msg.type = S2C_NEWLINE;
            newline_msg.user_id = -1;
            newline_msg.payload.newline_op.y = y;
            newline_msg.payload.newline_op.x = global_doc.line_lengths[y];
            send(sock, &newline_msg, sizeof(NetMessage), 0);
        }
    }
    pthread_mutex_unlock(&doc_mutex);
    
    // 3. Listen for client operations
    NetMessage client_msg;
    while (recv(sock, &client_msg, sizeof(NetMessage), 0) > 0) {
        client_msg.user_id = user_id; // Ensure message is tagged with correct user
        apply_operation(&client_msg);
    }
    
    // Client disconnected
    printf("Client %d disconnected.\n", user_id);
    pthread_mutex_lock(&clients_mutex);
    client_sockets[user_id] = 0; // Mark as disconnected
    client_cursors[user_id].active = false;
    // Broadcast cursor deactivation
    NetMessage cursor_msg;
    cursor_msg.type = S2C_CURSOR_UPDATE;
    cursor_msg.payload.cursor_op = client_cursors[user_id];
    broadcast_message(&cursor_msg);
    pthread_mutex_unlock(&clients_mutex);
    
    close(sock);
    return NULL;
}

void apply_operation(NetMessage *msg) {
    pthread_mutex_lock(&doc_mutex);
    int y, x;
    NetMessage broadcast_msg = *msg; // Copy message for broadcasting

    switch ((C2S_OpType)msg->type) {
        case C2S_INSERT:
            y = msg->payload.insert_op.y;
            x = msg->payload.insert_op.x;
            if (y < global_doc.num_lines && x <= global_doc.line_lengths[y] && global_doc.line_lengths[y] < MAX_LINE_LEN) {
                memmove(&global_doc.content[y][x + 1], &global_doc.content[y][x], (global_doc.line_lengths[y] - x) * sizeof(StyledChar));
                global_doc.content[y][x] = msg->payload.insert_op.s_char;
                global_doc.line_lengths[y]++;
                broadcast_msg.type = S2C_INSERT;
                broadcast_message(&broadcast_msg);
            }
            break;

        case C2S_DELETE:
            y = msg->payload.delete_op.y;
            x = msg->payload.delete_op.x;
            if (y < global_doc.num_lines && x > 0 && x <= global_doc.line_lengths[y]) {
                memmove(&global_doc.content[y][x - 1], &global_doc.content[y][x], (global_doc.line_lengths[y] - x + 1) * sizeof(StyledChar));
                global_doc.line_lengths[y]--;
                broadcast_msg.type = S2C_DELETE;
                broadcast_message(&broadcast_msg);
            }
            break;

        case C2S_DELETE_FORWARD:
            y = msg->payload.delete_op.y;
            x = msg->payload.delete_op.x;
            if (y < global_doc.num_lines && x < global_doc.line_lengths[y]) {
                memmove(&global_doc.content[y][x], &global_doc.content[y][x + 1], (global_doc.line_lengths[y] - x) * sizeof(StyledChar));
                global_doc.line_lengths[y]--;
                broadcast_msg.type = S2C_DELETE_FORWARD;
                broadcast_message(&broadcast_msg);
            }
            break;
        
        case C2S_MERGE_LINE:
            y = msg->payload.delete_op.y; // y is the line to merge up
            if (y > 0 && y < global_doc.num_lines) {
                int dest_y = y - 1;
                int dest_x = global_doc.line_lengths[dest_y]; // Merge point
                int len_to_move = global_doc.line_lengths[y];

                if (dest_x + len_to_move < MAX_LINE_LEN) {
                    memcpy(&global_doc.content[dest_y][dest_x], &global_doc.content[y][0], len_to_move * sizeof(StyledChar));
                    global_doc.line_lengths[dest_y] += len_to_move;
                    
                    memmove(&global_doc.content[y], &global_doc.content[y + 1], (global_doc.num_lines - y - 1) * sizeof(global_doc.content[0]));
                    memmove(&global_doc.line_lengths[y], &global_doc.line_lengths[y + 1], (global_doc.num_lines - y - 1) * sizeof(int));

                    global_doc.num_lines--;
                    
                    broadcast_msg.type = S2C_MERGE_LINE;
                    broadcast_msg.payload.delete_op.y = y;
                    broadcast_msg.payload.delete_op.x = dest_x; // Send merge point
                    broadcast_message(&broadcast_msg);
                }
            }
            break;

        case C2S_NEWLINE:
            y = msg->payload.newline_op.y;
            x = msg->payload.newline_op.x;
            if (y < global_doc.num_lines && x <= global_doc.line_lengths[y] && global_doc.num_lines < MAX_LINES) {
                int len_to_move = global_doc.line_lengths[y] - x;
                StyledChar buffer[MAX_LINE_LEN];
                memcpy(buffer, &global_doc.content[y][x], len_to_move * sizeof(StyledChar));
                
                memmove(&global_doc.content[y + 2], &global_doc.content[y + 1], (global_doc.num_lines - y - 1) * sizeof(global_doc.content[0]));
                memmove(&global_doc.line_lengths[y + 2], &global_doc.line_lengths[y + 1], (global_doc.num_lines - y - 1) * sizeof(int));
                
                global_doc.line_lengths[y] = x;
                global_doc.line_lengths[y + 1] = len_to_move;
                memcpy(&global_doc.content[y + 1][0], buffer, len_to_move * sizeof(StyledChar));
                global_doc.num_lines++;

                broadcast_msg.type = S2C_NEWLINE;
                broadcast_message(&broadcast_msg);
            }
            break;

        case C2S_FORMAT_CHANGE:
            y = msg->payload.format_op.y;
            x = msg->payload.format_op.x;
            if (y < global_doc.num_lines && x < global_doc.line_lengths[y]) {
                global_doc.content[y][x].format = msg->payload.format_op.format;
                broadcast_msg.type = S2C_FORMAT_UPDATE;
                broadcast_message(&broadcast_msg);
            }
            break;
            
        case C2S_CURSOR_MOVE:
            client_cursors[msg->user_id] = msg->payload.cursor_op;
            broadcast_msg.type = S2C_CURSOR_UPDATE;
            broadcast_message(&broadcast_msg);
            break;
        
        case C2S_RUN_CODE:
            pthread_mutex_unlock(&doc_mutex);
            run_code_block(msg);
            return;
    }
    pthread_mutex_unlock(&doc_mutex);
}

void run_code_block(NetMessage *msg) {
    char filename[] = "/tmp/collab_code_XXXXXX.c";
    char exename[] = "/tmp/collab_code_XXXXXX";
    char command[256];
    char output[1024] = {0};
    FILE *fp;

    int c_fd = mkstemps(filename, 2);
    int exe_fd = mkstemp(exename);
    if (c_fd == -1 || exe_fd == -1) {
        perror("mkstemp"); return;
    }
    close(c_fd); close(exe_fd);

    pthread_mutex_lock(&doc_mutex);
    int start_y = -1, start_x = -1, end_y = -1, end_x = -1;
    bool in_block = false;

    for (int y = 0; y < global_doc.num_lines; y++) {
        for (int x = 0; x < global_doc.line_lengths[y] - 2; x++) {
            if (global_doc.content[y][x].ch == '`' && global_doc.content[y][x+1].ch == '`' && global_doc.content[y][x+2].ch == '`') {
                if (!in_block) {
                    start_y = y; start_x = x + 3; in_block = true;
                } else {
                    end_y = y; end_x = x; goto block_found;
                }
            }
        }
    }

block_found:
    if (start_y != -1 && end_y != -1) {
        fp = fopen(filename, "w");
        for (int y = start_y; y <= end_y; y++) {
            int line_start_x = (y == start_y) ? start_x : 0;
            int line_end_x = (y == end_y) ? end_x : global_doc.line_lengths[y];
            for (int x = line_start_x; x < line_end_x; x++) {
                fputc(global_doc.content[y][x].ch, fp);
            }
            fputc('\n', fp);
        }
        fclose(fp);
    }
    pthread_mutex_unlock(&doc_mutex);

    if (start_y == -1 || end_y == -1) {
        snprintf(output, sizeof(output), "Error: Code block with ```...``` not found.");
    } else {
        snprintf(command, sizeof(command), "gcc -std=c11 -Wall %s -o %s 2>&1 && %s 2>&1", filename, exename, exename);
        fp = popen(command, "r");
        if (fp) {
            fread(output, 1, sizeof(output) - 1, fp);
            pclose(fp);
        } else {
            snprintf(output, sizeof(output), "Error: Failed to execute code.");
        }
    }
    
    unlink(filename); unlink(exename);

    NetMessage out_msg;
    out_msg.type = S2C_CODE_OUTPUT;
    out_msg.user_id = msg->user_id;
    strncpy(out_msg.payload.code_output, output, sizeof(out_msg.payload.code_output) - 1);
    broadcast_message(&out_msg);
}

