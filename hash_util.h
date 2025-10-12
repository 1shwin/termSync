// hash_util.h - Simple hash function (no OpenSSL needed)
#ifndef HASH_UTIL_H
#define HASH_UTIL_H

#include <stdint.h>
#include <string.h>
#include "common.h"

// Simple FNV-1a hash (good enough for consistency checking)
static inline void compute_document_hash(StyledChar content[MAX_LINES][MAX_LINE_LEN],
                                        int line_lengths[MAX_LINES],
                                        int num_lines,
                                        uint8_t hash[32]) {
    // FNV-1a parameters
    uint64_t h1 = 14695981039346656037ULL;
    uint64_t h2 = 14695981039346656037ULL;
    uint64_t h3 = 14695981039346656037ULL;
    uint64_t h4 = 14695981039346656037ULL;
    const uint64_t fnv_prime = 1099511628211ULL;
    
    for (int y = 0; y < num_lines; y++) {
        // Hash line length
        h1 ^= line_lengths[y];
        h1 *= fnv_prime;
        
        // Hash content
        for (int x = 0; x < line_lengths[y]; x++) {
            h2 ^= (uint8_t)content[y][x].ch;
            h2 *= fnv_prime;
            h3 ^= (uint8_t)content[y][x].format;
            h3 *= fnv_prime;
        }
        
        // Hash line separator
        h4 ^= '\n';
        h4 *= fnv_prime;
    }
    
    // Store as 32 bytes
    memcpy(&hash[0],  &h1, 8);
    memcpy(&hash[8],  &h2, 8);
    memcpy(&hash[16], &h3, 8);
    memcpy(&hash[24], &h4, 8);
}

static inline bool hashes_equal(uint8_t h1[32], uint8_t h2[32]) {
    return memcmp(h1, h2, 32) == 0;
}

#endif // HASH_UTIL_H