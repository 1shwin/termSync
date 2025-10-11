#include <ncurses.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include "mongoose.h"
#include "jansson.h"
#include "common.h"

// --- Global State ---
typedef enum {
    MODE_EDIT,
    MODE_SELECT,
    MODE_COMMAND
} EditorMode;

typedef struct {
    char c;
    char styles[MAX_STYLES][16];
    int num_styles;
} CharInfo;

typedef enum {
    AUTH_PENDING,
    AUTH_NEED_SERVER_PASS,
    AUTH_SUCCESS,
    AUTH_FAIL
} AuthState;

static Document g_doc;
static EditorMode g_mode = MODE_EDIT;
static int g_done = 0;
static struct mg_mgr g_mgr;
static char g_command_buffer[100];
static int g_cursor_x = 0, g_cursor_y = 1;
static int g_select_start_x = -1; 
static int g_select_end_x = -1;

// Auth-related globals
static AuthState g_auth_state = AUTH_PENDING;
static char g_username[256];
static char g_password[256];


// --- Forward Declarations ---
void draw_ui();
void apply_style_to_selection(const char* style);
void rebuild_paragraph_from_char_infos(Paragraph *p, CharInfo *char_infos, int total_chars);
void handle_edit_input(int ch);
int get_paragraph_char_len(const Paragraph *p);
void send_update();

// --- Client-Specific Networking ---

void send_update() {
    char *doc_json = document_to_json(&g_doc);
    if (!doc_json) return;

    json_t *root = json_object();
    json_object_set_new(root, "type", json_string("DOC_UPDATE"));
    json_object_set_new(root, "payload", json_loads(doc_json, 0, NULL));
    
    char *final_json = json_dumps(root, 0);
    if (final_json) {
        struct mg_connection *c;
        for (c = g_mgr.conns; c != NULL; c = c->next) {
            if (c->flags & MG_F_IS_WEBSOCKET) {
                mg_ws_send(c, final_json, strlen(final_json), WEBSOCKET_OP_TEXT);
                break;
            }
        }
        free(final_json);
    }
    free(doc_json);
    json_decref(root);
}

static void event_handler(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
    if (ev == MG_EV_ERROR) {
        g_auth_state = AUTH_FAIL;
        g_done = 1;
    } else if (ev == MG_EV_WS_OPEN) {
        // Send initial LOGIN request
        json_t *root = json_object();
        json_object_set_new(root, "type", json_string("LOGIN"));
        json_object_set_new(root, "user", json_string(g_username));
        json_object_set_new(root, "pass", json_string(g_password));
        char *auth_str = json_dumps(root, 0);
        if (auth_str) {
            mg_ws_send(c, auth_str, strlen(auth_str), WEBSOCKET_OP_TEXT);
            free(auth_str);
        }
        json_decref(root);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        json_error_t error;
        json_t *root = json_loads(wm->data.p, 0, &error);
        if(!root) return;

        const char *type = json_string_value(json_object_get(root, "type"));
        if (type && strcmp(type, "AUTH_SUCCESS") == 0) {
            json_t *payload = json_object_get(root, "payload");
            char *payload_str = json_dumps(payload, 0);
            json_to_document(payload_str, &g_doc);
            free(payload_str);
            g_auth_state = AUTH_SUCCESS;
        } else if (type && strcmp(type, "AUTH_FAIL") == 0) {
            const char* reason = json_string_value(json_object_get(root, "reason"));
            fprintf(stderr, "Authentication failed: %s\n", reason ? reason : "Unknown error");
            g_auth_state = AUTH_FAIL;
        } else if (type && strcmp(type, "NEED_SERVER_PASS") == 0) {
            g_auth_state = AUTH_NEED_SERVER_PASS;
        } else if (type && strcmp(type, "DOC_UPDATE") == 0 && g_auth_state == AUTH_SUCCESS) {
            json_t *payload = json_object_get(root, "payload");
            char *payload_str = json_dumps(payload, 0);
            json_to_document(payload_str, &g_doc);
            free(payload_str);
            draw_ui(); // Redraw immediately on update
        }
        json_decref(root);
    }
    (void) fn_data;
}


int get_paragraph_char_len(const Paragraph *p) {
    int len = 0;
    for (int i = 0; i < p->num_segments; i++) {
        len += strlen(p->segments[i].text);
    }
    return len;
}

void draw_ui() {
    clear();
    mvprintw(0, 0, "--- Collaborative Editor (Client) ---");
    
    for (int p_idx = 0; p_idx < g_doc.num_paragraphs; p_idx++) {
        Paragraph *p = &g_doc.paragraphs[p_idx];
        int screen_x = 0;
        int screen_y = p_idx + 1;

        for (int i = 0; i < p->num_segments; i++) {
            TextSegment *seg = &p->segments[i];
            attr_t attrs = A_NORMAL;
            for (int j = 0; j < seg->num_styles; j++) {
                if (strcmp(seg->styles[j], "bold") == 0) attrs |= A_BOLD;
                if (strcmp(seg->styles[j], "underline") == 0) attrs |= A_UNDERLINE;
            }

            for (size_t k = 0; k < strlen(seg->text); k++) {
                attr_t current_attrs = attrs;
                if (g_mode == MODE_SELECT && screen_y == g_cursor_y) {
                    int start = (g_select_start_x < g_select_end_x) ? g_select_start_x : g_select_end_x;
                    int end = (g_select_start_x > g_select_end_x) ? g_select_start_x : g_select_end_x;
                    if (screen_x >= start && screen_x < end) {
                        current_attrs |= A_REVERSE;
                    }
                }
                attron(current_attrs);
                mvaddch(screen_y, screen_x, seg->text[k]);
                attroff(current_attrs);
                screen_x++;
            }
        }
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    char *status = malloc(max_x + 1);
    if (!status) return;

    attron(A_REVERSE);
    if (g_mode == MODE_EDIT) {
        snprintf(status, max_x + 1, " EDIT MODE | Shift+Arrows to select | Alt+c for command mode");
    } else if (g_mode == MODE_SELECT) {
        snprintf(status, max_x + 1, " SELECT MODE | Press ESC for edit mode | ':' for command");
    } else {
        snprintf(status, max_x + 1, "COMMAND: %s", g_command_buffer);
    }
    
    size_t len = strlen(status);
    for (size_t i = len; (int)i < max_x; i++) status[i] = ' ';
    status[max_x] = '\0';
    
    mvprintw(max_y - 1, 0, "%s", status);
    attroff(A_REVERSE);
    free(status);
    
    if (g_mode == MODE_EDIT || g_mode == MODE_SELECT) {
        move(g_cursor_y, g_cursor_x);
    } else {
        move(max_y-1, strlen("COMMAND: ") + strlen(g_command_buffer));
    }
    refresh();
}

void rebuild_paragraph_from_char_infos(Paragraph *p, CharInfo *char_infos, int total_chars) {
    memset(p, 0, sizeof(Paragraph));
    if (total_chars > 0) {
        p->num_segments = 1;
        TextSegment *current_seg = &p->segments[0];
        current_seg->text[0] = char_infos[0].c;
        current_seg->text[1] = '\0';
        current_seg->num_styles = char_infos[0].num_styles;
        for(int i=0; i<char_infos[0].num_styles; i++) {
             strcpy(current_seg->styles[i], char_infos[0].styles[i]);
        }

        for (int i = 1; i < total_chars; i++) {
            bool same_style = (char_infos[i].num_styles == char_infos[i-1].num_styles);
            if(same_style) {
                 for(int j=0; j<char_infos[i].num_styles; j++){
                    bool found_match = false;
                    for(int k=0; k<char_infos[i-1].num_styles; k++){
                        if(strcmp(char_infos[i].styles[j], char_infos[i-1].styles[k]) == 0){
                            found_match = true;
                            break;
                        }
                    }
                    if(!found_match) same_style = false;
                }
            }
            
            if (same_style && strlen(current_seg->text) < MAX_TEXT_LEN - 1) {
                int len = strlen(current_seg->text);
                current_seg->text[len] = char_infos[i].c;
                current_seg->text[len + 1] = '\0';
            } else {
                if (p->num_segments < MAX_TEXT_SEGMENTS) {
                    current_seg = &p->segments[p->num_segments++];
                    current_seg->text[0] = char_infos[i].c;
                    current_seg->text[1] = '\0';
                    current_seg->num_styles = char_infos[i].num_styles;
                    for(int j=0; j<char_infos[i].num_styles; j++) {
                        strcpy(current_seg->styles[j], char_infos[i].styles[j]);
                    }
                }
            }
        }
    } else {
        p->num_segments = 1;
        p->segments[0].text[0] = '\0';
        p->segments[0].num_styles = 0;
    }
}


void apply_style_to_selection(const char* style) {
    if (g_select_start_x == -1 || g_select_start_x == g_select_end_x) return;

    Paragraph *p = &g_doc.paragraphs[g_cursor_y - 1];
    int start = (g_select_start_x < g_select_end_x) ? g_select_start_x : g_select_end_x;
    int end = (g_select_start_x > g_select_end_x) ? g_select_start_x : g_select_end_x;

    CharInfo *char_infos = malloc(sizeof(CharInfo) * MAX_TEXT_LEN * MAX_TEXT_SEGMENTS);
    if (!char_infos) return;
    int total_chars = 0;
    for (int i = 0; i < p->num_segments; i++) {
        for (size_t k = 0; k < strlen(p->segments[i].text); k++) {
            char_infos[total_chars].c = p->segments[i].text[k];
            char_infos[total_chars].num_styles = p->segments[i].num_styles;
            memcpy(char_infos[total_chars].styles, p->segments[i].styles, sizeof(p->segments[i].styles));
            total_chars++;
        }
    }
    
    bool all_chars_have_style = (start < end);
    for (int i = start; i < end; i++) {
        if (i >= total_chars) { all_chars_have_style = false; break; }
        bool has_style = false;
        for (int j = 0; j < char_infos[i].num_styles; j++) {
            if (strcmp(char_infos[i].styles[j], style) == 0) { has_style = true; break; }
        }
        if (!has_style) { all_chars_have_style = false; break; }
    }

    for (int i = start; i < end; i++) {
        if (i >= total_chars) continue;
        if (all_chars_have_style) { 
            int style_idx = -1;
            for (int j = 0; j < char_infos[i].num_styles; j++) {
                if (strcmp(char_infos[i].styles[j], style) == 0) { style_idx = j; break; }
            }
            if (style_idx != -1) {
                for (int j = style_idx; j < char_infos[i].num_styles - 1; j++) {
                    strcpy(char_infos[i].styles[j], char_infos[i].styles[j + 1]);
                }
                char_infos[i].num_styles--;
            }
        } else { 
            bool exists = false;
            for (int j = 0; j < char_infos[i].num_styles; j++) {
                if (strcmp(char_infos[i].styles[j], style) == 0) { exists = true; break; }
            }
            if (!exists && char_infos[i].num_styles < MAX_STYLES) {
                strcpy(char_infos[i].styles[char_infos[i].num_styles++], style);
            }
        }
    }
    
    rebuild_paragraph_from_char_infos(p, char_infos, total_chars);
    free(char_infos);

    g_select_start_x = -1;
    g_select_end_x = -1;
    g_mode = MODE_EDIT;
    send_update();
}

void handle_edit_input(int ch) {
    if (g_cursor_y < 1 || g_cursor_y > g_doc.num_paragraphs) return;
    
    Paragraph *p = &g_doc.paragraphs[g_cursor_y - 1];
    
    int total_len = get_paragraph_char_len(p);
    CharInfo *char_infos = malloc(sizeof(CharInfo) * (total_len + 5)); // +5 for tab
    if (!char_infos) return;

    int char_offset = 0;
    for (int i = 0; i < p->num_segments; i++) {
        for (size_t k = 0; k < strlen(p->segments[i].text); k++) {
            char_infos[char_offset].c = p->segments[i].text[k];
            char_infos[char_offset].num_styles = p->segments[i].num_styles;
            memcpy(char_infos[char_offset].styles, p->segments[i].styles, sizeof(p->segments[i].styles));
            char_offset++;
        }
    }

    bool changed = false;
    if (ch == '\n' || ch == '\r') {
        if (g_doc.num_paragraphs < MAX_PARAGRAPHS) {
            memmove(&g_doc.paragraphs[g_cursor_y + 1], &g_doc.paragraphs[g_cursor_y], (g_doc.num_paragraphs - g_cursor_y) * sizeof(Paragraph));
            g_doc.num_paragraphs++;
            
            Paragraph *current_p = &g_doc.paragraphs[g_cursor_y - 1];
            Paragraph *new_p = &g_doc.paragraphs[g_cursor_y];

            rebuild_paragraph_from_char_infos(new_p, &char_infos[g_cursor_x], total_len - g_cursor_x);
            rebuild_paragraph_from_char_infos(current_p, char_infos, g_cursor_x);
            
            g_cursor_y++;
            g_cursor_x = 0;
            changed = true;
        }
    } else if ((ch == KEY_BACKSPACE || ch == 127)) {
        if (g_cursor_x > 0) {
            memmove(&char_infos[g_cursor_x - 1], &char_infos[g_cursor_x], (total_len - g_cursor_x) * sizeof(CharInfo));
            total_len--;
            g_cursor_x--;
            rebuild_paragraph_from_char_infos(p, char_infos, total_len);
            changed = true;
        } else if (g_cursor_y > 1) { // Merge with previous line
            Paragraph *prev_p = &g_doc.paragraphs[g_cursor_y - 2];
            int prev_len = get_paragraph_char_len(prev_p);
            
            CharInfo *prev_char_infos = malloc(sizeof(CharInfo) * (prev_len + total_len + 1));
            int prev_char_offset = 0;
            for (int i = 0; i < prev_p->num_segments; i++) {
                for(size_t k=0; k < strlen(prev_p->segments[i].text); k++){
                    prev_char_infos[prev_char_offset].c = prev_p->segments[i].text[k];
                    prev_char_infos[prev_char_offset].num_styles = prev_p->segments[i].num_styles;
                    memcpy(prev_char_infos[prev_char_offset].styles, prev_p->segments[i].styles, sizeof(prev_p->segments[i].styles));
                    prev_char_offset++;
                }
            }
            memcpy(&prev_char_infos[prev_len], char_infos, total_len * sizeof(CharInfo));
            
            rebuild_paragraph_from_char_infos(prev_p, prev_char_infos, prev_len + total_len);
            free(prev_char_infos);

            memmove(&g_doc.paragraphs[g_cursor_y - 1], &g_doc.paragraphs[g_cursor_y], (g_doc.num_paragraphs - g_cursor_y) * sizeof(Paragraph));
            g_doc.num_paragraphs--;
            g_cursor_y--;
            g_cursor_x = prev_len;
            changed = true;
        }
    } else if (ch == KEY_DC) { // Delete key
        if (g_cursor_x < total_len) {
            memmove(&char_infos[g_cursor_x], &char_infos[g_cursor_x + 1], (total_len - g_cursor_x - 1) * sizeof(CharInfo));
            total_len--;
            rebuild_paragraph_from_char_infos(p, char_infos, total_len);
            changed = true;
        } else if (g_cursor_y < g_doc.num_paragraphs) { // Merge with next line
            Paragraph *next_p = &g_doc.paragraphs[g_cursor_y];
            int next_len = get_paragraph_char_len(next_p);

            CharInfo *next_char_infos = malloc(sizeof(CharInfo) * (total_len + next_len + 1));
            int next_char_offset = 0;
            for (int i = 0; i < next_p->num_segments; i++) {
                for(size_t k=0; k < strlen(next_p->segments[i].text); k++){
                    next_char_infos[next_char_offset].c = next_p->segments[i].text[k];
                    next_char_infos[next_char_offset].num_styles = next_p->segments[i].num_styles;
                    memcpy(next_char_infos[next_char_offset].styles, next_p->segments[i].styles, sizeof(next_p->segments[i].styles));
                    next_char_offset++;
                }
            }
            memcpy(&char_infos[total_len], next_char_infos, next_len * sizeof(CharInfo));
            free(next_char_infos);
            
            rebuild_paragraph_from_char_infos(p, char_infos, total_len + next_len);
            
            memmove(&g_doc.paragraphs[g_cursor_y], &g_doc.paragraphs[g_cursor_y + 1], (g_doc.num_paragraphs - g_cursor_y -1) * sizeof(Paragraph));
            g_doc.num_paragraphs--;
            changed = true;
        }
    } else if (ch == '\t') { // Tab key
        int tab_size = 4;
        CharInfo tab_chars[4];
        for(int i = 0; i < tab_size; i++) {
            tab_chars[i].c = ' ';
            tab_chars[i].num_styles = 0;
            if (g_cursor_x > 0 && g_cursor_x <= total_len) {
                tab_chars[i].num_styles = char_infos[g_cursor_x - 1].num_styles;
                memcpy(tab_chars[i].styles, char_infos[g_cursor_x - 1].styles, sizeof(char_infos[g_cursor_x - 1].styles));
            }
        }
        memmove(&char_infos[g_cursor_x + tab_size], &char_infos[g_cursor_x], (total_len - g_cursor_x) * sizeof(CharInfo));
        memcpy(&char_infos[g_cursor_x], tab_chars, tab_size * sizeof(CharInfo));
        total_len += tab_size;
        g_cursor_x += tab_size;
        rebuild_paragraph_from_char_infos(p, char_infos, total_len);
        changed = true;
    } else if (ch >= 32 && ch <= 126) {
        CharInfo new_char;
        new_char.c = (char)ch;
        new_char.num_styles = 0;
        if (g_cursor_x > 0 && g_cursor_x <= total_len) {
             new_char.num_styles = char_infos[g_cursor_x - 1].num_styles;
             memcpy(new_char.styles, char_infos[g_cursor_x - 1].styles, sizeof(char_infos[g_cursor_x - 1].styles));
        }
        memmove(&char_infos[g_cursor_x + 1], &char_infos[g_cursor_x], (total_len - g_cursor_x) * sizeof(CharInfo));
        char_infos[g_cursor_x] = new_char;
        total_len++;
        g_cursor_x++;
        rebuild_paragraph_from_char_infos(p, char_infos, total_len);
        changed = true;
    }
    
    if (changed) {
        send_update();
    }
    free(char_infos);
}


void handle_input() {
    int ch = getch();
    if (ch == ERR) return;

    if (ch == 27) { 
        nodelay(stdscr, TRUE);
        int next_ch = getch();
        nodelay(stdscr, FALSE);

        if (next_ch == ERR) { 
            g_mode = MODE_EDIT;
            g_select_start_x = -1;
            g_select_end_x = -1;
            return;
        } else if (next_ch == 'c') { 
            g_mode = MODE_COMMAND;
            g_command_buffer[0] = '\0';
            return;
        } else {
            ungetch(next_ch);
            g_mode = MODE_EDIT;
            g_select_start_x = -1;
            g_select_end_x = -1;
            return;
        }
    }

    switch(g_mode) {
        case MODE_EDIT:
            if (ch == KEY_UP) {
                if (g_cursor_y > 1) g_cursor_y--;
            } else if (ch == KEY_DOWN) {
                if (g_cursor_y < g_doc.num_paragraphs) g_cursor_y++;
            } else if (ch == KEY_LEFT) {
                if (g_cursor_x > 0) {
                    g_cursor_x--;
                } else if (g_cursor_y > 1) {
                    g_cursor_y--;
                    g_cursor_x = get_paragraph_char_len(&g_doc.paragraphs[g_cursor_y - 1]);
                }
            } else if (ch == KEY_RIGHT) {
                int len = get_paragraph_char_len(&g_doc.paragraphs[g_cursor_y - 1]);
                if (g_cursor_x < len) {
                    g_cursor_x++;
                } else if (g_cursor_y < g_doc.num_paragraphs) {
                    g_cursor_y++;
                    g_cursor_x = 0;
                }
            } else if (ch == KEY_SLEFT || ch == KEY_SRIGHT) {
                g_mode = MODE_SELECT;
                g_select_start_x = g_cursor_x;
                if (ch == KEY_SLEFT && g_cursor_x > 0) g_cursor_x--;
                if (ch == KEY_SRIGHT) {
                     int len = get_paragraph_char_len(&g_doc.paragraphs[g_cursor_y - 1]);
                     if(g_cursor_x < len) g_cursor_x++;
                }
                g_select_end_x = g_cursor_x;
            } else {
                handle_edit_input(ch);
            }
            
            // Clamp cursor X after vertical movement
            if(ch == KEY_UP || ch == KEY_DOWN) {
                int len = get_paragraph_char_len(&g_doc.paragraphs[g_cursor_y - 1]);
                if (g_cursor_x > len) g_cursor_x = len;
            }
            break;
        
        case MODE_SELECT:
            if (ch == KEY_LEFT || ch == KEY_SLEFT) {
                if (g_cursor_x > 0) g_cursor_x--;
                g_select_end_x = g_cursor_x;
            } else if (ch == KEY_RIGHT || ch == KEY_SRIGHT) {
                int len = get_paragraph_char_len(&g_doc.paragraphs[g_cursor_y - 1]);
                if (g_cursor_x < len) g_cursor_x++;
                g_select_end_x = g_cursor_x;
            } else if (ch == ':') {
                g_mode = MODE_COMMAND;
                g_command_buffer[0] = '\0';
            }
            break;

        case MODE_COMMAND:
            if (ch == '\n' || ch == '\r') {
                if (strcmp(g_command_buffer, "quit") == 0) {
                    g_done = 1;
                } else if (strcmp(g_command_buffer, "bold") == 0 ||
                           strcmp(g_command_buffer, "ital") == 0 ||
                           strcmp(g_command_buffer, "unline") == 0 ||
                           strcmp(g_command_buffer, "strike") == 0) {
                    
                    char style[16];
                    if (strcmp(g_command_buffer, "ital") == 0) strcpy(style, "italic");
                    else if (strcmp(g_command_buffer, "unline") == 0) strcpy(style, "underline");
                    else if (strcmp(g_command_buffer, "strike") == 0) strcpy(style, "strikethrough");
                    else strcpy(style, "bold");

                    apply_style_to_selection(style);
                }
                g_command_buffer[0] = '\0';
                g_mode = MODE_EDIT;
            } else if (ch == KEY_BACKSPACE || ch == 127) {
                int len = strlen(g_command_buffer);
                if (len > 0) g_command_buffer[len - 1] = '\0';
            } else if (strlen(g_command_buffer) < sizeof(g_command_buffer) - 1 && ch >= 32 && ch <= 126) {
                int len = strlen(g_command_buffer);
                g_command_buffer[len] = ch; g_command_buffer[len+1] = '\0';
            }
            break;
    }
}


int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <username> <password>\n", argv[0]);
        return 1;
    }
    strncpy(g_username, argv[2], sizeof(g_username)-1);
    strncpy(g_password, argv[3], sizeof(g_password)-1);

    mg_mgr_init(&g_mgr);
    
    char url[100];
    snprintf(url, sizeof(url), "ws://%s:8000", argv[1]);
    printf("Connecting to %s...\n", url);
    struct mg_connection *c = mg_ws_connect(&g_mgr, url, event_handler, NULL, NULL);
    if (c == NULL) {
        fprintf(stderr, "Failed to connect to server.\n");
        return 1;
    }

    // Authentication loop
    while (g_auth_state == AUTH_PENDING || g_auth_state == AUTH_NEED_SERVER_PASS) {
        mg_mgr_poll(&g_mgr, 100);
        if (g_auth_state == AUTH_NEED_SERVER_PASS) {
            char server_pass[256];
            printf("Server password required for new user: ");
            scanf("%255s", server_pass);

            json_t *root = json_object();
            json_object_set_new(root, "type", json_string("SERVERPASS"));
            json_object_set_new(root, "pass", json_string(server_pass));
            char *sp_str = json_dumps(root, 0);

            mg_ws_send(c, sp_str, strlen(sp_str), WEBSOCKET_OP_TEXT);

            free(sp_str);
            json_decref(root);
            g_auth_state = AUTH_PENDING; // Wait for server's response
        }
    }

    if (g_auth_state != AUTH_SUCCESS) {
        fprintf(stderr, "Could not authenticate. Exiting.\n");
        mg_mgr_free(&g_mgr);
        return 1;
    }

    printf("Authentication successful. Starting editor...\n");
    sleep(1);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    
    while (!g_done) {
        handle_input();
        draw_ui();
        mg_mgr_poll(&g_mgr, 50);
    }

    mg_mgr_free(&g_mgr);
    endwin();
    return 0;
}

