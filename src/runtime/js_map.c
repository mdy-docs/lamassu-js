#include "lamassu_internal.h"

void js_map_init(JsMap *m) {
    m->entries = NULL;
    m->count = 0;
    m->used = 0;
    m->capacity = 0;
}

void js_map_free(JsVm *vm, JsMap *m) {
    js_realloc_raw(vm, m->entries, (size_t)m->capacity * sizeof *m->entries, 0);
    js_map_init(m);
}

static JsMapEntry *js_map_find(const JsMap *m, const JsString *key) {
    if (!m->capacity)
        return NULL;
    uint32_t mask = m->capacity - 1;
    uint32_t i = key->hash & mask;
    for (;;) {
        JsMapEntry *e = &m->entries[i];
        if (e->key == key)
            return e;
        if (e->key == NULL)
            return NULL;
        i = (i + 1) & mask;
    }
}

static bool js_map_grow(JsVm *vm, JsMap *m, uint32_t ncap) {
    JsMapEntry *ne = js_realloc_raw(vm, NULL, 0, (size_t)ncap * sizeof *ne);
    if (!ne)
        return false;
    memset(ne, 0, (size_t)ncap * sizeof *ne);
    uint32_t mask = ncap - 1;
    for (uint32_t i = 0; i < m->capacity; i++) {
        JsMapEntry *e = &m->entries[i];
        if (!e->key || e->key == JS_MAP_TOMBSTONE)
            continue;
        uint32_t j = e->key->hash & mask;
        while (ne[j].key)
            j = (j + 1) & mask;
        ne[j] = *e;
    }
    js_realloc_raw(vm, m->entries, (size_t)m->capacity * sizeof *ne, 0);
    m->entries = ne;
    m->capacity = ncap;
    m->used = m->count;
    return true;
}

bool js_map_set(JsVm *vm, JsMap *m, JsString *key, JsValue value) {
    if (m->used + 1 > m->capacity - m->capacity / 4) {
        /* Mostly tombstones? Rehash in place instead of doubling. */
        uint32_t ncap = m->count + 1 > m->capacity * 3 / 8
                            ? (m->capacity ? m->capacity * 2 : 8)
                            : m->capacity;
        if (!js_map_grow(vm, m, ncap))
            return false;
    }
    uint32_t mask = m->capacity - 1;
    uint32_t i = key->hash & mask;
    JsMapEntry *tomb = NULL;
    for (;;) {
        JsMapEntry *e = &m->entries[i];
        if (e->key == key) {
            e->value = value;
            return true;
        }
        if (e->key == JS_MAP_TOMBSTONE) {
            if (!tomb)
                tomb = e;
        } else if (e->key == NULL) {
            JsMapEntry *dst = tomb ? tomb : e;
            if (!tomb)
                m->used++;
            dst->key = key;
            dst->value = value;
            m->count++;
            return true;
        }
        i = (i + 1) & mask;
    }
}

JsValue js_map_get(const JsMap *m, const JsString *key, bool *found) {
    JsMapEntry *e = js_map_find(m, key);
    *found = e != NULL;
    return e ? e->value : js_undefined();
}

bool js_map_delete(JsMap *m, const JsString *key) {
    JsMapEntry *e = js_map_find(m, key);
    if (!e)
        return false;
    e->key = JS_MAP_TOMBSTONE;
    e->value = js_undefined();
    m->count--;
    return true;
}
