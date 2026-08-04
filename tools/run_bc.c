/*
 * lamassu-runtime-only — run a precompiled .jsbc file, with no compiler.
 *
 * This includes <lamassu.h> and NOT <lamassu_compile.h>, and `make
 * check-runtime-only` links it against liblamassu_runtime.a alone. That link
 * is the guard on the frontend/runtime split: if a runtime source ever calls
 * the lexer, parser, or compiler again, this fails with an undefined symbol
 * rather than quietly re-coupling the two halves.
 *
 * It is also, deliberately, the shape of a fleet binary: bytecode in, output
 * out, no parser in the process at all. A real one would take its bytecode
 * from the host rather than from argv, and would set a fuel budget and a heap
 * limit through JsVmConfig — but the link surface is exactly this.
 */
#include <stdio.h>
#include <stdlib.h>

#include "lamassu.h"

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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: lamassu-runtime-only FILE.jsbc\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return 2;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);

    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = vm ? js_context_new(vm) : NULL;
    if (!ctx) {
        free(buf);
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    js_register_native(ctx, print_name, 5, native_print, NULL);

    const char *err = NULL;
    JsValue fn = js_bytecode_load(ctx, buf, (size_t)sz, &err);
    free(buf);
    int status = 0;
    if (!js_is_function(fn)) {
        fprintf(stderr, "%s: %s\n", argv[1], err ? err : "invalid bytecode");
        status = 1;
    } else {
        js_gc_protect(vm, &fn);
        JsValue p = js_run_module(ctx, fn);
        int st = js_promise_state(p);
        JsValue result = js_promise_result(p);
        js_gc_protect(vm, &result);
        JsValue str = js_to_string(ctx, result);
        size_t slen;
        const uint16_t *su = js_string_units(str, &slen);
        if (st == 2) {
            fprintf(stderr, "%s: uncaught ", argv[1]);
            if (su)
                print_utf16(stderr, su, slen);
            fputc('\n', stderr);
            status = 1;
        } else {
            if (su)
                print_utf16(stdout, su, slen);
            fputc('\n', stdout);
        }
        js_gc_unprotect(vm, &result);
        js_gc_unprotect(vm, &fn);
    }
    js_vm_free(vm);
    return status;
}
