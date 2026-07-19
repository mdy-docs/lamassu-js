#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js_syntax.h"

static int checks_run;
static int checks_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks_run++;                                                        \
        if (!(cond)) {                                                       \
            checks_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

typedef struct {
    long net_bytes;
    long live_allocs;
} CountAlloc;

static void *count_realloc(void *ud, void *ptr, size_t old_size, size_t new_size) {
    CountAlloc *ca = ud;
    if (new_size == 0) {
        if (ptr) {
            ca->net_bytes -= (long)old_size;
            ca->live_allocs--;
            free(ptr);
        }
        return NULL;
    }
    void *p = realloc(ptr, new_size);
    if (!p)
        return NULL;
    ca->net_bytes += (long)new_size - (long)old_size;
    if (!ptr)
        ca->live_allocs++;
    return p;
}

/* ---- string builder ---- */

typedef struct {
    char *buf;
    size_t len, cap;
} Sb;

static void sb_printf(Sb *sb, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(sb->buf ? sb->buf + sb->len : NULL,
                          sb->cap - sb->len, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (sb->len + (size_t)n < sb->cap) {
            sb->len += (size_t)n;
            return;
        }
        size_t ncap = sb->cap ? sb->cap * 2 : 256;
        while (ncap <= sb->len + (size_t)n)
            ncap *= 2;
        sb->buf = realloc(sb->buf, ncap);
        sb->cap = ncap;
    }
}

static void sb_units(Sb *sb, const uint16_t *u, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        sb_printf(sb, "%c", u[i] < 128 ? (char)u[i] : '?');
}

/* ---- AST dumper (s-expressions) ---- */

static const char *op_text(uint8_t op) {
    switch ((JsTokKind)op) {
    case JS_T_EQ: return "=";
    case JS_T_PLUS_EQ: return "+="; case JS_T_MINUS_EQ: return "-=";
    case JS_T_STAR_EQ: return "*="; case JS_T_SLASH_EQ: return "/=";
    case JS_T_PERCENT_EQ: return "%="; case JS_T_POW_EQ: return "**=";
    case JS_T_SHL_EQ: return "<<="; case JS_T_SHR_EQ: return ">>=";
    case JS_T_USHR_EQ: return ">>>="; case JS_T_AMP_EQ: return "&=";
    case JS_T_BAR_EQ: return "|="; case JS_T_CARET_EQ: return "^=";
    case JS_T_ANDAND_EQ: return "&&="; case JS_T_OROR_EQ: return "||=";
    case JS_T_QUESQUES_EQ: return "?\?=";
    case JS_T_EQEQ: return "=="; case JS_T_NE: return "!=";
    case JS_T_EQEQEQ: return "==="; case JS_T_STRICT_NE: return "!==";
    case JS_T_LT: return "<"; case JS_T_GT: return ">";
    case JS_T_LE: return "<="; case JS_T_GE: return ">=";
    case JS_T_SHL: return "<<"; case JS_T_SHR: return ">>";
    case JS_T_USHR: return ">>>";
    case JS_T_AMP: return "&"; case JS_T_BAR: return "|"; case JS_T_CARET: return "^";
    case JS_T_ANDAND: return "&&"; case JS_T_OROR: return "||";
    case JS_T_QUESQUES: return "??";
    case JS_T_PLUS: return "+"; case JS_T_MINUS: return "-";
    case JS_T_STAR: return "*"; case JS_T_SLASH: return "/";
    case JS_T_PERCENT: return "%"; case JS_T_POW: return "**";
    case JS_T_IN: return "in";
    case JS_T_NOT: return "!"; case JS_T_TILDE: return "~";
    case JS_T_TYPEOF: return "typeof"; case JS_T_VOID: return "void";
    case JS_T_DELETE: return "delete";
    case JS_T_INC: return "++"; case JS_T_DEC: return "--";
    default: return "?op?";
    }
}

static void dump_node(Sb *sb, const JsAstNode *n);

static void dump_child(Sb *sb, const JsAstNode *n) {
    sb_printf(sb, " ");
    if (n)
        dump_node(sb, n);
    else
        sb_printf(sb, "_");
}

static void dump_list(Sb *sb, JsAstNode *const *items, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        dump_child(sb, items[i]);
}

static void dump_func(Sb *sb, const JsAstNode *n) {
    if (n->flags & JS_F_ARROW)
        sb_printf(sb, "(%s", (n->flags & JS_F_ASYNC) ? "a=>" : "=>");
    else {
        sb_printf(sb, "(%s ", (n->flags & JS_F_ASYNC) ? "afn" : "fn");
        if (n->len)
            sb_units(sb, n->units, n->len);
        else
            sb_printf(sb, "_");
    }
    sb_printf(sb, " (");
    for (uint32_t i = 0; i < n->count; i++) {
        if (i)
            sb_printf(sb, " ");
        dump_node(sb, n->items[i]);
    }
    sb_printf(sb, ")");
    dump_child(sb, n->a);
    sb_printf(sb, ")");
}

static void dump_node(Sb *sb, const JsAstNode *n) {
    switch ((JsAstKind)n->kind) {
    case JS_AST_MODULE:
        sb_printf(sb, "(module");
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_BLOCK:
        sb_printf(sb, "(block");
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_EMPTY:
        sb_printf(sb, "(empty)");
        break;
    case JS_AST_EXPR_STMT:
        sb_printf(sb, "(expr");
        dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_LET_DECL:
        sb_printf(sb, "(%s", (n->flags & JS_F_CONST) ? "const" : "let");
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_DECLARATOR:
        sb_printf(sb, "(d");
        dump_child(sb, n->a);
        if (n->b)
            dump_child(sb, n->b);
        sb_printf(sb, ")");
        break;
    case JS_AST_IF:
        sb_printf(sb, "(if");
        dump_child(sb, n->a);
        dump_child(sb, n->b);
        if (n->c)
            dump_child(sb, n->c);
        sb_printf(sb, ")");
        break;
    case JS_AST_FOR:
        sb_printf(sb, "(for");
        dump_child(sb, n->a);
        dump_child(sb, n->b);
        dump_child(sb, n->c);
        dump_child(sb, n->d);
        sb_printf(sb, ")");
        break;
    case JS_AST_FOR_OF:
        sb_printf(sb, "(of %s",
                  (n->flags & JS_F_DECL)
                      ? ((n->flags & JS_F_CONST) ? "const" : "let")
                      : "assign");
        dump_child(sb, n->a);
        dump_child(sb, n->b);
        dump_child(sb, n->c);
        sb_printf(sb, ")");
        break;
    case JS_AST_WHILE:
        sb_printf(sb, "(while");
        dump_child(sb, n->a);
        dump_child(sb, n->b);
        sb_printf(sb, ")");
        break;
    case JS_AST_DO_WHILE:
        sb_printf(sb, "(do");
        dump_child(sb, n->a);
        dump_child(sb, n->b);
        sb_printf(sb, ")");
        break;
    case JS_AST_SWITCH:
        sb_printf(sb, "(switch");
        dump_child(sb, n->a);
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_SWITCH_CASE:
        if (n->a) {
            sb_printf(sb, "(case");
            dump_child(sb, n->a);
        } else {
            sb_printf(sb, "(default");
        }
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_BREAK:
    case JS_AST_CONTINUE:
        sb_printf(sb, "(%s", n->kind == JS_AST_BREAK ? "break" : "continue");
        if (n->len) {
            sb_printf(sb, " ");
            sb_units(sb, n->units, n->len);
        }
        sb_printf(sb, ")");
        break;
    case JS_AST_RETURN:
        sb_printf(sb, "(return");
        if (n->a)
            dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_THROW:
        sb_printf(sb, "(throw");
        dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_TRY:
        sb_printf(sb, "(try");
        dump_child(sb, n->a);
        if (n->c) {
            sb_printf(sb, " (catch");
            if (n->b)
                dump_child(sb, n->b);
            dump_child(sb, n->c);
            sb_printf(sb, ")");
        }
        if (n->d) {
            sb_printf(sb, " (finally");
            dump_child(sb, n->d);
            sb_printf(sb, ")");
        }
        sb_printf(sb, ")");
        break;
    case JS_AST_LABELED:
        sb_printf(sb, "(label ");
        sb_units(sb, n->units, n->len);
        dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_FUNC_DECL:
    case JS_AST_FUNC_EXPR:
        dump_func(sb, n);
        break;
    case JS_AST_IMPORT:
        sb_printf(sb, "(import \"");
        sb_units(sb, n->units, n->len);
        sb_printf(sb, "\"");
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_IMPORT_SPEC:
        if (n->flags & JS_F_SPEC_DEFAULT) {
            sb_printf(sb, "(default ");
            sb_units(sb, n->units2, n->len2);
        } else if (n->flags & JS_F_SPEC_NS) {
            sb_printf(sb, "(ns ");
            sb_units(sb, n->units2, n->len2);
        } else {
            sb_printf(sb, "(n ");
            sb_units(sb, n->units, n->len);
            sb_printf(sb, " ");
            sb_units(sb, n->units2, n->len2);
        }
        sb_printf(sb, ")");
        break;
    case JS_AST_EXPORT_NAMED:
        if (n->a) {
            sb_printf(sb, "(export");
            dump_child(sb, n->a);
        } else if (n->len) {
            sb_printf(sb, "(export-from \"");
            sb_units(sb, n->units, n->len);
            sb_printf(sb, "\"");
            dump_list(sb, n->items, n->count);
        } else {
            sb_printf(sb, "(export");
            dump_list(sb, n->items, n->count);
        }
        sb_printf(sb, ")");
        break;
    case JS_AST_EXPORT_DEFAULT:
        sb_printf(sb, "(export-default");
        dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_EXPORT_ALL:
        sb_printf(sb, "(export-all \"");
        sb_units(sb, n->units, n->len);
        sb_printf(sb, "\"");
        if (n->len2) {
            sb_printf(sb, " ");
            sb_units(sb, n->units2, n->len2);
        }
        sb_printf(sb, ")");
        break;
    case JS_AST_EXPORT_SPEC:
        sb_printf(sb, "(n ");
        sb_units(sb, n->units, n->len);
        sb_printf(sb, " ");
        sb_units(sb, n->units2, n->len2);
        sb_printf(sb, ")");
        break;
    case JS_AST_IDENT:
        sb_units(sb, n->units, n->len);
        break;
    case JS_AST_NUMBER:
        sb_printf(sb, "%g", n->number);
        break;
    case JS_AST_STRING:
        sb_printf(sb, "\"");
        sb_units(sb, n->units, n->len);
        sb_printf(sb, "\"");
        break;
    case JS_AST_TEMPLATE:
        sb_printf(sb, "(tpl");
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_REGEX:
        sb_printf(sb, "(regex \"");
        sb_units(sb, n->units, n->len);
        sb_printf(sb, "\" \"");
        sb_units(sb, n->units2, n->len2);
        sb_printf(sb, "\")");
        break;
    case JS_AST_NULL_LIT:
        sb_printf(sb, "null");
        break;
    case JS_AST_BOOL_LIT:
        sb_printf(sb, (n->flags & JS_F_TRUE) ? "true" : "false");
        break;
    case JS_AST_THIS:
        sb_printf(sb, "this");
        break;
    case JS_AST_ARRAY:
    case JS_AST_ARRAY_PATTERN:
        sb_printf(sb, n->kind == JS_AST_ARRAY ? "(arr" : "(apat");
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_OBJECT:
    case JS_AST_OBJECT_PATTERN:
        sb_printf(sb, n->kind == JS_AST_OBJECT ? "(obj" : "(opat");
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_PROPERTY:
        if (n->flags & JS_F_METHOD)
            sb_printf(sb, "(m");
        else if (n->flags & JS_F_COMPUTED)
            sb_printf(sb, "(pc");
        else
            sb_printf(sb, "(p");
        dump_child(sb, n->a);
        dump_child(sb, n->b);
        sb_printf(sb, ")");
        break;
    case JS_AST_CALL:
        sb_printf(sb, (n->flags & JS_F_OPTIONAL) ? "(?call" : "(call");
        dump_child(sb, n->a);
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_NEW:
        sb_printf(sb, "(new");
        dump_child(sb, n->a);
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_MEMBER:
        if (n->flags & JS_F_COMPUTED) {
            sb_printf(sb, (n->flags & JS_F_OPTIONAL) ? "(?idx" : "(idx");
            dump_child(sb, n->a);
            dump_child(sb, n->b);
        } else {
            sb_printf(sb, (n->flags & JS_F_OPTIONAL) ? "(?." : "(.");
            dump_child(sb, n->a);
            sb_printf(sb, " ");
            sb_units(sb, n->units, n->len);
        }
        sb_printf(sb, ")");
        break;
    case JS_AST_UNARY:
        if (n->op == JS_T_MINUS)
            sb_printf(sb, "(neg");
        else if (n->op == JS_T_PLUS)
            sb_printf(sb, "(pos");
        else
            sb_printf(sb, "(%s", op_text(n->op));
        dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_UPDATE:
        sb_printf(sb, "(%s%s", op_text(n->op),
                  (n->flags & JS_F_PREFIX) ? "pre" : "post");
        dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_BINARY:
    case JS_AST_LOGICAL:
    case JS_AST_ASSIGN:
        sb_printf(sb, "(%s", op_text(n->op));
        dump_child(sb, n->a);
        dump_child(sb, n->b);
        sb_printf(sb, ")");
        break;
    case JS_AST_COND:
        sb_printf(sb, "(?:");
        dump_child(sb, n->a);
        dump_child(sb, n->b);
        dump_child(sb, n->c);
        sb_printf(sb, ")");
        break;
    case JS_AST_SEQUENCE:
        sb_printf(sb, "(,");
        dump_list(sb, n->items, n->count);
        sb_printf(sb, ")");
        break;
    case JS_AST_SPREAD:
        sb_printf(sb, "(...");
        dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_AWAIT:
        sb_printf(sb, "(await");
        dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_HOLE:
        sb_printf(sb, "_");
        break;
    case JS_AST_REST:
        sb_printf(sb, "(rest");
        dump_child(sb, n->a);
        sb_printf(sb, ")");
        break;
    case JS_AST_DEFAULT:
        sb_printf(sb, "(def");
        dump_child(sb, n->a);
        dump_child(sb, n->b);
        sb_printf(sb, ")");
        break;
    case JS_AST_COVER:
        sb_printf(sb, "(?cover?)");
        break;
    }
}

/* ---- harness ---- */

static uint16_t *to_u16(const char *src, size_t *len_out) {
    size_t len = strlen(src);
    uint16_t *u = malloc((len + 1) * sizeof(uint16_t));
    for (size_t i = 0; i < len; i++)
        u[i] = (uint16_t)(unsigned char)src[i];
    u[len] = 0;
    *len_out = len;
    return u;
}

static void check_dump(const char *src, const char *expected) {
    CountAlloc ca = {0, 0};
    JsVmConfig cfg = {.realloc_fn = count_realloc, .alloc_ud = &ca};
    JsVm *vm = js_vm_new(&cfg);
    size_t len;
    uint16_t *u = to_u16(src, &len);
    JsArena arena;
    js_arena_init(&arena, vm);
    JsParseResult res;
    bool ok = js_parse_module(&arena, u, len, &res);
    CHECK(ok);
    if (!ok) {
        fprintf(stderr, "  source: %s\n  error: %s (at %u)\n", src,
                res.err_msg, res.err_pos);
    } else {
        Sb sb = {0};
        dump_node(&sb, res.module);
        checks_run++;
        if (strcmp(sb.buf, expected) != 0) {
            checks_failed++;
            fprintf(stderr, "FAIL dump mismatch\n  source:   %s\n  expected: %s\n  actual:   %s\n",
                    src, expected, sb.buf);
        }
        free(sb.buf);
    }
    js_arena_free(&arena);
    free(u);
    js_vm_free(vm);
    CHECK(ca.net_bytes == 0 && ca.live_allocs == 0);
}

static void check_parses(const char *src) {
    JsVm *vm = js_vm_new(NULL);
    size_t len;
    uint16_t *u = to_u16(src, &len);
    JsArena arena;
    js_arena_init(&arena, vm);
    JsParseResult res;
    bool ok = js_parse_module(&arena, u, len, &res);
    CHECK(ok);
    if (!ok)
        fprintf(stderr, "  source: %s\n  error: %s (at %u)\n", src, res.err_msg,
                res.err_pos);
    js_arena_free(&arena);
    free(u);
    js_vm_free(vm);
}

static void check_err(const char *src, const char *substr, long pos) {
    JsVm *vm = js_vm_new(NULL);
    size_t len;
    uint16_t *u = to_u16(src, &len);
    JsArena arena;
    js_arena_init(&arena, vm);
    JsParseResult res;
    bool ok = js_parse_module(&arena, u, len, &res);
    checks_run++;
    if (ok) {
        checks_failed++;
        fprintf(stderr, "FAIL expected error, parsed ok\n  source: %s\n  wanted: %s\n",
                src, substr);
    } else if (!strstr(res.err_msg, substr)) {
        checks_failed++;
        fprintf(stderr, "FAIL wrong error\n  source: %s\n  wanted: %s\n  got:    %s\n",
                src, substr, res.err_msg);
    } else if (pos >= 0) {
        checks_run++;
        if (res.err_pos != (uint32_t)pos) {
            checks_failed++;
            fprintf(stderr, "FAIL wrong error pos\n  source: %s\n  wanted: %ld  got: %u\n",
                    src, pos, res.err_pos);
        }
    }
    js_arena_free(&arena);
    free(u);
    js_vm_free(vm);
}

/* ---- tests ---- */

static void test_declarations(void) {
    check_dump("let x = 1;", "(module (let (d x 1)))");
    check_dump("let a, b = 2;", "(module (let (d a) (d b 2)))");
    check_dump("const [a, , b = 2, ...r] = xs;",
               "(module (const (d (apat a _ (def b 2) (rest r)) xs)))");
    check_dump("let {x, y: {z} = q, ...rest} = o;",
               "(module (let (d (opat (p x x) (p y (def (opat (p z z)) q)) (rest rest)) o)))");
    check_dump("let {a = 1} = o;",
               "(module (let (d (opat (p a (def a 1))) o)))");
    check_dump("let {[k]: v} = o;",
               "(module (let (d (opat (pc k v)) o)))");
}

static void test_operators(void) {
    check_dump("a = b + c * d ** e;",
               "(module (expr (= a (+ b (* c (** d e))))))");
    check_dump("x = a ?? (b || c);",
               "(module (expr (= x (?? a (|| b c)))))");
    check_dump("y = -x; z = !a;",
               "(module (expr (= y (neg x))) (expr (= z (! a))))");
    check_dump("++i; i--;",
               "(module (expr (++pre i)) (expr (--post i)))");
    check_dump("q = a / b / c;",
               "(module (expr (= q (/ (/ a b) c))))");
    check_dump("t = a === b ? 'y' : 'n';",
               "(module (expr (= t (?: (=== a b) \"y\" \"n\"))))");
    check_dump("x = (a, b);", "(module (expr (= x (, a b))))");
    check_dump("k = 'a' in o;", "(module (expr (= k (in \"a\" o))))");
    check_dump("v ?\?= w;", "(module (expr (?\?= v w)))");
    check_dump("delete a.b;", "(module (expr (delete (. a b))))");
}

static void test_functions(void) {
    check_dump("function add(a, b) { return a + b; }",
               "(module (fn add (a b) (block (return (+ a b)))))");
    check_dump("async function f() { return await g(); }",
               "(module (afn f () (block (return (await (call g))))))");
    check_dump("f = (a, b = 1, ...rest) => a + b;",
               "(module (expr (= f (=> (a (def b 1) (rest rest)) (+ a b)))))");
    check_dump("g = async x => await h(x);",
               "(module (expr (= g (a=> (x) (await (call h x))))))");
    check_dump("h = () => ({a: 1});",
               "(module (expr (= h (=> () (obj (p a 1))))))");
    check_dump("let e = function named() { return 1; };",
               "(module (let (d e (fn named () (block (return 1))))))");
    check_dump("f(...args);", "(module (expr (call f (... args))))");
}

static void test_literals(void) {
    check_dump("t = `a${x + 1}b`;",
               "(module (expr (= t (tpl \"a\" (+ x 1) \"b\"))))");
    check_dump("t = `${a}${b}`;",
               "(module (expr (= t (tpl \"\" a \"\" b \"\"))))");
    check_dump("t = `nested ${`in${x}`}`;",
               "(module (expr (= t (tpl \"nested \" (tpl \"in\" x \"\") \"\"))))");
    check_dump("r = /a+b/gi;", "(module (expr (= r (regex \"a+b\" \"gi\"))))");
    check_dump("s = '\\u0041\\n';", "(module (expr (= s \"A\n\")))");
    check_dump("o = {a, b: 2, [k]: 3, m() { return 1; }, ...rest};",
               "(module (expr (= o (obj (p a a) (p b 2) (pc k 3) "
               "(m m (fn _ () (block (return 1)))) (... rest)))))");
    check_dump("arr = [1, , 2, ...xs];",
               "(module (expr (= arr (arr 1 _ 2 (... xs)))))");
    check_dump("n = 0x10 + 0b101 + 0o17 + 1.5e2 + .25;",
               "(module (expr (= n (+ (+ (+ (+ 16 5) 15) 150) 0.25))))");
    check_dump("b = true; c = null;",
               "(module (expr (= b true)) (expr (= c null)))");
}

static void test_control_flow(void) {
    check_dump("if (a) b(); else c();",
               "(module (if a (expr (call b)) (expr (call c))))");
    check_dump("while (a) { b(); }",
               "(module (while a (block (expr (call b)))))");
    check_dump("do f(); while (x)",
               "(module (do (expr (call f)) x))");
    check_dump("for (let i = 0; i < 10; i++) ;",
               "(module (for (let (d i 0)) (< i 10) (++post i) (empty)))");
    check_dump("for (;;) ;", "(module (for _ _ _ (empty)))");
    check_dump("for (const v of xs) { f(v); }",
               "(module (of const v xs (block (expr (call f v)))))");
    check_dump("for ([a, b] of pairs) g(a, b);",
               "(module (of assign (apat a b) pairs (expr (call g a b))))");
    check_dump("switch (x) { case 1: a(); break; default: b(); }",
               "(module (switch x (case 1 (expr (call a)) (break)) (default (expr (call b)))))");
    check_dump("try { f(); } catch (e) { g(e); } finally { h(); }",
               "(module (try (block (expr (call f))) (catch e (block (expr (call g e)))) "
               "(finally (block (expr (call h))))))");
    check_dump("try { f(); } catch { g(); }",
               "(module (try (block (expr (call f))) (catch (block (expr (call g))))))");
    check_dump("outer: for (;;) { break outer; }",
               "(module (label outer (for _ _ _ (block (break outer)))))");
    check_dump("function f() { throw new_error(); }",
               "(module (fn f () (block (throw (call new_error)))))");
}

static void test_members_calls(void) {
    check_dump("v = a?.b?.[c]?.(d);",
               "(module (expr (= v (?call (?idx (?. a b) c) d))))");
    check_dump("w = a.b[c](d).e;",
               "(module (expr (= w (. (call (idx (. a b) c) d) e))))");
    check_dump("obj.default = 1;",
               "(module (expr (= (. obj default) 1)))");
}

static void test_new(void) {
    check_dump("new Foo();", "(module (expr (new Foo)))");
    check_dump("new Foo;", "(module (expr (new Foo)))");
    check_dump("new Foo(1, 2);", "(module (expr (new Foo 1 2)))");
    check_dump("new a.b.C(1);", "(module (expr (new (. (. a b) C) 1)))");
    check_dump("new Foo().bar();", "(module (expr (call (. (new Foo) bar))))");
    /* both () bind to `new`: `new (new Foo())()`, not a trailing call */
    check_dump("new new Foo()();", "(module (expr (new (new Foo))))");
    check_dump("new Foo(...args);", "(module (expr (new Foo (... args))))");
}

static void test_destructuring_assign(void) {
    check_dump("[a, b.c] = xs;",
               "(module (expr (= (apat a (. b c)) xs)))");
    check_dump("({p: q} = o);",
               "(module (expr (= (opat (p p q)) o)))");
    check_dump("({a = 1, ...r} = o);",
               "(module (expr (= (opat (p a (def a 1)) (rest r)) o)))");
}

static void test_modules(void) {
    check_dump("import \"m\";", "(module (import \"m\"))");
    check_dump("import d from \"m\";",
               "(module (import \"m\" (default d)))");
    check_dump("import * as ns from \"m\";",
               "(module (import \"m\" (ns ns)))");
    check_dump("import d, {a as b, default as dd} from \"m\";",
               "(module (import \"m\" (default d) (n a b) (n default dd)))");
    check_dump("export const x = 1;",
               "(module (export (const (d x 1))))");
    check_dump("export {x as y, z};",
               "(module (export (n x y) (n z z)))");
    check_dump("export {a} from \"m\";",
               "(module (export-from \"m\" (n a a)))");
    check_dump("export * from \"m\"; export * as ns from \"m\";",
               "(module (export-all \"m\") (export-all \"m\" ns))");
    check_dump("export default 42;",
               "(module (export-default 42))");
    check_dump("export default function f() { return 1; }",
               "(module (export-default (fn f () (block (return 1)))))");
    check_dump("export async function go() { await x(); }",
               "(module (export (afn go () (block (expr (await (call x)))))))");
}

static void test_asi(void) {
    check_dump("let a = 1\nlet b = 2",
               "(module (let (d a 1)) (let (d b 2)))");
    check_dump("function f() { return\n1 }",
               "(module (fn f () (block (return) (expr 1))))");
    check_dump("a\n++b;",
               "(module (expr a) (expr (++pre b)))");
    check_dump("x = 1\ny = 2",
               "(module (expr (= x 1)) (expr (= y 2)))");
}

static void test_top_level_await(void) {
    check_parses("let d = await fetchData();");
    check_parses("for (const x of await list()) { use(x); }");
}

static void test_errors(void) {
    check_err("var x = 1;", "'var' is not supported", 0);
    check_err("class Foo {}", "classes are not supported", 0);
    check_err("with (o) {}", "'with' is not allowed", 0);
    check_err("let x = yield;", "reserved word", -1);
    check_err("for (let k in o) {}", "for-in is not supported", -1);
    check_err("for (k in o) {}", "for-in is not supported", -1);
    check_err("new.target;", "'new.target' is not supported", -1);
    check_err("x instanceof y;", "'instanceof' is not supported", -1);
    check_err("function f() { await g(); }", "'await' is only valid in async", -1);
    check_err("a ?? b || c;", "'?\?' cannot be mixed", -1);
    check_err("const x;", "requires an initializer", -1);
    check_err("let [a];", "destructuring declaration requires an initializer", -1);
    check_err("break;", "'break' outside loop or switch", 0);
    check_err("continue;", "'continue' outside loop", 0);
    check_err("return 1;", "'return' outside function", 0);
    check_err("throw\n1;", "no line break allowed after 'throw'", 0);
    check_err("let s = 'abc", "unterminated string", -1);
    check_err("t = `abc", "unterminated template", -1);
    check_err("x = ({a = 1});", "shorthand property initializer", -1);
    check_err("0x;", "invalid number", 0);
    check_err("01;", "legacy octal", 0);
    check_err("1_000;", "numeric separators are not supported", -1);
    check_err("x = 5n;", "BigInt literals are not supported", -1);
    check_err("eval = 1;", "cannot assign to 'eval'", -1);
    check_err("let eval = 1;", "cannot bind 'eval'", -1);
    check_err("o = {get x() { return 1; }};", "getter/setter properties are not supported", -1);
    check_err("f = x => y => import(\"m\");", "dynamic import() is not supported", -1);
    check_err("tag`x`;", "tagged templates are not supported", -1);
    check_err("if (x) import \"m\";", "top level", -1);
    check_err("x = '\\101';", "octal escapes are not allowed", -1);
    check_err("debugger;", "'debugger' is not supported", 0);
}

static void test_depth_limit(void) {
    char src[4096];
    size_t n = 0;
    src[n++] = 'x';
    src[n++] = '=';
    for (int i = 0; i < 400; i++)
        src[n++] = '(';
    src[n++] = '1';
    for (int i = 0; i < 400; i++)
        src[n++] = ')';
    src[n++] = ';';
    src[n] = '\0';
    check_err(src, "nesting too deep", -1);
}

static void test_garbage_resilience(void) {
    static const char *cases[] = {
        "#$%^", "let = = =", "((((", "}}}}", "\\", "`${`${`${",
        "0x 0b 0o", "'\\u{}'", "'\\u{110000}'", "a?.=1", "...x",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        JsVm *vm = js_vm_new(NULL);
        size_t len;
        uint16_t *u = to_u16(cases[i], &len);
        JsArena arena;
        js_arena_init(&arena, vm);
        JsParseResult res;
        bool ok = js_parse_module(&arena, u, len, &res);
        CHECK(!ok);
        CHECK(res.err_msg != NULL);
        js_arena_free(&arena);
        free(u);
        js_vm_free(vm);
    }
}

static void test_line_col(void) {
    const char *src = "let a = 1;\nlet b = 2;\r\nlet c = 3;";
    size_t len;
    uint16_t *u = to_u16(src, &len);
    uint32_t line, col;
    js_source_line_col(u, len, 0, &line, &col);
    CHECK(line == 1 && col == 1);
    js_source_line_col(u, len, 11, &line, &col); /* start of line 2 */
    CHECK(line == 2 && col == 1);
    js_source_line_col(u, len, 23, &line, &col); /* start of line 3 */
    CHECK(line == 3 && col == 1);
    js_source_line_col(u, len, 27, &line, &col); /* 'c' */
    CHECK(line == 3 && col == 5);
    free(u);
}

int main(void) {
    test_declarations();
    test_operators();
    test_functions();
    test_literals();
    test_control_flow();
    test_members_calls();
    test_new();
    test_destructuring_assign();
    test_modules();
    test_asi();
    test_top_level_await();
    test_errors();
    test_depth_limit();
    test_garbage_resilience();
    test_line_col();

    if (checks_failed) {
        fprintf(stderr, "%d/%d syntax checks FAILED\n", checks_failed, checks_run);
        return 1;
    }
    printf("all %d syntax checks passed\n", checks_run);
    return 0;
}
