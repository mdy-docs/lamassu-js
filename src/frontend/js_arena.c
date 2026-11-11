#include <stdarg.h>
#include <string.h>
#include "js_syntax.h"
#include "lamassu_internal.h"

struct JsArenaChunk {
    JsArenaChunk *next;
    size_t size; /* data bytes following the header */
    size_t used;
};

#define JS_ARENA_CHUNK_BYTES ((size_t)8192)

void js_arena_init(JsArena *a, JsVm *vm) {
    a->vm = vm;
    a->chunks = NULL;
}

void *js_arena_alloc(JsArena *a, size_t size) {
    /* Reject sizes so large that the 8-byte round-up or the chunk-header add
     * below would wrap size_t (unreachable on 64-bit — the parser never asks
     * for petabyte allocations — but a cheap guard against silent under-alloc). */
    if (size > SIZE_MAX - 7 - sizeof(JsArenaChunk))
        return NULL;
    size = (size + 7) & ~(size_t)7;
    JsArenaChunk *c = a->chunks;
    if (!c || c->size - c->used < size) {
        size_t data = size > JS_ARENA_CHUNK_BYTES ? size : JS_ARENA_CHUNK_BYTES;
        c = js_realloc_raw(a->vm, NULL, 0, sizeof *c + data);
        if (!c)
            return NULL;
        c->next = a->chunks;
        c->size = data;
        c->used = 0;
        a->chunks = c;
    }
    void *p = (char *)(c + 1) + c->used;
    c->used += size;
    return p;
}

void js_arena_free(JsArena *a) {
    JsArenaChunk *c = a->chunks;
    while (c) {
        JsArenaChunk *next = c->next;
        js_realloc_raw(a->vm, c, sizeof *c + c->size, 0);
        c = next;
    }
    a->chunks = NULL;
}

const char *js_compile_error_fmt(JsVm *vm, const char *fmt, ...) {
    char *out = vm->compile_error;
    size_t cap = sizeof vm->compile_error - 4, n = 0; /* room for one more UTF-8 sequence */
    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f && n < cap; f++) {
        if (*f != '%' || !f[1]) {
            out[n++] = *f;
            continue;
        }
        f++;
        if (*f == 's') {
            for (const char *s = va_arg(ap, const char *); *s && n < cap; s++)
                out[n++] = *s;
        } else if (*f == 'U') {
            const uint16_t *u = va_arg(ap, const uint16_t *);
            uint32_t len = va_arg(ap, uint32_t);
            bool cut = len > 60;
            if (cut)
                len = 60;
            for (uint32_t i = 0; i < len && n < cap; i++) {
                uint32_t c = u[i];
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
                    i++;
                }
                if (c < 0x80) {
                    out[n++] = (char)c;
                } else if (c < 0x800) {
                    out[n++] = (char)(0xC0 | (c >> 6));
                    out[n++] = (char)(0x80 | (c & 0x3F));
                } else if (c < 0x10000) {
                    out[n++] = (char)(0xE0 | (c >> 12));
                    out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (c & 0x3F));
                } else {
                    out[n++] = (char)(0xF0 | (c >> 18));
                    out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
                    out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (c & 0x3F));
                }
            }
            for (const char *s = "..."; cut && *s && n < cap; s++)
                out[n++] = *s;
        } else {
            out[n++] = *f;
        }
    }
    va_end(ap);
    out[n] = '\0';
    return out;
}
