#include "lamassu_internal.h"

/*
 * The property map: a dense array of entries in insertion order, with a
 * hash index beside it. The order is not a nicety — a script's Object.keys,
 * for-in and JSON.stringify are specified to see string keys in the order
 * they were added, and a host handing a tree of properties across the
 * boundary and back (mdy's transforms) relies on getting it back as it was.
 * The index is open addressing over positions; a tombstoned entry keeps its
 * position so the probe chains through it stay whole, and a rebuild — on
 * growth, or when tombstones outnumber the live — packs the array down.
 */

void js_map_init(JsMap *m) {
    m->entries = NULL;
    m->index = NULL;
    m->count = 0;
    m->used = 0;
    m->capacity = 0;
    m->index_cap = 0;
}

void js_map_free(JsVm *vm, JsMap *m) {
    js_realloc_raw(vm, m->entries, (size_t)m->capacity * sizeof *m->entries, 0);
    js_realloc_raw(vm, m->index, (size_t)m->index_cap * sizeof *m->index, 0);
    js_map_init(m);
}

static JsMapEntry *js_map_find(const JsMap *m, const JsString *key) {
    if (!m->index_cap)
        return NULL;
    uint32_t mask = m->index_cap - 1;
    uint32_t i = key->hash & mask;
    for (;;) {
        uint32_t at = m->index[i];
        if (at == JS_MAP_EMPTY)
            return NULL;
        JsMapEntry *e = &m->entries[at];
        if (e->key == key)
            return e;
        i = (i + 1) & mask;
    }
}

/* Rebuild with room for `ncap` entries: the live ones, packed, in order. */
static bool js_map_grow(JsVm *vm, JsMap *m, uint32_t ncap) {
    uint32_t nidx = ncap * 2; /* load stays at or under a half */
    JsMapEntry *ne = js_realloc_raw(vm, NULL, 0, (size_t)ncap * sizeof *ne);
    if (!ne)
        return false;
    uint32_t *ni = js_realloc_raw(vm, NULL, 0, (size_t)nidx * sizeof *ni);
    if (!ni) {
        js_realloc_raw(vm, ne, (size_t)ncap * sizeof *ne, 0);
        return false;
    }
    memset(ne, 0, (size_t)ncap * sizeof *ne);
    memset(ni, 0xff, (size_t)nidx * sizeof *ni); /* every slot JS_MAP_EMPTY */
    uint32_t mask = nidx - 1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->used; i++) {
        JsMapEntry *e = &m->entries[i];
        if (!e->key || e->key == JS_MAP_TOMBSTONE)
            continue;
        ne[n] = *e;
        uint32_t j = e->key->hash & mask;
        while (ni[j] != JS_MAP_EMPTY)
            j = (j + 1) & mask;
        ni[j] = n;
        n++;
    }
    js_realloc_raw(vm, m->entries, (size_t)m->capacity * sizeof *ne, 0);
    js_realloc_raw(vm, m->index, (size_t)m->index_cap * sizeof *ni, 0);
    m->entries = ne;
    m->index = ni;
    m->capacity = ncap;
    m->index_cap = nidx;
    m->used = n;
    m->count = n;
    return true;
}

bool js_map_set(JsVm *vm, JsMap *m, JsString *key, JsValue value) {
    JsMapEntry *have = js_map_find(m, key);
    if (have) {
        have->value = value;
        return true;
    }
    if (m->used == m->capacity) {
        /* Mostly tombstones? Pack in place rather than doubling. */
        uint32_t ncap = m->count + 1 > m->capacity / 2
                            ? (m->capacity ? m->capacity * 2 : 8)
                            : m->capacity;
        if (!js_map_grow(vm, m, ncap))
            return false;
    }
    uint32_t mask = m->index_cap - 1;
    uint32_t i = key->hash & mask;
    while (m->index[i] != JS_MAP_EMPTY)
        i = (i + 1) & mask;
    m->index[i] = m->used;
    JsMapEntry *dst = &m->entries[m->used++];
    dst->key = key;
    dst->value = value;
    m->count++;
    return true;
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
