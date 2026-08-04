/*
 * UTF-16 <-> UTF-8 primitives, shared by every wasm embedding.
 *
 * The engine speaks UTF-16 code units; every host boundary speaks UTF-8, so
 * both src/wasm_api.c (emscripten) and src/reactor.c (WASI) need exactly these
 * two functions. They live here so the encoding tables exist in one place —
 * getting an unpaired surrogate wrong in one copy and right in the other is
 * the kind of bug that survives for years.
 *
 * Header-only static inline: the two embeddings are never linked together, so
 * there is nothing to share at link time and no archive to complicate.
 */
#ifndef JS_UTF8_H
#define JS_UTF8_H

#include <stddef.h>
#include <stdint.h>

/*
 * Decodes the code unit at u[*i] into a scalar value and advances *i: a valid
 * surrogate pair combines and consumes two units; an unpaired surrogate yields
 * U+FFFD (so the emitted UTF-8 is always well-formed, never CESU-8).
 */
static inline unsigned js_utf16_next_cp(const uint16_t *u, size_t n, size_t *i) {
    unsigned cp = u[*i];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (*i + 1 < n && u[*i + 1] >= 0xDC00 && u[*i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[*i + 1] - 0xDC00);
            *i += 2;
            return cp;
        }
        cp = 0xFFFD; /* unpaired high surrogate */
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        cp = 0xFFFD; /* unpaired low surrogate */
    }
    *i += 1;
    return cp;
}

/* Encodes scalar cp into t[0..3]; returns the byte count (1..4). */
static inline int js_utf8_encode_cp(unsigned cp, char t[4]) {
    if (cp < 0x80) {
        t[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    t[0] = (char)(0xF0 | (cp >> 18));
    t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    t[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/*
 * UTF-8 -> UTF-16, well-formed only: an overlong encoding, a surrogate
 * codepoint, a value above U+10FFFF, a truncated sequence, or a bad
 * continuation byte each becomes one U+FFFD and decoding resyncs at the next
 * byte. Writes at most `len` units (one per input byte is always enough) and
 * returns how many were written.
 */
static inline size_t js_utf8_to_utf16(const uint8_t *in, size_t len, uint16_t *out) {
    size_t n = 0, i = 0;
    while (i < len) {
        uint8_t b = in[i];
        uint32_t cp, min;
        int seqlen;
        if (b < 0x80) {
            out[n++] = b;
            i++;
            continue;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1Fu; seqlen = 2; min = 0x80;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0Fu; seqlen = 3; min = 0x800;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07u; seqlen = 4; min = 0x10000;
        } else {
            out[n++] = 0xFFFD; i++; continue;
        }
        int ok = i + (size_t)seqlen <= len;
        for (int k = 1; ok && k < seqlen; k++) {
            uint8_t c = in[i + (size_t)k];
            if ((c & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (!ok || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out[n++] = 0xFFFD; i++; continue;
        }
        i += (size_t)seqlen;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

#endif /* JS_UTF8_H */
