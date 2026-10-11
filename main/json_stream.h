#pragma once
#include <stdbool.h>
#include <stddef.h>

/* A small streaming JSON reader for responses too large to hold in RAM.
 * Bytes are fed in as they arrive; every scalar value, and the end of every
 * object, is reported with the path leading to it. */

#define JS_MAX_DEPTH  12
#define JS_MAX_KEY    24
#define JS_MAX_VALUE  96

typedef struct {
    bool is_array;
    int  index;                 /* current element, for arrays */
    char key[JS_MAX_KEY];       /* current key, for objects */
} js_frame_t;

typedef enum { JS_STRING, JS_NUMBER, JS_LITERAL, JS_OBJECT_END } js_event_t;

struct js_parser;
/* value is NULL for JS_OBJECT_END, where the path leads to the object. */
typedef void (*js_callback_t)(struct js_parser *p, js_event_t ev, const char *value);

typedef struct js_parser {
    js_frame_t frames[JS_MAX_DEPTH];
    int depth;                  /* frames in use */
    int path_depth;             /* frames that make up the reported path */
    bool in_string, escape, expect_key;
    int unicode_left;           /* hex digits left in a \uXXXX escape */
    unsigned unicode;
    char buf[JS_MAX_VALUE];
    int len;
    bool in_scalar;
    bool error;
    js_callback_t cb;
    void *ctx;
} js_parser_t;

void js_init(js_parser_t *p, js_callback_t cb, void *ctx);
void js_feed(js_parser_t *p, const char *data, size_t len);

/* Match the current path against a pattern like
 * "journeys/0/legs/#/stopSequence/#/parent/name", where # is any index. */
bool js_path_is(const js_parser_t *p, const char *pattern);
