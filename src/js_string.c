#include "lamassu_internal.h"

uint32_t js_units_hash(const uint16_t *units, size_t len, uint32_t seed) {
    /* Seeded FNV-1a over code units. seed == 0 (the default) reproduces the
     * plain FNV hash exactly; a non-zero per-VM seed (set when the embedder
     * provides rng_seed) perturbs the basis so an attacker cannot precompute a
     * colliding key set — HashDoS hardening. */
    uint32_t h = 2166136261u ^ seed; /* FNV-1a over code units */
    for (size_t i = 0; i < len; i++) {
        h ^= units[i];
        h *= 16777619u;
    }
    return h;
}

JsString *js_string_cell_new(JsVm *vm, const uint16_t *units, size_t len) {
    if (len > UINT32_MAX)
        return NULL;
    JsGcCell *c = js_gc_new_cell(vm, JS_KIND_STRING,
                                 sizeof(JsString) + len * sizeof(uint16_t));
    if (!c)
        return NULL;
    JsString *s = (JsString *)c;
    s->length = (uint32_t)len;
    s->hash = js_units_hash(units, len, vm->hash_seed);
    s->interned = false;
    if (len)
        memcpy(s->units, units, len * sizeof(uint16_t));
    return s;
}

JsValue js_string_new(JsVm *vm, const uint16_t *units, size_t len) {
    JsString *s = js_string_cell_new(vm, units, len);
    return s ? js_value_from_cell(&s->gc) : js_undefined();
}

const uint16_t *js_string_units(JsValue str, size_t *len) {
    if (!js_is_string(str)) {
        if (len)
            *len = 0;
        return NULL;
    }
    JsString *s = js_value_string(str);
    if (len)
        *len = s->length;
    return s->units;
}

size_t js_string_length(JsValue str) {
    return js_is_string(str) ? js_value_string(str)->length : 0;
}

bool js_string_equals(JsValue a, JsValue b) {
    if (!js_is_string(a) || !js_is_string(b))
        return false;
    JsString *x = js_value_string(a);
    JsString *y = js_value_string(b);
    if (x == y)
        return true;
    if (x->hash != y->hash || x->length != y->length)
        return false;
    return memcmp(x->units, y->units, (size_t)x->length * sizeof(uint16_t)) == 0;
}

/* ---- atom table ---- */

JsString *js_atoms_find(JsVm *vm, const uint16_t *units, size_t len, uint32_t hash) {
    JsAtomTable *t = &vm->atoms;
    if (!t->capacity)
        return NULL;
    uint32_t mask = t->capacity - 1;
    uint32_t i = hash & mask;
    for (;;) {
        JsString *s = t->slots[i];
        if (s == NULL)
            return NULL;
        if (s != JS_MAP_TOMBSTONE && s->hash == hash && s->length == len &&
            memcmp(s->units, units, len * sizeof(uint16_t)) == 0)
            return s;
        i = (i + 1) & mask;
    }
}

static bool js_atoms_grow(JsVm *vm, uint32_t ncap) {
    JsAtomTable *t = &vm->atoms;
    JsString **ns = js_realloc_raw(vm, NULL, 0, (size_t)ncap * sizeof *ns);
    if (!ns)
        return false;
    memset(ns, 0, (size_t)ncap * sizeof *ns);
    uint32_t mask = ncap - 1;
    for (uint32_t i = 0; i < t->capacity; i++) {
        JsString *s = t->slots[i];
        if (!s || s == JS_MAP_TOMBSTONE)
            continue;
        uint32_t j = s->hash & mask;
        while (ns[j])
            j = (j + 1) & mask;
        ns[j] = s;
    }
    js_realloc_raw(vm, t->slots, (size_t)t->capacity * sizeof *ns, 0);
    t->slots = ns;
    t->capacity = ncap;
    t->used = t->count;
    return true;
}

static bool js_atoms_insert(JsVm *vm, JsString *s) {
    JsAtomTable *t = &vm->atoms;
    if (t->used + 1 > t->capacity - t->capacity / 4) {
        uint32_t ncap = t->count + 1 > t->capacity * 3 / 8
                            ? (t->capacity ? t->capacity * 2 : 32)
                            : t->capacity;
        if (!js_atoms_grow(vm, ncap))
            return false;
    }
    uint32_t mask = t->capacity - 1;
    uint32_t i = s->hash & mask;
    while (t->slots[i] && t->slots[i] != JS_MAP_TOMBSTONE)
        i = (i + 1) & mask;
    if (t->slots[i] == NULL)
        t->used++;
    t->slots[i] = s;
    t->count++;
    return true;
}

JsValue js_atom(JsVm *vm, const uint16_t *units, size_t len) {
    uint32_t hash = js_units_hash(units, len, vm->hash_seed);
    JsString *found = js_atoms_find(vm, units, len, hash);
    if (found)
        return js_value_from_cell(&found->gc);
    JsString *s = js_string_cell_new(vm, units, len);
    if (!s)
        return js_undefined();
    if (!js_atoms_insert(vm, s))
        return js_undefined(); /* s stays uninterned garbage; next GC frees it */
    s->interned = true;
    return js_value_from_cell(&s->gc);
}

JsString *js_intern_cell(JsVm *vm, JsString *s) {
    if (s->interned)
        return s;
    JsString *found = js_atoms_find(vm, s->units, s->length, s->hash);
    if (found)
        return found;
    if (!js_atoms_insert(vm, s))
        return NULL;
    s->interned = true;
    return s;
}

void js_atoms_remove(JsVm *vm, JsString *s) {
    JsAtomTable *t = &vm->atoms;
    if (!t->capacity)
        return;
    uint32_t mask = t->capacity - 1;
    uint32_t i = s->hash & mask;
    for (;;) {
        if (t->slots[i] == s) {
            t->slots[i] = JS_MAP_TOMBSTONE;
            t->count--;
            return;
        }
        if (t->slots[i] == NULL)
            return; /* not present; nothing to do */
        i = (i + 1) & mask;
    }
}

void js_atoms_free(JsVm *vm) {
    JsAtomTable *t = &vm->atoms;
    js_realloc_raw(vm, t->slots, (size_t)t->capacity * sizeof *t->slots, 0);
    t->slots = NULL;
    t->count = t->used = t->capacity = 0;
}
