#include "js_syntax.h"

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
