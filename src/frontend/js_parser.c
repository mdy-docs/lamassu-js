#include "js_syntax.h"

/*
 * Recursive-descent / Pratt parser for the lamassu subset: always strict,
 * always module goal, no var/class/eval/new. Parenthesized expressions and
 * arrow parameter lists share a cover grammar (JS_AST_COVER) resolved when
 * `=>` is (or isn't) seen. Nesting depth is hard-limited so hostile input
 * cannot exhaust the C stack.
 */

#define JS_PARSE_MAX_DEPTH 256

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
    int loop_depth, switch_depth;
    /* pending {a = 1} cover-initializers awaiting pattern conversion */
    int cover_init_open;
    uint32_t cover_init_pos;
    bool module_tla;
} JsParser;

typedef struct JsNodeVec {
    JsAstNode **items;
    uint32_t count, cap;
} JsNodeVec;

#define TOK(p) ((p)->lx.tok)

static JsAstNode *parse_statement(JsParser *p);
static JsAstNode *parse_block(JsParser *p);
static JsAstNode *parse_expression(JsParser *p);
static JsAstNode *parse_new_expr(JsParser *p);
static JsAstNode *parse_assign(JsParser *p);
static JsAstNode *parse_arguments(JsParser *p, JsNodeVec *out);
static JsAstNode *parse_binding_target(JsParser *p);
static JsAstNode *parse_binding_element(JsParser *p);
static JsAstNode *parse_func_common(JsParser *p, uint8_t flags, bool need_name,
                                    uint32_t pos, uint8_t kind);
static JsAstNode *expr_to_pattern(JsParser *p, JsAstNode *n, bool binding);

static JsAstNode *perr(JsParser *p, uint32_t pos, const char *msg) {
    if (!p->err_msg) {
        p->err_msg = msg;
        p->err_pos = pos;
    }
    return NULL;
}

static JsAstNode *perr_here(JsParser *p, const char *msg) {
    return perr(p, TOK(p).pos, msg);
}

static bool advance(JsParser *p) {
    if (p->err_msg)
        return false;
    if (!js_lexer_next(&p->lx)) {
        p->err_msg = p->lx.err_msg;
        p->err_pos = p->lx.err_pos;
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
    if (t->kind != JS_T_IDENT)
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

static const char *reserved_message(const JsToken *t) {
    if (units_is(t->u.str.units, t->u.str.len, "var"))
        return "'var' is not supported; use 'let' or 'const'";
    if (units_is(t->u.str.units, t->u.str.len, "class") ||
        units_is(t->u.str.units, t->u.str.len, "extends") ||
        units_is(t->u.str.units, t->u.str.len, "super"))
        return "classes are not supported";
    if (units_is(t->u.str.units, t->u.str.len, "with"))
        return "'with' is not allowed in strict mode";
    if (units_is(t->u.str.units, t->u.str.len, "debugger"))
        return "'debugger' is not supported";
    return "reserved word cannot be used as an identifier";
}

/* Identifier node from the current token (consumes it). */
static JsAstNode *parse_ident(JsParser *p, const char *msg) {
    if (TOK(p).kind == JS_T_RESERVED)
        return perr_here(p, reserved_message(&TOK(p)));
    if (TOK(p).kind != JS_T_IDENT)
        return perr_here(p, msg);
    JsAstNode *n = new_node(p, JS_AST_IDENT, TOK(p).pos);
    if (!n)
        return NULL;
    n->units = TOK(p).u.str.units;
    n->len = TOK(p).u.str.len;
    if (!advance(p))
        return NULL;
    return n;
}

static JsAstNode *check_binding_name(JsParser *p, JsAstNode *ident) {
    if (!ident)
        return NULL;
    if (units_is(ident->units, ident->len, "eval") ||
        units_is(ident->units, ident->len, "arguments"))
        return perr(p, ident->pos, "cannot bind 'eval' or 'arguments' in strict mode");
    return ident;
}

/* IdentifierName: keywords allowed (obj.default, import { default as x }). */
static JsAstNode *parse_name_any(JsParser *p, const char *msg) {
    JsToken *t = &TOK(p);
    if (t->kind != JS_T_IDENT && t->kind != JS_T_RESERVED &&
        !(t->kind >= JS_T_AWAIT && t->kind <= JS_T_WHILE))
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

static bool check_cover_drained(JsParser *p) {
    if (p->err_msg)
        return false;
    if (p->cover_init_open > 0) {
        perr(p, p->cover_init_pos,
             "shorthand property initializer is only valid in destructuring");
        return false;
    }
    return true;
}

/* ---- patterns (declaration/parameter positions) ---- */

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
            rest->a = check_binding_name(p, parse_ident(p, "expected identifier after '...'"));
            if (!rest->a || !vec_push(p, &props, rest))
                return NULL;
            break; /* rest must be last */
        }
        JsAstNode *prop = new_node(p, JS_AST_PROPERTY, TOK(p).pos);
        if (!prop)
            return NULL;
        if (TOK(p).kind == JS_T_LBRACK) {
            prop->flags |= JS_F_COMPUTED;
            if (!advance(p))
                return NULL;
            prop->a = parse_assign(p);
            if (!prop->a || !expect(p, JS_T_RBRACK, "expected ']'"))
                return NULL;
            if (!expect(p, JS_T_COLON, "expected ':' after computed key"))
                return NULL;
            prop->b = parse_binding_element(p);
        } else if (TOK(p).kind == JS_T_STR || TOK(p).kind == JS_T_NUM) {
            prop->a = new_node(p, TOK(p).kind == JS_T_STR ? JS_AST_STRING : JS_AST_NUMBER,
                               TOK(p).pos);
            if (!prop->a)
                return NULL;
            if (TOK(p).kind == JS_T_STR) {
                prop->a->units = TOK(p).u.str.units;
                prop->a->len = TOK(p).u.str.len;
            } else {
                prop->a->number = TOK(p).u.number;
            }
            if (!advance(p) || !expect(p, JS_T_COLON, "expected ':' after property key"))
                return NULL;
            prop->b = parse_binding_element(p);
        } else {
            JsAstNode *key = parse_name_any(p, "expected property name");
            if (!key)
                return NULL;
            prop->a = key;
            if (accept(p, JS_T_COLON)) {
                prop->b = parse_binding_element(p);
            } else {
                /* shorthand: key must be a valid binding identifier */
                if (TOK(p).kind != JS_T_COMMA && TOK(p).kind != JS_T_RBRACE &&
                    TOK(p).kind != JS_T_EQ)
                    return perr_here(p, "expected ',', '}' or ':'");
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
                    JsAstNode *init = parse_assign(p);
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
            rest->a = parse_binding_target(p);
            if (!rest->a || !vec_push(p, &elems, rest))
                return NULL;
            break;
        }
        JsAstNode *el = parse_binding_element(p);
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
    switch (TOK(p).kind) {
    case JS_T_LBRACE: n = parse_object_pattern(p); break;
    case JS_T_LBRACK: n = parse_array_pattern(p); break;
    default:
        n = check_binding_name(p, parse_ident(p, "expected binding identifier"));
        break;
    }
    leave(p);
    return n;
}

static JsAstNode *parse_binding_element(JsParser *p) {
    JsAstNode *target = parse_binding_target(p);
    if (!target)
        return NULL;
    if (accept(p, JS_T_EQ)) {
        JsAstNode *def = new_node(p, JS_AST_DEFAULT, target->pos);
        JsAstNode *init = parse_assign(p);
        if (!def || !init)
            return NULL;
        def->a = target;
        def->b = init;
        return def;
    }
    return target;
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

static JsAstNode *expr_to_pattern(JsParser *p, JsAstNode *n, bool binding) {
    if (!n)
        return NULL;
    switch (n->kind) {
    case JS_AST_IDENT:
        if (units_is(n->units, n->len, "eval") ||
            units_is(n->units, n->len, "arguments"))
            return perr(p, n->pos, "cannot assign to 'eval' or 'arguments' in strict mode");
        return n;
    case JS_AST_MEMBER:
        if (binding)
            return perr(p, n->pos, "property access is not a valid binding target");
        if (!is_simple_target(n))
            return perr(p, n->pos, "optional chain is not a valid assignment target");
        return n;
    case JS_AST_ASSIGN: {
        if (n->op != JS_T_EQ)
            return perr(p, n->pos, "invalid destructuring target");
        JsAstNode *def = new_node(p, JS_AST_DEFAULT, n->pos);
        if (!def)
            return NULL;
        def->a = expr_to_pattern(p, n->a, binding);
        def->b = n->b;
        return def->a ? def : NULL;
    }
    case JS_AST_ARRAY: {
        if (n->flags & JS_F_PAREN)
            return perr(p, n->pos, "parenthesized destructuring pattern");
        JsAstNode *pat = new_node(p, JS_AST_ARRAY_PATTERN, n->pos);
        if (!pat)
            return NULL;
        for (uint32_t i = 0; i < n->count; i++) {
            JsAstNode *el = n->items[i];
            if (el->kind == JS_AST_HOLE)
                continue;
            if (el->kind == JS_AST_SPREAD) {
                if (i + 1 != n->count)
                    return perr(p, el->pos, "rest element must be last");
                JsAstNode *rest = new_node(p, JS_AST_REST, el->pos);
                if (!rest)
                    return NULL;
                rest->a = expr_to_pattern(p, el->a, binding);
                if (!rest->a)
                    return NULL;
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
            return perr(p, n->pos, "parenthesized destructuring pattern");
        JsAstNode *pat = new_node(p, JS_AST_OBJECT_PATTERN, n->pos);
        if (!pat)
            return NULL;
        bool had_cover = false;
        for (uint32_t i = 0; i < n->count; i++) {
            JsAstNode *prop = n->items[i];
            if (prop->kind == JS_AST_SPREAD) {
                if (i + 1 != n->count)
                    return perr(p, prop->pos, "rest element must be last");
                JsAstNode *rest = new_node(p, JS_AST_REST, prop->pos);
                if (!rest)
                    return NULL;
                rest->a = expr_to_pattern(p, prop->a, binding);
                if (!rest->a)
                    return NULL;
                if (rest->a->kind != JS_AST_IDENT && rest->a->kind != JS_AST_MEMBER)
                    return perr(p, prop->pos, "invalid rest target in object pattern");
                n->items[i] = rest;
                continue;
            }
            if (prop->flags & (JS_F_METHOD | JS_F_PROP_ASYNC))
                return perr(p, prop->pos, "method is not a valid destructuring target");
            if (prop->flags & JS_F_COVER_INIT) {
                prop->flags &= (uint8_t)~JS_F_COVER_INIT;
                had_cover = true;
                /* value is already DEFAULT(ident, init) */
                if (binding && !check_binding_name(p, prop->b->a))
                    return NULL;
                continue;
            }
            prop->b = expr_to_pattern(p, prop->b, binding);
            if (!prop->b)
                return NULL;
        }
        if (had_cover)
            p->cover_init_open--;
        vec_store(pat, &(JsNodeVec){.items = n->items, .count = n->count});
        return pat;
    }
    case JS_AST_ARRAY_PATTERN:
    case JS_AST_OBJECT_PATTERN:
    case JS_AST_DEFAULT:
    case JS_AST_REST:
        return n; /* already a pattern (nested conversion) */
    default:
        return perr(p, n->pos, "invalid assignment target");
    }
}

/* ---- cover grammar: ( ... ) ---- */

static JsAstNode *parse_paren_cover(JsParser *p, uint8_t extra_flags) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* ( */
        return NULL;
    JsAstNode *cover = new_node(p, JS_AST_COVER, pos);
    if (!cover)
        return NULL;
    cover->flags = extra_flags;
    JsNodeVec items = {0};
    while (TOK(p).kind != JS_T_RPAREN) {
        if (TOK(p).kind == JS_T_DOTDOTDOT) {
            cover->flags |= JS_F_COVER_SPREAD;
            JsAstNode *sp = new_node(p, JS_AST_SPREAD, TOK(p).pos);
            if (!sp || !advance(p))
                return NULL;
            sp->a = parse_assign(p);
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
    if (!expect(p, JS_T_RPAREN, "expected ')'"))
        return NULL;
    vec_store(cover, &items);
    return cover;
}

/* A cover that is not followed by `=>`: must be a plain parenthesized expr. */
static JsAstNode *collapse_cover(JsParser *p, JsAstNode *cover) {
    if (cover->flags & JS_F_COVER_ASYNC) {
        /* `async (...)` not followed by => is a call to an `async` binding */
        JsAstNode *callee = new_node(p, JS_AST_IDENT, cover->pos);
        JsAstNode *call = new_node(p, JS_AST_CALL, cover->pos);
        static const uint16_t async_name[] = {'a', 's', 'y', 'n', 'c'};
        if (!callee || !call)
            return NULL;
        callee->units = async_name;
        callee->len = 5;
        call->a = callee;
        call->items = cover->items;
        call->count = cover->count;
        return call; /* spread items are valid as call arguments */
    }
    if (cover->count == 0)
        return perr(p, cover->pos, "expected expression");
    if (cover->flags & JS_F_COVER_SPREAD)
        return perr(p, cover->pos, "'...' is only valid in arrow parameters here");
    if (cover->flags & JS_F_COVER_TRAIL)
        return perr(p, cover->pos, "trailing comma is only valid in arrow parameters here");
    if (cover->count == 1) {
        JsAstNode *e = cover->items[0];
        e->flags |= JS_F_PAREN;
        return e;
    }
    JsAstNode *seq = new_node(p, JS_AST_SEQUENCE, cover->pos);
    if (!seq)
        return NULL;
    seq->items = cover->items;
    seq->count = cover->count;
    seq->flags |= JS_F_PAREN;
    return seq;
}

/* ---- function bodies / arrows ---- */

typedef struct {
    bool in_function, fn_async;
    int loop_depth, switch_depth;
} JsSavedCtx;

static JsSavedCtx ctx_enter_function(JsParser *p, bool is_async) {
    JsSavedCtx s = {p->in_function, p->fn_async, p->loop_depth, p->switch_depth};
    p->in_function = true;
    p->fn_async = is_async;
    p->fn_nesting++;
    p->loop_depth = 0;
    p->switch_depth = 0;
    return s;
}

static void ctx_leave_function(JsParser *p, JsSavedCtx s) {
    p->in_function = s.in_function;
    p->fn_async = s.fn_async;
    p->fn_nesting--;
    p->loop_depth = s.loop_depth;
    p->switch_depth = s.switch_depth;
}

static JsAstNode *parse_params(JsParser *p, JsNodeVec *out) {
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

static JsAstNode *parse_func_common(JsParser *p, uint8_t flags, bool need_name,
                                    uint32_t pos, uint8_t kind) {
    JsAstNode *fn = new_node(p, kind, pos);
    if (!fn)
        return NULL;
    fn->flags = flags;
    if (TOK(p).kind == JS_T_IDENT) {
        JsAstNode *name = check_binding_name(p, parse_ident(p, "expected name"));
        if (!name)
            return NULL;
        fn->units = name->units;
        fn->len = name->len;
    } else if (need_name) {
        return perr_here(p, "expected function name");
    }
    JsNodeVec params = {0};
    if (!parse_params(p, &params))
        return NULL;
    vec_store(fn, &params);
    JsSavedCtx s = ctx_enter_function(p, (flags & JS_F_ASYNC) != 0);
    fn->a = parse_block(p);
    ctx_leave_function(p, s);
    return fn->a ? fn : NULL;
}

static JsAstNode *build_arrow(JsParser *p, JsAstNode *params_src, bool is_async) {
    /* current token is `=>` */
    if (TOK(p).newline_before)
        return perr_here(p, "no line break allowed before '=>'");
    uint32_t pos = params_src->pos;
    JsAstNode *fn = new_node(p, JS_AST_FUNC_EXPR, pos);
    if (!fn || !advance(p))
        return NULL;
    fn->flags = (uint8_t)(JS_F_ARROW | (is_async ? JS_F_ASYNC : 0));

    JsNodeVec params = {0};
    if (params_src->kind == JS_AST_IDENT) {
        JsAstNode *id = expr_to_pattern(p, params_src, true);
        if (!id || !check_binding_name(p, id) || !vec_push(p, &params, id))
            return NULL;
    } else { /* COVER */
        for (uint32_t i = 0; i < params_src->count; i++) {
            JsAstNode *item = params_src->items[i];
            if (item->kind == JS_AST_SPREAD) {
                if (i + 1 != params_src->count)
                    return perr(p, item->pos, "rest parameter must be last");
                JsAstNode *rest = new_node(p, JS_AST_REST, item->pos);
                if (!rest)
                    return NULL;
                rest->a = expr_to_pattern(p, item->a, true);
                if (!rest->a || !vec_push(p, &params, rest))
                    return NULL;
                continue;
            }
            JsAstNode *pat = expr_to_pattern(p, item, true);
            if (!pat || !vec_push(p, &params, pat))
                return NULL;
        }
    }
    vec_store(fn, &params);

    JsSavedCtx s = ctx_enter_function(p, is_async);
    if (TOK(p).kind == JS_T_LBRACE) {
        fn->a = parse_block(p);
    } else {
        fn->flags |= JS_F_EXPR_BODY;
        fn->a = parse_assign(p);
    }
    ctx_leave_function(p, s);
    return fn->a ? fn : NULL;
}

/* ---- primary / postfix ---- */

static JsAstNode *parse_template(JsParser *p) {
    JsAstNode *tpl = new_node(p, JS_AST_TEMPLATE, TOK(p).pos);
    if (!tpl)
        return NULL;
    JsNodeVec parts = {0};
    for (;;) {
        JsAstNode *chunk = new_node(p, JS_AST_STRING, TOK(p).pos);
        if (!chunk)
            return NULL;
        chunk->units = TOK(p).u.str.units;
        chunk->len = TOK(p).u.str.len;
        bool cont = TOK(p).u.str.cont;
        if (!vec_push(p, &parts, chunk))
            return NULL;
        if (!cont)
            break;
        if (!advance(p))
            return NULL;
        JsAstNode *e = parse_expression(p);
        if (!e || !vec_push(p, &parts, e))
            return NULL;
        if (TOK(p).kind != JS_T_RBRACE)
            return perr_here(p, "expected '}' in template literal");
        if (!js_lexer_rescan_template(&p->lx)) {
            p->err_msg = p->lx.err_msg;
            p->err_pos = p->lx.err_pos;
            return NULL;
        }
    }
    if (!advance(p)) /* past final chunk */
        return NULL;
    vec_store(tpl, &parts);
    return tpl;
}

static JsAstNode *parse_array_literal(JsParser *p) {
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

static bool prop_name_follows(JsParser *p) {
    JsToken t = peek(p);
    return t.kind != JS_T_COLON && t.kind != JS_T_LPAREN && t.kind != JS_T_COMMA &&
           t.kind != JS_T_RBRACE && t.kind != JS_T_EQ;
}

static JsAstNode *parse_object_literal(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* { */
        return NULL;
    JsNodeVec props = {0};
    bool has_cover_init = false;
    while (TOK(p).kind != JS_T_RBRACE) {
        if (TOK(p).kind == JS_T_DOTDOTDOT) {
            JsAstNode *sp = new_node(p, JS_AST_SPREAD, TOK(p).pos);
            if (!sp || !advance(p))
                return NULL;
            sp->a = parse_assign(p);
            if (!sp->a || !vec_push(p, &props, sp))
                return NULL;
        } else {
            JsAstNode *prop = new_node(p, JS_AST_PROPERTY, TOK(p).pos);
            if (!prop)
                return NULL;
            if ((tok_is_name(&TOK(p), "get") || tok_is_name(&TOK(p), "set")) &&
                prop_name_follows(p))
                return perr_here(p, "getter/setter properties are not supported");
            if (tok_is_name(&TOK(p), "async") && !peek(p).newline_before &&
                prop_name_follows(p)) {
                prop->flags |= JS_F_PROP_ASYNC;
                if (!advance(p))
                    return NULL;
            }
            /* key */
            if (TOK(p).kind == JS_T_LBRACK) {
                prop->flags |= JS_F_COMPUTED;
                if (!advance(p))
                    return NULL;
                prop->a = parse_assign(p);
                if (!prop->a || !expect(p, JS_T_RBRACK, "expected ']'"))
                    return NULL;
            } else if (TOK(p).kind == JS_T_STR || TOK(p).kind == JS_T_NUM) {
                prop->a = new_node(p, TOK(p).kind == JS_T_STR ? JS_AST_STRING : JS_AST_NUMBER,
                                   TOK(p).pos);
                if (!prop->a)
                    return NULL;
                if (TOK(p).kind == JS_T_STR) {
                    prop->a->units = TOK(p).u.str.units;
                    prop->a->len = TOK(p).u.str.len;
                } else {
                    prop->a->number = TOK(p).u.number;
                }
                if (!advance(p))
                    return NULL;
            } else {
                prop->a = parse_name_any(p, "expected property name");
                if (!prop->a)
                    return NULL;
            }
            /* value */
            if (TOK(p).kind == JS_T_LPAREN) { /* method */
                prop->flags |= JS_F_METHOD;
                JsAstNode *fn = new_node(p, JS_AST_FUNC_EXPR, prop->a->pos);
                if (!fn)
                    return NULL;
                fn->flags = (prop->flags & JS_F_PROP_ASYNC) ? JS_F_ASYNC : 0;
                JsNodeVec params = {0};
                if (!parse_params(p, &params))
                    return NULL;
                vec_store(fn, &params);
                JsSavedCtx s = ctx_enter_function(p, (fn->flags & JS_F_ASYNC) != 0);
                fn->a = parse_block(p);
                ctx_leave_function(p, s);
                if (!fn->a)
                    return NULL;
                prop->b = fn;
            } else if (prop->flags & JS_F_PROP_ASYNC) {
                return perr_here(p, "expected '(' after async method name");
            } else if (accept(p, JS_T_COLON)) {
                prop->b = parse_assign(p);
            } else if (!(prop->flags & JS_F_COMPUTED) &&
                       prop->a->kind == JS_AST_IDENT) {
                /* shorthand */
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
                    JsAstNode *init = parse_assign(p);
                    if (!def || !init)
                        return NULL;
                    def->a = val;
                    def->b = init;
                    prop->b = def;
                    prop->flags |= JS_F_COVER_INIT;
                    has_cover_init = true;
                } else {
                    prop->b = val;
                }
            } else {
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
    if (has_cover_init) {
        if (p->cover_init_open == 0)
            p->cover_init_pos = pos;
        p->cover_init_open++;
    }
    return n;
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
            p->err_msg = p->lx.err_msg;
            p->err_pos = p->lx.err_pos;
            return NULL;
        }
        n = new_node(p, JS_AST_REGEX, t->pos);
        if (!n)
            return NULL;
        n->units = t->u.regex.pattern;
        n->len = t->u.regex.pattern_len;
        n->units2 = t->u.regex.flags;
        n->len2 = t->u.regex.flags_len;
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
    case JS_T_FUNCTION:
        if (!advance(p))
            return NULL;
        return parse_func_common(p, 0, false, t->pos, JS_AST_FUNC_EXPR);
    case JS_T_LBRACK:
        return parse_array_literal(p);
    case JS_T_LBRACE:
        return parse_object_literal(p);
    case JS_T_LPAREN:
        return parse_paren_cover(p, 0);
    case JS_T_IDENT: {
        if (tok_is_name(t, "async")) {
            JsToken nx = peek(p);
            if (!nx.newline_before) {
                if (nx.kind == JS_T_FUNCTION) {
                    uint32_t pos = t->pos;
                    if (!advance(p) || !advance(p))
                        return NULL;
                    return parse_func_common(p, JS_F_ASYNC, false, pos, JS_AST_FUNC_EXPR);
                }
                if (nx.kind == JS_T_LPAREN) {
                    if (!advance(p))
                        return NULL;
                    return parse_paren_cover(p, JS_F_COVER_ASYNC);
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
                        return build_arrow(p, param, true);
                    }
                }
            }
        }
        return parse_ident(p, "expected expression");
    }
    case JS_T_RESERVED:
        return perr_here(p, reserved_message(t));
    case JS_T_NEW:
        /* reachable only via covers/edge paths that bypass parse_call_member's
         * dedicated new-expression handling (see parse_new_expr) */
        return perr_here(p, "'new' is not supported here");
    case JS_T_IMPORT: {
        JsToken nx = peek(p);
        if (nx.kind == JS_T_DOT)
            return perr_here(p, "import.meta is not supported");
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

static JsAstNode *parse_arguments(JsParser *p, JsNodeVec *out) {
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
            base = m;
        } else if (k == JS_T_LBRACK) {
            JsAstNode *m = new_node(p, JS_AST_MEMBER, base->pos);
            if (!m || !advance(p))
                return NULL;
            m->flags |= JS_F_COMPUTED;
            m->a = base;
            m->b = parse_expression(p);
            if (!m->b || !expect(p, JS_T_RBRACK, "expected ']'"))
                return NULL;
            base = m;
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
    if (TOK(p).kind == JS_T_DOT)
        return perr_here(p, "'new.target' is not supported");
    JsAstNode *callee = parse_new_callee(p);
    if (!callee)
        return NULL;
    JsAstNode *n = new_node(p, JS_AST_NEW, pos);
    if (!n)
        return NULL;
    n->a = callee;
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
                if (!m || !advance(p))
                    return NULL;
                m->flags |= JS_F_COMPUTED | JS_F_OPTIONAL;
                m->a = base;
                m->b = parse_expression(p);
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
            base = m;
        } else if (k == JS_T_LBRACK) {
            JsAstNode *m = new_node(p, JS_AST_MEMBER, base->pos);
            if (!m || !advance(p))
                return NULL;
            m->flags |= JS_F_COMPUTED;
            m->a = base;
            m->b = parse_expression(p);
            if (!m->b || !expect(p, JS_T_RBRACK, "expected ']'"))
                return NULL;
            base = m;
        } else if (k == JS_T_LPAREN) {
            JsAstNode *call = new_node(p, JS_AST_CALL, base->pos);
            if (!call)
                return NULL;
            call->a = base;
            JsNodeVec args = {0};
            if (!parse_arguments(p, &args))
                return NULL;
            call->items = args.items;
            call->count = args.count;
            base = call;
        } else if (k == JS_T_TEMPLATE) {
            return perr_here(p, "tagged templates are not supported");
        } else {
            return base;
        }
    }
}

/* ---- unary / binary / conditional / assignment ---- */

static JsAstNode *validate_simple_target(JsParser *p, JsAstNode *n, const char *msg) {
    if (!n)
        return NULL;
    if (n->kind == JS_AST_IDENT) {
        if (units_is(n->units, n->len, "eval") ||
            units_is(n->units, n->len, "arguments"))
            return perr(p, n->pos, "cannot assign to 'eval' or 'arguments' in strict mode");
        return n;
    }
    if (n->kind == JS_AST_MEMBER && is_simple_target(n))
        return n;
    return perr(p, n->pos, msg);
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
        JsAstNode *operand = validate_simple_target(p, parse_unary(p),
                                                    "invalid increment/decrement target");
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
            JsAstNode *target = validate_simple_target(p, result,
                                                       "invalid increment/decrement target");
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
        int prec = binop_prec(k);
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
        if (k == JS_T_INSTANCEOF)
            return perr_here(p, "'instanceof' is not supported");
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
            return perr(p, pos, "'?\?' cannot be mixed with '&&'/'||' without parentheses");
        if ((k == JS_T_ANDAND || k == JS_T_OROR) &&
            (is_unparen_logical(left, false) || is_unparen_logical(right, false)))
            return perr(p, pos, "'?\?' cannot be mixed with '&&'/'||' without parentheses");
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
    n->b = parse_assign(p);
    if (!n->b || !expect(p, JS_T_COLON, "expected ':' in conditional expression"))
        return NULL;
    n->c = parse_assign(p);
    return n->c ? n : NULL;
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
    JsAstNode *lhs = parse_conditional(p);
    if (!lhs)
        goto out;

    if (TOK(p).kind == JS_T_ARROW &&
        (lhs->kind == JS_AST_IDENT || lhs->kind == JS_AST_COVER) &&
        !(lhs->flags & JS_F_PAREN)) {
        bool is_async = lhs->kind == JS_AST_COVER && (lhs->flags & JS_F_COVER_ASYNC);
        result = build_arrow(p, lhs, is_async);
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
            target = validate_simple_target(p, lhs, "invalid assignment target");
        }
        if (!target)
            goto out;
        JsAstNode *n = new_node(p, JS_AST_ASSIGN, lhs->pos);
        if (!n || !advance(p))
            goto out;
        n->op = (uint8_t)op;
        n->a = target;
        n->b = parse_assign(p);
        result = n->b ? n : NULL;
        goto out;
    }
    result = lhs;
out:
    leave(p);
    return result;
}

static JsAstNode *parse_expression(JsParser *p) {
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

static JsAstNode *parse_block(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!enter(p))
        return NULL;
    JsAstNode *result = NULL;
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
        result = new_node(p, JS_AST_BLOCK, pos);
        if (!result)
            goto out;
        vec_store(result, &stmts);
    }
out:
    leave(p);
    return result;
}

static JsAstNode *parse_var_decl(JsParser *p, bool is_const) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p)) /* let/const */
        return NULL;
    JsNodeVec decls = {0};
    for (;;) {
        JsAstNode *target = parse_binding_target(p);
        if (!target)
            return NULL;
        JsAstNode *init = NULL;
        if (accept(p, JS_T_EQ)) {
            init = parse_assign(p);
            if (!init)
                return NULL;
            if (init->kind == JS_AST_COVER) {
                init = collapse_cover(p, init);
                if (!init)
                    return NULL;
            }
        } else if (is_const) {
            return perr_here(p, "'const' declaration requires an initializer");
        } else if (target->kind != JS_AST_IDENT) {
            return perr_here(p, "destructuring declaration requires an initializer");
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

static JsAstNode *parse_loop_body(JsParser *p) {
    p->loop_depth++;
    JsAstNode *body = parse_statement(p);
    p->loop_depth--;
    return body;
}

static JsAstNode *parse_for(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p) || !expect(p, JS_T_LPAREN, "expected '(' after 'for'"))
        return NULL;

    JsAstNode *init = NULL;
    if (TOK(p).kind == JS_T_LET || TOK(p).kind == JS_T_CONST) {
        bool is_const = TOK(p).kind == JS_T_CONST;
        uint32_t decl_pos = TOK(p).pos;
        if (!advance(p))
            return NULL;
        JsAstNode *target = parse_binding_target(p);
        if (!target)
            return NULL;
        if (TOK(p).kind == JS_T_IN)
            return perr_here(p, "for-in is not supported; use for-of");
        if (tok_is_name(&TOK(p), "of")) {
            if (!advance(p))
                return NULL;
            JsAstNode *n = new_node(p, JS_AST_FOR_OF, pos);
            if (!n)
                return NULL;
            n->flags = (uint8_t)(JS_F_DECL | (is_const ? JS_F_CONST : 0));
            n->a = target;
            n->b = parse_assign(p);
            if (!n->b || !expect(p, JS_T_RPAREN, "expected ')'"))
                return NULL;
            n->c = parse_loop_body(p);
            return n->c ? n : NULL;
        }
        /* classic for with declaration: finish first declarator, then rest */
        JsNodeVec decls = {0};
        for (;;) {
            JsAstNode *dinit = NULL;
            if (accept(p, JS_T_EQ)) {
                dinit = parse_assign(p);
                if (!dinit)
                    return NULL;
            } else if (is_const) {
                return perr_here(p, "'const' declaration requires an initializer");
            } else if (target->kind != JS_AST_IDENT) {
                return perr_here(p, "destructuring declaration requires an initializer");
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
            target = parse_binding_target(p);
            if (!target)
                return NULL;
        }
        init = new_node(p, JS_AST_LET_DECL, decl_pos);
        if (!init)
            return NULL;
        if (is_const)
            init->flags |= JS_F_CONST;
        vec_store(init, &decls);
    } else if (TOK(p).kind != JS_T_SEMI) {
        init = parse_expression(p);
        if (!init)
            return NULL;
        if (tok_is_name(&TOK(p), "of")) {
            if (!advance(p))
                return NULL;
            JsAstNode *target = expr_to_pattern(p, init, false);
            if (!target)
                return NULL;
            JsAstNode *n = new_node(p, JS_AST_FOR_OF, pos);
            if (!n)
                return NULL;
            n->a = target;
            n->b = parse_assign(p);
            if (!n->b || !expect(p, JS_T_RPAREN, "expected ')'"))
                return NULL;
            n->c = parse_loop_body(p);
            return n->c ? n : NULL;
        }
        if (init->kind == JS_AST_BINARY && init->op == JS_T_IN &&
            TOK(p).kind == JS_T_RPAREN)
            return perr(p, init->pos, "for-in is not supported; use for-of");
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

static JsAstNode *parse_switch(JsParser *p) {
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
        if (accept(p, JS_T_LPAREN)) {
            n->b = parse_binding_target(p);
            if (!n->b || !expect(p, JS_T_RPAREN, "expected ')'"))
                return NULL;
        }
        n->c = parse_block(p);
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

    switch (t->kind) {
    case JS_T_LBRACE:
        result = parse_block(p);
        break;
    case JS_T_SEMI:
        result = new_node(p, JS_AST_EMPTY, pos);
        if (result && !advance(p))
            result = NULL;
        break;
    case JS_T_LET:
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
        JsAstNode *cons = parse_statement(p);
        if (!cons)
            break;
        JsAstNode *alt = NULL;
        if (accept(p, JS_T_ELSE)) {
            alt = parse_statement(p);
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
        result = parse_for(p);
        break;
    case JS_T_SWITCH:
        result = parse_switch(p);
        break;
    case JS_T_TRY:
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
            if (!advance(p)) {
                result = NULL;
                break;
            }
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
        if (!p->in_function) {
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
        result = parse_func_common(p, 0, true, pos, JS_AST_FUNC_DECL);
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
        result = perr(p, pos, reserved_message(t));
        break;
    case JS_T_IDENT: {
        if (tok_is_name(t, "async")) {
            JsToken nx = peek(p);
            if (nx.kind == JS_T_FUNCTION && !nx.newline_before) {
                if (!advance(p) || !advance(p))
                    break;
                result = parse_func_common(p, JS_F_ASYNC, true, pos, JS_AST_FUNC_DECL);
                break;
            }
        }
        if (peek(p).kind == JS_T_COLON) {
            JsAstNode *label = parse_ident(p, "expected label");
            if (!label || !advance(p)) /* skip ':' */
                break;
            JsAstNode *body = parse_statement(p);
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

static JsAstNode *parse_module_source(JsParser *p, JsAstNode *n) {
    if (TOK(p).kind != JS_T_STR)
        return perr_here(p, "expected module specifier string");
    n->units = TOK(p).u.str.units;
    n->len = TOK(p).u.str.len;
    if (!advance(p))
        return NULL;
    /* an attribute clause after the specifier gets a dedicated error, not
     * the generic expected-';' fallthrough */
    JsToken *t = &TOK(p);
    if (((t->kind == JS_T_RESERVED && units_is(t->u.str.units, t->u.str.len, "with")) ||
         tok_is_name(t, "assert")) &&
        peek(p).kind == JS_T_LBRACE)
        return perr_here(p, "import attributes are not supported");
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
        JsAstNode *imported = parse_name_any(p, "expected import name");
        if (!imported)
            return NULL;
        spec->units = imported->units;
        spec->len = imported->len;
        if (tok_is_name(&TOK(p), "as")) {
            if (!advance(p))
                return NULL;
            JsAstNode *local = check_binding_name(p, parse_ident(p, "expected local name"));
            if (!local)
                return NULL;
            spec->units2 = local->units;
            spec->len2 = local->len;
        } else {
            spec->units2 = spec->units;
            spec->len2 = spec->len;
        }
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
        if (!local)
            return NULL;
        JsAstNode *spec = new_node(p, JS_AST_IMPORT_SPEC, local->pos);
        if (!spec)
            return NULL;
        spec->flags |= JS_F_SPEC_DEFAULT;
        spec->units2 = local->units;
        spec->len2 = local->len;
        if (!vec_push(p, &specs, spec))
            return NULL;
        if (accept(p, JS_T_COMMA)) {
            if (TOK(p).kind != JS_T_STAR && TOK(p).kind != JS_T_LBRACE)
                return perr_here(p, "expected '*' or '{' after ','");
        } else {
            goto from;
        }
    }
    if (TOK(p).kind == JS_T_STAR) {
        if (!advance(p))
            return NULL;
        if (!tok_is_name(&TOK(p), "as"))
            return perr_here(p, "expected 'as' after '*'");
        if (!advance(p))
            return NULL;
        JsAstNode *local = check_binding_name(p, parse_ident(p, "expected namespace name"));
        if (!local)
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
    if (!tok_is_name(&TOK(p), "from"))
        return perr_here(p, "expected 'from'");
    if (!advance(p))
        return NULL;
    vec_store(n, &specs);
    return parse_module_source(p, n);
}

static JsAstNode *parse_export(JsParser *p) {
    uint32_t pos = TOK(p).pos;
    if (!advance(p))
        return NULL;

    if (TOK(p).kind == JS_T_DEFAULT) {
        if (!advance(p))
            return NULL;
        JsAstNode *n = new_node(p, JS_AST_EXPORT_DEFAULT, pos);
        if (!n)
            return NULL;
        if (TOK(p).kind == JS_T_FUNCTION) {
            if (!advance(p))
                return NULL;
            n->a = parse_func_common(p, 0, false, pos, JS_AST_FUNC_EXPR);
            return n->a ? n : NULL;
        }
        if (tok_is_name(&TOK(p), "async") && peek(p).kind == JS_T_FUNCTION &&
            !peek(p).newline_before) {
            if (!advance(p) || !advance(p))
                return NULL;
            n->a = parse_func_common(p, JS_F_ASYNC, false, pos, JS_AST_FUNC_EXPR);
            return n->a ? n : NULL;
        }
        n->a = parse_assign(p);
        if (!n->a || !expect_semicolon(p))
            return NULL;
        return n;
    }

    if (TOK(p).kind == JS_T_STAR) {
        if (!advance(p))
            return NULL;
        JsAstNode *n = new_node(p, JS_AST_EXPORT_ALL, pos);
        if (!n)
            return NULL;
        if (tok_is_name(&TOK(p), "as")) {
            if (!advance(p))
                return NULL;
            JsAstNode *alias = parse_name_any(p, "expected export alias");
            if (!alias)
                return NULL;
            n->units2 = alias->units;
            n->len2 = alias->len;
        }
        if (!tok_is_name(&TOK(p), "from"))
            return perr_here(p, "expected 'from'");
        if (!advance(p))
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
        while (TOK(p).kind != JS_T_RBRACE) {
            JsAstNode *spec = new_node(p, JS_AST_EXPORT_SPEC, TOK(p).pos);
            if (!spec)
                return NULL;
            JsAstNode *local = parse_name_any(p, "expected export name");
            if (!local)
                return NULL;
            spec->units = local->units;
            spec->len = local->len;
            if (tok_is_name(&TOK(p), "as")) {
                if (!advance(p))
                    return NULL;
                JsAstNode *exported = parse_name_any(p, "expected export alias");
                if (!exported)
                    return NULL;
                spec->units2 = exported->units;
                spec->len2 = exported->len;
            } else {
                spec->units2 = spec->units;
                spec->len2 = spec->len;
            }
            if (!vec_push(p, &specs, spec))
                return NULL;
            if (TOK(p).kind != JS_T_RBRACE && !expect(p, JS_T_COMMA, "expected ',' or '}'"))
                return NULL;
        }
        if (!advance(p)) /* } */
            return NULL;
        vec_store(n, &specs);
        if (tok_is_name(&TOK(p), "from")) {
            if (!advance(p))
                return NULL;
            return parse_module_source(p, n); /* re-export */
        }
        if (!expect_semicolon(p))
            return NULL;
        return n;
    }

    /* export <declaration> */
    JsAstNode *decl = NULL;
    if (TOK(p).kind == JS_T_LET || TOK(p).kind == JS_T_CONST) {
        decl = parse_var_decl(p, TOK(p).kind == JS_T_CONST);
        if (decl && !expect_semicolon(p))
            decl = NULL;
    } else if (TOK(p).kind == JS_T_FUNCTION) {
        if (!advance(p))
            return NULL;
        decl = parse_func_common(p, 0, true, TOK(p).pos, JS_AST_FUNC_DECL);
    } else if (tok_is_name(&TOK(p), "async") && peek(p).kind == JS_T_FUNCTION &&
               !peek(p).newline_before) {
        if (!advance(p) || !advance(p))
            return NULL;
        decl = parse_func_common(p, JS_F_ASYNC, true, TOK(p).pos, JS_AST_FUNC_DECL);
    } else {
        return perr_here(p, "expected declaration or '{' after 'export'");
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

bool js_parse_module(JsArena *arena, const uint16_t *src, size_t len,
                     JsParseResult *out) {
    JsParser p;
    memset(&p, 0, sizeof p);
    p.arena = arena;
    p.fn_async = true; /* module top level: await allowed (TLA) */
    js_lexer_init(&p.lx, arena, src, len);

    out->module = NULL;
    out->err_msg = NULL;
    out->err_pos = 0;

    if (!advance(&p))
        goto fail;
    JsAstNode *mod = new_node(&p, JS_AST_MODULE, 0);
    if (!mod)
        goto fail;
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
