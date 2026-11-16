/*
 * lamassu — compile and run a JavaScript (lamassu subset) file.
 *
 * Host-side tool: file I/O and UTF-8 <-> UTF-16 conversion live here, never
 * in the core. Prints the module's completion value (REPL-style), or a
 * diagnostic with line:col on error.
 *
 * A file that imports or exports is a module, named by its path, and what it
 * imports are files: see the module loader below. Its completion value is not
 * printed (a module has none to give).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js_syntax.h" /* js_source_line_col */
#include "lamassu.h"
#include "lamassu_compile.h"

static uint8_t *read_open(FILE *f, size_t *len_out) {
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

static uint8_t *read_file(const char *path, size_t *len_out) {
    return read_open(fopen(path, "rb"), len_out);
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

/* ---- module loader: a module is a file, and its specifier that file's path ---- */

/*
 * A module's identity (its canonical specifier) is the full path of its file: the entry's from
 * the command line and the directory the run starts in, an imported module's from the import
 * and from the module that asked for it. So a file is one module however it is come to -- the
 * entry imported back by a module it imports, "./a.js" and "./sub/../a.js" -- and "./x.js" in
 * sub/m.js is sub/x.js.
 *
 *   ./x  ../x   beside the module that imports it; beside the entry, for an import() from an
 *               entry that is run without the loader (see main)
 *   /x          as written
 *   x           beside the entry: there are no packages to look in
 *
 * "." and ".." are taken out of the path by name, and links are not followed (d8 does the
 * same; Node follows them). A file name with no extension is given ".js".
 */
#define PATH_CAP 2048
static uint16_t g_entry[PATH_CAP]; /* the entry's canonical specifier */
static size_t g_entry_len;
/* the entry's source, read before the run: what the loader answers for g_entry with */
static const uint16_t *g_entry_src;
static size_t g_entry_src_len;

/*
 * Where a path meets the system: the directory the run starts in, the entry's name as the
 * command line has it, and opening a file. Each fills OUT (CAP units) and returns the length,
 * 0 if it cannot. Windows names its files in UTF-16, as specifiers are, and separates with
 * either slash; a path here has "/" only.
 */
#ifdef _WIN32
#include <windows.h>

static size_t path_slashes(uint16_t *out, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (out[i] == '\\')
            out[i] = '/';
    return n;
}

static size_t cwd_path(uint16_t *out, size_t cap) {
    wchar_t w[PATH_CAP];
    DWORD n = GetCurrentDirectoryW(PATH_CAP, w);
    if (n == 0 || n >= PATH_CAP || n > cap)
        return 0;
    for (DWORD i = 0; i < n; i++)
        out[i] = (uint16_t)w[i];
    return path_slashes(out, n);
}

static size_t arg_path(const char *arg, uint16_t *out, size_t cap) {
    wchar_t w[PATH_CAP];
    int n = MultiByteToWideChar(CP_ACP, 0, arg, -1, w, PATH_CAP);
    if (n <= 1 || (size_t)(n - 1) > cap)
        return 0;
    for (int i = 0; i < n - 1; i++)
        out[i] = (uint16_t)w[i];
    return path_slashes(out, (size_t)(n - 1));
}

static FILE *open_path(const uint16_t *path, size_t n) {
    wchar_t w[PATH_CAP + 4];
    if (n > PATH_CAP)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        if (path[i] == 0)
            return NULL;
        w[i] = (wchar_t)path[i];
    }
    w[n] = 0;
    return _wfopen(w, L"rb");
}

/* The length of the root PATH starts at: "/", "//" (a share), "C:/", "C:"; 0 if it is relative. */
static size_t path_root(const uint16_t *path, size_t n) {
    if (n && path[0] == '/')
        return n > 1 && path[1] == '/' ? 2 : 1;
    if (n > 1 && path[1] == ':' && ((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z'))
        return n > 2 && path[2] == '/' ? 3 : 2;
    return 0;
}
#else
#include <unistd.h>

static size_t utf8_path(const char *bytes, uint16_t *out, size_t cap) {
    size_t n;
    uint16_t *u = utf8_to_utf16((const uint8_t *)bytes, strlen(bytes), &n);
    if (!u || n > cap)
        n = 0;
    if (n)
        memcpy(out, u, n * sizeof *u);
    free(u);
    return n;
}

static size_t cwd_path(uint16_t *out, size_t cap) {
    char buf[4 * PATH_CAP];
    return getcwd(buf, sizeof buf) ? utf8_path(buf, out, cap) : 0;
}

static size_t arg_path(const char *arg, uint16_t *out, size_t cap) {
    return utf8_path(arg, out, cap);
}

/* The path as UTF-8. No file is named with a NUL or with half of a surrogate pair. */
static FILE *open_path(const uint16_t *path, size_t n) {
    char out[4 * PATH_CAP + 1];
    size_t m = 0;
    if (n > PATH_CAP)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = path[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && path[i + 1] >= 0xDC00 &&
            path[i + 1] <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (path[++i] - 0xDC00);
        else if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF))
            return NULL;
        if (cp < 0x80) {
            out[m++] = (char)cp;
        } else if (cp < 0x800) {
            out[m++] = (char)(0xC0 | (cp >> 6));
            out[m++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[m++] = (char)(0xE0 | (cp >> 12));
            out[m++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[m++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[m++] = (char)(0xF0 | (cp >> 18));
            out[m++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[m++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[m++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[m] = 0;
    return fopen(out, "rb");
}

/* The length of the root PATH starts at, "/"; 0 if it is relative. */
static size_t path_root(const uint16_t *path, size_t n) {
    return n && path[0] == '/' ? 1 : 0;
}
#endif

/* IN without its "." and empty segments, and without each ".." and the segment before it. False
 * if the result has no room in OUT (PATH_CAP units) or names nothing. */
static bool path_normalize(const uint16_t *in, size_t n, uint16_t *out, size_t *out_len) {
    size_t root = path_root(in, n);
    size_t m = root;
    size_t fixed = root; /* out[0, fixed): the root or a leading "../..", which no ".." takes away */
    memcpy(out, in, root * sizeof *in);
    for (size_t i = root; i < n;) {
        size_t start = i;
        while (i < n && in[i] != '/')
            i++;
        size_t len = i - start;
        if (i < n)
            i++;
        if (len == 0 || (len == 1 && in[start] == '.'))
            continue;
        bool up = len == 2 && in[start] == '.' && in[start + 1] == '.';
        if (up && m > fixed) {
            while (m > fixed && out[m - 1] != '/')
                m--;
            if (m > fixed)
                m--;
            continue;
        }
        if (up && root)
            continue; /* above the root is the root */
        if (m + len + 1 > PATH_CAP)
            return false;
        if (m > root && out[m - 1] != '/')
            out[m++] = '/';
        memcpy(out + m, in + start, len * sizeof *in);
        m += len;
        if (up)
            fixed = m;
    }
    *out_len = m;
    return m > root;
}

static bool file_canonicalize(void *ud, const uint16_t *spec, size_t n, const uint16_t *ref,
                              size_t ref_len, const uint16_t **out, size_t *out_len) {
    (void)ud;
    static uint16_t joined[2 * PATH_CAP], canon[PATH_CAP + 3];
    if (n > PATH_CAP)
        return false;
    size_t j = 0;
    if (!path_root(spec, n)) {
        bool relative = n && spec[0] == '.' &&
                        (n == 1 || spec[1] == '/' || (spec[1] == '.' && (n == 2 || spec[2] == '/')));
        const uint16_t *from = relative && ref_len ? ref : g_entry;
        size_t from_len = relative && ref_len ? ref_len : g_entry_len;
        for (size_t i = 0; i < from_len; i++)
            if (from[i] == '/')
                j = i + 1; /* its directory, with the slash */
        if (j > PATH_CAP)
            return false;
        memcpy(joined, from, j * sizeof *from);
    }
    memcpy(joined + j, spec, n * sizeof *spec);
    size_t m;
    if (!path_normalize(joined, j + n, canon, &m))
        return false;
    /* ".js" for a name without an extension, unless it is the entry, which is the file named on
     * the command line whatever it is called */
    bool entry = m == g_entry_len && memcmp(canon, g_entry, m * sizeof *canon) == 0;
    size_t base = m;
    while (base && canon[base - 1] != '/')
        base--;
    size_t dot = m;
    for (size_t i = base; i < m; i++)
        if (canon[i] == '.')
            dot = i;
    bool has_ext = dot > base && dot + 1 < m;
    if (!entry && !has_ext) {
        if (m + 3 > PATH_CAP)
            return false;
        canon[m++] = '.';
        canon[m++] = 'j';
        canon[m++] = 's';
    }
    *out = canon;
    *out_len = m;
    return true;
}

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

/* SPEC is canonical: the path of the file. */
static JsValue file_loader(void *ud, JsContext *ctx, const uint16_t *spec, size_t spec_len,
                           const uint16_t *ref, size_t ref_len) {
    JsVm *vm = ud;
    (void)ref;
    (void)ref_len;
    if (g_entry_src && spec_len == g_entry_len &&
        memcmp(spec, g_entry, spec_len * sizeof *spec) == 0)
        return cli_fulfilled(ctx, vm, js_atom(vm, g_entry_src, g_entry_src_len));
    size_t bl;
    uint8_t *bytes = read_open(open_path(spec, spec_len), &bl);
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

/* The entry's canonical specifier: the path on the command line, from the directory the run
 * starts in. (Where that directory cannot be had, the path as it was given.) */
static bool set_entry(const char *file) {
    static uint16_t joined[2 * PATH_CAP + 1];
    uint16_t arg[PATH_CAP];
    size_t n = arg_path(file, arg, PATH_CAP);
    if (!n)
        return false;
    size_t j = 0;
    if (!path_root(arg, n)) {
        j = cwd_path(joined, PATH_CAP);
        if (j)
            joined[j++] = '/';
    }
    memcpy(joined + j, arg, n * sizeof *arg);
    return path_normalize(joined, j + n, g_entry, &g_entry_len);
}

/* the directory the bytecode loader reads from: the root file's */
static char g_root_dir[1024];

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
/*
 * Sandbox limits, off unless asked for. A developer running their own script
 * wants neither, but the moment this is pointed at a file someone else wrote
 * they are the difference between a failed run and a wedged terminal.
 */
static uint64_t g_fuel;
static size_t g_heap_limit;

static JsVmConfig limit_config(void) {
    JsVmConfig cfg = {0};
    cfg.heap_limit = g_heap_limit;
    return cfg;
}

/*
 * Arms the instruction budget for one turn -- a top-level run plus the
 * microtask drain it feeds. Must be called before each evaluation, not once
 * per context, or a REPL session shares a single budget across every line and
 * the second line inherits whatever the first left of it.
 */
static void arm_fuel(JsContext *ctx) {
    if (g_fuel)
        js_context_set_fuel(ctx, g_fuel);
}

static int run_repl(void) {
    JsVmConfig cfg = limit_config();
    JsVm *vm = js_vm_new(&cfg);
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
        arm_fuel(ctx); /* re-armed per line: the budget covers one turn */
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
    JsVmConfig cfg = limit_config();
    JsVm *vm = js_vm_new(&cfg);
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
    JsVmConfig cfg = limit_config();
    JsVm *vm = js_vm_new(&cfg);
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
        arm_fuel(ctx);
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

static void usage(FILE *out) {
    fprintf(out,
        "usage: lamassu [options] [file.js]             run a source file, or the REPL\n"
        "       lamassu [options] --run-bytecode FILE   run a bytecode cache\n"
        "       lamassu --emit-bytecode SRC OUT        compile SRC to a bytecode cache\n"
        "\n"
        "options:\n"
        "  --fuel N          bytecode-instruction budget per evaluation (0 = unlimited).\n"
        "                    Roughly 3e8 per second. Exceeding it throws an error the\n"
        "                    script cannot catch, and the run unwinds.\n"
        "  --heap-limit N    cap on the guest heap. Accepts a K/M/G suffix (e.g. 64M).\n"
        "  -h, --help        this message\n"
        "\n"
        "Both limits are off by default, which is what you want for your own code and\n"
        "not what you want for anyone else's. Neither bounds wall-clock on its own: a\n"
        "single instruction can enter a builtin that runs a while, so if the input is\n"
        "untrusted, also run this where you can kill it. See docs/security.md.\n");
}

/* Parses a byte count with an optional K/M/G suffix. False on anything else,
 * including a bare negative, which strtoull would otherwise wrap into a
 * near-unlimited cap. */
static bool parse_size(const char *text, unsigned long long *out) {
    if (!text || !*text || *text == '-')
        return false;
    char *end;
    unsigned long long v = strtoull(text, &end, 10);
    if (end == text)
        return false;
    unsigned long long scale = 1;
    if (*end == 'k' || *end == 'K') scale = 1024ULL, end++;
    else if (*end == 'm' || *end == 'M') scale = 1024ULL * 1024, end++;
    else if (*end == 'g' || *end == 'G') scale = 1024ULL * 1024 * 1024, end++;
    if (*end)
        return false;
    if (v > (unsigned long long)-1 / scale)
        return false;
    *out = v * scale;
    return true;
}

int main(int argc, char **argv) {
    /* Options first, then the subcommand or file. Kept hand-rolled rather than
     * getopt so the wasip2 build has nothing extra to link. */
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(a, "--fuel") == 0 || strcmp(a, "--heap-limit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "lamassu: %s needs a value\n", a);
                return 2;
            }
            unsigned long long v;
            if (!parse_size(argv[i + 1], &v)) {
                fprintf(stderr, "lamassu: bad value for %s: %s\n", a, argv[i + 1]);
                return 2;
            }
            if (strcmp(a, "--fuel") == 0)
                g_fuel = (uint64_t)v;
            else
                g_heap_limit = (size_t)v;
            i++;
            continue;
        }
        if (strcmp(a, "--") == 0) { /* everything after is a path */
            i++;
            break;
        }
        break; /* not an option: the subcommand or the file */
    }

    int rest = argc - i;
    if (rest == 0)
        return run_repl(); /* no file: interactive REPL */
    if (rest == 3 && strcmp(argv[i], "--emit-bytecode") == 0)
        return emit_bytecode(argv[i + 1], argv[i + 2]);
    if (rest == 2 && strcmp(argv[i], "--run-bytecode") == 0)
        return run_bytecode(argv[i + 1]);
    if (rest != 1 || argv[i][0] == '-') {
        if (rest >= 1 && argv[i][0] == '-')
            fprintf(stderr, "lamassu: unknown option %s\n", argv[i]);
        usage(stderr);
        return 2;
    }
    argv += i - 1; /* argv[1] is the script path from here on */
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

    JsVmConfig cfg = limit_config();
    JsVm *vm = js_vm_new(&cfg);
    JsContext *ctx = js_context_new(vm);
    if (!vm || !ctx) {
        fprintf(stderr, "lamassu: out of memory\n");
        return 2;
    }

    static const uint16_t print_name[] = {'p', 'r', 'i', 'n', 't'};
    js_register_native(ctx, print_name, 5, native_print, NULL);
    if (!set_entry(argv[1])) {
        fprintf(stderr, "lamassu: cannot read %s\n", argv[1]);
        return 2;
    }
    js_set_module_loader(ctx, file_loader, file_canonicalize, vm);
    js_enable_source_modules(ctx);

    int status = 0;
    const char *err_msg;
    uint32_t err_pos;
    JsValue fn = js_compile_module(ctx, src, len, &err_msg, &err_pos);
    bool is_module = !js_is_function(fn) && err_msg &&
                     strcmp(err_msg, JS_ERR_NEEDS_MODULE_LOADER) == 0;

    if (is_module) {
        /* The file uses import/export: it is run through the module pipeline, as the module its
         * path names, so an import that comes back to it finds it there. The loader answers
         * with the source read above. The specifier goes through the canonicalizer as any does:
         * "./" and the file's name, beside the entry, is the entry. */
        g_entry_src = src;
        g_entry_src_len = len;
        static uint16_t root_spec[PATH_CAP + 2];
        size_t base = g_entry_len;
        while (base && g_entry[base - 1] != '/')
            base--;
        root_spec[0] = '.';
        root_spec[1] = '/';
        memcpy(root_spec + 2, g_entry + base, (g_entry_len - base) * sizeof *g_entry);
        arm_fuel(ctx);
        JsValue p = js_eval_module(ctx, root_spec, 2 + g_entry_len - base);
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
        g_entry_src = NULL;
    } else if (!js_is_function(fn)) {
        uint32_t line, col;
        js_source_line_col(src, len, err_pos, &line, &col);
        fprintf(stderr, "%s:%u:%u: SyntaxError: %s\n", argv[1], line, col, err_msg);
        status = 1;
    } else {
        js_gc_protect(vm, &fn);
        arm_fuel(ctx);
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
