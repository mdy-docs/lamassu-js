#include "lamassu_internal.h"

/*
 * The property map: a dense array of entries in insertion order, with a
 * hash index beside it. The order is not a nicety — a script's Object.keys
 * and JSON.stringify are specified to see string keys in the order they were
 * added (array-index keys first, ascending), and a host handing a tree of
 * properties across the boundary and back (mdy's transforms) relies on
 * getting it back as it was.
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
    m->unsorted = false;
    m->by_name = false;
}

/* Bytes of an entries allocation with room for CAP entries: the entries, then an attribute byte
 * for each (JS_MAP_ATTRS) */
static size_t entries_bytes(uint32_t cap) {
    return (size_t)cap * (sizeof(JsMapEntry) + 1);
}

void js_map_free(JsVm *vm, JsMap *m) {
    js_realloc_raw(vm, m->entries, entries_bytes(m->capacity), 0);
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
    JsMapEntry *ne = js_realloc_raw(vm, NULL, 0, entries_bytes(ncap));
    if (!ne)
        return false;
    uint32_t *ni = js_realloc_raw(vm, NULL, 0, (size_t)nidx * sizeof *ni);
    if (!ni) {
        js_realloc_raw(vm, ne, entries_bytes(ncap), 0);
        return false;
    }
    memset(ne, 0, entries_bytes(ncap)); /* every attribute byte 0 too */
    uint8_t *na = (uint8_t *)(ne + ncap);
    memset(ni, 0xff, (size_t)nidx * sizeof *ni); /* every slot JS_MAP_EMPTY */
    uint32_t mask = nidx - 1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->used; i++) {
        JsMapEntry *e = &m->entries[i];
        if (!e->key || e->key == JS_MAP_TOMBSTONE)
            continue;
        ne[n] = *e;
        na[n] = JS_MAP_ATTRS(m)[i];
        uint32_t j = e->key->hash & mask;
        while (ni[j] != JS_MAP_EMPTY)
            j = (j + 1) & mask;
        ni[j] = n;
        n++;
    }
    js_realloc_raw(vm, m->entries, entries_bytes(m->capacity), 0);
    js_realloc_raw(vm, m->index, (size_t)m->index_cap * sizeof *ni, 0);
    m->entries = ne;
    m->index = ni;
    m->capacity = ncap;
    m->index_cap = nidx;
    m->used = n;
    m->count = n;
    return true;
}

/* An array index as a property name: "0", or digits with no leading zero, below 2^32 - 1. */
static bool key_index(const JsString *k, uint32_t *out) {
    uint32_t n = k->length;
    if (n == 0 || n > 10 || k->units[0] < '0' || k->units[0] > '9' || (n > 1 && k->units[0] == '0'))
        return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c = k->units[i];
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (uint64_t)(c - '0');
    }
    if (v > 4294967294u)
        return false;
    *out = (uint32_t)v;
    return true;
}

/* Enumeration order of the entries at positions a and b: index keys first, by value; the rest by
 * position (insertion order). Total, since keys are distinct. */
static bool order_before(const JsMap *m, uint32_t a, uint32_t b) {
    if (m->by_name) { /* a module namespace: export names in code-unit order */
        const JsString *ka = m->entries[a].key, *kb = m->entries[b].key;
        uint32_t n = ka->length < kb->length ? ka->length : kb->length;
        for (uint32_t i = 0; i < n; i++)
            if (ka->units[i] != kb->units[i])
                return ka->units[i] < kb->units[i];
        return ka->length < kb->length;
    }
    uint32_t va, vb;
    bool ia = key_index(m->entries[a].key, &va), ib = key_index(m->entries[b].key, &vb);
    if (ia != ib)
        return ia;
    return ia ? va < vb : a < b;
}

static void sift_down(const JsMap *m, uint32_t *perm, uint32_t root, uint32_t n) {
    for (;;) {
        uint32_t child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && order_before(m, perm[child], perm[child + 1]))
            child++;
        if (!order_before(m, perm[root], perm[child]))
            return;
        uint32_t t = perm[root];
        perm[root] = perm[child];
        perm[child] = t;
        root = child;
    }
}

/*
 * Put the entries in enumeration order. It allocates nothing — the hash index (at least twice the
 * entries) holds the permutation while the entries are sorted, then is rebuilt — so it can run from
 * a host call that has no VM to allocate with (js_object_key_at). Packs tombstones out as it goes.
 */
void js_map_sort_keys(JsMap *m) {
    if (!m->unsorted)
        return;
    m->unsorted = false;
    uint8_t *attrs = JS_MAP_ATTRS(m);
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->used; i++)
        if (m->entries[i].key && m->entries[i].key != JS_MAP_TOMBSTONE) {
            attrs[n] = attrs[i];
            m->entries[n++] = m->entries[i];
        }
    for (uint32_t i = n; i < m->used; i++) {
        m->entries[i].key = NULL;
        m->entries[i].value = js_undefined();
        attrs[i] = 0;
    }
    m->used = n;
    m->count = n;
    uint32_t *perm = m->index; /* perm[k]: the position whose entry goes to k */
    for (uint32_t k = 0; k < n; k++)
        perm[k] = k;
    for (uint32_t i = n / 2; i-- > 0;)
        sift_down(m, perm, i, n);
    for (uint32_t end = n; end-- > 1;) {
        uint32_t t = perm[0];
        perm[0] = perm[end];
        perm[end] = t;
        sift_down(m, perm, 0, end);
    }
    for (uint32_t s = 0; s < n; s++) { /* apply it in place, a cycle at a time */
        if (perm[s] == s)
            continue;
        JsMapEntry first = m->entries[s];
        uint8_t first_attrs = attrs[s];
        uint32_t j = s;
        for (;;) {
            uint32_t src = perm[j];
            perm[j] = j;
            if (src == s) {
                m->entries[j] = first;
                attrs[j] = first_attrs;
                break;
            }
            m->entries[j] = m->entries[src];
            attrs[j] = attrs[src];
            j = src;
        }
    }
    memset(m->index, 0xff, (size_t)m->index_cap * sizeof *m->index);
    uint32_t mask = m->index_cap - 1;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t j = m->entries[k].key->hash & mask;
        while (m->index[j] != JS_MAP_EMPTY)
            j = (j + 1) & mask;
        m->index[j] = k;
    }
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
    /* In order, the last entry is the greatest: an index key stays in order only behind a smaller
     * one. (A tombstone there could hide anything, so it counts as out of order.) */
    uint32_t v, last;
    if (m->by_name && m->used)
        m->unsorted = true; /* rare (a namespace gains names once); sorted when next enumerated */
    if (!m->unsorted && m->used && key_index(key, &v)) {
        const JsString *lk = m->entries[m->used - 1].key;
        if (lk == JS_MAP_TOMBSTONE || !key_index(lk, &last) || last > v)
            m->unsorted = true;
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
    JS_MAP_ATTRS(m)[e - m->entries] = 0;
    m->count--;
    return true;
}

bool js_map_add_attrs(JsMap *m, const JsString *key, uint8_t attrs) {
    JsMapEntry *e = js_map_find(m, key);
    if (!e)
        return false;
    JS_MAP_ATTRS(m)[e - m->entries] |= attrs;
    return true;
}
