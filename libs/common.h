#ifndef COMMON_H
#define COMMON_H

#include "jansson.h"

#define MAX_STYLES 4
#define MAX_TEXT_LEN 256
#define MAX_TEXT_SEGMENTS 64
#define MAX_PARAGRAPHS 128

// Represents a piece of text with a consistent set of styles
typedef struct {
    char text[MAX_TEXT_LEN];
    char styles[MAX_STYLES][16];
    int num_styles;
} TextSegment;

// Represents a single paragraph, which is a collection of text segments
typedef struct {
    TextSegment segments[MAX_TEXT_SEGMENTS];
    int num_segments;
} Paragraph;

// Represents the entire document
typedef struct {
    Paragraph paragraphs[MAX_PARAGRAPHS];
    int num_paragraphs;
} Document;


// --- JSON Helper Function Prototypes ---

// Converts a Document struct to a JSON string. The caller must free the returned string.
char *document_to_json(const Document *doc);

// Parses a JSON string and populates a Document struct. Returns 0 on success.
int json_to_document(const char *json_str, Document *doc);


#endif // COMMON_H
