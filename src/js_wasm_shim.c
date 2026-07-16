/*
 * Freestanding-build support: definitions of the mem* functions the
 * compiler emits calls to. Only compiled into the wasm32 -nostdlib build
 * (see Makefile); native builds link libc instead. Must be compiled with
 * -fno-builtin so these bodies don't get folded into calls to themselves.
 */
#include <stddef.h>

int js_wasm_shim_present; /* keeps the TU non-empty when the guard is off */

#if defined(__wasm__) && defined(JSVM_FREESTANDING)

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (size_t i = n; i-- > 0;)
            d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    for (size_t i = 0; i < n; i++)
        d[i] = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a;
    const unsigned char *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

#endif /* __wasm__ && JSVM_FREESTANDING */
