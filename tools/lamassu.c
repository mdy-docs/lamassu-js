/*
 * lamassu — compile and run a JavaScript (lamassu subset) file.
 *
 * Host-side tool: file I/O and UTF-8 <-> UTF-16 conversion live here, never
 * in the core. Prints the module's completion value (REPL-style), or a
 * diagnostic with line:col on error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js_syntax.h" /* js_source_line_col */
#include "lamassu.h"

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

/*
 * UTF-8 -> UTF-16. Well-formed only: an overlong encoding, a surrogate
 * codepoint (U+D800..U+DFFF), a value above U+10FFFF, a truncated sequence, or
 * a bad continuation byte each becomes a single U+FFFD, and decoding resyncs at
 * the next byte. This rejects the malformed forms a naive decoder would silently
 * accept (e.g. 0xC0 0x80 for NUL, or CESU-8 surrogate halves).
 */
static uint16_t *utf8_to_utf16(const uint8_t *in, size_t len, size_t *out_len) {
    uint16_t *out = malloc((len + 1) * sizeof(uint16_t)); /* <=1 unit per byte */
    if (!out)
        return NULL;
    size_t n = 0, i = 0;
    while (i < len) {
        uint8_t b = in[i];
        uint32_t cp;
        int seqlen;
        uint32_t min; /* smallest value not overlong for this length */
        if (b < 0x80) {
            out[n++] = b; /* ASCII fast path */
            i++;
            continue;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F; seqlen = 2; min = 0x80;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F; seqlen = 3; min = 0x800;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07; seqlen = 4; min = 0x10000;
        } else {
            out[n++] = 0xFFFD; i++; continue; /* stray continuation / 0xF8+ */
        }
        bool ok = i + (size_t)seqlen <= len;
        for (int k = 1; ok && k < seqlen; k++) {
            uint8_t c = in[i + (size_t)k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out[n++] = 0xFFFD; i++; continue; /* malformed: emit U+FFFD, resync */
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

/* ---- module loader (file-based, relative to the root file's dir) ---- */

static char g_root_dir[1024];
/* the entry file's pre-read source, served for the '<root>' specifier */
static const uint16_t *g_root_src;
static size_t g_root_src_len;

static JsValue cli_fulfilled(JsContext *ctx, JsVm *vm, JsValue v) {
    js_gc_protect(vm, &v);
    JsValue p = js_promise_new(ctx);
    js_resolve(ctx, p, v);
    js_gc_unprotect(vm, &v);
    return p;
}

static JsValue cli_rejected(JsContext *ctx, JsVm *vm, const char *msg) {
    size_t n = strlen(msg);
    uint16_t u[128];
    if (n > 127)
        n = 127;
    for (size_t i = 0; i < n; i++)
        u[i] = (uint16_t)(unsigned char)msg[i];
    JsValue reason = js_atom(vm, u, n);
    js_gc_protect(vm, &reason);
    JsValue p = js_promise_new(ctx);
    js_reject(ctx, p, reason);
    js_gc_unprotect(vm, &reason);
    return p;
}

static JsValue file_loader(void *ud, JsContext *ctx, const uint16_t *spec, size_t spec_len,
                           const uint16_t *ref, size_t ref_len) {
    JsVm *vm = ud;
    (void)ref;
    (void)ref_len;
    static const uint16_t rootspec[] = {'<', 'r', 'o', 'o', 't', '>'};
    if (g_root_src && spec_len == 6 && memcmp(spec, rootspec, sizeof rootspec) == 0)
        return cli_fulfilled(ctx, vm, js_atom(vm, g_root_src, g_root_src_len));
    /* build path = root_dir/spec, appending .js if no extension */
    char name[512];
    size_t n = spec_len < 500 ? spec_len : 500;
    for (size_t i = 0; i < n; i++)
        name[i] = (char)(spec[i] < 128 ? spec[i] : '_');
    name[n] = 0;
    /* strip a leading ./ */
    char *rel = name;
    if (rel[0] == '.' && rel[1] == '/')
        rel += 2;
    char path[2048];
    /* Append ".js" only when the specifier doesn't already end in it — a proper
     * suffix test, not strstr (which would also match "a.jsx" or "x.js.bak"). */
    size_t rl = strlen(rel);
    bool has_ext = rl >= 3 && strcmp(rel + rl - 3, ".js") == 0;
    snprintf(path, sizeof path, "%s/%s%s", g_root_dir, rel, has_ext ? "" : ".js");

    size_t bl;
    uint8_t *bytes = read_file(path, &bl);
    if (!bytes)
        return cli_rejected(ctx, vm, "module not found");
    size_t slen;
    uint16_t *src = utf8_to_utf16(bytes, bl, &slen);
    free(bytes);
    if (!src)
        return cli_rejected(ctx, vm, "out of memory");
    JsValue v = js_atom(vm, src, slen);
    free(src);
    return cli_fulfilled(ctx, vm, v);
}

static void set_root_dir(const char *file) {
    snprintf(g_root_dir, sizeof g_root_dir, "%s", file);
    char *slash = strrchr(g_root_dir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(g_root_dir, sizeof g_root_dir, ".");
}

/* Interactive REPL over one persistent context (top-level let/const/function
 * carry across lines through the lexical scope). */
static int run_repl(void) {
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    if (!vm || !ctx) {
        fprintf(stderr, "lamassu: out of memory\n");
        return 2;
    }
    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    js_register_native(ctx, print_name, 5, native_print, NULL);

    fprintf(stderr, "lamassu REPL — Ctrl-D to exit\n");
    char line[8192];
    for (;;) {
        fputs("> ", stderr);
        fflush(stderr);
        if (!fgets(line, sizeof line, stdin))
            break;
        size_t bl = strlen(line);
        if (bl == 0)
            continue;
        size_t len;
        uint16_t *src = utf8_to_utf16((const uint8_t *)line, bl, &len);
        if (!src)
            continue;
        const char *err_msg;
        uint32_t err_pos;
        JsValue fn = js_compile_module_repl(ctx, src, len, &err_msg, &err_pos);
        if (!js_is_function(fn)) {
            fprintf(stderr, "SyntaxError: %s\n", err_msg ? err_msg : "compile error");
        } else {
            js_gc_protect(vm, &fn);
            JsValue p = js_run_module(ctx, fn);
            int st = js_promise_state(p);
            bool ok = st == 0 || st == 1;
            JsValue result = js_promise_result(p);
            js_gc_protect(vm, &result);
            JsValue str = js_to_string(ctx, result);
            size_t sl;
            const uint16_t *su = js_string_units(str, &sl);
            if (!ok)
                fputs("Uncaught ", stdout);
            if (su)
                print_utf16(stdout, su, sl);
            fputc('\n', stdout);
            fflush(stdout);
            js_gc_unprotect(vm, &result);
            js_gc_unprotect(vm, &fn);
        }
        free(src);
    }
    js_vm_free(vm);
    return 0;
}

/* Writes `len` bytes to `path`; returns 0 on success, else an errno-ish 2. */
static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return 2;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : 2;
}

/* Module identity for the CLI: a specifier's base name without directory or a
 * trailing .js/.jsbc extension (so "./util.js" and "util" both map to "util"),
 * written as UTF-16 into `out` (cap units); returns the length. */
static size_t spec_base(const uint16_t *spec, size_t n, uint16_t *out, size_t cap) {
    size_t start = 0, end = n;
    for (size_t i = 0; i < n; i++)
        if (spec[i] == '/')
            start = i + 1;
    /* strip a trailing .js or .jsbc */
    static const char *exts[] = {".jsbc", ".js"};
    for (int e = 0; e < 2; e++) {
        size_t el = strlen(exts[e]);
        if (end - start > el) {
            bool match = true;
            for (size_t i = 0; i < el; i++)
                if (spec[end - el + i] != (uint16_t)exts[e][i])
                    match = false;
            if (match) {
                end -= el;
                break;
            }
        }
    }
    size_t m = 0;
    for (size_t i = start; i < end && m < cap; i++)
        out[m++] = spec[i];
    return m;
}

/* ---- bytecode module loader (reads sibling <base>.jsbc from root dir) ---- */

/* canonical identity for bytecode modules is the specifier's base name */
static uint16_t g_bc_canon[256];
static bool bc_canonicalize(void *ud, const uint16_t *spec, size_t spec_len,
                            const uint16_t *ref, size_t ref_len,
                            const uint16_t **out, size_t *out_len) {
    (void)ud;
    (void)ref;
    (void)ref_len;
    *out_len = spec_base(spec, spec_len, g_bc_canon, 256);
    *out = g_bc_canon;
    return true;
}

static JsValue bc_file_loader(void *ud, JsContext *ctx, const uint16_t *spec, size_t spec_len,
                              const uint16_t *ref, size_t ref_len) {
    JsVm *vm = ud;
    (void)ref;
    (void)ref_len;
    char path[2048];
    char name[512];
    size_t nn = spec_len < 500 ? spec_len : 500;
    for (size_t i = 0; i < nn; i++)
        name[i] = (char)(spec[i] < 128 ? spec[i] : '_');
    name[nn] = 0;
    snprintf(path, sizeof path, "%s/%s.jsbc", g_root_dir, name);

    size_t bl;
    uint8_t *bytes = read_file(path, &bl);
    if (!bytes)
        return cli_rejected(ctx, vm, "module bytecode not found");
    JsValue v = js_bytecode_value(ctx, bytes, bl);
    free(bytes);
    return cli_fulfilled(ctx, vm, v);
}

/*
 * lamassu --emit-bytecode SRC OUT: compile SRC (a non-module script) and write a
 * validated-on-load bytecode cache to OUT.
 */
static int emit_bytecode(const char *src_path, const char *out_path) {
    size_t byte_len;
    uint8_t *bytes = read_file(src_path, &byte_len);
    if (!bytes) {
        fprintf(stderr, "lamassu: cannot read %s\n", src_path);
        return 2;
    }
    size_t len;
    uint16_t *src = utf8_to_utf16(bytes, byte_len, &len);
    free(bytes);
    if (!src) {
        fprintf(stderr, "lamassu: out of memory\n");
        return 2;
    }
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    if (!vm || !ctx) {
        fprintf(stderr, "lamassu: out of memory\n");
        return 2;
    }
    const char *err_msg;
    uint32_t err_pos;
    JsValue fn = js_compile_module(ctx, src, len, &err_msg, &err_pos);
    int status = 0;
    bool is_module = !js_is_function(fn) && err_msg &&
                     strcmp(err_msg, JS_ERR_NEEDS_MODULE_LOADER) == 0;
    uint8_t *out = NULL;
    size_t out_len = 0;
    bool have = false;

    if (is_module) {
        /* compile as a standalone module (body + link metadata, no deps).
         * The module identity is the *file's* base name, so derive it from
         * src_path — not from the source text (spec_base wants a path). */
        uint16_t pathu[512];
        size_t pn = 0;
        for (const char *pc = src_path; *pc && pn < 512; pc++)
            pathu[pn++] = (uint16_t)(unsigned char)*pc;
        uint16_t spec[256];
        size_t sn = spec_base(pathu, pn, spec, 256);
        have = js_bytecode_compile_module(ctx, spec, sn, src, len, &out, &out_len,
                                          &err_msg, &err_pos);
        if (!have) {
            uint32_t line, col;
            js_source_line_col(src, len, err_pos, &line, &col);
            fprintf(stderr, "%s:%u:%u: %s\n", src_path, line, col,
                    err_msg ? err_msg : "module compile error");
            status = 1;
        }
    } else if (!js_is_function(fn)) {
        uint32_t line, col;
        js_source_line_col(src, len, err_pos, &line, &col);
        fprintf(stderr, "%s:%u:%u: SyntaxError: %s\n", src_path, line, col, err_msg);
        status = 1;
    } else {
        js_gc_protect(vm, &fn);
        have = js_bytecode_serialize(ctx, fn, &out, &out_len);
        if (!have) {
            fprintf(stderr, "lamassu: bytecode serialization failed\n");
            status = 2;
        }
        js_gc_unprotect(vm, &fn);
    }

    if (have) {
        status = write_file(out_path, out, out_len);
        if (status)
            fprintf(stderr, "lamassu: cannot write %s\n", out_path);
        js_bytecode_free(ctx, out, out_len);
    }
    js_vm_free(vm);
    free(src);
    return status;
}

/* lamassu --run-bytecode FILE.jsbc: load, validate, and run a bytecode cache. */
static int run_bytecode(const char *path) {
    size_t len;
    uint8_t *bytes = read_file(path, &len);
    if (!bytes) {
        fprintf(stderr, "lamassu: cannot read %s\n", path);
        return 2;
    }
    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    if (!vm || !ctx) {
        fprintf(stderr, "lamassu: out of memory\n");
        free(bytes);
        return 2;
    }
    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    js_register_native(ctx, print_name, 5, native_print, NULL);

    int status = 0;
    int kind = js_bytecode_kind(bytes, len);

    if (kind == 1 /* JS_BC_MODULE */) {
        /* Module: the graph (root included) loads from sibling <base>.jsbc
         * files; the canonicalizer maps any specifier to its base name. */
        set_root_dir(path);
        js_set_module_loader(ctx, bc_file_loader, bc_canonicalize, vm);
        uint16_t spec[256];
        /* build a UTF-16 view of the path, then take its base name as identity */
        uint16_t pathu[2048];
        size_t pn = 0;
        for (const char *p = path; *p && pn < 2048; p++)
            pathu[pn++] = (uint16_t)(unsigned char)*p;
        size_t sn = spec_base(pathu, pn, spec, 256);
        free(bytes); /* the loader re-reads the root from disk */
        JsValue pr = js_eval_module(ctx, spec, sn);
        js_gc_protect(vm, &pr);
        if (js_promise_state(pr) != 1) {
            JsValue reason = js_promise_result(pr);
            fprintf(stderr, "%s: ", path);
            JsValue s = js_to_string(ctx, reason);
            size_t sl;
            const uint16_t *su = js_string_units(s, &sl);
            if (su)
                print_utf16(stderr, su, sl);
            fputc('\n', stderr);
            js_gc_unprotect(vm, &pr);
            status = 1;
        } else {
            JsValue ns = js_promise_result(pr);
            js_gc_unprotect(vm, &pr);
            /* a page template can `export default` its rendered string */
            static const uint16_t dflt[] = {'d','e','f','a','u','l','t'};
            JsValue def = js_module_get_export(ctx, ns, dflt, 7);
            if (!js_is_undefined(def)) {
                js_gc_protect(vm, &def);
                JsValue s = js_to_string(ctx, def);
                size_t sl;
                const uint16_t *su = js_string_units(s, &sl);
                if (su)
                    print_utf16(stdout, su, sl);
                fputc('\n', stdout);
                js_gc_unprotect(vm, &def);
            }
        }
        js_vm_free(vm);
        return status;
    }

    const char *err_msg;
    JsValue fn = js_bytecode_load(ctx, bytes, len, &err_msg);
    free(bytes);
    if (!js_is_function(fn)) {
        fprintf(stderr, "%s: %s\n", path, err_msg ? err_msg : "invalid bytecode");
        status = 1;
    } else {
        js_gc_protect(vm, &fn);
        JsValue p = js_run_module(ctx, fn);
        int st = js_promise_state(p);
        bool ok = st == 0 || st == 1;
        JsValue result = js_promise_result(p);
        js_gc_protect(vm, &result);
        JsValue str = js_to_string(ctx, result);
        size_t slen;
        const uint16_t *su = js_string_units(str, &slen);
        if (ok) {
            if (su)
                print_utf16(stdout, su, slen);
            fputc('\n', stdout);
        } else {
            fprintf(stderr, "%s: uncaught ", path);
            if (su)
                print_utf16(stderr, su, slen);
            fputc('\n', stderr);
            status = 1;
        }
    }
    js_vm_free(vm);
    return status;
}

int main(int argc, char **argv) {
    if (argc == 1)
        return run_repl(); /* no file: interactive REPL */
    if (argc == 4 && strcmp(argv[1], "--emit-bytecode") == 0)
        return emit_bytecode(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "--run-bytecode") == 0)
        return run_bytecode(argv[2]);
    if (argc != 2) {
        fprintf(stderr,
                "usage: lamassu [file.js]                 run a source file (or REPL if omitted)\n"
                "       lamassu --emit-bytecode SRC OUT   compile SRC to a bytecode cache OUT\n"
                "       lamassu --run-bytecode FILE       run a bytecode cache\n");
        return 2;
    }
    size_t byte_len;
    uint8_t *bytes = read_file(argv[1], &byte_len);
    if (!bytes) {
        fprintf(stderr, "lamassu: cannot read %s\n", argv[1]);
        return 2;
    }
    size_t len;
    uint16_t *src = utf8_to_utf16(bytes, byte_len, &len);
    free(bytes);
    if (!src) {
        fprintf(stderr, "lamassu: out of memory\n");
        return 2;
    }

    JsVm *vm = js_vm_new(NULL);
    JsContext *ctx = js_context_new(vm);
    if (!vm || !ctx) {
        fprintf(stderr, "lamassu: out of memory\n");
        return 2;
    }

    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    js_register_native(ctx, print_name, 5, native_print, NULL);
    set_root_dir(argv[1]);
    js_set_module_loader(ctx, file_loader, NULL, vm);

    int status = 0;
    const char *err_msg;
    uint32_t err_pos;
    JsValue fn = js_compile_module(ctx, src, len, &err_msg, &err_pos);
    bool is_module = !js_is_function(fn) && err_msg &&
                     strcmp(err_msg, JS_ERR_NEEDS_MODULE_LOADER) == 0;

    if (is_module) {
        /* the file uses import/export: run it through the module pipeline
         * (the loader serves this pre-read source for '<root>') */
        g_root_src = src;
        g_root_src_len = len;
        static const uint16_t root_spec[] = {'<', 'r', 'o', 'o', 't', '>'};
        JsValue p = js_eval_module(ctx, root_spec, 6);
        js_gc_protect(vm, &p);
        if (js_promise_state(p) == 2) {
            JsValue str = js_to_string(ctx, js_promise_result(p));
            size_t slen;
            const uint16_t *su = js_string_units(str, &slen);
            fprintf(stderr, "%s: uncaught ", argv[1]);
            if (su)
                print_utf16(stderr, su, slen);
            fputc('\n', stderr);
            status = 1;
        }
        js_gc_unprotect(vm, &p);
        g_root_src = NULL;
    } else if (!js_is_function(fn)) {
        uint32_t line, col;
        js_source_line_col(src, len, err_pos, &line, &col);
        fprintf(stderr, "%s:%u:%u: SyntaxError: %s\n", argv[1], line, col, err_msg);
        status = 1;
    } else {
        js_gc_protect(vm, &fn);
        JsValue p = js_run_module(ctx, fn);
        int st = js_promise_state(p);
        bool ok = st == 0 || st == 1;
        JsValue result = js_promise_result(p);
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
