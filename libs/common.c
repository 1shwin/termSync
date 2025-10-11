#include "common.h"
#include <string.h>
#include <stdlib.h>

// --- JSON Helper Implementations ---
char *document_to_json(const Document *doc) {
    json_t *root = json_object();
    json_object_set_new(root, "docId", json_string("document-cs"));
    json_t *content = json_array();

    for (int p_idx = 0; p_idx < doc->num_paragraphs; p_idx++) {
        const Paragraph *p = &doc->paragraphs[p_idx];
        json_t *paragraph_node = json_object();
        json_object_set_new(paragraph_node, "type", json_string("paragraph"));
        json_t *children = json_array();
        for (int i = 0; i < p->num_segments; i++) {
            json_t *segment_node = json_object();
            json_object_set_new(segment_node, "text", json_string(p->segments[i].text));
            json_t *styles_array = json_array();
            for(int j=0; j < p->segments[i].num_styles; j++) {
                json_array_append_new(styles_array, json_string(p->segments[i].styles[j]));
            }
            json_object_set_new(segment_node, "styles", styles_array);
            json_array_append_new(children, segment_node);
        }
        json_object_set_new(paragraph_node, "children", children);
        json_array_append_new(content, paragraph_node);
    }

    json_object_set_new(root, "content", content);
    char *result = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    return result;
}

int json_to_document(const char *json_str, Document *doc) {
    json_error_t error;
    json_t *root = json_loads(json_str, 0, &error);
    if (!root) return -1;
    
    memset(doc, 0, sizeof(Document));
    json_t *content = json_object_get(root, "content");
    if (!json_is_array(content)) {
        json_decref(root);
        return -1;
    }

    size_t p_idx;
    json_t *paragraph_node;
    json_array_foreach(content, p_idx, paragraph_node) {
        if (doc->num_paragraphs >= MAX_PARAGRAPHS) break;
        Paragraph *current_p = &doc->paragraphs[doc->num_paragraphs];
        
        json_t *children = json_object_get(paragraph_node, "children");
        size_t i;
        json_t *segment_node;
        json_array_foreach(children, i, segment_node) {
            if (current_p->num_segments >= MAX_TEXT_SEGMENTS) break;
            TextSegment *current_seg = &current_p->segments[current_p->num_segments];
            
            json_t *text_node = json_object_get(segment_node, "text");
            strncpy(current_seg->text, json_string_value(text_node), MAX_TEXT_LEN - 1);
            current_seg->text[MAX_TEXT_LEN - 1] = '\0';
            
            json_t *styles_array = json_object_get(segment_node, "styles");
            size_t j;
            json_t *style_node;
            json_array_foreach(styles_array, j, style_node) {
                if (current_seg->num_styles >= MAX_STYLES) break;
                strncpy(current_seg->styles[current_seg->num_styles], json_string_value(style_node), 15);
                current_seg->styles[current_seg->num_styles][15] = '\0';
                current_seg->num_styles++;
            }
            current_p->num_segments++;
        }
        doc->num_paragraphs++;
    }

    if (doc->num_paragraphs == 0) { // Handle empty document case
        doc->num_paragraphs = 1;
        doc->paragraphs[0].num_segments = 1;
        doc->paragraphs[0].segments[0].text[0] = '\0';
        doc->paragraphs[0].segments[0].num_styles = 0;
    }

    json_decref(root);
    return 0;
}
