/*
 * jsvm — compile and run a JavaScript (jsvm subset) file.
 *
 * Host-side tool: file I/O and UTF-8 <-> UTF-16 conversion live here, never
 * in the core. Prints the module's completion value (REPL-style), or a
 * diagnostic with line:col on error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js_syntax.h" /* js_source_line_col */
#include "jsvm.h"

static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

/* UTF-8 -> UTF-16; invalid sequences become U+FFFD. */
static uint16_t *utf8_to_utf16(const uint8_t *in, size_t len, size_t *out_len) {
    uint16_t *out = malloc((len + 1) * sizeof(uint16_t)); /* upper bound */
    if (!out)
        return NULL;
    size_t n = 0, i = 0;
    while (i < len) {
        uint32_t cp;
        uint8_t b = in[i];
        if (b < 0x80) {
            cp = b;
            i += 1;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((uint32_t)(b & 0x1F) << 6) | (in[i + 1] & 0x3F);
            i += 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(in[i + 1] & 0x3F) << 6) |
                 (in[i + 2] & 0x3F);
            i += 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < len) {
            cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(in[i + 1] & 0x3F) << 12) |
                 ((uint32_t)(in[i + 2] & 0x3F) << 6) | (in[i + 3] & 0x3F);
            i += 4;
        } else {
            cp = 0xFFFD;
            i += 1;
        }
        if (cp > 0x10FFFF)
            cp = 0xFFFD;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = (uint16_t)cp;
        }
    }
    *out_len = n;
    return out;
}

static void print_utf16(FILE *f, const uint16_t *units, size_t len);

/* print(...args): host native, space-separated, trailing newline. */
static bool native_print(JsContext *ctx, JsValue this_val, const JsValue *args,
                         int argc, JsValue *result) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        if (i)
            fputc(' ', stdout);
        JsValue s = js_to_string(ctx, args[i]);
        size_t len;
        const uint16_t *u = js_string_units(s, &len);
        if (u)
            print_utf16(stdout, u, len);
    }
    fputc('\n', stdout);
    *result = js_undefined();
    return true;
}

static void print_utf16(FILE *f, const uint16_t *units, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            fputc((int)cp, f);
        } else if (cp < 0x800) {
            fputc(0xC0 | (int)(cp >> 6), f);
            fputc(0x80 | (int)(cp & 0x3F), f);
        } else if (cp < 0x10000) {
            fputc(0xE0 | (int)(cp >> 12), f);
            fputc(0x80 | (int)((cp >> 6) & 0x3F), f);
            fputc(0x80 | (int)(cp & 0x3F), f);
        } else {
            fputc(0xF0 | (int)(cp >> 18), f);
            fputc(0x80 | (int)((cp >> 12) & 0x3F), f);
            fputc(0x80 | (int)((cp >> 6) & 0x3F), f);
            fputc(0x80 | (int)(cp & 0x3F), f);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: jsvm <file.js>\n");
        return 2;
    }
    size_t byte_len;
    uint8_t *bytes = read_file(argv[1], &byte_len);
    if (!bytes) {
        fprintf(stderr, "jsvm: cannot read %s\n", argv[1]);
        return 2;
    }
    size_t len;
    uint16_t *src = utf8_to_utf16(bytes, byte_len, &len);
    free(bytes);
    if (!src) {
        fprintf(stderr, "jsvm: out of memory\n");
        return 2;
    }

    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    if (!vm || !ctx) {
        fprintf(stderr, "jsvm: out of memory\n");
        return 2;
    }

    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    js_register_native(ctx, print_name, 5, native_print, NULL);

    int status = 0;
    const char *err_msg;
    uint32_t err_pos;
    JsValue fn = js_compile_module(ctx, src, len, &err_msg, &err_pos);
    if (!js_is_function(fn)) {
        uint32_t line, col;
        js_source_line_col(src, len, err_pos, &line, &col);
        fprintf(stderr, "%s:%u:%u: SyntaxError: %s\n", argv[1], line, col, err_msg);
        status = 1;
    } else {
        js_gc_protect(vm, &fn);
        JsValue result;
        bool ok = js_run_module(ctx, fn, &result);
        js_gc_protect(vm, &result);
        JsValue str = js_to_string(ctx, result);
        size_t slen;
        const uint16_t *sunits = js_string_units(str, &slen);
        if (ok) {
            if (sunits)
                print_utf16(stdout, sunits, slen);
            fputc('\n', stdout);
        } else {
            uint32_t line, col;
            js_source_line_col(src, len, js_context_error_pos(ctx), &line, &col);
            fprintf(stderr, "%s:%u:%u: uncaught ", argv[1], line, col);
            if (sunits)
                print_utf16(stderr, sunits, slen);
            fputc('\n', stderr);
            status = 1;
        }
    }

    js_vm_free(vm);
    free(src);
    return status;
}
