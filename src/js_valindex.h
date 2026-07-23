/*
 * Value-keyed hash index for the JS Map and Set collections.
 *
 * Map/Set keep their entries in dense, insertion-ordered arrays (required for
 * spec iteration order and used directly by forEach/snapshotting). On top of
 * that ordered storage this header layers an open-addressed hash *index*:
 * hash(key) -> position in the ordered array. It turns get/has/set membership
 * tests from an O(n) linear scan into O(1) amortized, so a bulk build
 * (`for (…) m.set(i, i)`) is O(n) rather than O(n^2).
 *
 * The index stores positions, never pointers, so growing the ordered arrays
 * (a realloc that keeps positions) leaves it valid. A delete shifts the
 * ordered arrays to preserve insertion order and therefore rebuilds the index
 * (O(n) per delete — the same tradeoff the ordered storage already makes).
 *
 * Key equality is SameValueZero and the hash matches it: NaN hashes to one
 * value (all NaNs are equal), -0 and +0 hash alike (they are equal), strings
 * hash by content (this engine does not intern every string), and everything
 * else by its bit pattern (reference/fixed-pattern identity).
 */
#ifndef JS_VALINDEX_H
#define JS_VALINDEX_H

#include "lamassu_internal.h"
#include <string.h>

/* SameValueZero: like ===, but NaN equals NaN and +0 equals -0; strings by
 * content, everything else by raw bits. */
static inline bool js_same_value_zero(JsValue a, JsValue b) {
    if (js_is_number(a) && js_is_number(b)) {
        double da = js_get_number(a), db = js_get_number(b);
        if (da != da)
            return db != db; /* NaN */
        return da == db;     /* also true for +0 == -0 */
    }
    if (js_is_string(a) && js_is_string(b))
        return js_string_equals(a, b);
    return js_same_value(a, b);
}

/* Under SameValueZero -0 and +0 are the same key; the spec stores the
 * normalized +0 so Map/Set iteration never surfaces a -0 key. */
static inline JsValue js_normalize_map_key(JsValue k) {
    if (js_is_number(k) && js_get_number(k) == 0.0)
        return js_number(0.0); /* collapse -0 to +0 */
    return k;
}

static inline uint32_t js_value_hash(JsValue v) {
    if (js_is_number(v)) {
        double d = js_get_number(v);
        if (d != d)
            return UINT32_C(0x7ff80000); /* every NaN hashes alike */
        if (d == 0.0)
            d = 0.0; /* normalize -0 to +0 (they compare equal) */
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        return (uint32_t)(bits ^ (bits >> 32));
    }
    if (js_is_string(v))
        return js_value_string(v)->hash;
    uint64_t b = v.bits;
    return (uint32_t)(b ^ (b >> 32));
}

/* Position of key among keys[0..], or -1. Slots hold a position or -1 (empty);
 * linear probing terminates at the first empty slot. */
static inline int64_t js_valindex_find(const int32_t *index, uint32_t index_cap,
                                       const JsValue *keys, JsValue key) {
    if (!index_cap)
        return -1;
    uint32_t mask = index_cap - 1;
    uint32_t i = js_value_hash(key) & mask;
    for (;;) {
        int32_t pos = index[i];
        if (pos < 0)
            return -1;
        if (js_same_value_zero(keys[(uint32_t)pos], key))
            return pos;
        i = (i + 1) & mask;
    }
}

/* Inserts keys[pos] -> pos into an index known to have a free slot. */
static inline void js_valindex_put(int32_t *index, uint32_t index_cap,
                                   const JsValue *keys, uint32_t pos) {
    uint32_t mask = index_cap - 1;
    uint32_t i = js_value_hash(keys[pos]) & mask;
    while (index[i] >= 0)
        i = (i + 1) & mask;
    index[i] = (int32_t)pos;
}

/* Clears the index and reinserts keys[0..count). Used after a delete shifts
 * positions, and by js_valindex_add when it grows. */
static inline void js_valindex_rebuild(int32_t *index, uint32_t index_cap,
                                       const JsValue *keys, uint32_t count) {
    if (!index_cap)
        return;
    memset(index, 0xFF, (size_t)index_cap * sizeof(int32_t)); /* all -1 */
    for (uint32_t p = 0; p < count; p++)
        js_valindex_put(index, index_cap, keys, p);
}

/*
 * Records keys[newpos] in the index, where `count` is the new live entry count
 * (keys[newpos] is the entry just appended, so newpos == count - 1). Grows and
 * rehashes when the table would pass a 75% load factor; otherwise a single
 * O(1) insert. Returns false only on allocation failure.
 */
static inline bool js_valindex_add(JsVm *vm, int32_t **index, uint32_t *index_cap,
                                   const JsValue *keys, uint32_t count, uint32_t newpos) {
    if (!*index || count > *index_cap - *index_cap / 4) {
        uint32_t ncap = *index_cap ? *index_cap : 8;
        while (count > ncap - ncap / 4)
            ncap *= 2;
        int32_t *ni = js_realloc_raw(vm, *index, (size_t)*index_cap * sizeof(int32_t),
                                     (size_t)ncap * sizeof(int32_t));
        if (!ni)
            return false;
        *index = ni;
        *index_cap = ncap;
        js_valindex_rebuild(ni, ncap, keys, count); /* inserts newpos too */
        return true;
    }
    js_valindex_put(*index, *index_cap, keys, newpos);
    return true;
}

#endif /* JS_VALINDEX_H */
