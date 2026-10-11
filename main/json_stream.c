/* Streaming JSON reader, see json_stream.h. It accepts valid JSON and
 * ignores what it doesn't understand; values longer than JS_MAX_VALUE are
 * truncated. */
#include <stdlib.h>
#include <string.h>
#include "json_stream.h"

void js_init(js_parser_t *p, js_callback_t cb, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->ctx = ctx;
}

static void put(js_parser_t *p, char c)
{
    if (p->len < JS_MAX_VALUE - 1) {
        p->buf[p->len++] = c;
    }
}

/* Append a code point as UTF-8. */
static void put_utf8(js_parser_t *p, unsigned cp)
{
    if (cp < 0x80) {
        put(p, cp);
    } else if (cp < 0x800) {
        put(p, 0xC0 | (cp >> 6));
        put(p, 0x80 | (cp & 0x3F));
    } else {
        put(p, 0xE0 | (cp >> 12));
        put(p, 0x80 | ((cp >> 6) & 0x3F));
        put(p, 0x80 | (cp & 0x3F));
    }
}

static void emit(js_parser_t *p, js_event_t ev)
{
    p->buf[p->len] = '\0';
    p->path_depth = p->depth;
    p->cb(p, ev, p->buf);
    p->len = 0;
}

static void end_scalar(js_parser_t *p)
{
    if (p->in_scalar) {
        p->in_scalar = false;
        char c = p->buf[0];
        emit(p, (c == '-' || (c >= '0' && c <= '9')) ? JS_NUMBER : JS_LITERAL);
    }
}

static void push(js_parser_t *p, bool is_array)
{
    if (p->depth >= JS_MAX_DEPTH) {
        p->error = true;
        return;
    }
    js_frame_t *f = &p->frames[p->depth++];
    memset(f, 0, sizeof(*f));
    f->is_array = is_array;
    p->expect_key = !is_array;
}

static void pop(js_parser_t *p, bool is_array)
{
    if (p->depth == 0) {
        p->error = true;
        return;
    }
    p->depth--;
    if (!is_array) {
        /* Report the end of the object, with the path leading to it. */
        p->path_depth = p->depth;
        p->cb(p, JS_OBJECT_END, NULL);
    }
    p->expect_key = false;
}

void js_feed(js_parser_t *p, const char *data, size_t n)
{
    for (size_t i = 0; i < n && !p->error; i++) {
        char c = data[i];
        if (p->in_string) {
            if (p->unicode_left) {
                char h[2] = { c, 0 };
                p->unicode = p->unicode * 16 + strtoul(h, NULL, 16);
                if (--p->unicode_left == 0) {
                    put_utf8(p, p->unicode);
                }
            } else if (p->escape) {
                p->escape = false;
                switch (c) {
                case 'n': put(p, '\n'); break;
                case 't': put(p, '\t'); break;
                case 'r': case 'b': case 'f': break;
                case 'u': p->unicode_left = 4; p->unicode = 0; break;
                default:  put(p, c);
                }
            } else if (c == '\\') {
                p->escape = true;
            } else if (c == '"') {
                p->in_string = false;
                js_frame_t *f = p->depth ? &p->frames[p->depth - 1] : NULL;
                if (f && !f->is_array && p->expect_key) {
                    p->buf[p->len] = '\0';
                    strlcpy(f->key, p->buf, sizeof(f->key));
                    p->len = 0;
                } else {
                    emit(p, JS_STRING);
                }
            } else {
                put(p, c);
            }
            continue;
        }
        switch (c) {
        case '"':
            end_scalar(p);
            p->in_string = true;
            p->len = 0;
            break;
        case '{': end_scalar(p); push(p, false); break;
        case '[': end_scalar(p); push(p, true); break;
        case '}': end_scalar(p); pop(p, false); break;
        case ']': end_scalar(p); pop(p, true); break;
        case ':':
            end_scalar(p);
            p->expect_key = false;
            break;
        case ',':
            end_scalar(p);
            if (p->depth) {
                js_frame_t *f = &p->frames[p->depth - 1];
                if (f->is_array) {
                    f->index++;
                } else {
                    p->expect_key = true;
                }
            }
            break;
        case ' ': case '\n': case '\r': case '\t':
            end_scalar(p);
            break;
        default:
            if (!p->in_scalar) {
                p->in_scalar = true;
                p->len = 0;
            }
            put(p, c);
        }
    }
}

bool js_path_is(const js_parser_t *p, const char *pattern)
{
    const char *s = pattern;
    for (int i = 0; i < p->path_depth; i++) {
        const js_frame_t *f = &p->frames[i];
        const char *end = strchr(s, '/');
        size_t n = end ? (size_t)(end - s) : strlen(s);
        if (n == 0) {
            return false;
        }
        if (f->is_array) {
            if (!(n == 1 && s[0] == '#') && atoi(s) != f->index) {
                return false;
            }
        } else if (strlen(f->key) != n || strncmp(f->key, s, n) != 0) {
            return false;
        }
        if (!end) {
            return i == p->path_depth - 1;
        }
        s = end + 1;
    }
    return false;
}
