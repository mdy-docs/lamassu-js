#include <stdio.h>
#include <string.h>
#include "js_syntax.h"
#include "js_regexp.h"

/*
 * Recursive-descent / Pratt parser for the lamassu subset: always strict,
 * always module goal, no var/class/eval/new. Parenthesized expressions and
 * arrow parameter lists share a cover grammar (JS_AST_COVER) resolved when
 * `=>` is (or isn't) seen. Nesting depth is hard-limited so hostile input
 * cannot exhaust the C stack.
 */

#define JS_PARSE_MAX_DEPTH 256

struct JsCoverOpen {
    uint32_t pos, end; /* the key; the end of its initializer (of the key, for a `__proto__`) */
    bool proto;        /* a second `__proto__: v`, not a cover initializer */
};

typedef struct JsParser {
    JsLexer lx;
    JsArena *arena;
    const char *err_msg;
    uint32_t err_pos;
    int depth;
    /* context */
    bool in_function;
    bool fn_async;   /* await allowed (module top level counts) */
    int fn_nesting;  /* 0 = module top level */
    bool new_target_ok; /* inside a function that is not an arrow (an arrow inherits it) */
    bool super_ok;      /* inside a method of an object literal (an arrow inherits it) */
    int loop_depth, switch_depth;
    /* The labels in scope in this function (label_base and up; an enclosing function's are below
     * it and out of reach). The statement about to be parsed carries the top pending_labels of
     * them, the top pending_own of those by a chain of labels alone: `a: b: for` gives the loop
     * both as its own, `a: if (x) for` hands `a` down through the `if` and the loop does not own
     * it. V8 keeps the same two sets (labels, own_labels) and words its errors from them. */
    struct JsLabel *labels;
    uint32_t label_count, label_cap, label_base;
    uint32_t pending_labels, pending_own;
    /* The object literals read with a cover initializer (`{a = 1}`) that no pattern has claimed
     * yet, each by the position of its first such key, in source order; and with them, by its
     * second such key, each literal that says `__proto__: v` twice, which a pattern may and a
     * literal may not. Every expression that can no longer become a pattern must leave the list
     * as long as it found it (cover_closed): V8's expression scopes, which report the first
     * one left open. cover_base: where the function being read began, for the check at the end
     * of each statement. */
    struct JsCoverOpen *cover_open;
    uint32_t cover_count, cover_cap, cover_base;
    /* The last octal literal or escape among the tokens read, which strict mode refuses but V8
     * reports only where the function around it ends (octal_check), or with another error */
    const char *octal_msg;
    uint32_t octal_pos, octal_end;
    bool module_tla;
    bool allow_return; /* the entry module: `return` at top level ends it (js_eval_entry) */
    int pattern_depth; /* expr_to_pattern nesting: V8 words a bad target inside a pattern apart */
    bool decl_element; /* reading, as an expression, an element of a declaration's pattern */
    uint32_t prev_end; /* end of the token before the current one: where a function's text ends */
    uint32_t prev_pos; /* and where that token starts: V8 points some errors at the last token read */
    bool no_in;        /* in a `for (` head: `in` is no operator here */
    /* Declarations, checked as V8 checks them while it reads (Scope::Declare), so a name
     * declared twice is an error where the second one is read and before anything after it:
     * the scopes the parser is in, innermost last, and the names declared in them, each
     * scope's from its base on. */
    struct JsPDecl *decls;
    uint32_t decl_count, decl_cap;
    struct JsPScope *scopes;
    uint32_t scope_count, scope_cap;
    int8_t bind_kind;  /* what a binding name read now declares (JS_BIND_*), NONE in expressions */
    bool body_block;   /* the block about to be read is a function's body: the function's scope */
    bool catch_block;  /* the block about to be read is a catch clause's: checked at its end */
    bool default_fn;   /* parse_func_common reads `export default function`: it declares */
    /* The module's exports, for V8's checks once the module is read (Validate) */
    struct JsPExport *exports;
    uint32_t export_count, export_cap, ns_exports;
} JsParser;

/* What a declaration is, for which others it clashes with: a let, const, import, catch parameter
 * or function in a block or the module (lexical); a function at the top level of a function's
 * body, which V8 declares as a `var` (two of them, or one and a parameter, do not clash); a
 * parameter (two of them are the parameter list's own error, once the list is read). */
enum { JS_BIND_NONE = -1, JS_BIND_LEX = 0, JS_BIND_FN_VAR, JS_BIND_PARAM };
enum { JS_SCOPE_BLOCK, JS_SCOPE_FUNC, JS_SCOPE_CATCH, JS_SCOPE_MODULE };

typedef struct JsPDecl {
    const uint16_t *name;
    uint32_t len;
    uint32_t pos; /* where V8 points an error about it */
    int8_t kind;
} JsPDecl;

typedef struct JsPScope {
    uint32_t base; /* its first declaration */
    uint8_t kind;
} JsPScope;

/* One export, as V8's module descriptor has it: a local one (`export { a as b }`, a declaration,
 * `export default`, and `export * as b`, which V8 makes an import of a hidden name exported), or
 * a re-export of another module's (SPECIAL) */
typedef struct JsPExport {
    const uint16_t *local, *name;
    uint32_t local_len, name_len;
    uint32_t pos;
    bool special;
    bool defined; /* its local name is declared here by what made it */
} JsPExport;

/* What a label names, for `continue`: nothing a `continue` can see (V8: "Undefined label"), a
 * statement that is not a loop ("does not denote an iteration statement"), or a loop. `break`
 * may name any of them. */
enum { JS_LABEL_PLAIN, JS_LABEL_TARGET, JS_LABEL_LOOP };

typedef struct JsLabel {
    const uint16_t *units;
    uint32_t len;
    uint8_t kind;
} JsLabel;

typedef struct JsNodeVec {
    JsAstNode **items;
    uint32_t count, cap;
} JsNodeVec;

#define TOK(p) ((p)->lx.tok)

static JsAstNode *parse_statement(JsParser *p);
static JsAstNode *parse_block(JsParser *p);
static JsAstNode *parse_expression(JsParser *p);
static JsAstNode *parse_expression_cover(JsParser *p);
static JsAstNode *parse_assign_scoped(JsParser *p);
static JsAstNode *parse_new_expr(JsParser *p);
static JsAstNode *parse_call_member(JsParser *p);
static JsAstNode *parse_assign(JsParser *p);
static JsAstNode *parse_index_expression(JsParser *p);
static JsAstNode *parse_arguments(JsParser *p, JsNodeVec *out);
static JsAstNode *parse_binding_target(JsParser *p);
static JsAstNode *parse_binding_element(JsParser *p);
static JsAstNode *parse_pattern_element(JsParser *p);
static bool is_simple_target(const JsAstNode *n);
static JsAstNode *parse_func_common(JsParser *p, uint8_t flags, bool need_name,
                                    uint32_t pos, uint8_t kind, uint32_t src_start);
static JsAstNode *expr_to_pattern(JsParser *p, JsAstNode *n, bool binding);

static const char DUPLICATE_PROTO[] = "Duplicate __proto__ fields are not allowed in object literals";
static bool units_is(const uint16_t *u, uint32_t len, const char *ascii);

/* V8's wording for a parse that stops at the current token: "Unexpected token ';'",
 * "Unexpected identifier 'x'", "Unexpected end of input". */
static const char *unexpected_token_text(JsParser *p, const JsToken *t) {
    JsVm *vm = p->arena->vm;
    switch (t->kind) {
    case JS_T_EOF:
        return "Unexpected end of input";
    case JS_T_LET: /* where it does not start a declaration it is a name, and reserved */
        return "Unexpected strict mode reserved word";
    case JS_T_AWAIT:
        return "Unexpected reserved word";
    case JS_T_IDENT:
        return js_compile_error_fmt(vm, "Unexpected identifier '%U'", t->u.str.units, t->u.str.len);
    case JS_T_NUM:
    case JS_T_BIGINT:
        return "Unexpected number";
    case JS_T_STR:
        return "Unexpected string";
    case JS_T_TEMPLATE:
        return "Unexpected template string";
    case JS_T_REGEX:
        return "Unexpected regular expression";
    case JS_T_RESERVED:
        if (units_is(t->u.str.units, t->u.str.len, "enum"))
            return "Unexpected reserved word";
        if (units_is(t->u.str.units, t->u.str.len, "yield") ||
            units_is(t->u.str.units, t->u.str.len, "static") ||
            units_is(t->u.str.units, t->u.str.len, "implements") ||
            units_is(t->u.str.units, t->u.str.len, "interface") ||
            units_is(t->u.str.units, t->u.str.len, "package") ||
            units_is(t->u.str.units, t->u.str.len, "private") ||
            units_is(t->u.str.units, t->u.str.len, "protected") ||
            units_is(t->u.str.units, t->u.str.len, "public"))
            return "Unexpected strict mode reserved word";
        break;
    default:
        break;
    }
    return js_compile_error_fmt(vm, "Unexpected token '%U'", p->lx.src + t->pos, t->end - t->pos);
}

static const char *unexpected_text(JsParser *p) {
    return unexpected_token_text(p, &TOK(p));
}

/* The parser's own messages, worded as V8 words the same errors. */
static const char *v8_message(const char *msg) {
    static const char *const map[][2] = {
        {"'return' outside function", "Illegal return statement"},
        {"'break' outside loop or switch", "Illegal break statement"},
        {"'continue' outside loop", "Illegal continue statement: no surrounding iteration statement"},
        {"no line break allowed after 'throw'", "Illegal newline after throw"},
        {"rest element must be last", "Rest element must be last element"},
        {"rest parameter must be last", "Rest parameter must be last formal parameter"},
        {"invalid assignment target", "Invalid left-hand side in assignment"},
        {"optional chain is not a valid assignment target", "Invalid left-hand side in assignment"},
        {"invalid destructuring target", "Invalid destructuring assignment target"},
        {"method is not a valid destructuring target", "Invalid destructuring assignment target"},
        {"parenthesized destructuring pattern", "Invalid left-hand side in assignment"},
        {"shorthand property initializer is only valid in destructuring", "Invalid shorthand property initializer"},
        {"'await' is only valid in async functions", "Unexpected reserved word"},
        {"cannot assign to 'eval' or 'arguments' in strict mode", "Unexpected eval or arguments in strict mode"},
        {"cannot bind 'eval' or 'arguments' in strict mode", "Unexpected eval or arguments in strict mode"},
        {"'delete' target must be a property access", "Delete of an unqualified identifier in strict mode."},
        {"'const' declaration requires an initializer", "Missing initializer in const declaration"},
        {"destructuring declaration requires an initializer", "Missing initializer in destructuring declaration"},
        {"duplicate 'default' clause", "More than one default clause in switch statement"},
        {"unary operator before '**' must be parenthesized",
         "Unary operator used immediately before exponentiation expression. Parenthesis must be used to "
         "disambiguate operator precedence"},
        {"'with' is not allowed in strict mode", "Strict mode code may not include a with statement"},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcmp(msg, map[i][0]) == 0)
            return map[i][1];
    }
    return NULL;
}

/* Errors V8 reports as the token where parsing stopped */
static bool reported_as_token(const char *msg) {
    return strncmp(msg, "expected", 8) == 0 ||
           strcmp(msg, "import/export is only allowed at the top level of a module") == 0 ||
           strcmp(msg, "'import' is only allowed at the top level of a module") == 0 ||
           strcmp(msg, "no line break allowed before '=>'") == 0 ||
           strcmp(msg, "'...' is only valid in arrow parameters here") == 0;
}

/* Every error ends here. V8 goes on unwinding after an error, each expression scope and each
 * function on the way out reports what it still holds, and of all the reports the one earliest
 * in the source is kept (PendingCompilationErrorHandler; one that ends where the kept one
 * begins does not replace it). So an error after a cover initializer that no pattern has
 * claimed, however far out the literal it is in, is that initializer's error, and an error
 * after an octal literal is the literal's. */
static void fail_at(JsParser *p, uint32_t pos, const char *msg) {
    if (p->err_msg)
        return;
    if (p->cover_count && p->cover_open[0].end < pos) {
        pos = p->cover_open[0].pos;
        msg = p->cover_open[0].proto ? DUPLICATE_PROTO : "Invalid shorthand property initializer";
    }
    if (p->lx.late_msg) { /* V8's scanner is at the token being looked at: its octal is the last */
        if (p->lx.late_end < pos) {
            pos = p->lx.late_pos;
            msg = p->lx.late_msg;
        }
    } else if (p->octal_msg && p->octal_end < pos) {
        pos = p->octal_pos;
        msg = p->octal_msg;
    }
    p->err_msg = msg;
    p->err_pos = pos;
}

/* Where a function (or the module) that began at START ends: strict mode's error for an octal
 * literal in it, the last one as V8 has it */
static bool octal_check(JsParser *p, uint32_t start) {
    if (p->err_msg)
        return false;
    if (p->octal_msg && p->octal_pos >= start) {
        fail_at(p, p->octal_pos, p->octal_msg);
        return false;
    }
    return true;
}

static void fail_lexer(JsParser *p) {
    fail_at(p, p->lx.err_pos, p->lx.err_msg);
}

static JsAstNode *perr(JsParser *p, uint32_t pos, const char *msg) {
    if (!p->err_msg) {
        const char *v8 = v8_message(msg);
        if (v8) {
            msg = v8;
        } else if (reported_as_token(msg)) {
            msg = unexpected_text(p);
            pos = TOK(p).pos;
        }
        fail_at(p, pos, msg);
    }
    return NULL;
}

static JsAstNode *perr_here(JsParser *p, const char *msg) {
    return perr(p, TOK(p).pos, msg);
}

/* An error V8 reports on its way out of a construct that has already failed further on: of
 * two reports it keeps the one earlier in the source (see fail_at), so this one, about the
 * source from POS to END, replaces a later one -- unless that begins right where this ends */
static JsAstNode *perr_earlier(JsParser *p, uint32_t pos, uint32_t end, const char *msg) {
    if (p->err_msg && (p->err_pos <= end || strcmp(p->err_msg, "out of memory") == 0))
        return NULL;
    p->err_msg = NULL;
    fail_at(p, pos, msg);
    return NULL;
}

/* ---- declarations ---- */

static bool same_name(const uint16_t *a, uint32_t alen, const uint16_t *b, uint32_t blen) {
    return alen == blen && memcmp(a, b, (size_t)alen * sizeof *a) == 0;
}

static bool grow_array(JsParser *p, void **items, uint32_t count, uint32_t *cap, size_t size) {
    if (count < *cap)
        return true;
    uint32_t ncap = *cap ? *cap * 2 : 16;
    void *n = js_arena_alloc(p->arena, (size_t)ncap * size);
    if (!n) {
        perr(p, TOK(p).pos, "out of memory");
        return false;
    }
    if (count)
        memcpy(n, *items, (size_t)count * size);
    *items = n;
    *cap = ncap;
    return true;
}

static bool scope_push(JsParser *p, uint8_t kind) {
    if (!grow_array(p, (void **)&p->scopes, p->scope_count, &p->scope_cap, sizeof *p->scopes))
        return false;
    p->scopes[p->scope_count++] = (JsPScope){p->decl_count, kind};
    return true;
}

static void scope_pop(JsParser *p) {
    if (p->scope_count)
        p->decl_count = p->scopes[--p->scope_count].base;
}

static bool fail_redeclared(JsParser *p, const uint16_t *name, uint32_t len, uint32_t pos) {
    perr(p, pos, js_compile_error_fmt(p->arena->vm, "Identifier '%U' has already been declared", name, len));
    return false;
}

/* Declares NAME in the innermost scope, as KIND; POS is where an error about it points */
static bool declare(JsParser *p, const uint16_t *name, uint32_t len, uint32_t pos, int8_t kind) {
    const JsPScope *s = &p->scopes[p->scope_count - 1];
    for (uint32_t i = s->base; i < p->decl_count; i++) {
        const JsPDecl *d = &p->decls[i];
        if (!same_name(d->name, d->len, name, len))
            continue;
        if (kind == JS_BIND_PARAM && d->kind == JS_BIND_PARAM)
            continue; /* the parameter list's own error (params_check) */
        if (kind == JS_BIND_FN_VAR && d->kind != JS_BIND_LEX)
            continue; /* a var over a var or a parameter */
        return fail_redeclared(p, name, len, pos);
    }
    if (!grow_array(p, (void **)&p->decls, p->decl_count, &p->decl_cap, sizeof *p->decls))
        return false;
    p->decls[p->decl_count++] = (JsPDecl){name, len, pos, kind};
    return true;
}

/* What a function declaration is where it is read: a var at the top of a function's body */
static int8_t function_kind(const JsParser *p) {
    return p->scopes[p->scope_count - 1].kind == JS_SCOPE_FUNC ? JS_BIND_FN_VAR : JS_BIND_LEX;
}

/* Every name a parameter or declaration pattern binds, in source order */
static bool declare_pattern(JsParser *p, const JsAstNode *pat, int8_t kind) {
    switch ((JsAstKind)pat->kind) {
    case JS_AST_IDENT:
        return declare(p, pat->units, pat->len, pat->pos, kind);
    case JS_AST_DEFAULT:
    case JS_AST_REST:
        return declare_pattern(p, pat->a, kind);
    case JS_AST_ARRAY_PATTERN:
        for (uint32_t i = 0; i < pat->count; i++)
            if (pat->items[i]->kind != JS_AST_HOLE && !declare_pattern(p, pat->items[i], kind))
                return false;
        return true;
    case JS_AST_OBJECT_PATTERN:
        for (uint32_t i = 0; i < pat->count; i++) {
            const JsAstNode *prop = pat->items[i];
            if (!declare_pattern(p, prop->kind == JS_AST_REST ? prop->a : prop->b, kind))
                return false;
        }
        return true;
    default:
        return true;
    }
}

/* Once a parameter list is read (and before the body): the first parameter that repeats an
 * earlier one's name, where it is (V8's ValidateFormalParameters, strict as lamassu is) */
static bool params_check(JsParser *p) {
    const JsPScope *s = &p->scopes[p->scope_count - 1];
    for (uint32_t i = s->base + 1; i < p->decl_count; i++)
        for (uint32_t j = s->base; j < i; j++)
            if (same_name(p->decls[i].name, p->decls[i].len, p->decls[j].name, p->decls[j].len)) {
                perr(p, p->decls[i].pos, "Duplicate parameter name not allowed in this context");
                return false;
            }
    return true;
}

/* At the end of a catch clause's block: a declaration of the block's own that takes the name of
 * the parameter, or of one the parameter's pattern binds (`catch (e) { let e; }`). V8 checks
 * this only once the block is read; with more than one such name it reports the one its hash
 * table gives first, lamassu the first in the source. */
static bool catch_check(JsParser *p) {
    const JsPScope *block = &p->scopes[p->scope_count - 1], *clause = block - 1;
    for (uint32_t i = block->base; i < p->decl_count; i++)
        for (uint32_t j = clause->base; j < block->base; j++)
            if (same_name(p->decls[i].name, p->decls[i].len, p->decls[j].name, p->decls[j].len))
                return fail_redeclared(p, p->decls[i].name, p->decls[i].len, p->decls[i].pos);
    return true;
}

static bool export_add(JsParser *p, const uint16_t *local, uint32_t local_len, const uint16_t *name,
                       uint32_t name_len, uint32_t pos, bool special, bool defined) {
    if (!grow_array(p, (void **)&p->exports, p->export_count, &p->export_cap, sizeof *p->exports))
        return false;
    p->exports[p->export_count++] = (JsPExport){local, name, local_len, name_len, pos, special, defined};
    return true;
}

/* The order of V8's AstRawString::Compare: code units, a prefix before the longer name */
static int name_cmp(const uint16_t *a, uint32_t alen, const uint16_t *b, uint32_t blen) {
    uint32_t n = alen < blen ? alen : blen;
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return alen == blen ? 0 : alen < blen ? -1 : 1;
}

/* Sorts V[0..N) stably by KEY, which gives each item's name (merge sort; TMP holds N) */
static void sort_by_name(JsPExport **v, JsPExport **tmp, uint32_t n, bool by_local) {
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                const JsPExport *a = v[i], *b = v[j];
                int c = by_local ? name_cmp(b->local, b->local_len, a->local, a->local_len)
                                 : name_cmp(b->name, b->name_len, a->name, a->name_len);
                tmp[k++] = c < 0 ? v[j++] : v[i++];
            }
            while (i < mid)
                tmp[k++] = v[i++];
            while (j < hi)
                tmp[k++] = v[j++];
        }
        memcpy(v, tmp, (size_t)n * sizeof *v);
    }
}

/*
 * Once the module is read, V8's SourceTextModuleDescriptor::Validate: first a name exported twice,
 * then a local export of a name the module does not declare. V8 keeps the local exports in a map
 * ordered by local name and the re-exports after them in source order; of the exports whose name
 * an earlier one in that order already took, it reports the one latest in the source (counting
 * the first taker of the first such name), and of the undeclared ones the first in that order.
 */
static bool exports_check(JsParser *p) {
    uint32_t n = p->export_count, regular = 0;
    if (!n)
        return true;
    JsPExport **order = js_arena_alloc(p->arena, (size_t)n * sizeof *order);
    JsPExport **by_name = js_arena_alloc(p->arena, (size_t)n * sizeof *by_name);
    JsPExport **tmp = js_arena_alloc(p->arena, (size_t)n * sizeof *tmp);
    JsPExport **first = js_arena_alloc(p->arena, (size_t)n * sizeof *first);
    if (!order || !by_name || !tmp || !first) {
        perr(p, 0, "out of memory");
        return false;
    }
    for (uint32_t i = 0; i < n; i++)
        if (!p->exports[i].special)
            order[regular++] = &p->exports[i];
    sort_by_name(order, tmp, regular, true);
    for (uint32_t i = 0, k = regular; i < n; i++)
        if (p->exports[i].special)
            order[k++] = &p->exports[i];
    /* the first export in that order to take each export's name */
    memcpy(by_name, order, (size_t)n * sizeof *by_name);
    sort_by_name(by_name, tmp, n, false);
    for (uint32_t i = 0; i < n; i++) {
        bool same = i && same_name(by_name[i]->name, by_name[i]->name_len, by_name[i - 1]->name,
                                   by_name[i - 1]->name_len);
        first[by_name[i] - p->exports] = same ? first[by_name[i - 1] - p->exports] : by_name[i];
    }
    const JsPExport *dup = NULL;
    for (uint32_t i = 0; i < n; i++) {
        JsPExport *e = order[i], *taker = first[e - p->exports];
        if (taker == e)
            continue;
        if (!dup)
            dup = taker;
        if (e->pos > dup->pos)
            dup = e;
    }
    if (dup) {
        perr(p, dup->pos, js_compile_error_fmt(p->arena->vm, "Duplicate export of '%U'", dup->name, dup->name_len));
        return false;
    }
    for (uint32_t i = 0; i < regular; i++) {
        const JsPExport *e = order[i];
        if (e->defined)
            continue;
        bool found = false;
        for (uint32_t d = 0; d < p->decl_count && !found; d++)
            found = same_name(p->decls[d].name, p->decls[d].len, e->local, e->local_len);
        if (!found) {
            perr(p, e->pos, js_compile_error_fmt(p->arena->vm, "Export '%U' is not defined in module", e->local, e->local_len));
            return false;
        }
    }
    return true;
}

static bool advance(JsParser *p) {
    if (p->err_msg)
        return false;
    p->prev_end = p->lx.tok.end;
    p->prev_pos = p->lx.tok.pos;
    if (p->lx.late_msg) {
        p->octal_msg = p->lx.late_msg;
        p->octal_pos = p->lx.late_pos;
        p->octal_end = p->lx.late_end;
    }
    if (!js_lexer_next(&p->lx)) {
        fail_lexer(p);
        return false;
    }
    return true;
}

static bool expect(JsParser *p, JsTokKind k, const char *msg) {
    if (p->err_msg)
        return false;
    if (TOK(p).kind != k) {
        perr_here(p, msg);
        return false;
    }
    return advance(p);
}

static bool accept(JsParser *p, JsTokKind k) {
    if (p->err_msg || TOK(p).kind != k)
        return false;
    return advance(p);
}

static JsToken peek(JsParser *p) {
    JsLexer save = p->lx;
    JsToken t;
    if (js_lexer_next(&p->lx))
        t = p->lx.tok;
    else {
        t = p->lx.tok;
        t.kind = JS_T_EOF;
    }
    p->lx = save;
    return t;
}

static JsAstNode *new_node(JsParser *p, JsAstKind kind, uint32_t pos) {
    JsAstNode *n = js_arena_alloc(p->arena, sizeof *n);
    if (!n)
        return perr(p, pos, "out of memory");
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind;
    n->pos = pos;
    return n;
}

static bool vec_push(JsParser *p, JsNodeVec *v, JsAstNode *n) {
    if (!n)
        return false;
    if (v->count == v->cap) {
        uint32_t ncap = v->cap ? v->cap * 2 : 8;
        JsAstNode **ni = js_arena_alloc(p->arena, (size_t)ncap * sizeof *ni);
        if (!ni) {
            perr(p, n->pos, "out of memory");
            return false;
        }
        if (v->count)
            memcpy(ni, v->items, v->count * sizeof *ni);
        v->items = ni;
        v->cap = ncap;
    }
    v->items[v->count++] = n;
    return true;
}

static void vec_store(JsAstNode *n, const JsNodeVec *v) {
    n->items = v->items;
    n->count = v->count;
}

static bool enter(JsParser *p) {
    if (++p->depth > JS_PARSE_MAX_DEPTH) {
        perr_here(p, "nesting too deep");
        return false;
    }
    return true;
}

static void leave(JsParser *p) { p->depth--; }

static bool tok_is_name(const JsToken *t, const char *ascii) {
    if (t->kind != JS_T_IDENT || t->escaped) /* an escaped word is never a contextual keyword */
        return false;
    for (uint32_t i = 0; i < t->u.str.len; i++) {
        if (ascii[i] == '\0' ||
            t->u.str.units[i] != (uint16_t)(unsigned char)ascii[i])
            return false;
    }
    return ascii[t->u.str.len] == '\0';
}

static bool units_is(const uint16_t *u, uint32_t len, const char *ascii) {
    for (uint32_t i = 0; i < len; i++) {
        if (ascii[i] == '\0' || u[i] != (uint16_t)(unsigned char)ascii[i])
            return false;
    }
    return ascii[len] == '\0';
}

static bool tok_is_reserved(const JsToken *t, const char *word) {
    return t->kind == JS_T_RESERVED && units_is(t->u.str.units, t->u.str.len, word);
}

/* Identifier node from the current token (consumes it). */
static bool escaped_keyword(const JsToken *t) {
    return t->kind == JS_T_IDENT && t->escaped && js_lexer_is_reserved_word(t->u.str.units, t->u.str.len);
}

/* An escaped `async` where it would start an async function: V8 refuses the escape there */
static bool escaped_async_function(JsParser *p) {
    const JsToken *t = &TOK(p);
    if (t->kind != JS_T_IDENT || !t->escaped || !units_is(t->u.str.units, t->u.str.len, "async"))
        return false;
    JsToken nx = peek(p);
    return nx.kind == JS_T_FUNCTION && !nx.newline_before;
}

static JsAstNode *parse_ident(JsParser *p, const char *msg) {
    /* a reserved word where a name belongs: V8 does not compile that either, so its words, and
     * not the ones for a statement lamassu leaves out (perr_var, perr_class) */
    if (TOK(p).kind == JS_T_RESERVED)
        return perr_here(p, unexpected_text(p));
    if (TOK(p).kind != JS_T_IDENT)
        return perr_here(p, msg);
    if (escaped_keyword(&TOK(p)))
        return perr_here(p, "Keyword must not contain escaped characters");
    JsAstNode *n = new_node(p, JS_AST_IDENT, TOK(p).pos);
    if (!n)
        return NULL;
    n->units = TOK(p).u.str.units;
    n->len = TOK(p).u.str.len;
    if (!advance(p))
        return NULL;
    return n;
}

/* A shorthand property (`{x}`, in a literal or a pattern) reads or binds the name its key
 * spells, so the key must be an identifier: `{if}` and `{let}` are V8's error for that word. */
static bool shorthand_name_ok(JsParser *p, const JsToken *key) {
    if (key->kind == JS_T_IDENT && !escaped_keyword(key))
        return true;
    perr(p, key->pos, key->kind == JS_T_IDENT ? "Keyword must not contain escaped characters"
                                              : unexpected_token_text(p, key));
    return false;
}

static JsAstNode *check_binding_name(JsParser *p, JsAstNode *ident) {
    if (!ident)
        return NULL;
    if (units_is(ident->units, ident->len, "eval") ||
        units_is(ident->units, ident->len, "arguments"))
        return perr(p, ident->pos, "cannot bind 'eval' or 'arguments' in strict mode");
    if (p->bind_kind != JS_BIND_NONE && !declare(p, ident->units, ident->len, ident->pos, p->bind_kind))
        return NULL;
    return ident;
}

/* A token that is an IdentifierName: an identifier or any reserved word */
static bool tok_is_any_name(const JsToken *t) {
    return t->kind == JS_T_IDENT || t->kind == JS_T_RESERVED ||
           (t->kind >= JS_T_AWAIT && t->kind <= JS_T_WHILE);
}

/* IdentifierName: keywords allowed (obj.default, import { default as x }). */
static JsAstNode *parse_name_any(JsParser *p, const char *msg) {
    JsToken *t = &TOK(p);
    if (!tok_is_any_name(t))
        return perr_here(p, msg);
    JsAstNode *n = new_node(p, JS_AST_IDENT, t->pos);
    if (!n)
        return NULL;
    if (t->u.str.units) {
        n->units = t->u.str.units;
        n->len = t->u.str.len;
    } else {
        n->units = p->lx.src + t->pos;
        n->len = t->end - t->pos;
    }
    if (!advance(p))
        return NULL;
    return n;
}

/* A token that can name a binding in a module: an identifier that spells no reserved word,
 * escaped or not (`await`, `let`, `yield` and `static` are reserved in every module) */
static bool tok_is_binding_name(const JsToken *t) {
    return t->kind == JS_T_IDENT && !escaped_keyword(t);
}

/* `as` and `from` where a module declaration cannot go on without them. Spelled with an escape
 * the word is there and refused, as V8 has it; anything else is the unexpected token. */
static bool expect_contextual(JsParser *p, const char *word) {
    const JsToken *t = &TOK(p);
    if (tok_is_name(t, word))
        return advance(p);
    if (t->kind == JS_T_IDENT && t->escaped && units_is(t->u.str.units, t->u.str.len, word))
        perr_here(p, word[0] == 'a' ? "'as' must not contain escaped characters"
                                    : "'from' must not contain escaped characters");
    else
        perr_here(p, word[0] == 'a' ? "expected 'as'" : "expected 'from'");
    return false;
}

/* ---- ASI ---- */

static bool expect_semicolon(JsParser *p) {
    if (p->err_msg)
        return false;
    if (TOK(p).kind == JS_T_SEMI)
        return advance(p);
    if (TOK(p).kind == JS_T_RBRACE || TOK(p).kind == JS_T_EOF ||
        TOK(p).newline_before)
        return true;
    perr_here(p, "expected ';'");
    return false;
}

static bool cover_push(JsParser *p, uint32_t pos, uint32_t end, bool proto) {
    if (p->cover_count == p->cover_cap) {
        uint32_t cap = p->cover_cap ? p->cover_cap * 2 : 8;
        struct JsCoverOpen *v = js_arena_alloc(p->arena, (size_t)cap * sizeof *v);
        if (!v) {
            perr(p, pos, "out of memory");
            return false;
        }
        if (p->cover_count)
            memcpy(v, p->cover_open, p->cover_count * sizeof *v);
        p->cover_open = v;
        p->cover_cap = cap;
    }
    p->cover_open[p->cover_count++] = (struct JsCoverOpen){pos, end, proto};
    return true;
}

/* The object literal whose first cover initializer (or second `__proto__`) is at POS became a
 * pattern */
static void cover_drop(JsParser *p, uint32_t pos) {
    for (uint32_t i = p->cover_count; i-- > 0;) {
        if (p->cover_open[i].pos == pos) {
            memmove(&p->cover_open[i], &p->cover_open[i + 1],
                    (p->cover_count - i - 1) * sizeof *p->cover_open);
            p->cover_count--;
            return;
        }
    }
}

/* False, with V8's error at the first of them, when a cover initializer (or a second
 * `__proto__`) read since MARK (the list's length then) was claimed by no pattern */
static bool cover_closed(JsParser *p, uint32_t mark) {
    if (p->err_msg)
        return false;
    if (p->cover_count > mark) {
        perr(p, p->cover_open[mark].pos,
             p->cover_open[mark].proto ? DUPLICATE_PROTO
                                       : "shorthand property initializer is only valid in destructuring");
        return false;
    }
    return true;
}

static bool check_cover_drained(JsParser *p) {
    return cover_closed(p, p->cover_base);
}

/* An element of an array literal or the value of a property that is a property access can only
 * ever be the target of an assignment, so V8 settles at once what was read inside it:
 * `[{a = 1}.b] = []` is the bad initializer, though the array goes on to be a pattern. */
static bool cover_element(JsParser *p, uint32_t mark, const JsAstNode *e) {
    if (e->kind != JS_AST_MEMBER || (e->flags & JS_F_PAREN) || !is_simple_target(e))
        return true;
    return cover_closed(p, mark);
}

/* Where the expression N starts: V8 points an error about a whole expression at its first token,
 * which for a parenthesized one is the `(`. */
static uint32_t expr_start(const JsAstNode *n) {
    for (;;) {
        if (n->flags & JS_F_PAREN)
            return n->paren_pos;
        switch (n->kind) {
        case JS_AST_MEMBER: case JS_AST_CALL: case JS_AST_TAGGED_TEMPLATE:
        case JS_AST_BINARY: case JS_AST_LOGICAL: case JS_AST_COND: case JS_AST_ASSIGN:
            n = n->a;
            break;
        case JS_AST_UPDATE:
            if (n->flags & JS_F_PREFIX)
                return n->pos;
            n = n->a;
            break;
        case JS_AST_SEQUENCE:
            n = n->items[0];
            break;
        case JS_AST_FUNC_EXPR:
            return n->src_start;
        default:
            return n->pos;
        }
    }
}

/* ---- patterns (declaration/parameter positions) ---- */

static const char INVALID_TARGET[] = "Invalid destructuring assignment target";

/* After `get`, `set` or `async` at the start of a property: the word is the property's key if
 * what follows is what follows a key, and a modifier of the key to come otherwise */
static bool prop_name_follows(JsParser *p) {
    JsToken t = peek(p);
    return t.kind != JS_T_COLON && t.kind != JS_T_LPAREN && t.kind != JS_T_COMMA &&
           t.kind != JS_T_RBRACE && t.kind != JS_T_EQ;
}

/* A property's key that is a literal, into PROP: a string, a number, or a BigInt. A BigInt names
 * the property its value does (`5n` is "5"), which is what a computed key of that constant
 * names, so that is how it is compiled. */
static bool parse_literal_key(JsParser *p, JsAstNode *prop) {
    const JsToken *t = &TOK(p);
    JsAstNode *key = new_node(p, t->kind == JS_T_STR ? JS_AST_STRING :
                                 t->kind == JS_T_NUM ? JS_AST_NUMBER : JS_AST_BIGINT, t->pos);
    if (!key)
        return false;
    if (t->kind == JS_T_NUM) {
        key->number = t->u.number;
    } else {
        key->units = t->u.str.units;
        key->len = t->u.str.len;
        if (t->kind == JS_T_BIGINT)
            prop->flags |= JS_F_COMPUTED;
    }
    prop->a = key;
    return advance(p);
}

static bool tok_is_literal_key(const JsToken *t) {
    return t->kind == JS_T_STR || t->kind == JS_T_NUM || t->kind == JS_T_BIGINT;
}

/* A key that is not a name, with no value after it (`{ 5 }`, `{ "a" = 1 }`, `{ [k], b }`): V8's
 * error names the key's first token, KEY, and points at its last. */
static bool key_without_value(JsParser *p, const JsToken *key) {
    if (TOK(p).kind != JS_T_COMMA && TOK(p).kind != JS_T_RBRACE && TOK(p).kind != JS_T_EQ)
        return false;
    perr(p, p->prev_pos, unexpected_token_text(p, key));
    return true;
}

/* `get`, `set` or `async` spelled with an escape where it would be a modifier, which is V8's
 * error at the word. Where the word is the key itself (`{ g\u0065t: 1 }`) the escape is allowed;
 * V8 takes it for the key before `;` too, and `get` and `set` before `*`. */
static bool escaped_prop_modifier(JsParser *p) {
    const JsToken *t = &TOK(p);
    if (t->kind != JS_T_IDENT || !t->escaped)
        return false;
    bool async = units_is(t->u.str.units, t->u.str.len, "async");
    if (!async && !units_is(t->u.str.units, t->u.str.len, "get") &&
        !units_is(t->u.str.units, t->u.str.len, "set"))
        return false;
    JsToken nx = peek(p);
    if (nx.kind == JS_T_SEMI || (async ? nx.newline_before : nx.kind == JS_T_STAR))
        return false;
    return prop_name_follows(p);
}

/* A property of a declaration's pattern that begins as a method does (`*`, or `get`, `set` or
 * `async` before a key): V8 reads the key, and with it knows the property is no target. The
 * current token is the first after the modifiers; START is where the property began. */
static JsAstNode *perr_pattern_method(JsParser *p, uint32_t start) {
    if (TOK(p).kind == JS_T_LBRACK) {
        if (!advance(p) || !parse_assign_scoped(p) || !expect(p, JS_T_RBRACK, "expected ']'"))
            return NULL;
    } else if (tok_is_literal_key(&TOK(p))) {
        if (!advance(p))
            return NULL;
    } else if (!parse_name_any(p, "expected property name")) {
        return NULL;
    }
    return perr(p, start, INVALID_TARGET);
}

static JsAstNode *parse_object_pattern(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* { */
        return NULL;
    JsNodeVec props = {0};
    while (TOK(p).kind != JS_T_RBRACE) {
        if (TOK(p).kind == JS_T_DOTDOTDOT) {
            JsAstNode *rest = new_node(p, JS_AST_REST, TOK(p).pos);
            if (!rest || !advance(p))
                return NULL;
            uint32_t rest_pos = TOK(p).pos;
            rest->a = parse_pattern_element(p);
            if (!rest->a)
                return NULL;
            if (rest->a->kind != JS_AST_IDENT)
                return perr(p, rest_pos, "`...` must be followed by an identifier in declaration contexts");
            if (!vec_push(p, &props, rest))
                return NULL;
            if (TOK(p).kind == JS_T_COMMA) /* V8 points at the last token of what is left over */
                return perr(p, p->prev_pos, "rest element must be last");
            break; /* rest must be last */
        }
        JsAstNode *prop = new_node(p, JS_AST_PROPERTY, TOK(p).pos);
        if (!prop)
            return NULL;
        if (escaped_prop_modifier(p))
            return perr_here(p, "Keyword must not contain escaped characters");
        /* a method is no target, and V8 says so at the property, however it begins: with `*`,
         * with `get`, `set` or `async` before a key, or (below) with a key and then `(` */
        if (TOK(p).kind == JS_T_STAR ||
            ((tok_is_name(&TOK(p), "get") || tok_is_name(&TOK(p), "set")) && prop_name_follows(p))) {
            if (!advance(p))
                return NULL;
            return perr_pattern_method(p, prop->pos);
        }
        if (tok_is_name(&TOK(p), "async") && !peek(p).newline_before && prop_name_follows(p)) {
            if (!advance(p) || (TOK(p).kind == JS_T_STAR && !advance(p)))
                return NULL;
            return perr_pattern_method(p, prop->pos);
        }
        JsToken keytok = TOK(p);
        if (TOK(p).kind == JS_T_LBRACK) {
            prop->flags |= JS_F_COMPUTED;
            if (!advance(p))
                return NULL;
            prop->a = parse_assign_scoped(p);
            if (!prop->a || !expect(p, JS_T_RBRACK, "expected ']'"))
                return NULL;
            if (TOK(p).kind == JS_T_LPAREN)
                return perr(p, prop->pos, INVALID_TARGET);
            if (key_without_value(p, &keytok) ||
                !expect(p, JS_T_COLON, "expected ':' after computed key"))
                return NULL;
            prop->b = parse_pattern_element(p);
        } else if (tok_is_literal_key(&TOK(p))) {
            if (!parse_literal_key(p, prop))
                return NULL;
            if (TOK(p).kind == JS_T_LPAREN)
                return perr(p, prop->pos, INVALID_TARGET);
            if (key_without_value(p, &keytok) ||
                !expect(p, JS_T_COLON, "expected ':' after property key"))
                return NULL;
            prop->b = parse_pattern_element(p);
        } else {
            JsAstNode *key = parse_name_any(p, "expected property name");
            if (!key)
                return NULL;
            prop->a = key;
            if (TOK(p).kind == JS_T_LPAREN)
                return perr(p, prop->pos, INVALID_TARGET);
            if (accept(p, JS_T_COLON)) {
                prop->b = parse_pattern_element(p);
            } else {
                /* shorthand: key must be a valid binding identifier */
                if (TOK(p).kind != JS_T_COMMA && TOK(p).kind != JS_T_RBRACE &&
                    TOK(p).kind != JS_T_EQ)
                    return perr_here(p, "expected ',', '}' or ':'");
                if (!shorthand_name_ok(p, &keytok))
                    return NULL;
                prop->flags |= JS_F_SHORTHAND;
                JsAstNode *target = new_node(p, JS_AST_IDENT, key->pos);
                if (!target)
                    return NULL;
                target->units = key->units;
                target->len = key->len;
                if (!check_binding_name(p, target))
                    return NULL;
                if (accept(p, JS_T_EQ)) {
                    JsAstNode *def = new_node(p, JS_AST_DEFAULT, key->pos);
                    JsAstNode *init = parse_assign_scoped(p);
                    if (!def || !init)
                        return NULL;
                    def->a = target;
                    def->b = init;
                    prop->b = def;
                } else {
                    prop->b = target;
                }
            }
        }
        if (!prop->b || !vec_push(p, &props, prop))
            return NULL;
        if (TOK(p).kind != JS_T_RBRACE && !expect(p, JS_T_COMMA, "expected ',' or '}'"))
            return NULL;
    }
    if (!expect(p, JS_T_RBRACE, "expected '}'"))
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_OBJECT_PATTERN, pos);
    if (!n)
        return NULL;
    vec_store(n, &props);
    return n;
}

static JsAstNode *parse_array_pattern(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* [ */
        return NULL;
    JsNodeVec elems = {0};
    while (TOK(p).kind != JS_T_RBRACK) {
        if (TOK(p).kind == JS_T_COMMA) {
            JsAstNode *hole = new_node(p, JS_AST_HOLE, TOK(p).pos);
            if (!hole || !vec_push(p, &elems, hole) || !advance(p))
                return NULL;
            continue;
        }
        if (TOK(p).kind == JS_T_DOTDOTDOT) {
            JsAstNode *rest = new_node(p, JS_AST_REST, TOK(p).pos);
            if (!rest || !advance(p))
                return NULL;
            rest->a = parse_pattern_element(p);
            if (!rest->a || !vec_push(p, &elems, rest))
                return NULL;
            if (rest->a->kind == JS_AST_DEFAULT) /* a rest element takes no default */
                return perr(p, rest->pos, "Invalid destructuring assignment target");
            if (TOK(p).kind == JS_T_COMMA)
                return perr(p, rest->pos, "rest element must be last");
            break;
        }
        JsAstNode *el = parse_pattern_element(p);
        if (!el || !vec_push(p, &elems, el))
            return NULL;
        if (TOK(p).kind != JS_T_RBRACK && !expect(p, JS_T_COMMA, "expected ',' or ']'"))
            return NULL;
    }
    if (!expect(p, JS_T_RBRACK, "expected ']'"))
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_ARRAY_PATTERN, pos);
    if (!n)
        return NULL;
    vec_store(n, &elems);
    return n;
}

static JsAstNode *parse_binding_target(JsParser *p) {
    if (!enter(p))
        return NULL;
    JsAstNode *n;
    bool no_in = p->no_in; /* inside brackets `in` is an operator again */
    p->no_in = false;
    switch (TOK(p).kind) {
    case JS_T_LBRACE: n = parse_object_pattern(p); break;
    case JS_T_LBRACK: n = parse_array_pattern(p); break;
    default:
        n = check_binding_name(p, parse_ident(p, "expected binding identifier"));
        break;
    }
    p->no_in = no_in;
    leave(p);
    return n;
}

static JsAstNode *binding_default(JsParser *p, JsAstNode *target) {
    if (!target)
        return NULL;
    if (accept(p, JS_T_EQ)) {
        JsAstNode *def = new_node(p, JS_AST_DEFAULT, target->pos);
        JsAstNode *init = parse_assign_scoped(p);
        if (!def || !init)
            return NULL;
        def->a = target;
        def->b = init;
        return def;
    }
    return target;
}

static JsAstNode *parse_binding_element(JsParser *p) {
    return binding_default(p, parse_binding_target(p));
}

static bool ends_pattern_element(JsTokKind k) {
    return k == JS_T_COMMA || k == JS_T_RBRACK || k == JS_T_RBRACE || k == JS_T_EQ;
}

/* An element inside a declaration's pattern, its default included. V8 reads the element as an
 * expression and then asks whether that was a binding, so what is not one is refused in its
 * words for the whole expression, and only after any error inside it: `let [a.b] = []` is an
 * illegal property, `let [0] = []` an invalid target, `let [a +] = []` the unexpected `]`.
 * A name or a pattern with nothing after it, which is every element of a valid program, is read
 * directly; only what cannot be a binding goes the way V8 goes, to be worded as V8 words it. */
static JsAstNode *parse_pattern_element(JsParser *p) {
    JsTokKind k = TOK(p).kind;
    if (k == JS_T_IDENT) {
        JsToken nx = peek(p);
        if (ends_pattern_element(nx.kind)) {
            /* `eval` as an element by itself is V8's invalid target; with a default, and as a
             * shorthand, it is the strict mode error check_binding_name gives */
            if (nx.kind != JS_T_EQ && (tok_is_name(&TOK(p), "eval") || tok_is_name(&TOK(p), "arguments")))
                return perr_here(p, "Invalid destructuring assignment target");
            return binding_default(p, parse_binding_target(p));
        }
    } else if (k == JS_T_LBRACE || k == JS_T_LBRACK) {
        JsLexer lx = p->lx;
        uint32_t prev_end = p->prev_end, prev_pos = p->prev_pos, cover_count = p->cover_count;
        uint32_t decl_count = p->decl_count;
        JsAstNode *target = parse_binding_target(p);
        if (!target)
            return NULL;
        if (ends_pattern_element(TOK(p).kind))
            return binding_default(p, target);
        p->lx = lx; /* `[a].b`, `{a} + 1`: not a pattern after all, and an error either way */
        p->prev_end = prev_end;
        p->prev_pos = prev_pos;
        p->cover_count = cover_count;
        p->decl_count = decl_count;
    }
    bool was = p->decl_element;
    int8_t bind = p->bind_kind;
    p->decl_element = true;
    p->bind_kind = JS_BIND_NONE;
    JsAstNode *e = parse_assign(p);
    p->decl_element = was;
    p->bind_kind = bind;
    if (!e)
        return NULL;
    p->pattern_depth++; /* an element: never the whole target */
    e = expr_to_pattern(p, e, true);
    p->pattern_depth--;
    return e;
}

/* The expressions in an index or a template's `${}`. Inside an element of a declaration's
 * pattern V8 asks each of them, too, to be a binding (they are read by the function that reads
 * arrow parameters), and reports at once the one that is not: `let [a[0]] = []` is an invalid
 * target where `let [a.b] = []` is an illegal property. */
static JsAstNode *parse_index_expression(JsParser *p) {
    bool no_in = p->no_in;
    p->no_in = false;
    JsAstNode *e = parse_expression_cover(p);
    p->no_in = no_in;
    if (!e || !p->decl_element)
        return e;
    JsAstNode *const *items = e->kind == JS_AST_SEQUENCE && !(e->flags & JS_F_PAREN) ? e->items : &e;
    uint32_t count = e->kind == JS_AST_SEQUENCE && !(e->flags & JS_F_PAREN) ? e->count : 1;
    for (uint32_t i = 0; i < count; i++) {
        const JsAstNode *it = items[i];
        bool binding = !(it->flags & JS_F_PAREN) &&
                       (it->kind == JS_AST_IDENT || it->kind == JS_AST_ARRAY ||
                        it->kind == JS_AST_OBJECT || it->kind == JS_AST_ASSIGN);
        if (!binding)
            return perr(p, expr_start(it), "Invalid destructuring assignment target");
    }
    return e;
}

/* ---- expression -> pattern conversion (assignment/arrow covers) ---- */

static bool is_simple_target(const JsAstNode *n) {
    if (n->kind == JS_AST_IDENT)
        return true;
    if (n->kind != JS_AST_MEMBER)
        return false;
    /* no optional chaining anywhere in the target chain */
    for (const JsAstNode *m = n; m; m = m->a) {
        if ((m->kind == JS_AST_MEMBER || m->kind == JS_AST_CALL) &&
            (m->flags & JS_F_OPTIONAL))
            return false;
        if (m->kind != JS_AST_MEMBER && m->kind != JS_AST_CALL)
            break;
    }
    return true;
}

static JsAstNode *expr_to_pattern_inner(JsParser *p, JsAstNode *n, bool binding);

static JsAstNode *expr_to_pattern(JsParser *p, JsAstNode *n, bool binding) {
    p->pattern_depth++;
    JsAstNode *r = expr_to_pattern_inner(p, n, binding);
    p->pattern_depth--;
    return r;
}

static bool is_eval_or_arguments(const JsAstNode *n) {
    return n->kind == JS_AST_IDENT &&
           (units_is(n->units, n->len, "eval") || units_is(n->units, n->len, "arguments"));
}

static const char ILLEGAL_PROPERTY[] = "Illegal property in declaration context";
static const char REST_BINDING[] = "`...` must be followed by an identifier in declaration contexts";
static const char REST_ASSIGNMENT[] =
    "`...` must be followed by an assignable reference in assignment contexts";

/* A pattern an assignment inside the expression already made (`[a.b] = []` as an element), now
 * to bind: what an assignment may store to and a binding may not is refused. */
static bool pattern_as_binding(JsParser *p, const JsAstNode *n) {
    switch (n->kind) {
    case JS_AST_IDENT:
        if (n->flags & JS_F_PAREN) {
            perr(p, expr_start(n), INVALID_TARGET);
            return false;
        }
        return true;
    case JS_AST_MEMBER:
        perr(p, expr_start(n), ILLEGAL_PROPERTY);
        return false;
    case JS_AST_DEFAULT:
    case JS_AST_REST:
        return pattern_as_binding(p, n->a);
    case JS_AST_PROPERTY:
        return pattern_as_binding(p, n->b);
    case JS_AST_ARRAY_PATTERN:
    case JS_AST_OBJECT_PATTERN:
        for (uint32_t i = 0; i < n->count; i++) {
            if (n->items[i]->kind != JS_AST_HOLE && !pattern_as_binding(p, n->items[i]))
                return false;
        }
        return true;
    default:
        return true;
    }
}

/* The expression N as a pattern: one that binds (arrow parameters, an element of a declaration
 * read as an expression) or one that assigns. The errors are V8's, in the order V8 meets them:
 * an element's own after those inside it. */
static JsAstNode *expr_to_pattern_inner(JsParser *p, JsAstNode *n, bool binding) {
    if (!n)
        return NULL;
    bool nested = p->pattern_depth > 1; /* an element, not the whole target */
    switch (n->kind) {
    case JS_AST_IDENT:
        if (binding && (n->flags & JS_F_PAREN))
            return perr(p, expr_start(n), INVALID_TARGET);
        /* `eval` as an element is V8's invalid target; as the whole target, with a default
         * (refused where the default was read) and as a shorthand it is the strict mode error */
        if (is_eval_or_arguments(n))
            return perr(p, n->pos, nested ? INVALID_TARGET
                                          : "cannot assign to 'eval' or 'arguments' in strict mode");
        return n;
    case JS_AST_MEMBER:
        if (!is_simple_target(n))
            return perr(p, expr_start(n), nested || binding ? INVALID_TARGET
                                                            : "optional chain is not a valid assignment target");
        if (binding)
            return perr(p, expr_start(n), nested ? ILLEGAL_PROPERTY : INVALID_TARGET);
        return n;
    case JS_AST_ASSIGN: {
        if (n->op != JS_T_EQ || (n->flags & JS_F_PAREN))
            return perr(p, expr_start(n), "invalid destructuring target");
        JsAstNode *def = new_node(p, JS_AST_DEFAULT, n->pos);
        if (!def)
            return NULL;
        def->a = expr_to_pattern(p, n->a, binding);
        def->b = n->b;
        return def->a ? def : NULL;
    }
    case JS_AST_ARRAY: {
        if (n->flags & JS_F_PAREN)
            return perr(p, expr_start(n), nested || binding ? INVALID_TARGET : "parenthesized destructuring pattern");
        JsAstNode *pat = new_node(p, JS_AST_ARRAY_PATTERN, n->pos);
        if (!pat)
            return NULL;
        for (uint32_t i = 0; i < n->count; i++) {
            JsAstNode *el = n->items[i];
            if (el->kind == JS_AST_HOLE)
                continue;
            if (el->kind == JS_AST_SPREAD) {
                JsAstNode *rest = new_node(p, JS_AST_REST, el->pos);
                if (!rest)
                    return NULL;
                rest->a = expr_to_pattern(p, el->a, binding);
                if (!rest->a)
                    return NULL;
                if (rest->a->kind == JS_AST_DEFAULT) /* a rest element takes no default */
                    return perr(p, el->pos, INVALID_TARGET);
                if (i + 1 != n->count || (el->flags & JS_F_SPREAD_COMMA))
                    return perr(p, el->pos, "rest element must be last");
                n->items[i] = rest;
                continue;
            }
            n->items[i] = expr_to_pattern(p, el, binding);
            if (!n->items[i])
                return NULL;
        }
        vec_store(pat, &(JsNodeVec){.items = n->items, .count = n->count});
        return pat;
    }
    case JS_AST_OBJECT: {
        if (n->flags & JS_F_PAREN)
            return perr(p, expr_start(n), nested || binding ? INVALID_TARGET : "parenthesized destructuring pattern");
        JsAstNode *pat = new_node(p, JS_AST_OBJECT_PATTERN, n->pos);
        if (!pat)
            return NULL;
        bool had_cover = false;
        int protos = 0;
        for (uint32_t i = 0; i < n->count; i++) {
            JsAstNode *prop = n->items[i];
            if (prop->kind != JS_AST_SPREAD && (prop->flags & JS_F_PROTO)) {
                /* in a pattern `__proto__` is a key like any other, as often as it likes */
                prop->flags &= (uint8_t)~JS_F_PROTO;
                if (++protos == 2) /* the literal's second: the one it is listed by */
                    cover_drop(p, prop->a->pos);
            }
            if (prop->kind == JS_AST_SPREAD) {
                JsAstNode *rest = new_node(p, JS_AST_REST, prop->pos);
                if (!rest)
                    return NULL;
                rest->a = expr_to_pattern(p, prop->a, binding);
                if (!rest->a)
                    return NULL;
                /* what is left of an object goes to a name (or, assigning, to a property) */
                if (rest->a->kind != JS_AST_IDENT && rest->a->kind != JS_AST_MEMBER)
                    return perr(p, rest->a->pos, binding ? REST_BINDING : REST_ASSIGNMENT);
                if (i + 1 != n->count || (prop->flags & JS_F_SPREAD_COMMA))
                    return perr(p, prop->pos2, "rest element must be last"); /* its last token */
                n->items[i] = rest;
                continue;
            }
            if (prop->flags & (JS_F_METHOD | JS_F_PROP_ASYNC))
                return perr(p, prop->pos, "method is not a valid destructuring target");
            if (prop->flags & JS_F_COVER_INIT) {
                prop->flags &= (uint8_t)~JS_F_COVER_INIT;
                if (!had_cover) /* the literal's first: the one it is listed by */
                    cover_drop(p, prop->a->pos);
                had_cover = true;
                /* value is already DEFAULT(ident, init) */
                if (is_eval_or_arguments(prop->b->a))
                    return perr(p, prop->b->a->pos, "cannot assign to 'eval' or 'arguments' in strict mode");
                continue;
            }
            if ((prop->flags & JS_F_SHORTHAND) && is_eval_or_arguments(prop->b))
                return perr(p, prop->b->pos, "cannot assign to 'eval' or 'arguments' in strict mode");
            prop->b = expr_to_pattern(p, prop->b, binding);
            if (!prop->b)
                return NULL;
        }
        vec_store(pat, &(JsNodeVec){.items = n->items, .count = n->count});
        return pat;
    }
    case JS_AST_ARRAY_PATTERN:
    case JS_AST_OBJECT_PATTERN:
    case JS_AST_DEFAULT:
    case JS_AST_REST:
        /* already a pattern: the left side of an assignment inside the expression */
        return !binding || pattern_as_binding(p, n) ? n : NULL;
    default:
        return perr(p, expr_start(n), nested || binding ? "invalid destructuring target" : "invalid assignment target");
    }
}

/* ---- arrow parameters: what V8 calls declaration errors ---- */

/* Reading an arrow's parameters V8 does not yet know they are parameters, so it notes what
 * would be wrong were they a declaration and reports, once the `=>` is seen, the LAST such note
 * before any error of the pattern itself: `([0, a.b]) => 1` is the illegal property, and
 * `([a.b], (c)) => 1` the invalid target. This walks the parameters for that last note. */
typedef struct {
    const char *msg;
    uint32_t pos;
} JsDeclError;

static void decl_error_element(const JsAstNode *n, JsDeclError *e);

static void decl_error_target(const JsAstNode *t, JsDeclError *e) {
    switch (t->kind) {
    case JS_AST_IDENT:
        if (t->flags & JS_F_PAREN)
            *e = (JsDeclError){INVALID_TARGET, expr_start(t)};
        break;
    case JS_AST_MEMBER:
        if (is_simple_target(t))
            *e = (JsDeclError){ILLEGAL_PROPERTY, expr_start(t)};
        break;
    case JS_AST_ARRAY:
    case JS_AST_OBJECT:
    case JS_AST_ARRAY_PATTERN:
    case JS_AST_OBJECT_PATTERN:
        if (t->flags & JS_F_PAREN)
            break; /* parentheses are a scope of their own: its notes went with it */
        for (uint32_t i = 0; i < t->count; i++) {
            const JsAstNode *it = t->items[i];
            if (it->kind == JS_AST_HOLE)
                continue;
            if (it->kind == JS_AST_PROPERTY) {
                if (!(it->flags & (JS_F_COVER_INIT | JS_F_METHOD | JS_F_PROP_ASYNC)))
                    decl_error_element(it->b, e);
                continue;
            }
            if (it->kind != JS_AST_SPREAD && it->kind != JS_AST_REST) {
                decl_error_element(it, e);
                continue;
            }
            decl_error_element(it->a, e);
            bool object = t->kind == JS_AST_OBJECT || t->kind == JS_AST_OBJECT_PATTERN;
            bool reference = (it->a->kind == JS_AST_IDENT && !is_eval_or_arguments(it->a)) ||
                             (it->a->kind == JS_AST_MEMBER && is_simple_target(it->a));
            if (object && !reference)
                *e = (JsDeclError){REST_BINDING, it->a->pos};
        }
        break;
    default:
        break;
    }
}

static void decl_error_element(const JsAstNode *n, JsDeclError *e) {
    if ((n->kind == JS_AST_ASSIGN && !(n->flags & JS_F_PAREN)) || n->kind == JS_AST_DEFAULT)
        n = n->a; /* an assignment of any kind notes what its left side is */
    decl_error_target(n, e);
}

static bool arrow_declaration_errors(JsParser *p, const JsAstNode *cover) {
    JsDeclError e = {0};
    for (uint32_t i = 0; i < cover->count; i++) {
        const JsAstNode *it = cover->items[i];
        const JsAstNode *spread = it->kind == JS_AST_SPREAD ? it : NULL;
        if (spread)
            it = it->a;
        decl_error_element(it, &e);
        bool binding = it->kind == JS_AST_IDENT || it->kind == JS_AST_ASSIGN ||
                       it->kind == JS_AST_ARRAY || it->kind == JS_AST_OBJECT;
        if (!binding || (it->flags & JS_F_PAREN))
            e = (JsDeclError){INVALID_TARGET, expr_start(it)};
        /* an async arrow's `...` was read as a call's spread argument: what is wrong with it
         * as a rest parameter is two more notes (the other arrows report them as they read) */
        if (spread && (cover->flags & JS_F_COVER_ASYNC)) {
            if (it->kind == JS_AST_ASSIGN) /* at the last token of the argument */
                e = (JsDeclError){"Rest parameter may not have a default initializer", spread->pos2};
            if (spread->flags & JS_F_SPREAD_COMMA) /* at the comma */
                e = (JsDeclError){"rest parameter must be last", spread->src_start};
        }
    }
    if (e.msg)
        perr(p, e.pos, e.msg);
    return !e.msg;
}

/* ---- cover grammar: ( ... ) ---- */

static JsAstNode *parse_array_literal(JsParser *p);
static JsAstNode *parse_object_literal(JsParser *p);

static JsAstNode *parse_paren_cover_inner(JsParser *p, uint8_t extra_flags);

static JsAstNode *parse_paren_cover(JsParser *p, uint8_t extra_flags) {
    bool was = p->decl_element; /* what is in parentheses is no part of a declaration's pattern */
    bool no_in = p->no_in;
    p->decl_element = false;
    p->no_in = false;
    JsAstNode *cover = parse_paren_cover_inner(p, extra_flags);
    p->decl_element = was;
    p->no_in = no_in;
    return cover;
}

static JsAstNode *parse_paren_cover_inner(JsParser *p, uint8_t extra_flags) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* ( */
        return NULL;
    JsAstNode *cover = new_node(p, JS_AST_COVER, pos);
    if (!cover)
        return NULL;
    cover->flags = extra_flags;
    cover->src_start = pos; /* an async arrow's caller moves it back to `async` */
    cover->len = p->cover_count; /* the cover initializers from here on are this cover's */
    JsNodeVec items = {0};
    while (TOK(p).kind != JS_T_RPAREN) {
        if (TOK(p).kind == JS_T_DOTDOTDOT) {
            cover->flags |= JS_F_COVER_SPREAD;
            JsAstNode *sp = new_node(p, JS_AST_SPREAD, TOK(p).pos);
            if (!sp || !advance(p))
                return NULL;
            if (extra_flags & JS_F_COVER_ASYNC) {
                sp->a = parse_assign(p); /* an argument of a call to `async`, until a `=>` */
                sp->pos2 = p->prev_pos;  /* where V8 points an arrow's errors about it: the */
                sp->src_start = TOK(p).pos; /* argument's last token, and the comma after it */
                if (sp->a && TOK(p).kind == JS_T_COMMA)
                    sp->flags |= JS_F_SPREAD_COMMA;
            } else {
                /* `...` in parentheses is only ever an arrow's rest parameter, and V8 reads it
                 * as one: a name or a pattern, no default, nothing after it but `) =>` */
                if (TOK(p).kind == JS_T_IDENT)
                    sp->a = check_binding_name(p, parse_ident(p, "expected parameter"));
                else if (TOK(p).kind == JS_T_LBRACK)
                    sp->a = parse_array_literal(p);
                else if (TOK(p).kind == JS_T_LBRACE)
                    sp->a = parse_object_literal(p);
                else
                    return perr_here(p, unexpected_text(p));
                if (!sp->a)
                    return NULL;
                if (TOK(p).kind == JS_T_EQ) /* V8 points both at the parameter's last token */
                    return perr(p, p->prev_pos, "Rest parameter may not have a default initializer");
                if (TOK(p).kind == JS_T_COMMA)
                    return perr(p, p->prev_pos, "rest parameter must be last");
                if (TOK(p).kind != JS_T_RPAREN || peek(p).kind != JS_T_ARROW)
                    return perr(p, sp->pos, "Unexpected token '...'");
            }
            if (!sp->a || !vec_push(p, &items, sp))
                return NULL;
        } else {
            JsAstNode *e = parse_assign(p);
            if (!e || !vec_push(p, &items, e))
                return NULL;
        }
        if (TOK(p).kind != JS_T_RPAREN) {
            if (!expect(p, JS_T_COMMA, "expected ',' or ')'"))
                return NULL;
            if (TOK(p).kind == JS_T_RPAREN) {
                cover->flags |= JS_F_COVER_TRAIL;
                break;
            }
        }
    }
    cover->pos2 = TOK(p).pos; /* the `)` */
    if (!expect(p, JS_T_RPAREN, "expected ')'"))
        return NULL;
    vec_store(cover, &items);
    return cover;
}

/* V8's position for a call: the callee's own name when the token before `(` is one (`f(`,
 * `o.m(`), else the `(` itself (`a[0](`, `f()(`, `(function () {})(`) */
static uint32_t call_pos(const JsAstNode *callee, uint32_t lparen) {
    if (callee->flags & JS_F_PAREN)
        return lparen;
    if (callee->kind == JS_AST_IDENT)
        return callee->pos;
    if (callee->kind == JS_AST_MEMBER && !(callee->flags & JS_F_COMPUTED) && callee->pos2)
        return callee->pos2;
    return lparen;
}

/* A cover that is not followed by `=>`: must be a plain parenthesized expr. */
static JsAstNode *collapse_cover(JsParser *p, JsAstNode *cover) {
    if (cover->flags & JS_F_COVER_ASYNC) {
        /* `async (...)` not followed by => is a call to an `async` binding */
        JsAstNode *callee = new_node(p, JS_AST_IDENT, cover->src_start);
        JsAstNode *call = new_node(p, JS_AST_CALL, cover->pos);
        if (call)
            call->pos2 = cover->src_start; /* `async` */
        static const uint16_t async_name[] = {'a', 's', 'y', 'n', 'c'};
        if (!callee || !call)
            return NULL;
        callee->units = async_name;
        callee->len = 5;
        call->a = callee;
        call->items = cover->items;
        call->count = cover->count;
        /* spread items are valid as call arguments; a cover initializer no longer is */
        return cover_closed(p, cover->len) ? call : NULL;
    }
    if (cover->count == 0) /* `()` and `(a,)` are an arrow's alone: V8 stops at the `)` */
        return perr(p, cover->pos2, "Unexpected token ')'");
    if (cover->flags & JS_F_COVER_SPREAD)
        return perr(p, cover->pos, "'...' is only valid in arrow parameters here");
    if (cover->flags & JS_F_COVER_TRAIL)
        return perr(p, cover->pos2, "Unexpected token ')'");
    if (!cover_closed(p, cover->len)) /* nor can what is in parentheses become a pattern */
        return NULL;
    if (cover->count == 1) {
        JsAstNode *e = cover->items[0];
        e->flags |= JS_F_PAREN;
        e->paren_pos = cover->pos;
        return e;
    }
    JsAstNode *seq = new_node(p, JS_AST_SEQUENCE, cover->pos);
    if (!seq)
        return NULL;
    seq->items = cover->items;
    seq->count = cover->count;
    seq->flags |= JS_F_PAREN;
    seq->paren_pos = cover->pos;
    return seq;
}

/* ---- function bodies / arrows ---- */

typedef struct {
    bool in_function, fn_async, new_target_ok, super_ok, decl_element, no_in;
    int loop_depth, switch_depth;
    uint32_t label_base, cover_base;
} JsSavedCtx;

static JsSavedCtx ctx_enter_function(JsParser *p, bool is_async, bool arrow) {
    JsSavedCtx s = {p->in_function, p->fn_async, p->new_target_ok, p->super_ok, p->decl_element,
                    p->no_in, p->loop_depth, p->switch_depth, p->label_base, p->cover_base};
    p->decl_element = false;
    p->no_in = false;
    p->cover_base = p->cover_count; /* what is open around the function is not its statements' */
    p->label_base = p->label_count; /* a label does not reach into a function */
    p->pending_labels = 0;
    p->pending_own = 0;
    p->in_function = true;
    p->new_target_ok = p->new_target_ok || !arrow;
    p->super_ok = p->super_ok && arrow; /* a method's caller says so after this */
    p->fn_async = is_async;
    p->fn_nesting++;
    p->loop_depth = 0;
    p->switch_depth = 0;
    return s;
}

static void ctx_leave_function(JsParser *p, JsSavedCtx s) {
    p->in_function = s.in_function;
    p->new_target_ok = s.new_target_ok;
    p->super_ok = s.super_ok;
    p->fn_async = s.fn_async;
    p->decl_element = s.decl_element;
    p->no_in = s.no_in;
    p->cover_base = s.cover_base;
    p->fn_nesting--;
    p->loop_depth = s.loop_depth;
    p->switch_depth = s.switch_depth;
    p->label_base = s.label_base;
}

static JsAstNode *parse_params_inner(JsParser *p, JsNodeVec *out);

/* A function's parameter list, read into the function's scope (opened here: the caller reads
 * the body into it and closes it), and checked once it is read */
static JsAstNode *parse_params(JsParser *p, JsNodeVec *out) {
    bool no_in = p->no_in;
    int8_t bind = p->bind_kind;
    p->no_in = false;
    if (!scope_push(p, JS_SCOPE_FUNC))
        return NULL;
    p->bind_kind = JS_BIND_PARAM;
    JsAstNode *r = parse_params_inner(p, out);
    p->no_in = no_in;
    p->bind_kind = bind;
    return r && params_check(p) ? r : NULL;
}

static JsAstNode *parse_params_inner(JsParser *p, JsNodeVec *out) {
    if (!expect(p, JS_T_LPAREN, "expected '('"))
        return NULL;
    while (TOK(p).kind != JS_T_RPAREN) {
        if (TOK(p).kind == JS_T_DOTDOTDOT) {
            JsAstNode *rest = new_node(p, JS_AST_REST, TOK(p).pos);
            if (!rest || !advance(p))
                return NULL;
            rest->a = parse_binding_target(p);
            if (!rest->a || !vec_push(p, out, rest))
                return NULL;
            if (TOK(p).kind == JS_T_EQ)
                return perr_here(p, "Rest parameter may not have a default initializer");
            if (TOK(p).kind == JS_T_COMMA)
                return perr_here(p, "rest parameter must be last");
            break;
        }
        JsAstNode *el = parse_binding_element(p);
        if (!el || !vec_push(p, out, el))
            return NULL;
        if (TOK(p).kind != JS_T_RPAREN && !expect(p, JS_T_COMMA, "expected ',' or ')'"))
            return NULL;
    }
    if (!expect(p, JS_T_RPAREN, "expected ')'"))
        return NULL;
    return (JsAstNode *)1; /* success marker; params are in *out */
}

/* SRC_START: where the function's text begins (`function`, or `async` before it) */
static JsAstNode *parse_func_common(JsParser *p, uint8_t flags, bool need_name,
                                    uint32_t pos, uint8_t kind, uint32_t src_start) {
    JsAstNode *fn = new_node(p, kind, pos);
    if (!fn)
        return NULL;
    fn->flags = flags;
    /* `function*` is a generator, which lamassu leaves out. Its head is read as any function's
     * is, as far as the `(`, so what V8 would refuse in it is refused in V8's words; a head V8
     * would go on from is the feature, and the error names it. */
    bool generator = TOK(p).kind == JS_T_STAR;
    uint32_t star_pos = TOK(p).pos;
    if (generator && !advance(p))
        return NULL;
    /* A declaration declares its name where it is read, before the parameters and the body;
     * V8 points a clash at `function` (or `async`). `export default function` without a name
     * declares the hidden binding of the default export there. */
    bool declares = kind == JS_AST_FUNC_DECL || p->default_fn;
    bool default_fn = p->default_fn;
    int8_t bind = p->bind_kind;
    p->default_fn = false;
    p->bind_kind = JS_BIND_NONE;
    if (TOK(p).kind == JS_T_IDENT) {
        JsAstNode *name = check_binding_name(p, parse_ident(p, "expected name"));
        if (!name)
            return NULL;
        fn->units = name->units;
        fn->len = name->len;
        if (declares && !declare(p, name->units, name->len, src_start, function_kind(p)))
            return NULL;
    } else if (default_fn) {
        static const uint16_t dot_default[] = {'.', 'd', 'e', 'f', 'a', 'u', 'l', 't'};
        if (!declare(p, dot_default, sizeof dot_default / sizeof dot_default[0], src_start, JS_BIND_LEX))
            return NULL;
    } else if (need_name) {
        /* V8 has words of its own only for the name left out, and points them at `function` */
        if (TOK(p).kind == JS_T_LPAREN)
            return perr(p, p->prev_pos, "Function statements require a function name");
        return perr_here(p, unexpected_text(p));
    }
    if (generator) {
        if (TOK(p).kind != JS_T_LPAREN)
            return perr_here(p, unexpected_text(p));
        return perr(p, star_pos, "generators are not supported");
    }
    JsNodeVec params = {0};
    bool super_ok = p->super_ok; /* the parameters' defaults are the function's, not the method's around it */
    p->super_ok = false;
    if (!parse_params(p, &params))
        return NULL;
    vec_store(fn, &params);
    JsSavedCtx s = ctx_enter_function(p, (flags & JS_F_ASYNC) != 0, false);
    p->body_block = true;
    fn->a = parse_block(p);
    ctx_leave_function(p, s);
    scope_pop(p);
    p->super_ok = super_ok;
    p->bind_kind = bind;
    fn->src_start = src_start;
    fn->src_end = p->prev_end;
    return fn->a && octal_check(p, src_start) ? fn : NULL;
}

/* SRC_START: where the arrow's text begins (its parameters, or `async` before them) */
static JsAstNode *build_arrow(JsParser *p, JsAstNode *params_src, bool is_async, uint32_t src_start) {
    /* current token is `=>` */
    if (TOK(p).newline_before)
        return perr_here(p, "no line break allowed before '=>'");
    uint32_t pos = params_src->pos;
    JsAstNode *fn = new_node(p, JS_AST_FUNC_EXPR, pos);
    /* the parameters are patterns from the `=>` on, before anything after it is read: what
     * was read with a cover initializer in them is claimed */
    if (params_src->kind == JS_AST_COVER)
        p->cover_count = params_src->len;
    if (!fn || !advance(p))
        return NULL;
    fn->flags = (uint8_t)(JS_F_ARROW | (is_async ? JS_F_ASYNC : 0));

    JsNodeVec params = {0};
    if (params_src->kind == JS_AST_IDENT) {
        JsAstNode *id = expr_to_pattern(p, params_src, true);
        if (!id || !check_binding_name(p, id) || !vec_push(p, &params, id))
            return NULL;
    } else { /* COVER */
        if (!arrow_declaration_errors(p, params_src))
            return NULL;
        for (uint32_t i = 0; i < params_src->count; i++) {
            JsAstNode *item = params_src->items[i];
            if (item->kind == JS_AST_SPREAD) {
                JsAstNode *rest = new_node(p, JS_AST_REST, item->pos);
                if (!rest)
                    return NULL;
                rest->a = expr_to_pattern(p, item->a, true);
                if (!rest->a || !vec_push(p, &params, rest))
                    return NULL;
                /* only `async (...)`, read as a call's arguments, gets this far with these */
                if (rest->a->kind == JS_AST_DEFAULT)
                    return perr(p, item->a->pos, "Rest parameter may not have a default initializer");
                if (i + 1 != params_src->count || (params_src->flags & JS_F_COVER_TRAIL))
                    return perr(p, item->pos, "rest parameter must be last");
                continue;
            }
            JsAstNode *pat = expr_to_pattern(p, item, true);
            if (!pat || !vec_push(p, &params, pat))
                return NULL;
        }
    }
    vec_store(fn, &params);
    if (!scope_push(p, JS_SCOPE_FUNC))
        return NULL;
    for (uint32_t i = 0; i < params.count; i++)
        if (!declare_pattern(p, params.items[i], JS_BIND_PARAM))
            return NULL;
    if (!params_check(p))
        return NULL;

    JsSavedCtx s = ctx_enter_function(p, is_async, true);
    int8_t bind = p->bind_kind;
    p->bind_kind = JS_BIND_NONE;
    if (TOK(p).kind == JS_T_LBRACE) {
        p->body_block = true;
        fn->a = parse_block(p);
    } else {
        fn->flags |= JS_F_EXPR_BODY;
        p->no_in = s.no_in; /* an expression body is still inside the `for (` head it is in */
        fn->a = parse_assign_scoped(p);
    }
    p->bind_kind = bind;
    ctx_leave_function(p, s);
    scope_pop(p);
    fn->src_start = src_start;
    fn->src_end = p->prev_end;
    return fn->a && octal_check(p, src_start) ? fn : NULL;
}

/* ---- primary / postfix ---- */

/* A template literal; TAGGED: its chunks keep their raw text, and one whose escape does not cook
 * is marked (JS_F_BAD_COOK) rather than an error */
static JsAstNode *parse_template_as(JsParser *p, bool tagged) {
    JsAstNode *tpl = new_node(p, JS_AST_TEMPLATE, TOK(p).pos);
    if (!tpl)
        return NULL;
    JsNodeVec parts = {0};
    for (;;) {
        if (TOK(p).u.str.bad_escape && !tagged)
            return perr(p, TOK(p).u.str.bad_pos, TOK(p).u.str.bad_escape);
        JsAstNode *chunk = new_node(p, JS_AST_STRING, TOK(p).pos);
        if (!chunk)
            return NULL;
        chunk->units = TOK(p).u.str.units;
        chunk->len = TOK(p).u.str.len;
        chunk->units2 = TOK(p).u.str.raw;
        chunk->len2 = TOK(p).u.str.raw_len;
        if (TOK(p).u.str.bad_escape)
            chunk->flags |= JS_F_BAD_COOK;
        bool cont = TOK(p).u.str.cont;
        if (!vec_push(p, &parts, chunk))
            return NULL;
        if (!cont)
            break;
        if (!advance(p))
            return NULL;
        JsAstNode *e = parse_index_expression(p);
        if (!e || !vec_push(p, &parts, e))
            return NULL;
        if (TOK(p).kind != JS_T_RBRACE)
            return perr_here(p, "expected '}' in template literal");
        if (!js_lexer_rescan_template(&p->lx)) {
            fail_lexer(p);
            return NULL;
        }
    }
    if (!advance(p)) /* past final chunk */
        return NULL;
    vec_store(tpl, &parts);
    return tpl;
}

static JsAstNode *parse_template(JsParser *p) {
    return parse_template_as(p, false);
}

static JsAstNode *parse_array_literal_inner(JsParser *p);
static JsAstNode *parse_object_literal_inner(JsParser *p);

static JsAstNode *parse_array_literal(JsParser *p) {
    bool no_in = p->no_in;
    p->no_in = false;
    JsAstNode *n = parse_array_literal_inner(p);
    p->no_in = no_in;
    return n;
}

static JsAstNode *parse_object_literal(JsParser *p) {
    bool no_in = p->no_in;
    p->no_in = false;
    JsAstNode *n = parse_object_literal_inner(p);
    p->no_in = no_in;
    return n;
}

static JsAstNode *parse_array_literal_inner(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* [ */
        return NULL;
    JsNodeVec elems = {0};
    while (TOK(p).kind != JS_T_RBRACK) {
        if (TOK(p).kind == JS_T_COMMA) {
            JsAstNode *hole = new_node(p, JS_AST_HOLE, TOK(p).pos);
            if (!hole || !vec_push(p, &elems, hole) || !advance(p))
                return NULL;
            continue;
        }
        JsAstNode *e;
        uint32_t mark = p->cover_count;
        if (TOK(p).kind == JS_T_DOTDOTDOT) {
            e = new_node(p, JS_AST_SPREAD, TOK(p).pos);
            if (!e || !advance(p))
                return NULL;
            e->a = parse_assign(p);
            if (!e->a || !cover_element(p, mark, e->a))
                return NULL;
            if (TOK(p).kind == JS_T_COMMA)
                e->flags |= JS_F_SPREAD_COMMA;
        } else {
            e = parse_assign(p);
            if (e && !cover_element(p, mark, e))
                return NULL;
        }
        if (!e || !vec_push(p, &elems, e))
            return NULL;
        if (TOK(p).kind != JS_T_RBRACK && !expect(p, JS_T_COMMA, "expected ',' or ']'"))
            return NULL;
    }
    if (!expect(p, JS_T_RBRACK, "expected ']'"))
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_ARRAY, pos);
    if (!n)
        return NULL;
    vec_store(n, &elems);
    return n;
}

static JsAstNode *parse_object_literal_inner(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* { */
        return NULL;
    JsNodeVec props = {0};
    bool has_cover_init = false;
    int protos = 0;
    while (TOK(p).kind != JS_T_RBRACE) {
        uint32_t mark = p->cover_count;
        if (TOK(p).kind == JS_T_DOTDOTDOT) {
            JsAstNode *sp = new_node(p, JS_AST_SPREAD, TOK(p).pos);
            if (!sp || !advance(p))
                return NULL;
            sp->a = parse_assign(p);
            if (!sp->a || !cover_element(p, mark, sp->a) || !vec_push(p, &props, sp))
                return NULL;
            sp->pos2 = p->prev_pos; /* its last token: where V8 says it must be last */
            if (TOK(p).kind == JS_T_COMMA)
                sp->flags |= JS_F_SPREAD_COMMA;
        } else {
            JsAstNode *prop = new_node(p, JS_AST_PROPERTY, TOK(p).pos);
            if (!prop)
                return NULL;
            /* `get` or `set` before a key is an accessor and `*` before one a generator method,
             * both of which lamassu leaves out. Either is read as V8 reads it, as far as the
             * `(` after its key: what V8 would refuse before that is refused in V8's words,
             * and a head V8 would go on from is the feature, which the error names. */
            const char *left_out = NULL;
            uint32_t left_out_pos = TOK(p).pos;
            if (escaped_prop_modifier(p))
                return perr_here(p, "Keyword must not contain escaped characters");
            if ((tok_is_name(&TOK(p), "get") || tok_is_name(&TOK(p), "set")) &&
                prop_name_follows(p)) {
                left_out = "getter/setter properties are not supported";
                if (!advance(p))
                    return NULL;
            } else {
                if (tok_is_name(&TOK(p), "async") && !peek(p).newline_before &&
                    prop_name_follows(p)) {
                    prop->flags |= JS_F_PROP_ASYNC;
                    if (!advance(p))
                        return NULL;
                }
                if (TOK(p).kind == JS_T_STAR) {
                    left_out = "generators are not supported";
                    left_out_pos = TOK(p).pos;
                    if (!advance(p))
                        return NULL;
                }
            }
            /* key */
            JsToken keytok = TOK(p);
            if (TOK(p).kind == JS_T_LBRACK) {
                prop->flags |= JS_F_COMPUTED;
                if (!advance(p))
                    return NULL;
                prop->a = parse_assign_scoped(p);
                if (!prop->a || !expect(p, JS_T_RBRACK, "expected ']'"))
                    return NULL;
            } else if (tok_is_literal_key(&TOK(p))) {
                if (!parse_literal_key(p, prop))
                    return NULL;
            } else {
                prop->a = parse_name_any(p, "expected property name");
                if (!prop->a)
                    return NULL;
            }
            if (left_out) {
                if (TOK(p).kind != JS_T_LPAREN)
                    return perr_here(p, unexpected_text(p));
                return perr(p, left_out_pos, left_out);
            }
            uint32_t key_end = p->prev_end;
            /* value */
            if (TOK(p).kind == JS_T_LPAREN) { /* method */
                prop->flags |= JS_F_METHOD;
                JsAstNode *fn = new_node(p, JS_AST_FUNC_EXPR, prop->a->pos);
                if (!fn)
                    return NULL;
                fn->flags = (uint8_t)(JS_F_METHOD_FN | ((prop->flags & JS_F_PROP_ASYNC) ? JS_F_ASYNC : 0));
                JsNodeVec params = {0};
                uint32_t params_pos = TOK(p).pos;
                bool super_ok = p->super_ok; /* a method may say `super.x`, from its parameters on */
                p->super_ok = true;
                if (!parse_params(p, &params))
                    return NULL;
                vec_store(fn, &params);
                JsSavedCtx s = ctx_enter_function(p, (fn->flags & JS_F_ASYNC) != 0, false);
                p->super_ok = true;
                p->body_block = true;
                fn->a = parse_block(p);
                ctx_leave_function(p, s);
                scope_pop(p);
                p->super_ok = super_ok;
                if (!fn->a || !octal_check(p, params_pos))
                    return NULL;
                fn->src_start = prop->pos; /* a method's text starts at its key (or `async`) */
                fn->src_end = p->prev_end;
                prop->b = fn;
            } else if (prop->flags & JS_F_PROP_ASYNC) {
                return perr_here(p, "expected '(' after async method name");
            } else if (accept(p, JS_T_COLON)) {
                /* `__proto__: v`, the key a name or a string and not computed, is no property:
                 * it sets the prototype. Once in a literal: V8 notes a second where its key is
                 * and reports it unless the literal turns out to be a pattern. */
                if (!(prop->flags & JS_F_COMPUTED) && prop->a->kind != JS_AST_NUMBER &&
                    units_is(prop->a->units, prop->a->len, "__proto__")) {
                    prop->flags |= JS_F_PROTO;
                    if (++protos == 2 && !cover_push(p, keytok.pos, key_end, true))
                        return NULL;
                }
                mark = p->cover_count;
                prop->b = parse_assign(p);
                if (prop->b && !cover_element(p, mark, prop->b))
                    return NULL;
            } else if (!(prop->flags & JS_F_COMPUTED) &&
                       prop->a->kind == JS_AST_IDENT) {
                /* shorthand, if the key is all there is: V8 looks at what follows the key
                 * before it asks whether the key can be a name */
                if (TOK(p).kind != JS_T_COMMA && TOK(p).kind != JS_T_RBRACE &&
                    TOK(p).kind != JS_T_EQ)
                    return perr_here(p, "expected ':' after property key");
                if (!shorthand_name_ok(p, &keytok))
                    return NULL;
                prop->flags |= JS_F_SHORTHAND;
                JsAstNode *val = new_node(p, JS_AST_IDENT, prop->a->pos);
                if (!val)
                    return NULL;
                val->units = prop->a->units;
                val->len = prop->a->len;
                if (TOK(p).kind == JS_T_EQ) { /* cover initializer */
                    if (!advance(p))
                        return NULL;
                    JsAstNode *def = new_node(p, JS_AST_DEFAULT, prop->a->pos);
                    JsAstNode *init = parse_assign_scoped(p);
                    if (!def || !init)
                        return NULL;
                    /* the literal's first is listed, once read (as V8 records it), until a
                     * pattern claims the literal */
                    if (!has_cover_init && !cover_push(p, prop->a->pos, p->prev_end, false))
                        return NULL;
                    has_cover_init = true;
                    def->a = val;
                    def->b = init;
                    prop->b = def;
                    prop->flags |= JS_F_COVER_INIT;
                } else {
                    prop->b = val;
                }
            } else {
                if (key_without_value(p, &keytok))
                    return NULL;
                return perr_here(p, "expected ':' after property key");
            }
            if (!prop->b || !vec_push(p, &props, prop))
                return NULL;
        }
        if (TOK(p).kind != JS_T_RBRACE && !expect(p, JS_T_COMMA, "expected ',' or '}'"))
            return NULL;
    }
    if (!expect(p, JS_T_RBRACE, "expected '}'"))
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_OBJECT, pos);
    if (!n)
        return NULL;
    vec_store(n, &props);
    return n;
}

/*
 * The words of what lamassu leaves out (`var`, `class`, `super`). The error names the feature
 * where the source has it: where V8 would go on to compile a declaration, a class or a
 * reference to `super`. Where V8 could not read one either, the word is only a word out of
 * place and the error is V8's own, at the token V8 stops at.
 */

/* `class`: a class as far as the `{` of its body, which is where lamassu stops reading it. Up to
 * there the head is read as V8 reads it: a name (which a declaration must have), `extends` and
 * an expression, and what is wrong in those is reported as V8 reports it. */
static JsAstNode *perr_class(JsParser *p, bool need_name) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p))
        return NULL;
    if (TOK(p).kind == JS_T_IDENT) {
        if (!check_binding_name(p, parse_ident(p, "expected name")))
            return NULL;
    } else if (need_name || (TOK(p).kind != JS_T_LBRACE && !tok_is_reserved(&TOK(p), "extends"))) {
        return perr_here(p, unexpected_text(p));
    }
    if (tok_is_reserved(&TOK(p), "extends") && (!advance(p) || !parse_call_member(p)))
        return NULL;
    if (TOK(p).kind != JS_T_LBRACE)
        return perr_here(p, unexpected_text(p));
    return perr(p, pos, "classes are not supported");
}

/* `super`: V8 takes `super.x` and `super[x]` in a method (and in the arrows inside one), and
 * nothing else outside a class */
static JsAstNode *perr_super(JsParser *p) {
    if (p->super_ok) {
        JsTokKind nx = peek(p).kind;
        if (nx == JS_T_DOT || nx == JS_T_LBRACK)
            return perr_here(p, "'super' is not supported");
    }
    return perr_here(p, "'super' keyword unexpected here");
}

/* `var` where a statement may begin: a declaration if a name or a pattern follows */
static JsAstNode *perr_var(JsParser *p) {
    JsTokKind nx = peek(p).kind;
    if (nx == JS_T_IDENT || nx == JS_T_LBRACK || nx == JS_T_LBRACE)
        return perr_here(p, "'var' is not supported; use 'let' or 'const'");
    if (!advance(p))
        return NULL;
    return perr_here(p, unexpected_text(p));
}

/* A reserved word where an expression begins: only `class` and `super` can begin one */
static JsAstNode *perr_reserved(JsParser *p) {
    if (tok_is_reserved(&TOK(p), "class"))
        return perr_class(p, false);
    if (tok_is_reserved(&TOK(p), "super"))
        return perr_super(p);
    return perr_here(p, unexpected_text(p));
}

static JsAstNode *parse_primary(JsParser *p) {
    JsToken *t = &TOK(p);
    JsAstNode *n;
    switch (t->kind) {
    case JS_T_NUM:
        n = new_node(p, JS_AST_NUMBER, t->pos);
        if (!n)
            return NULL;
        n->number = t->u.number;
        return advance(p) ? n : NULL;
    case JS_T_BIGINT:
        n = new_node(p, JS_AST_BIGINT, t->pos);
        if (!n)
            return NULL;
        n->units = t->u.str.units;
        n->len = t->u.str.len;
        return advance(p) ? n : NULL;
    case JS_T_STR:
        n = new_node(p, JS_AST_STRING, t->pos);
        if (!n)
            return NULL;
        n->units = t->u.str.units;
        n->len = t->u.str.len;
        return advance(p) ? n : NULL;
    case JS_T_TEMPLATE:
        return parse_template(p);
    case JS_T_SLASH:
    case JS_T_SLASH_EQ:
        if (!js_lexer_rescan_regex(&p->lx)) {
            fail_lexer(p);
            return NULL;
        }
        n = new_node(p, JS_AST_REGEX, t->pos);
        if (!n)
            return NULL;
        n->units = t->u.regex.pattern;
        n->len = t->u.regex.pattern_len;
        n->units2 = t->u.regex.flags;
        n->len2 = t->u.regex.flags_len;
#ifdef LAMASSU_HAS_REGEX
        {
            /* an early error, where V8's scanner reports it: "Invalid regular expression
             * flags", or the pattern and the reason */
            const char *em = js_regexp_validate(p->arena->vm, n->units, n->len, n->units2, n->len2);
            if (em) {
                if (strcmp(em, JS_REGEXP_FLAGS_ERROR) == 0)
                    return perr(p, n->pos, "Invalid regular expression flags");
                if (strncmp(em, "SyntaxError: ", 13) == 0)
                    em = js_compile_error_fmt(p->arena->vm, "Invalid regular expression: /%U/%U: %s", n->units,
                                              n->len, n->units2, n->len2, em + 13);
                return perr(p, n->pos, em);
            }
        }
#endif
        return advance(p) ? n : NULL;
    case JS_T_NULL:
        n = new_node(p, JS_AST_NULL_LIT, t->pos);
        return n && advance(p) ? n : NULL;
    case JS_T_TRUE:
    case JS_T_FALSE:
        n = new_node(p, JS_AST_BOOL_LIT, t->pos);
        if (!n)
            return NULL;
        if (t->kind == JS_T_TRUE)
            n->flags |= JS_F_TRUE;
        return advance(p) ? n : NULL;
    case JS_T_THIS:
        n = new_node(p, JS_AST_THIS, t->pos);
        return n && advance(p) ? n : NULL;
    case JS_T_FUNCTION: {
        uint32_t start = t->pos;
        if (!advance(p))
            return NULL;
        return parse_func_common(p, 0, false, start, JS_AST_FUNC_EXPR, start);
    }
    case JS_T_LBRACK:
        return parse_array_literal(p);
    case JS_T_LBRACE:
        return parse_object_literal(p);
    case JS_T_LPAREN:
        return parse_paren_cover(p, 0);
    case JS_T_IDENT: {
        if (escaped_keyword(t) || escaped_async_function(p))
            return perr_here(p, "Keyword must not contain escaped characters");
        if (tok_is_name(t, "async")) {
            JsToken nx = peek(p);
            uint32_t async_pos = t->pos;
            if (!nx.newline_before) {
                if (nx.kind == JS_T_FUNCTION) {
                    uint32_t pos = t->pos;
                    if (!advance(p) || !advance(p))
                        return NULL;
                    return parse_func_common(p, JS_F_ASYNC, false, pos, JS_AST_FUNC_EXPR, pos);
                }
                if (nx.kind == JS_T_LPAREN) {
                    if (!advance(p))
                        return NULL;
                    JsAstNode *cover = parse_paren_cover(p, JS_F_COVER_ASYNC);
                    if (cover)
                        cover->src_start = async_pos;
                    return cover;
                }
                if (nx.kind == JS_T_IDENT) {
                    /* `async x => ...` needs a second peek for the arrow */
                    JsLexer save = p->lx;
                    bool ok = js_lexer_next(&p->lx) && js_lexer_next(&p->lx);
                    bool is_arrow = ok && p->lx.tok.kind == JS_T_ARROW;
                    p->lx = save;
                    if (is_arrow) {
                        if (!advance(p))
                            return NULL;
                        JsAstNode *param = parse_ident(p, "expected parameter");
                        if (!param || TOK(p).kind != JS_T_ARROW)
                            return perr_here(p, "expected '=>'");
                        return build_arrow(p, param, true, async_pos);
                    }
                }
            }
        }
        return parse_ident(p, "expected expression");
    }
    case JS_T_RESERVED:
        return perr_reserved(p);
    case JS_T_NEW:
        /* reachable only via covers/edge paths that bypass parse_call_member's
         * dedicated new-expression handling (see parse_new_expr) */
        return perr_here(p, "'new' is not supported here");
    case JS_T_IMPORT: {
        JsToken nx = peek(p);
        if (nx.kind == JS_T_DOT) { /* import.meta */
            uint32_t pos = t->pos;
            if (!advance(p) || !advance(p))
                return NULL;
            if (TOK(p).kind != JS_T_IDENT || !tok_is_name(&TOK(p), "meta"))
                return perr_here(p, unexpected_text(p));
            JsAstNode *im = new_node(p, JS_AST_IMPORT_META, pos);
            if (!im || !advance(p))
                return NULL;
            return im;
        }
        if (nx.kind != JS_T_LPAREN)
            return perr_here(p, "'import' is only allowed at the top level of a module");
        /* dynamic import(specifier[, options]) — options is evaluated then
         * ignored (import attributes are not supported) */
        uint32_t pos = t->pos;
        if (!advance(p)) /* consume 'import'; TOK is now '(' */
            return NULL;
        JsNodeVec args = {0};
        if (!parse_arguments(p, &args))
            return NULL;
        if (args.count < 1 || args.count > 2)
            return perr(p, pos, "import() takes 1 or 2 arguments");
        if (args.items[0]->kind == JS_AST_SPREAD ||
            (args.count == 2 && args.items[1]->kind == JS_AST_SPREAD))
            return perr(p, pos, "spread is not allowed in import()");
        JsAstNode *node = new_node(p, JS_AST_IMPORT_CALL, pos);
        if (!node)
            return NULL;
        node->a = args.items[0];
        node->b = args.count == 2 ? args.items[1] : NULL;
        return node;
    }
    default:
        return perr_here(p, "expected expression");
    }
}

static JsAstNode *parse_arguments_inner(JsParser *p, JsNodeVec *out);

static JsAstNode *parse_arguments(JsParser *p, JsNodeVec *out) {
    bool no_in = p->no_in;
    p->no_in = false;
    JsAstNode *r = parse_arguments_inner(p, out);
    p->no_in = no_in;
    return r;
}

static JsAstNode *parse_arguments_inner(JsParser *p, JsNodeVec *out) {
    if (!advance(p)) /* ( */
        return NULL;
    while (TOK(p).kind != JS_T_RPAREN) {
        JsAstNode *e;
        if (TOK(p).kind == JS_T_DOTDOTDOT) {
            e = new_node(p, JS_AST_SPREAD, TOK(p).pos);
            if (!e || !advance(p))
                return NULL;
            e->a = parse_assign(p);
            if (!e->a)
                return NULL;
        } else {
            e = parse_assign(p);
        }
        if (!e || !vec_push(p, out, e))
            return NULL;
        if (TOK(p).kind != JS_T_RPAREN && TOK(p).kind != JS_T_COMMA && TOK(p).kind != JS_T_EOF)
            return perr_here(p, "missing ) after argument list");
        if (TOK(p).kind != JS_T_RPAREN && !expect(p, JS_T_COMMA, "expected ',' or ')'"))
            return NULL;
    }
    return advance(p) ? (JsAstNode *)1 : NULL;
}

/*
 * `new`'s callee: a MemberExpression (., []; no calls, no optional
 * chaining — `new a?.b()` is a syntax error in real JS too), recursing into
 * a nested `new` at the head (`new new Foo()` / `new Foo.Bar` etc).
 */
static JsAstNode *parse_new_callee(JsParser *p) {
    JsAstNode *base;
    if (TOK(p).kind == JS_T_NEW) {
        /* `new new … Foo()` recurses parse_new_expr/parse_new_callee once per
         * `new`; account for it against the depth guard. */
        if (!enter(p))
            return NULL;
        base = parse_new_expr(p);
        leave(p);
        if (!base)
            return NULL;
    } else {
        base = parse_primary(p);
        if (!base)
            return NULL;
    }
    for (;;) {
        if (p->err_msg)
            return NULL;
        if (base->kind == JS_AST_COVER) {
            base = collapse_cover(p, base);
            if (!base)
                return NULL;
        }
        JsTokKind k = TOK(p).kind;
        if (k == JS_T_DOT) {
            if (!advance(p))
                return NULL;
            JsAstNode *name = parse_name_any(p, "expected property name");
            if (!name)
                return NULL;
            JsAstNode *m = new_node(p, JS_AST_MEMBER, base->pos);
            if (!m)
                return NULL;
            m->a = base;
            m->units = name->units;
            m->len = name->len;
            m->pos2 = name->pos;
            base = m;
        } else if (k == JS_T_LBRACK) {
            JsAstNode *m = new_node(p, JS_AST_MEMBER, base->pos);
            if (!m)
                return NULL;
            m->pos2 = TOK(p).pos;
            if (!advance(p))
                return NULL;
            m->flags |= JS_F_COMPUTED;
            m->a = base;
            m->b = parse_index_expression(p);
            if (!m->b || !expect(p, JS_T_RBRACK, "expected ']'"))
                return NULL;
            base = m;
        } else if (k == JS_T_TEMPLATE) { /* a tagged template is a MemberExpression: new (F`x`) */
            JsAstNode *tt = new_node(p, JS_AST_TAGGED_TEMPLATE, base->pos);
            if (!tt)
                return NULL;
            tt->pos2 = TOK(p).pos;
            tt->a = base;
            tt->b = parse_template_as(p, true);
            if (!tt->b)
                return NULL;
            base = tt;
        } else {
            return base;
        }
    }
}

/* `new` NewExpression | `new` MemberExpression Arguments — the Arguments
 * are consumed here (bind to the nearest `new`); further `.`/`[]`/`()` on
 * the result are left for the caller's postfix loop. */
static JsAstNode *parse_new_expr(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* consume 'new' */
        return NULL;
    if (TOK(p).kind == JS_T_DOT) { /* new.target */
        if (!advance(p))
            return NULL;
        if (TOK(p).kind != JS_T_IDENT || !tok_is_name(&TOK(p), "target"))
            return perr_here(p, unexpected_text(p));
        if (!p->new_target_ok) /* V8 points at `target` */
            return perr_here(p, "new.target expression is not allowed here");
        JsAstNode *nt = new_node(p, JS_AST_NEW_TARGET, pos);
        if (!nt || !advance(p))
            return NULL;
        return nt;
    }
    JsAstNode *callee = parse_new_callee(p);
    if (!callee)
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_NEW, pos);
    if (!n)
        return NULL;
    n->a = callee;
    n->pos2 = pos; /* `new` */
    if (TOK(p).kind == JS_T_LPAREN) {
        JsNodeVec args = {0};
        if (!parse_arguments(p, &args))
            return NULL;
        n->items = args.items;
        n->count = args.count;
    }
    return n;
}

static JsAstNode *parse_call_member(JsParser *p) {
    JsAstNode *base = TOK(p).kind == JS_T_NEW ? parse_new_expr(p) : parse_primary(p);
    if (!base)
        return NULL;
    for (;;) {
        if (p->err_msg)
            return NULL;
        JsTokKind k = TOK(p).kind;
        if (k == JS_T_ARROW)
            return base; /* leave covers/idents for the arrow builder */
        if (base->kind == JS_AST_COVER) {
            base = collapse_cover(p, base);
            if (!base)
                return NULL;
        }
        if (k == JS_T_DOT || k == JS_T_QUESDOT) {
            bool optional = k == JS_T_QUESDOT;
            if (!advance(p))
                return NULL;
            if (optional && TOK(p).kind == JS_T_LPAREN) {
                JsAstNode *call = new_node(p, JS_AST_CALL, base->pos);
                if (!call)
                    return NULL;
                call->pos2 = TOK(p).pos;
                call->flags |= JS_F_OPTIONAL;
                call->a = base;
                JsNodeVec args = {0};
                if (!parse_arguments(p, &args))
                    return NULL;
                call->items = args.items;
                call->count = args.count;
                base = call;
                continue;
            }
            if (optional && TOK(p).kind == JS_T_LBRACK) {
                JsAstNode *m = new_node(p, JS_AST_MEMBER, base->pos);
                if (!m)
                    return NULL;
                m->pos2 = TOK(p).pos;
                if (!advance(p))
                    return NULL;
                m->flags |= JS_F_COMPUTED | JS_F_OPTIONAL;
                m->a = base;
                m->b = parse_index_expression(p);
                if (!m->b || !expect(p, JS_T_RBRACK, "expected ']'"))
                    return NULL;
                base = m;
                continue;
            }
            JsAstNode *name = parse_name_any(p, "expected property name");
            if (!name)
                return NULL;
            JsAstNode *m = new_node(p, JS_AST_MEMBER, base->pos);
            if (!m)
                return NULL;
            if (optional)
                m->flags |= JS_F_OPTIONAL;
            m->a = base;
            m->units = name->units;
            m->len = name->len;
            m->pos2 = name->pos;
            base = m;
        } else if (k == JS_T_LBRACK) {
            JsAstNode *m = new_node(p, JS_AST_MEMBER, base->pos);
            if (!m)
                return NULL;
            m->pos2 = TOK(p).pos;
            if (!advance(p))
                return NULL;
            m->flags |= JS_F_COMPUTED;
            m->a = base;
            m->b = parse_index_expression(p);
            if (!m->b || !expect(p, JS_T_RBRACK, "expected ']'"))
                return NULL;
            base = m;
        } else if (k == JS_T_LPAREN) {
            JsAstNode *call = new_node(p, JS_AST_CALL, base->pos);
            if (!call)
                return NULL;
            call->pos2 = call_pos(base, TOK(p).pos);
            call->a = base;
            JsNodeVec args = {0};
            if (!parse_arguments(p, &args))
                return NULL;
            call->items = args.items;
            call->count = args.count;
            base = call;
        } else if (k == JS_T_TEMPLATE) {
            for (const JsAstNode *c = base; c && (c->kind == JS_AST_MEMBER || c->kind == JS_AST_CALL) &&
                                            !(c->flags & JS_F_PAREN); c = c->a)
                if (c->flags & JS_F_OPTIONAL)
                    return perr_here(p, "Invalid tagged template on optional chain");
            JsAstNode *tt = new_node(p, JS_AST_TAGGED_TEMPLATE, base->pos);
            if (!tt)
                return NULL;
            tt->pos2 = TOK(p).pos;
            tt->a = base;
            tt->b = parse_template_as(p, true);
            if (!tt->b)
                return NULL;
            base = tt;
        } else {
            return base;
        }
    }
}

/* ---- unary / binary / conditional / assignment ---- */

/* N as what an assignment, `++` or a `for (... of` stores to: a name or a property. POS: where
 * the expression starts, which is where V8 points when it is neither. */
static JsAstNode *validate_simple_target(JsParser *p, JsAstNode *n, const char *msg, uint32_t pos) {
    if (!n)
        return NULL;
    if (n->kind == JS_AST_IDENT) {
        if (units_is(n->units, n->len, "eval") ||
            units_is(n->units, n->len, "arguments"))
            return perr(p, pos, "cannot assign to 'eval' or 'arguments' in strict mode");
        return n;
    }
    if (n->kind == JS_AST_MEMBER && is_simple_target(n))
        return n;
    return perr(p, pos, msg);
}

static JsAstNode *parse_unary(JsParser *p) {
    if (!enter(p))
        return NULL;
    JsAstNode *result = NULL;
    JsToken *t = &TOK(p);
    switch (t->kind) {
    case JS_T_DELETE: case JS_T_TYPEOF: case JS_T_VOID:
    case JS_T_NOT: case JS_T_TILDE: case JS_T_PLUS: case JS_T_MINUS: {
        JsTokKind op = t->kind;
        uint32_t pos = t->pos;
        if (!advance(p))
            goto out;
        JsAstNode *operand = parse_unary(p);
        if (!operand)
            goto out;
        if (op == JS_T_DELETE && operand->kind != JS_AST_MEMBER) {
            perr(p, pos, "'delete' target must be a property access");
            goto out;
        }
        result = new_node(p, JS_AST_UNARY, pos);
        if (!result)
            goto out;
        result->op = (uint8_t)op;
        result->a = operand;
        break;
    }
    case JS_T_INC: case JS_T_DEC: {
        JsTokKind op = t->kind;
        uint32_t pos = t->pos;
        if (!advance(p))
            goto out;
        uint32_t operand_pos = TOK(p).pos;
        JsAstNode *operand = validate_simple_target(
            p, parse_unary(p), "Invalid left-hand side expression in prefix operation", operand_pos);
        if (!operand)
            goto out;
        result = new_node(p, JS_AST_UPDATE, pos);
        if (!result)
            goto out;
        result->op = (uint8_t)op;
        result->flags |= JS_F_PREFIX;
        result->a = operand;
        break;
    }
    case JS_T_AWAIT: {
        uint32_t pos = t->pos;
        if (!p->fn_async) {
            perr(p, pos, "'await' is only valid in async functions");
            goto out;
        }
        if (t->escaped) {
            perr(p, pos, "Keyword must not contain escaped characters");
            goto out;
        }
        if (!advance(p))
            goto out;
        JsAstNode *operand = parse_unary(p);
        if (!operand)
            goto out;
        result = new_node(p, JS_AST_AWAIT, pos);
        if (!result)
            goto out;
        result->a = operand;
        if (p->fn_nesting == 0)
            p->module_tla = true;
        break;
    }
    default: {
        uint32_t start = t->pos;
        result = parse_call_member(p);
        if (!result)
            goto out;
        if ((TOK(p).kind == JS_T_INC || TOK(p).kind == JS_T_DEC) &&
            !TOK(p).newline_before) {
            if (result->kind == JS_AST_COVER) {
                result = collapse_cover(p, result);
                if (!result)
                    goto out;
            }
            JsAstNode *target = validate_simple_target(
                p, result, "Invalid left-hand side expression in postfix operation", start);
            if (!target) {
                result = NULL;
                goto out;
            }
            JsAstNode *upd = new_node(p, JS_AST_UPDATE, result->pos);
            if (!upd) {
                result = NULL;
                goto out;
            }
            upd->op = (uint8_t)TOK(p).kind;
            upd->a = target;
            result = advance(p) ? upd : NULL;
        }
        break;
    }
    }
out:
    leave(p);
    return result;
}

static int binop_prec(JsTokKind k) {
    switch (k) {
    case JS_T_QUESQUES: return 1;
    case JS_T_OROR: return 2;
    case JS_T_ANDAND: return 3;
    case JS_T_BAR: return 4;
    case JS_T_CARET: return 5;
    case JS_T_AMP: return 6;
    case JS_T_EQEQ: case JS_T_NE: case JS_T_EQEQEQ: case JS_T_STRICT_NE: return 7;
    case JS_T_LT: case JS_T_GT: case JS_T_LE: case JS_T_GE:
    case JS_T_IN: case JS_T_INSTANCEOF: return 8;
    case JS_T_SHL: case JS_T_SHR: case JS_T_USHR: return 9;
    case JS_T_PLUS: case JS_T_MINUS: return 10;
    case JS_T_STAR: case JS_T_SLASH: case JS_T_PERCENT: return 11;
    case JS_T_POW: return 12;
    default: return 0;
    }
}

static bool is_unparen_logical(const JsAstNode *n, bool andor) {
    if (n->kind != JS_AST_LOGICAL || (n->flags & JS_F_PAREN))
        return false;
    if (andor)
        return n->op == JS_T_ANDAND || n->op == JS_T_OROR;
    return n->op == JS_T_QUESQUES;
}

static JsAstNode *parse_binary(JsParser *p, int min_prec) {
    JsAstNode *left = parse_unary(p);
    if (!left)
        return NULL;
    for (;;) {
        JsTokKind k = TOK(p).kind;
        int prec = k == JS_T_IN && p->no_in ? 0 : binop_prec(k);
        if (prec == 0 || prec < min_prec)
            return left;
        /* `**` takes an UpdateExpression, not a UnaryExpression, on the left:
         * `-2 ** 2` / `typeof x ** 2` / `await p ** 2` are ambiguous and a
         * SyntaxError in real JS. A parenthesized operand carries JS_F_PAREN
         * (set when its cover collapsed), so `(-2) ** 2` is correctly allowed. */
        if (k == JS_T_POW && !(left->flags & JS_F_PAREN) &&
            (left->kind == JS_AST_UNARY || left->kind == JS_AST_AWAIT))
            return perr_here(p, "unary operator before '**' must be parenthesized");
        if (left->kind == JS_AST_COVER) {
            left = collapse_cover(p, left);
            if (!left)
                return NULL;
        }
        uint32_t pos = left->pos;
        if (!advance(p))
            return NULL;
        /* Right-associative '**' re-enters parse_binary at the same precedence,
         * so `a**b**c**…` nests one C-stack frame per operator. Account for the
         * recursion against the depth guard (flat left-assoc chains loop here
         * and unwind each step, so they don't accumulate). */
        if (!enter(p))
            return NULL;
        JsAstNode *right = parse_binary(p, k == JS_T_POW ? prec : prec + 1);
        leave(p);
        if (!right)
            return NULL;
        bool logical = k == JS_T_ANDAND || k == JS_T_OROR || k == JS_T_QUESQUES;
        if (k == JS_T_QUESQUES &&
            (is_unparen_logical(left, true) || is_unparen_logical(right, true)))
            return perr(p, pos, "Unexpected token '?\?'");
        if ((k == JS_T_ANDAND || k == JS_T_OROR) &&
            (is_unparen_logical(left, false) || is_unparen_logical(right, false)))
            return perr(p, pos, k == JS_T_ANDAND ? "Unexpected token '&&'" : "Unexpected token '||'");
        JsAstNode *n = new_node(p, logical ? JS_AST_LOGICAL : JS_AST_BINARY, pos);
        if (!n)
            return NULL;
        n->op = (uint8_t)k;
        n->a = left;
        n->b = right;
        left = n;
    }
}

static JsAstNode *parse_conditional(JsParser *p) {
    JsAstNode *test = parse_binary(p, 1);
    if (!test)
        return NULL;
    if (TOK(p).kind != JS_T_QUES)
        return test;
    if (test->kind == JS_AST_COVER) {
        test = collapse_cover(p, test);
        if (!test)
            return NULL;
    }
    JsAstNode *n = new_node(p, JS_AST_COND, test->pos);
    if (!n || !advance(p))
        return NULL;
    n->a = test;
    bool was = p->decl_element; /* the branches are no part of a declaration's pattern */
    bool no_in = p->no_in;      /* and between `?` and `:` an `in` is an operator again */
    p->decl_element = false;
    p->no_in = false;
    n->b = parse_assign_scoped(p);
    p->no_in = no_in;
    if (n->b && expect(p, JS_T_COLON, "expected ':' in conditional expression"))
        n->c = parse_assign_scoped(p);
    p->decl_element = was;
    return n->b && n->c ? n : NULL;
}

static bool is_assign_op(JsTokKind k) {
    switch (k) {
    case JS_T_EQ: case JS_T_PLUS_EQ: case JS_T_MINUS_EQ: case JS_T_STAR_EQ:
    case JS_T_SLASH_EQ: case JS_T_PERCENT_EQ: case JS_T_POW_EQ:
    case JS_T_SHL_EQ: case JS_T_SHR_EQ: case JS_T_USHR_EQ:
    case JS_T_AMP_EQ: case JS_T_BAR_EQ: case JS_T_CARET_EQ:
    case JS_T_ANDAND_EQ: case JS_T_OROR_EQ: case JS_T_QUESQUES_EQ:
        return true;
    default:
        return false;
    }
}

static JsAstNode *parse_assign(JsParser *p) {
    if (!enter(p))
        return NULL;
    JsAstNode *result = NULL;
    uint32_t lhs_pos = TOK(p).pos, mark = p->cover_count;
    JsAstNode *lhs = parse_conditional(p);
    if (!lhs)
        goto out;

    if (TOK(p).kind == JS_T_ARROW &&
        (lhs->kind == JS_AST_IDENT || lhs->kind == JS_AST_COVER) &&
        !(lhs->flags & JS_F_PAREN)) {
        bool is_async = lhs->kind == JS_AST_COVER && (lhs->flags & JS_F_COVER_ASYNC);
        result = build_arrow(p, lhs, is_async, lhs->kind == JS_AST_COVER ? lhs->src_start : lhs->pos);
        goto out;
    }
    if (lhs->kind == JS_AST_COVER) {
        lhs = collapse_cover(p, lhs);
        if (!lhs)
            goto out;
    }
    if (is_assign_op(TOK(p).kind)) {
        JsTokKind op = TOK(p).kind;
        JsAstNode *target;
        if (op == JS_T_EQ &&
            (lhs->kind == JS_AST_ARRAY || lhs->kind == JS_AST_OBJECT)) {
            target = expr_to_pattern(p, lhs, false);
        } else {
            target = validate_simple_target(p, lhs, "invalid assignment target", lhs_pos);
            /* a property is no pattern: what was read inside it is settled here (cover_element) */
            if (target && target->kind == JS_AST_MEMBER && !cover_closed(p, mark))
                goto out;
        }
        if (!target)
            goto out;
        JsAstNode *n = new_node(p, JS_AST_ASSIGN, lhs->pos);
        if (!n || !advance(p))
            goto out;
        n->op = (uint8_t)op;
        n->a = target;
        bool was = p->decl_element; /* nor is what is assigned */
        p->decl_element = false;
        n->b = parse_assign_scoped(p);
        p->decl_element = was;
        result = n->b ? n : NULL;
        goto out;
    }
    result = lhs;
out:
    leave(p);
    return result;
}

/* An AssignmentExpression that is an expression of its own: what an assignment's right side, an
 * initializer, a branch of `?:` or a computed key is. No pattern around it reaches in, so a
 * cover initializer still open when it ends is an error (V8's ValidateExpression). */
static JsAstNode *parse_assign_scoped(JsParser *p) {
    uint32_t mark = p->cover_count;
    int8_t bind = p->bind_kind; /* a default or a key inside a pattern declares nothing */
    p->bind_kind = JS_BIND_NONE;
    JsAstNode *e = parse_assign(p);
    p->bind_kind = bind;
    return e && cover_closed(p, mark) ? e : NULL;
}

static JsAstNode *parse_expression(JsParser *p) {
    uint32_t mark = p->cover_count;
    JsAstNode *e = parse_expression_cover(p);
    return e && cover_closed(p, mark) ? e : NULL;
}

/* An Expression that may yet be a pattern or part of one: an index, the head of a `for` */
static JsAstNode *parse_expression_cover(JsParser *p) {
    JsAstNode *first = parse_assign(p);
    if (!first)
        return NULL;
    if (first->kind == JS_AST_COVER) {
        first = collapse_cover(p, first);
        if (!first)
            return NULL;
    }
    if (TOK(p).kind != JS_T_COMMA)
        return first;
    JsNodeVec items = {0};
    if (!vec_push(p, &items, first))
        return NULL;
    while (accept(p, JS_T_COMMA)) {
        JsAstNode *e = parse_assign(p);
        if (!e)
            return NULL;
        if (e->kind == JS_AST_COVER) {
            e = collapse_cover(p, e);
            if (!e)
                return NULL;
        }
        if (!vec_push(p, &items, e))
            return NULL;
    }
    JsAstNode *seq = new_node(p, JS_AST_SEQUENCE, first->pos);
    if (!seq)
        return NULL;
    vec_store(seq, &items);
    return seq;
}

/* ---- statements ---- */

static JsLabel *label_find(JsParser *p, const uint16_t *units, uint32_t len) {
    for (uint32_t i = p->label_count; i-- > p->label_base;) {
        JsLabel *l = &p->labels[i];
        if (l->len == len && memcmp(l->units, units, (size_t)len * sizeof *units) == 0)
            return l;
    }
    return NULL;
}

static bool label_push(JsParser *p, const uint16_t *units, uint32_t len, uint32_t pos) {
    if (label_find(p, units, len)) {
        perr(p, pos, js_compile_error_fmt(p->arena->vm, "Label '%U' has already been declared", units, len));
        return false;
    }
    if (p->label_count == p->label_cap) {
        uint32_t ncap = p->label_cap ? p->label_cap * 2 : 8;
        JsLabel *nl = js_arena_alloc(p->arena, (size_t)ncap * sizeof *nl);
        if (!nl) {
            perr(p, pos, "out of memory");
            return false;
        }
        if (p->label_count)
            memcpy(nl, p->labels, p->label_count * sizeof *nl);
        p->labels = nl;
        p->label_cap = ncap;
    }
    p->labels[p->label_count++] = (JsLabel){units, len, JS_LABEL_PLAIN};
    return true;
}

/* The statement being parsed carries the top `n` labels: say what they name. A loop is a
 * `continue` target only for the top `own` of them. */
static void labels_mark(JsParser *p, uint32_t n, uint32_t own, uint8_t kind) {
    for (uint32_t i = 0; i < n; i++)
        p->labels[p->label_count - 1 - i].kind = (kind == JS_LABEL_LOOP && i >= own) ? JS_LABEL_TARGET : kind;
}

static JsAstNode *parse_block(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    /* a function's body is read into the function's scope, any other block into its own */
    bool body = p->body_block, catch_body = p->catch_block;
    p->body_block = false;
    p->catch_block = false;
    if (!enter(p))
        return NULL;
    JsAstNode *result = NULL;
    if (!body && !scope_push(p, JS_SCOPE_BLOCK)) {
        leave(p);
        return NULL;
    }
    if (!expect(p, JS_T_LBRACE, "expected '{'"))
        goto out;
    {
        JsNodeVec stmts = {0};
        while (TOK(p).kind != JS_T_RBRACE && TOK(p).kind != JS_T_EOF) {
            JsAstNode *s = parse_statement(p);
            if (!s || !vec_push(p, &stmts, s))
                goto out;
        }
        if (!expect(p, JS_T_RBRACE, "expected '}'"))
            goto out;
        if (catch_body && !catch_check(p))
            goto out;
        result = new_node(p, JS_AST_BLOCK, pos);
        if (!result)
            goto out;
        vec_store(result, &stmts);
    }
out:
    if (!body)
        scope_pop(p);
    leave(p);
    return result;
}

static JsAstNode *parse_var_decl(JsParser *p, bool is_const) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* let/const */
        return NULL;
    JsNodeVec decls = {0};
    for (;;) {
        uint32_t bind_pos = TOK(p).pos;
        int8_t bind = p->bind_kind;
        p->bind_kind = JS_BIND_LEX;
        JsAstNode *target = parse_binding_target(p);
        p->bind_kind = bind;
        if (!target)
            return NULL;
        JsAstNode *init = NULL;
        if (accept(p, JS_T_EQ)) {
            init = parse_assign_scoped(p);
            if (!init)
                return NULL;
        } else if (target->kind != JS_AST_IDENT) { /* V8 points both at the binding */
            return perr(p, bind_pos, "destructuring declaration requires an initializer");
        } else if (is_const) {
            return perr(p, bind_pos, "'const' declaration requires an initializer");
        }
        JsAstNode *d = new_node(p, JS_AST_DECLARATOR, target->pos);
        if (!d)
            return NULL;
        d->a = target;
        d->b = init;
        if (!vec_push(p, &decls, d))
            return NULL;
        if (!accept(p, JS_T_COMMA))
            break;
    }
    JsAstNode *n = new_node(p, JS_AST_LET_DECL, pos);
    if (!n)
        return NULL;
    if (is_const)
        n->flags |= JS_F_CONST;
    vec_store(n, &decls);
    return n;
}

/* `let` starts a declaration when a binding follows it, by V8's rule (IsNextLetKeyword); anywhere
 * else it is a name, which strict code reserves. */
static bool let_starts_declaration(JsParser *p) {
    JsToken nx = peek(p);
    switch (nx.kind) {
    case JS_T_IDENT:
    case JS_T_LBRACE:
    case JS_T_LBRACK:
    case JS_T_LET:
    case JS_T_AWAIT:
        return true;
    case JS_T_RESERVED:
        return units_is(nx.u.str.units, nx.u.str.len, "yield") ||
               units_is(nx.u.str.units, nx.u.str.len, "static");
    default:
        return false;
    }
}

/* The body of an `if`, a loop or a label is a Statement, not a declaration: V8 refuses one there
 * in these words. The labels handed to the statement stay pending for it. */
static JsAstNode *parse_substatement(JsParser *p) {
    const JsToken *t = &TOK(p);
    switch (t->kind) {
    case JS_T_FUNCTION:
        return perr_here(p, "In strict mode code, functions can only be declared at top level or inside a block.");
    case JS_T_CONST:
        return perr_here(p, unexpected_text(p));
    case JS_T_RESERVED:
        if (tok_is_reserved(t, "class")) /* a declaration, as `const` is */
            return perr_here(p, unexpected_text(p));
        break;
    case JS_T_LET: {
        JsToken nx = peek(p);
        if (nx.kind == JS_T_LBRACK ||
            ((nx.kind == JS_T_LBRACE || nx.kind == JS_T_IDENT) && !nx.newline_before))
            return perr_here(p, "Lexical declaration cannot appear in a single-statement context");
        return perr_here(p, unexpected_text(p));
    }
    case JS_T_IDENT:
        if (tok_is_name(t, "async")) {
            JsToken nx = peek(p);
            if (nx.kind == JS_T_FUNCTION && !nx.newline_before)
                return perr_here(p, "Async functions can only be declared at the top level or inside a block.");
        }
        break;
    default:
        break;
    }
    return parse_statement(p);
}

static JsAstNode *parse_loop_body(JsParser *p) {
    p->loop_depth++;
    JsAstNode *body = parse_substatement(p);
    p->loop_depth--;
    return body;
}

/* The rest of a for-of from its `of`: FLAGS and TARGET are what its head declared or named */
static JsAstNode *parse_for_of(JsParser *p, uint32_t pos, uint8_t flags, JsAstNode *target) {
    if (!advance(p)) /* of */
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_FOR_OF, pos);
    if (!n)
        return NULL;
    n->flags = flags;
    n->a = target;
    n->b = parse_assign_scoped(p);
    if (!n->b || !expect(p, JS_T_RPAREN, "expected ')'"))
        return NULL;
    n->c = parse_loop_body(p);
    return n->c ? n : NULL;
}

static JsAstNode *parse_for_inner(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p) || !expect(p, JS_T_LPAREN, "expected '(' after 'for'"))
        return NULL;

    JsAstNode *init = NULL;
    if (TOK(p).kind == JS_T_LET && !let_starts_declaration(p))
        return perr_here(p, unexpected_text(p));
    if (TOK(p).kind == JS_T_LET || TOK(p).kind == JS_T_CONST) {
        bool is_const = TOK(p).kind == JS_T_CONST;
        uint32_t decl_pos = TOK(p).pos;
        if (!advance(p))
            return NULL;
        /* The declarations first, as V8 reads them, and then what kind of loop they head */
        JsNodeVec decls = {0};
        uint32_t first_pos = TOK(p).pos, init_pos = 0;
        bool any_init = false;
        for (;;) {
            uint32_t bind_pos = TOK(p).pos;
            int8_t bind = p->bind_kind;
            p->bind_kind = JS_BIND_LEX;
            JsAstNode *target = parse_binding_target(p);
            p->bind_kind = bind;
            if (!target)
                return NULL;
            uint32_t last_pos = p->prev_pos; /* the binding's last token */
            JsAstNode *dinit = NULL;
            if (accept(p, JS_T_EQ)) {
                bool no_in = p->no_in;
                p->no_in = true;
                dinit = parse_assign_scoped(p);
                p->no_in = no_in;
                if (!dinit)
                    return NULL;
                if (!any_init)
                    init_pos = last_pos;
                any_init = true;
            } else if (TOK(p).kind != JS_T_IN && !tok_is_name(&TOK(p), "of")) {
                if (target->kind != JS_AST_IDENT)
                    return perr(p, bind_pos, "destructuring declaration requires an initializer");
                if (is_const)
                    return perr(p, bind_pos, "'const' declaration requires an initializer");
            }
            JsAstNode *d = new_node(p, JS_AST_DECLARATOR, target->pos);
            if (!d)
                return NULL;
            d->a = target;
            d->b = dinit;
            if (!vec_push(p, &decls, d))
                return NULL;
            if (!accept(p, JS_T_COMMA))
                break;
        }
        if (TOK(p).kind == JS_T_IN)
            return perr_here(p, "for-in is not supported; use for-of");
        if (tok_is_name(&TOK(p), "of")) {
            if (decls.count != 1)
                return perr(p, first_pos, "Invalid left-hand side in for-of loop: Must have a single binding.");
            if (any_init)
                return perr(p, init_pos, "for-of loop variable declaration may not have an initializer.");
            return parse_for_of(p, pos, (uint8_t)(JS_F_DECL | (is_const ? JS_F_CONST : 0)),
                                decls.items[0]->a);
        }
        init = new_node(p, JS_AST_LET_DECL, decl_pos);
        if (!init)
            return NULL;
        if (is_const)
            init->flags |= JS_F_CONST;
        vec_store(init, &decls);
    } else if (tok_is_reserved(&TOK(p), "var")) {
        return perr_var(p);
    } else if (TOK(p).kind != JS_T_SEMI) {
        uint32_t head_pos = TOK(p).pos, mark = p->cover_count;
        bool head_async = tok_is_name(&TOK(p), "async");
        bool no_in = p->no_in;
        p->no_in = true; /* an `in` would make this a for-in */
        init = parse_expression_cover(p);
        p->no_in = no_in;
        if (!init)
            return NULL;
        if (TOK(p).kind == JS_T_IN)
            return perr(p, head_pos, "for-in is not supported; use for-of");
        if (tok_is_name(&TOK(p), "of")) {
            bool paren = (init->flags & JS_F_PAREN) != 0;
            JsAstNode *target;
            /* `for (async of` would read on as an async arrow: the name must be escaped or in
             * parentheses to be the loop's variable */
            if (head_async && init->kind == JS_AST_IDENT && !paren)
                return perr(p, head_pos, "The left-hand side of a for-of loop may not be 'async'.");
            if (init->kind == JS_AST_ARRAY || init->kind == JS_AST_OBJECT)
                target = paren ? perr(p, head_pos, INVALID_TARGET) : expr_to_pattern(p, init, false);
            else
                target = validate_simple_target(p, init, "Invalid left-hand side in for-loop", head_pos);
            if (!target || !cover_closed(p, mark))
                return NULL;
            return parse_for_of(p, pos, 0, target);
        }
        if (!cover_closed(p, mark))
            return NULL;
        JsAstNode *wrapped = new_node(p, JS_AST_EXPR_STMT, init->pos);
        if (!wrapped)
            return NULL;
        wrapped->a = init;
        init = wrapped;
    }

    if (!expect(p, JS_T_SEMI, "expected ';' in for statement"))
        return NULL;
    JsAstNode *test = NULL, *update = NULL;
    if (TOK(p).kind != JS_T_SEMI) {
        test = parse_expression(p);
        if (!test)
            return NULL;
    }
    if (!expect(p, JS_T_SEMI, "expected ';' in for statement"))
        return NULL;
    if (TOK(p).kind != JS_T_RPAREN) {
        update = parse_expression(p);
        if (!update)
            return NULL;
    }
    if (!expect(p, JS_T_RPAREN, "expected ')'"))
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_FOR, pos);
    if (!n)
        return NULL;
    n->a = init;
    n->b = test;
    n->c = update;
    n->d = parse_loop_body(p);
    return n->d ? n : NULL;
}

/* A `for` statement: its head's declarations are a scope around the body */
static JsAstNode *parse_for(JsParser *p) {
    if (!scope_push(p, JS_SCOPE_BLOCK))
        return NULL;
    JsAstNode *n = parse_for_inner(p);
    scope_pop(p);
    return n;
}

static JsAstNode *parse_switch_inner(JsParser *p);

/* A `switch` statement: its cases are one scope */
static JsAstNode *parse_switch(JsParser *p) {
    if (!scope_push(p, JS_SCOPE_BLOCK))
        return NULL;
    JsAstNode *n = parse_switch_inner(p);
    scope_pop(p);
    return n;
}

static JsAstNode *parse_switch_inner(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p) || !expect(p, JS_T_LPAREN, "expected '(' after 'switch'"))
        return NULL;
    JsAstNode *disc = parse_expression(p);
    if (!disc || !expect(p, JS_T_RPAREN, "expected ')'") ||
        !expect(p, JS_T_LBRACE, "expected '{'"))
        return NULL;
    JsNodeVec cases = {0};
    bool seen_default = false;
    p->switch_depth++;
    while (TOK(p).kind != JS_T_RBRACE && !p->err_msg) {
        JsAstNode *c = new_node(p, JS_AST_SWITCH_CASE, TOK(p).pos);
        if (!c)
            break;
        if (accept(p, JS_T_CASE)) {
            c->a = parse_expression(p);
            if (!c->a)
                break;
        } else if (TOK(p).kind == JS_T_DEFAULT) {
            if (seen_default) {
                perr_here(p, "duplicate 'default' clause");
                break;
            }
            seen_default = true;
            if (!advance(p))
                break;
        } else {
            perr_here(p, "expected 'case' or 'default'");
            break;
        }
        if (!expect(p, JS_T_COLON, "expected ':'"))
            break;
        JsNodeVec stmts = {0};
        while (TOK(p).kind != JS_T_CASE && TOK(p).kind != JS_T_DEFAULT &&
               TOK(p).kind != JS_T_RBRACE && TOK(p).kind != JS_T_EOF) {
            JsAstNode *s = parse_statement(p);
            if (!s || !vec_push(p, &stmts, s))
                goto done;
        }
        vec_store(c, &stmts);
        if (!vec_push(p, &cases, c))
            break;
    }
done:
    p->switch_depth--;
    if (p->err_msg)
        return NULL;
    if (!expect(p, JS_T_RBRACE, "expected '}'"))
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_SWITCH, pos);
    if (!n)
        return NULL;
    n->a = disc;
    vec_store(n, &cases);
    return n;
}

static JsAstNode *parse_try(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p))
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_TRY, pos);
    if (!n)
        return NULL;
    n->a = parse_block(p);
    if (!n->a)
        return NULL;
    if (TOK(p).kind == JS_T_CATCH) {
        if (!advance(p))
            return NULL;
        bool param = accept(p, JS_T_LPAREN);
        if (param) {
            /* the parameter is a scope around the block, which is checked against it */
            int8_t bind = p->bind_kind;
            if (!scope_push(p, JS_SCOPE_CATCH))
                return NULL;
            p->bind_kind = JS_BIND_LEX;
            n->b = parse_binding_target(p);
            p->bind_kind = bind;
            if (!n->b || !expect(p, JS_T_RPAREN, "expected ')'"))
                return NULL;
        }
        p->catch_block = param;
        n->c = parse_block(p);
        if (param)
            scope_pop(p);
        if (!n->c)
            return NULL;
    }
    if (TOK(p).kind == JS_T_FINALLY) {
        if (!advance(p))
            return NULL;
        n->d = parse_block(p);
        if (!n->d)
            return NULL;
    }
    if (!n->c && !n->d)
        return perr_here(p, "expected 'catch' or 'finally'");
    return n;
}

static JsAstNode *parse_statement(JsParser *p) {
    if (p->err_msg)
        return NULL;
    if (!enter(p))
        return NULL;
    JsAstNode *result = NULL;
    JsToken *t = &TOK(p);
    uint32_t pos = t->pos;
    /* the labels this statement carries (see JsParser.labels); a statement inside it has none */
    uint32_t nlabels = p->pending_labels, own = p->pending_own;
    p->pending_labels = 0;
    p->pending_own = 0;

    switch (t->kind) {
    case JS_T_LBRACE:
        labels_mark(p, nlabels, own, JS_LABEL_TARGET);
        result = parse_block(p);
        break;
    case JS_T_SEMI:
        result = new_node(p, JS_AST_EMPTY, pos);
        if (result && !advance(p))
            result = NULL;
        break;
    case JS_T_LET:
        if (!let_starts_declaration(p)) {
            result = perr_here(p, unexpected_text(p));
            break;
        }
        /* fall through */
    case JS_T_CONST:
        result = parse_var_decl(p, t->kind == JS_T_CONST);
        if (result && !expect_semicolon(p))
            result = NULL;
        break;
    case JS_T_IF: {
        if (!advance(p) || !expect(p, JS_T_LPAREN, "expected '(' after 'if'"))
            break;
        JsAstNode *test = parse_expression(p);
        if (!test || !expect(p, JS_T_RPAREN, "expected ')'"))
            break;
        /* an `if` hands its labels to each branch, which does not own them */
        p->pending_labels = nlabels;
        JsAstNode *cons = parse_substatement(p);
        if (!cons)
            break;
        JsAstNode *alt = NULL;
        if (accept(p, JS_T_ELSE)) {
            labels_mark(p, nlabels, 0, JS_LABEL_PLAIN);
            p->pending_labels = nlabels;
            alt = parse_substatement(p);
            if (!alt)
                break;
        }
        result = new_node(p, JS_AST_IF, pos);
        if (result) {
            result->a = test;
            result->b = cons;
            result->c = alt;
        }
        break;
    }
    case JS_T_WHILE: {
        labels_mark(p, nlabels, own, JS_LABEL_LOOP);
        if (!advance(p) || !expect(p, JS_T_LPAREN, "expected '(' after 'while'"))
            break;
        JsAstNode *test = parse_expression(p);
        if (!test || !expect(p, JS_T_RPAREN, "expected ')'"))
            break;
        JsAstNode *body = parse_loop_body(p);
        if (!body)
            break;
        result = new_node(p, JS_AST_WHILE, pos);
        if (result) {
            result->a = test;
            result->b = body;
        }
        break;
    }
    case JS_T_DO: {
        labels_mark(p, nlabels, own, JS_LABEL_LOOP);
        if (!advance(p))
            break;
        JsAstNode *body = parse_loop_body(p);
        if (!body)
            break;
        if (!expect(p, JS_T_WHILE, "expected 'while' after do body") ||
            !expect(p, JS_T_LPAREN, "expected '('"))
            break;
        JsAstNode *test = parse_expression(p);
        if (!test || !expect(p, JS_T_RPAREN, "expected ')'"))
            break;
        accept(p, JS_T_SEMI); /* ASI: semicolon always optional here */
        if (p->err_msg)
            break;
        result = new_node(p, JS_AST_DO_WHILE, pos);
        if (result) {
            result->a = body;
            result->b = test;
        }
        break;
    }
    case JS_T_FOR:
        labels_mark(p, nlabels, own, JS_LABEL_LOOP);
        result = parse_for(p);
        break;
    case JS_T_SWITCH:
        labels_mark(p, nlabels, own, JS_LABEL_TARGET);
        result = parse_switch(p);
        break;
    case JS_T_TRY:
        labels_mark(p, nlabels, own, JS_LABEL_TARGET);
        result = parse_try(p);
        break;
    case JS_T_BREAK:
    case JS_T_CONTINUE: {
        bool is_break = t->kind == JS_T_BREAK;
        if (!advance(p))
            break;
        result = new_node(p, is_break ? JS_AST_BREAK : JS_AST_CONTINUE, pos);
        if (!result)
            break;
        if (TOK(p).kind == JS_T_IDENT && !TOK(p).newline_before) {
            result->units = TOK(p).u.str.units;
            result->len = TOK(p).u.str.len;
            const JsLabel *l = label_find(p, result->units, result->len);
            const char *bad = NULL;
            if (!l || (!is_break && l->kind == JS_LABEL_PLAIN))
                bad = "Undefined label '%U'";
            else if (!is_break && l->kind != JS_LABEL_LOOP)
                bad = "Illegal continue statement: '%U' does not denote an iteration statement";
            if (bad) {
                result = perr(p, TOK(p).pos,
                              js_compile_error_fmt(p->arena->vm, bad, result->units, result->len));
                break;
            }
            if (!advance(p)) {
                result = NULL;
                break;
            }
        } else if (!TOK(p).newline_before && TOK(p).kind != JS_T_SEMI && TOK(p).kind != JS_T_RBRACE &&
                   TOK(p).kind != JS_T_EOF) {
            result = perr_here(p, unexpected_text(p)); /* V8 reads a label here: `break await;` */
            break;
        } else if (is_break) {
            if (p->loop_depth == 0 && p->switch_depth == 0) {
                result = perr(p, pos, "'break' outside loop or switch");
                break;
            }
        } else if (p->loop_depth == 0) {
            result = perr(p, pos, "'continue' outside loop");
            break;
        }
        if (!expect_semicolon(p))
            result = NULL;
        break;
    }
    case JS_T_RETURN: {
        if (!p->in_function && !p->allow_return) {
            result = perr(p, pos, "'return' outside function");
            break;
        }
        if (!advance(p))
            break;
        result = new_node(p, JS_AST_RETURN, pos);
        if (!result)
            break;
        if (TOK(p).kind != JS_T_SEMI && TOK(p).kind != JS_T_RBRACE &&
            TOK(p).kind != JS_T_EOF && !TOK(p).newline_before) {
            result->a = parse_expression(p);
            if (!result->a) {
                result = NULL;
                break;
            }
        }
        if (!expect_semicolon(p))
            result = NULL;
        break;
    }
    case JS_T_THROW: {
        if (!advance(p))
            break;
        if (TOK(p).newline_before) {
            result = perr(p, pos, "no line break allowed after 'throw'");
            break;
        }
        result = new_node(p, JS_AST_THROW, pos);
        if (!result)
            break;
        result->a = parse_expression(p);
        if (!result->a || !expect_semicolon(p))
            result = NULL;
        break;
    }
    case JS_T_FUNCTION:
        if (!advance(p))
            break;
        result = parse_func_common(p, 0, true, pos, JS_AST_FUNC_DECL, pos);
        break;
    case JS_T_IMPORT: {
        JsToken nx = peek(p);
        if (nx.kind == JS_T_LPAREN || nx.kind == JS_T_DOT) {
            /* dynamic import() / import.meta: expression position (the
             * expression parser rejects import.meta with its own error) */
            JsAstNode *e = parse_expression(p);
            if (!e || !expect_semicolon(p))
                break;
            result = new_node(p, JS_AST_EXPR_STMT, pos);
            if (result)
                result->a = e;
            break;
        }
        result = perr(p, pos, "import/export is only allowed at the top level of a module");
        break;
    }
    case JS_T_EXPORT:
        result = perr(p, pos, "import/export is only allowed at the top level of a module");
        break;
    case JS_T_RESERVED:
        if (units_is(t->u.str.units, t->u.str.len, "debugger")) {
            /* a no-op: there is no debugger to stop in, as V8 has none without an inspector */
            result = new_node(p, JS_AST_EMPTY, pos);
            if (result && (!advance(p) || !expect_semicolon(p)))
                result = NULL;
            break;
        }
        if (tok_is_reserved(t, "var"))
            result = perr_var(p);
        else if (tok_is_reserved(t, "class"))
            result = perr_class(p, true);
        else if (tok_is_reserved(t, "with"))
            result = perr(p, pos, "'with' is not allowed in strict mode");
        else
            result = perr_reserved(p); /* an expression statement, then */
        break;
    case JS_T_IDENT: {
        if (tok_is_name(t, "async")) {
            JsToken nx = peek(p);
            if (nx.kind == JS_T_FUNCTION && !nx.newline_before) {
                if (!advance(p) || !advance(p))
                    break;
                result = parse_func_common(p, JS_F_ASYNC, true, pos, JS_AST_FUNC_DECL, pos);
                break;
            }
        }
        if (peek(p).kind == JS_T_COLON) {
            JsAstNode *label = parse_ident(p, "expected label");
            if (!label || !label_push(p, label->units, label->len, pos))
                break;
            if (!advance(p)) /* skip ':' */
                break;
            p->pending_labels = nlabels + 1;
            p->pending_own = own + 1;
            JsAstNode *body = parse_substatement(p);
            p->label_count--;
            if (!body)
                break;
            result = new_node(p, JS_AST_LABELED, pos);
            if (result) {
                result->units = label->units;
                result->len = label->len;
                result->a = body;
            }
            break;
        }
        /* fall through to expression statement */
        JsAstNode *e = parse_expression(p);
        if (!e || !expect_semicolon(p))
            break;
        result = new_node(p, JS_AST_EXPR_STMT, pos);
        if (result)
            result->a = e;
        break;
    }
    default: {
        JsAstNode *e = parse_expression(p);
        if (!e || !expect_semicolon(p))
            break;
        result = new_node(p, JS_AST_EXPR_STMT, pos);
        if (result)
            result->a = e;
        break;
    }
    }

    if (result && !check_cover_drained(p))
        result = NULL;
    leave(p);
    return result;
}

/* ---- imports / exports ---- */

/* `with { type: "json" }` after a module specifier: n->d becomes a STRING node holding the
 * attributes as the module request carries them, each "\0key\0value", sorted by key (the
 * compiler appends it to the specifier: a module and a JSON module of one specifier are two
 * requests). Keys are identifier names or strings, values strings; a key may not repeat. */
static bool parse_import_attributes(JsParser *p, JsAstNode *n) {
    if (!advance(p) || !advance(p)) /* with { */
        return false;
    const uint16_t *keys[32], *vals[32];
    uint32_t klen[32], vlen[32], count = 0;
    while (TOK(p).kind != JS_T_RBRACE) {
        JsToken *k = &TOK(p);
        if (k->kind != JS_T_STR && k->kind != JS_T_IDENT && k->kind != JS_T_RESERVED &&
            !(k->kind >= JS_T_AWAIT && k->kind <= JS_T_WHILE)) {
            perr_here(p, unexpected_text(p));
            return false;
        }
        if (count == 32) {
            perr_here(p, "too many import attributes");
            return false;
        }
        const uint16_t *ku = k->u.str.units;
        uint32_t kl = k->u.str.len;
        if (k->kind != JS_T_STR && k->kind != JS_T_IDENT && k->kind != JS_T_RESERVED) {
            /* a keyword token: its text from the source */
            ku = p->lx.src + k->pos;
            kl = k->end - k->pos;
        }
        for (uint32_t i = 0; i < count; i++)
            if (klen[i] == kl && !memcmp(keys[i], ku, kl * sizeof *ku)) {
                fail_at(p, k->pos, js_compile_error_fmt(p->arena->vm, "Import attribute has duplicate key '%U'", ku, kl));
                return false;
            }
        if (!advance(p) || !expect(p, JS_T_COLON, "expected ':'"))
            return false;
        if (TOK(p).kind != JS_T_STR) {
            perr_here(p, unexpected_text(p));
            return false;
        }
        keys[count] = ku;
        klen[count] = kl;
        vals[count] = TOK(p).u.str.units;
        vlen[count] = TOK(p).u.str.len;
        count++;
        if (!advance(p))
            return false;
        if (TOK(p).kind == JS_T_COMMA) {
            if (!advance(p))
                return false;
        } else if (TOK(p).kind != JS_T_RBRACE) {
            perr_here(p, unexpected_text(p));
            return false;
        }
    }
    if (!advance(p)) /* } */
        return false;
    if (!count)
        return true;
    for (uint32_t i = 1; i < count; i++) /* sorted by key: one request however it is written */
        for (uint32_t j = i; j > 0; j--) {
            uint32_t m = klen[j] < klen[j - 1] ? klen[j] : klen[j - 1];
            int c = memcmp(keys[j], keys[j - 1], m * sizeof(uint16_t));
            if (c > 0 || (c == 0 && klen[j] >= klen[j - 1]))
                break;
            const uint16_t *tu = keys[j]; keys[j] = keys[j - 1]; keys[j - 1] = tu;
            uint32_t tl = klen[j]; klen[j] = klen[j - 1]; klen[j - 1] = tl;
            tu = vals[j]; vals[j] = vals[j - 1]; vals[j - 1] = tu;
            tl = vlen[j]; vlen[j] = vlen[j - 1]; vlen[j - 1] = tl;
        }
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++)
        total += 2 + klen[i] + vlen[i];
    uint16_t *enc = js_arena_alloc(p->arena, total * sizeof *enc);
    JsAstNode *a = enc ? new_node(p, JS_AST_STRING, n->pos) : NULL;
    if (!a) {
        perr_here(p, "out of memory");
        return false;
    }
    uint32_t o = 0;
    for (uint32_t i = 0; i < count; i++) {
        enc[o++] = 0;
        memcpy(enc + o, keys[i], klen[i] * sizeof *enc);
        o += klen[i];
        enc[o++] = 0;
        memcpy(enc + o, vals[i], vlen[i] * sizeof *enc);
        o += vlen[i];
    }
    a->units = enc;
    a->len = total;
    n->d = a;
    return true;
}

static JsAstNode *parse_module_source(JsParser *p, JsAstNode *n) {
    if (TOK(p).kind != JS_T_STR)
        return perr_here(p, "expected module specifier string");
    n->units = TOK(p).u.str.units;
    n->len = TOK(p).u.str.len;
    if (!advance(p))
        return NULL;
    JsToken *t = &TOK(p);
    if (t->kind == JS_T_RESERVED && units_is(t->u.str.units, t->u.str.len, "with") &&
        peek(p).kind == JS_T_LBRACE && !parse_import_attributes(p, n))
        return NULL;
    if (!expect_semicolon(p))
        return NULL;
    return n;
}

static JsAstNode *parse_import_named(JsParser *p, JsNodeVec *specs) {
    if (!advance(p)) /* { */
        return NULL;
    while (TOK(p).kind != JS_T_RBRACE) {
        JsAstNode *spec = new_node(p, JS_AST_IMPORT_SPEC, TOK(p).pos);
        if (!spec)
            return NULL;
        /* The name of the export is any IdentifierName; the name it is bound to here, after
         * `as` or the same one without, is a binding. V8 calls every word that cannot be one
         * reserved, whichever it is (`import { if }`, `import { x as let }`). */
        bool binds = tok_is_binding_name(&TOK(p));
        JsAstNode *local = parse_name_any(p, "expected import name");
        if (!local)
            return NULL;
        spec->units = local->units;
        spec->len = local->len;
        if (tok_is_name(&TOK(p), "as")) {
            if (!advance(p))
                return NULL;
            binds = tok_is_binding_name(&TOK(p));
            local = parse_name_any(p, "expected local name");
            if (!local)
                return NULL;
        }
        if (!binds)
            return perr(p, local->pos, "Unexpected reserved word");
        if (!check_binding_name(p, local) || !declare(p, local->units, local->len, local->pos, JS_BIND_LEX))
            return NULL;
        spec->units2 = local->units;
        spec->len2 = local->len;
        if (!vec_push(p, specs, spec))
            return NULL;
        if (TOK(p).kind != JS_T_RBRACE && !expect(p, JS_T_COMMA, "expected ',' or '}'"))
            return NULL;
    }
    return advance(p) ? (JsAstNode *)1 : NULL;
}

static JsAstNode *parse_import(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p))
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_IMPORT, pos);
    if (!n)
        return NULL;
    JsNodeVec specs = {0};

    if (TOK(p).kind == JS_T_STR) { /* import "m"; */
        vec_store(n, &specs);
        return parse_module_source(p, n);
    }
    if (TOK(p).kind == JS_T_IDENT) { /* default binding */
        JsAstNode *local = check_binding_name(p, parse_ident(p, "expected binding"));
        if (!local || !declare(p, local->units, local->len, pos, JS_BIND_LEX))
            return NULL;
        JsAstNode *spec = new_node(p, JS_AST_IMPORT_SPEC, local->pos);
        if (!spec)
            return NULL;
        spec->flags |= JS_F_SPEC_DEFAULT;
        spec->units2 = local->units;
        spec->len2 = local->len;
        if (!vec_push(p, &specs, spec))
            return NULL;
        uint32_t comma = TOK(p).pos;
        if (accept(p, JS_T_COMMA)) {
            if (TOK(p).kind != JS_T_STAR && TOK(p).kind != JS_T_LBRACE)
                return perr(p, comma, "Unexpected token ','"); /* V8 has not looked past it */
        } else {
            goto from;
        }
    }
    if (TOK(p).kind == JS_T_STAR) {
        if (!advance(p) || !expect_contextual(p, "as"))
            return NULL;
        JsAstNode *local = check_binding_name(p, parse_ident(p, "expected namespace name"));
        if (!local || !declare(p, local->units, local->len, pos, JS_BIND_LEX))
            return NULL;
        JsAstNode *spec = new_node(p, JS_AST_IMPORT_SPEC, local->pos);
        if (!spec)
            return NULL;
        spec->flags |= JS_F_SPEC_NS;
        spec->units2 = local->units;
        spec->len2 = local->len;
        if (!vec_push(p, &specs, spec))
            return NULL;
    } else if (TOK(p).kind == JS_T_LBRACE) {
        if (!parse_import_named(p, &specs))
            return NULL;
    } else if (specs.count == 0) {
        return perr_here(p, "expected import bindings or module specifier");
    }
from:
    if (!expect_contextual(p, "from"))
        return NULL;
    vec_store(n, &specs);
    return parse_module_source(p, n);
}

/*
 * The names between the braces of `export { ... }`, up to and including the `}`. Each is any
 * IdentifierName, as a re-export (`export { if } from 'm'`) names another module's exports;
 * without a `from` the first of each pair is a binding of this module, and a reserved word is
 * none. RESERVED[0] to RESERVED[1] is where the first such word is (and *HAS_RESERVED that
 * there is one).
 */
static bool parse_export_specs(JsParser *p, JsNodeVec *specs, bool *has_reserved,
                               uint32_t reserved[2]) {
    while (TOK(p).kind != JS_T_RBRACE) {
        JsAstNode *spec = new_node(p, JS_AST_EXPORT_SPEC, TOK(p).pos);
        if (!spec)
            return false;
        if (!*has_reserved && tok_is_any_name(&TOK(p)) && !tok_is_binding_name(&TOK(p))) {
            *has_reserved = true;
            reserved[0] = TOK(p).pos;
            reserved[1] = TOK(p).end;
        }
        JsAstNode *local = parse_name_any(p, "expected export name");
        if (!local)
            return false;
        spec->units = local->units;
        spec->len = local->len;
        if (tok_is_name(&TOK(p), "as")) {
            if (!advance(p))
                return false;
            JsAstNode *exported = parse_name_any(p, "expected export alias");
            if (!exported)
                return false;
            spec->units2 = exported->units;
            spec->len2 = exported->len;
        } else {
            spec->units2 = spec->units;
            spec->len2 = spec->len;
        }
        if (!vec_push(p, specs, spec))
            return false;
        if (TOK(p).kind != JS_T_RBRACE && !expect(p, JS_T_COMMA, "expected ',' or '}'"))
            return false;
    }
    return advance(p); /* } */
}

/* The exports of a declaration's pattern, each where the declaration begins */
static bool export_pattern(JsParser *p, const JsAstNode *pat, uint32_t pos) {
    switch ((JsAstKind)pat->kind) {
    case JS_AST_IDENT:
        return export_add(p, pat->units, pat->len, pat->units, pat->len, pos, false, true);
    case JS_AST_DEFAULT:
    case JS_AST_REST:
        return export_pattern(p, pat->a, pos);
    case JS_AST_ARRAY_PATTERN:
        for (uint32_t i = 0; i < pat->count; i++)
            if (pat->items[i]->kind != JS_AST_HOLE && !export_pattern(p, pat->items[i], pos))
                return false;
        return true;
    case JS_AST_OBJECT_PATTERN:
        for (uint32_t i = 0; i < pat->count; i++) {
            const JsAstNode *prop = pat->items[i];
            if (!export_pattern(p, prop->kind == JS_AST_REST ? prop->a : prop->b, pos))
                return false;
        }
        return true;
    default:
        return true;
    }
}

static const uint16_t DOT_DEFAULT[] = {'.', 'd', 'e', 'f', 'a', 'u', 'l', 't'};
static const uint16_t DEFAULT_NAME[] = {'d', 'e', 'f', 'a', 'u', 'l', 't'};

static JsAstNode *parse_export_inner(JsParser *p);

/* An export declaration, and what it exports, for exports_check */
static JsAstNode *parse_export(JsParser *p) {
    uint32_t decl_pos = peek(p).pos; /* where V8 places a declaration's exports */
    JsAstNode *n = parse_export_inner(p);
    if (!n)
        return NULL;
    bool ok = true;
    switch ((JsAstKind)n->kind) {
    case JS_AST_EXPORT_DEFAULT: {
        const JsAstNode *f = (n->flags & JS_F_DEFAULT_DECL) ? n->a : NULL;
        ok = f && f->len ? export_add(p, f->units, f->len, DEFAULT_NAME, 7, n->pos2, false, true)
                         : export_add(p, DOT_DEFAULT, 8, DEFAULT_NAME, 7, n->pos2, false, true);
        break;
    }
    case JS_AST_EXPORT_ALL:
        if (n->len2) {
            /* `export * as x from 'm'` is V8's `import * as .ns-exportN from 'm'; export
             * { .ns-exportN as x }`, at `x` */
            char buf[32];
            int k = snprintf(buf, sizeof buf, ".ns-export%u", p->ns_exports++);
            uint16_t *local = js_arena_alloc(p->arena, (size_t)k * sizeof *local);
            if (!local) {
                perr(p, n->pos, "out of memory");
                return NULL;
            }
            for (int i = 0; i < k; i++)
                local[i] = (uint8_t)buf[i];
            ok = export_add(p, local, (uint32_t)k, n->units2, n->len2, n->src_start, false, true);
        }
        break;
    case JS_AST_EXPORT_NAMED:
        if (n->a && n->a->kind == JS_AST_LET_DECL) {
            for (uint32_t i = 0; ok && i < n->a->count; i++)
                ok = export_pattern(p, n->a->items[i]->a, decl_pos);
        } else if (n->a) {
            ok = export_add(p, n->a->units, n->a->len, n->a->units, n->a->len, decl_pos, false, true);
        } else {
            bool from = (n->flags & JS_F_REEXPORT) != 0;
            for (uint32_t i = 0; ok && i < n->count; i++) {
                const JsAstNode *sp = n->items[i];
                ok = from ? export_add(p, NULL, 0, sp->units2, sp->len2, sp->pos, true, true)
                          : export_add(p, sp->units, sp->len, sp->units2, sp->len2, sp->pos, false, false);
            }
        }
        break;
    default:
        break;
    }
    return ok ? n : NULL;
}

static JsAstNode *parse_export_inner(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p))
        return NULL;

    if (TOK(p).kind == JS_T_DEFAULT) {
        uint32_t default_pos = TOK(p).pos;
        if (!advance(p))
            return NULL;
        JsAstNode *n = new_node(p, JS_AST_EXPORT_DEFAULT, pos);
        if (!n)
            return NULL;
        n->pos2 = default_pos; /* where V8 places the export */
        if (TOK(p).kind == JS_T_FUNCTION) {
            uint32_t start = TOK(p).pos;
            if (!advance(p))
                return NULL;
            n->flags |= JS_F_DEFAULT_DECL;
            p->default_fn = true;
            n->a = parse_func_common(p, 0, false, pos, JS_AST_FUNC_EXPR, start);
            return n->a ? n : NULL;
        }
        if (tok_is_name(&TOK(p), "async") && peek(p).kind == JS_T_FUNCTION &&
            !peek(p).newline_before) {
            uint32_t start = TOK(p).pos;
            if (!advance(p) || !advance(p))
                return NULL;
            n->flags |= JS_F_DEFAULT_DECL;
            p->default_fn = true;
            n->a = parse_func_common(p, JS_F_ASYNC, false, pos, JS_AST_FUNC_EXPR, start);
            return n->a ? n : NULL;
        }
        n->a = parse_assign_scoped(p);
        /* the value's hidden binding, declared at `default` once the value is read */
        if (n->a && !declare(p, DOT_DEFAULT, 8, default_pos, JS_BIND_LEX))
            return NULL;
        if (!n->a || !expect_semicolon(p))
            return NULL;
        return n;
    }

    if (TOK(p).kind == JS_T_STAR) {
        uint32_t star = TOK(p).pos;
        if (!advance(p))
            return NULL;
        JsAstNode *n = new_node(p, JS_AST_EXPORT_ALL, pos);
        if (!n)
            return NULL;
        n->pos2 = star; /* where a link error about a name it brings points */
        if (tok_is_name(&TOK(p), "as")) {
            if (!advance(p))
                return NULL;
            JsAstNode *alias = parse_name_any(p, "expected export alias");
            if (!alias)
                return NULL;
            n->units2 = alias->units;
            n->len2 = alias->len;
            n->src_start = alias->pos; /* where V8 places the export */
        }
        if (!expect_contextual(p, "from"))
            return NULL;
        return parse_module_source(p, n);
    }

    if (TOK(p).kind == JS_T_LBRACE) {
        if (!advance(p))
            return NULL;
        JsAstNode *n = new_node(p, JS_AST_EXPORT_NAMED, pos);
        if (!n)
            return NULL;
        JsNodeVec specs = {0};
        bool has_reserved = false;
        uint32_t reserved[2] = {0, 0};
        bool closed = parse_export_specs(p, &specs, &has_reserved, reserved);
        if (closed && tok_is_name(&TOK(p), "from")) {
            if (!advance(p))
                return NULL;
            n->flags |= JS_F_REEXPORT;
            vec_store(n, &specs);
            return parse_module_source(p, n); /* re-export */
        }
        /* No `from`, so the names are this module's bindings. V8 learns that only here, after
         * the list, and also when the list itself went wrong: its report of the reserved word
         * is the earlier of the two and the one it keeps. */
        if (has_reserved)
            return perr_earlier(p, reserved[0], reserved[1], "Unexpected reserved word");
        if (!closed)
            return NULL;
        vec_store(n, &specs);
        if (!expect_semicolon(p))
            return NULL;
        return n;
    }

    /* export <declaration>: after `export`, `let` only ever begins one, so what follows it is
     * read as the binding and is where an error about it points */
    JsAstNode *decl = NULL;
    if (TOK(p).kind == JS_T_LET || TOK(p).kind == JS_T_CONST) {
        decl = parse_var_decl(p, TOK(p).kind == JS_T_CONST);
        if (decl && !expect_semicolon(p))
            decl = NULL;
    } else if (TOK(p).kind == JS_T_FUNCTION) {
        uint32_t start = TOK(p).pos;
        if (!advance(p))
            return NULL;
        decl = parse_func_common(p, 0, true, TOK(p).pos, JS_AST_FUNC_DECL, start);
    } else if (tok_is_name(&TOK(p), "async") && peek(p).kind == JS_T_FUNCTION &&
               !peek(p).newline_before) {
        uint32_t start = TOK(p).pos;
        if (!advance(p) || !advance(p))
            return NULL;
        decl = parse_func_common(p, JS_F_ASYNC, true, TOK(p).pos, JS_AST_FUNC_DECL, start);
    } else if (escaped_async_function(p)) {
        return perr_here(p, "Keyword must not contain escaped characters");
    } else if (TOK(p).kind == JS_T_IDENT &&
               units_is(TOK(p).u.str.units, TOK(p).u.str.len, "async")) {
        return perr_here(p, "Unexpected token 'async'"); /* V8 has taken it for the keyword */
    } else if (tok_is_reserved(&TOK(p), "var")) {
        return perr_var(p); /* the declarations lamassu leaves out */
    } else if (tok_is_reserved(&TOK(p), "class")) {
        return perr_class(p, true);
    } else {
        /* nothing that can be exported: the fault is the `export`, as V8 has it */
        return perr(p, pos, "Unexpected token 'export'");
    }
    if (!decl)
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_EXPORT_NAMED, pos);
    if (!n)
        return NULL;
    n->a = decl;
    return n;
}

/* ---- entry point ---- */

bool js_parse_module(JsArena *arena, const uint16_t *src, size_t len, bool allow_return,
                     JsParseResult *out) {
    JsParser p;
    memset(&p, 0, sizeof p);
    p.arena = arena;
    p.allow_return = allow_return;
    p.fn_async = true; /* module top level: await allowed (TLA) */
    js_lexer_init(&p.lx, arena, src, len);

    out->module = NULL;
    out->err_msg = NULL;
    out->err_pos = 0;

    p.bind_kind = JS_BIND_NONE;
    if (!scope_push(&p, JS_SCOPE_MODULE) || !advance(&p))
        goto fail;
    JsAstNode *mod = new_node(&p, JS_AST_MODULE, 0);
    if (!mod)
        goto fail;
    mod->units = src; /* the source, which the compiler keeps for its functions' text */
    mod->len = (uint32_t)len;
    JsNodeVec stmts = {0};
    while (TOK(&p).kind != JS_T_EOF) {
        JsAstNode *item;
        if (TOK(&p).kind == JS_T_IMPORT && peek(&p).kind != JS_T_LPAREN &&
            peek(&p).kind != JS_T_DOT)
            item = parse_import(&p);
        else if (TOK(&p).kind == JS_T_EXPORT)
            item = parse_export(&p);
        else
            item = parse_statement(&p);
        if (!item || !vec_push(&p, &stmts, item))
            goto fail;
        if (!check_cover_drained(&p))
            goto fail;
    }
    if (!exports_check(&p) || !octal_check(&p, 0))
        goto fail;
    vec_store(mod, &stmts);
    if (p.module_tla)
        mod->flags |= JS_F_HAS_TLA;
    out->module = mod;
    return true;

fail:
    out->err_msg = p.err_msg ? p.err_msg : "parse error";
    out->err_pos = p.err_pos;
    return false;
}
