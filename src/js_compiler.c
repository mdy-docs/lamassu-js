#include "js_bytecode.h"
#ifdef LAMASSU_HAS_REGEX
#include "js_regexp.h"
#endif

/*
 * AST -> bytecode.
 *
 * Each function compiles into its own JsFunctionCell with an independent
 * JsFuncState (locals, operand-depth simulation, loops, finally chain).
 * States link to their parent so free-variable references resolve to
 * upvalues that capture an enclosing local or a parent upvalue. Every
 * function cell under construction is GC-protected via its state's
 * fn_value, so constant interning may collect safely.
 *
 * Closures over loop variables get per-iteration freshness by closing the
 * loop's upvalues (JS_OP_CLOSE_UPVALS) at each iteration boundary and on
 * block exit; captured locals are tracked to emit those closes only when
 * needed. try/finally lowers finally to a GOSUB subroutine that every exit
 * path (normal, exception, break/continue/return) routes through.
 */

#define JS_MAX_LOCALS 60000
#define JS_MAX_CONSTS 60000
#define JS_MAX_LOOPS 128
#define JS_MAX_FINALLY 64
#define JS_COMPLETION_SLOT 0

typedef struct JsPatchList {
    uint32_t *offs;
    uint32_t count, cap;
} JsPatchList;

typedef struct JsLoopInfo {
    const uint16_t *label;
    uint32_t label_len;
    bool is_loop;
    bool is_label_only;
    int entry_depth;
    int continue_depth;
    uint32_t continue_target;
    JsPatchList breaks, continues;
    int finally_floor;         /* finally-stack height at loop entry */
    int loopvar_slot;          /* -1, else slot floor to close per iteration */
} JsLoopInfo;

typedef struct JsFinallyInfo {
    JsPatchList gosubs;        /* GOSUB operand offsets to patch to the body */
} JsFinallyInfo;

typedef struct JsLocalVar {
    const uint16_t *name;      /* NULL/len 0: anonymous (unnamed param slot) */
    uint32_t len;
    uint16_t slot;
    bool is_const;
    bool captured;             /* an inner function closes over it */
} JsLocalVar;

typedef struct JsUpName {
    const uint16_t *name;
    uint32_t len;
    bool is_const;
} JsUpName;

typedef struct JsFuncState {
    struct JsFuncState *parent;
    JsFunctionCell *fn;
    JsValue fn_value;          /* GC root while building */
    bool is_module;
    bool is_arrow;
    JsLocalVar *locals;
    uint32_t local_count, local_cap;
    uint16_t slot_count, slot_max;
    int scratch_slot;
    int depth, max_depth;
    JsLoopInfo loops[JS_MAX_LOOPS];
    int loop_count;
    JsUpName *upnames;
    uint16_t upname_cap;
    JsFinallyInfo finallys[JS_MAX_FINALLY];
    int finally_count;
    int scope_depth; /* nesting of blocks/loops within this function */
    uint32_t last_line_pos;
} JsFuncState;

typedef struct JsCompiler {
    JsContext *ctx;
    JsVm *vm;
    JsArena arena;
    JsFuncState *cur;
    bool repl;       /* REPL: top-level declarations join the lexical scope */
    bool decl_const; /* const-ness of the declaration currently being compiled */
    const char *err_msg;
    uint32_t err_pos;
    int func_depth;
    /* module compilation (NULL if compiling a plain script) */
    JsModule *module;
    JsModBinding *mbind; /* top-level module bindings (exports/imports) */
    uint32_t mbind_count;
} JsCompiler;

#define FN (cx->cur->fn)

static bool compile_stmt(JsCompiler *cx, const JsAstNode *n);
static bool compile_expr(JsCompiler *cx, const JsAstNode *n);
static bool compile_pattern_store(JsCompiler *cx, const JsAstNode *pat, bool declaring);
static bool compile_function(JsCompiler *cx, const JsAstNode *fnode);
static int emit_module_load(JsCompiler *cx, const JsAstNode *n);
static bool compile_function(JsCompiler *cx, const JsAstNode *fnode);

static bool cerr(JsCompiler *cx, uint32_t pos, const char *msg) {
    if (!cx->err_msg) {
        cx->err_msg = msg;
        cx->err_pos = pos;
    }
    return false;
}

/* ---- emission ---- */

static bool emit8(JsCompiler *cx, uint8_t b) {
    JsFunctionCell *f = FN;
    if (f->code_len == f->code_cap) {
        uint32_t ncap = f->code_cap ? f->code_cap * 2 : 256;
        uint8_t *nc = js_realloc_raw(cx->vm, f->code, f->code_cap, ncap);
        if (!nc)
            return cerr(cx, 0, "out of memory");
        f->code = nc;
        f->code_cap = ncap;
    }
    f->code[f->code_len++] = b;
    return true;
}

static void adjust(JsCompiler *cx, int delta) {
    cx->cur->depth += delta;
    if (cx->cur->depth > cx->cur->max_depth)
        cx->cur->max_depth = cx->cur->depth;
}

static void set_depth(JsCompiler *cx, int d) {
    cx->cur->depth = d;
    if (d > cx->cur->max_depth)
        cx->cur->max_depth = d;
}

static bool emit_op(JsCompiler *cx, JsOp op, int delta) {
    if (!emit8(cx, (uint8_t)op))
        return false;
    adjust(cx, delta);
    return true;
}

static bool emit_u16(JsCompiler *cx, uint16_t v) {
    return emit8(cx, (uint8_t)(v & 0xFF)) && emit8(cx, (uint8_t)(v >> 8));
}

static bool emit_u32(JsCompiler *cx, uint32_t v) {
    return emit8(cx, (uint8_t)(v & 0xFF)) && emit8(cx, (uint8_t)((v >> 8) & 0xFF)) &&
           emit8(cx, (uint8_t)((v >> 16) & 0xFF)) && emit8(cx, (uint8_t)(v >> 24));
}

static bool emit_op_u16(JsCompiler *cx, JsOp op, uint16_t arg, int delta) {
    return emit_op(cx, op, delta) && emit_u16(cx, arg);
}

static bool emit_jump(JsCompiler *cx, JsOp op, int delta, uint32_t *patch) {
    if (!emit_op(cx, op, delta))
        return false;
    *patch = FN->code_len;
    return emit_u32(cx, 0xFFFFFFFF);
}

static void patch_at(JsCompiler *cx, uint32_t operand_off, uint32_t target) {
    FN->code[operand_off] = (uint8_t)(target & 0xFF);
    FN->code[operand_off + 1] = (uint8_t)((target >> 8) & 0xFF);
    FN->code[operand_off + 2] = (uint8_t)((target >> 16) & 0xFF);
    FN->code[operand_off + 3] = (uint8_t)(target >> 24);
}

static void patch_here(JsCompiler *cx, uint32_t operand_off) {
    patch_at(cx, operand_off, FN->code_len);
}

static bool emit_jump_to(JsCompiler *cx, JsOp op, int delta, uint32_t target) {
    return emit_op(cx, op, delta) && emit_u32(cx, target);
}

static void note_pos(JsCompiler *cx, uint32_t pos) {
    JsFunctionCell *f = FN;
    if (f->line_count && cx->cur->last_line_pos == pos)
        return;
    if (f->line_count == f->line_cap) {
        uint32_t ncap = f->line_cap ? f->line_cap * 2 : 64;
        JsLineEntry *nl = js_realloc_raw(cx->vm, f->lines,
                                         (size_t)f->line_cap * sizeof(JsLineEntry),
                                         (size_t)ncap * sizeof(JsLineEntry));
        if (!nl)
            return;
        f->lines = nl;
        f->line_cap = ncap;
    }
    f->lines[f->line_count].off = f->code_len;
    f->lines[f->line_count].pos = pos;
    f->line_count++;
    cx->cur->last_line_pos = pos;
}

/* ---- constants ---- */

static bool const_add(JsCompiler *cx, JsValue v, uint16_t *idx) {
    JsFunctionCell *f = FN;
    if (!js_value_is_cell(v)) { /* dedup immediates only (cells vary) */
        for (uint32_t i = 0; i < f->const_count; i++) {
            if (f->consts[i].bits == v.bits) {
                *idx = (uint16_t)i;
                return true;
            }
        }
    } else if (js_is_string(v)) {
        for (uint32_t i = 0; i < f->const_count; i++) {
            if (f->consts[i].bits == v.bits) { /* interned atoms compare by id */
                *idx = (uint16_t)i;
                return true;
            }
        }
    }
    if (f->const_count >= JS_MAX_CONSTS)
        return cerr(cx, 0, "too many constants");
    if (f->const_count == f->const_cap) {
        uint32_t ncap = f->const_cap ? f->const_cap * 2 : 16;
        JsValue *nc = js_realloc_raw(cx->vm, f->consts,
                                     (size_t)f->const_cap * sizeof(JsValue),
                                     (size_t)ncap * sizeof(JsValue));
        if (!nc)
            return cerr(cx, 0, "out of memory");
        f->consts = nc;
        f->const_cap = ncap;
    }
    *idx = (uint16_t)f->const_count;
    f->consts[f->const_count++] = v;
    return true;
}

static bool atom_const(JsCompiler *cx, const uint16_t *units, uint32_t len,
                       uint32_t pos, uint16_t *idx) {
    JsValue a = js_atom(cx->vm, units, len);
    if (!js_is_string(a))
        return cerr(cx, pos, "out of memory");
    return const_add(cx, a, idx);
}

static bool emit_const_value(JsCompiler *cx, JsValue v, uint32_t pos) {
    uint16_t idx;
    if (!const_add(cx, v, &idx))
        return false;
    (void)pos;
    return emit_op_u16(cx, JS_OP_CONST, idx, +1);
}

static bool emit_number(JsCompiler *cx, double d, uint32_t pos) {
    return emit_const_value(cx, js_number(d), pos);
}

static bool emit_string_const(JsCompiler *cx, const uint16_t *units, uint32_t len,
                              uint32_t pos) {
    JsValue s = js_atom(cx->vm, units, len);
    if (!js_is_string(s))
        return cerr(cx, pos, "out of memory");
    return emit_const_value(cx, s, pos);
}

/* ---- locals & scopes ---- */

typedef struct JsScopeMark {
    uint32_t local_count;
    uint16_t slot_count;
} JsScopeMark;

static bool units_equal(const uint16_t *a, uint32_t alen, const uint16_t *b,
                        uint32_t blen) {
    return alen == blen && (alen == 0 || memcmp(a, b, alen * sizeof(uint16_t)) == 0);
}

static JsScopeMark scope_enter(JsCompiler *cx) {
    JsScopeMark m = {cx->cur->local_count, cx->cur->slot_count};
    cx->cur->scope_depth++;
    return m;
}

/* Emits CLOSE_UPVALS if any local being discarded was captured. */
static bool scope_leave(JsCompiler *cx, JsScopeMark m) {
    JsFuncState *fs = cx->cur;
    fs->scope_depth--;
    bool captured = false;
    for (uint32_t i = m.local_count; i < fs->local_count; i++) {
        if (fs->locals[i].captured) {
            captured = true;
            break;
        }
    }
    fs->local_count = m.local_count;
    fs->slot_count = m.slot_count;
    /* The function-wide scratch slot is allocated on demand via slot_count++.
     * If it was allocated inside the scope now being left, its slot index is
     * reclaimed here and a sibling scope's local would reuse it — so drop the
     * cache and let the next scratch use re-allocate above the live locals. */
    if (fs->scratch_slot >= (int)m.slot_count)
        fs->scratch_slot = -1;
    if (captured)
        return emit_op_u16(cx, JS_OP_CLOSE_UPVALS, m.slot_count, 0);
    return true;
}

/* REPL top-level scope: declarations here become persistent globals. */
static bool is_repl_top(JsCompiler *cx) {
    return cx->repl && cx->cur->is_module && cx->cur->scope_depth == 1;
}

static int resolve_local_in(JsFuncState *fs, const uint16_t *name, uint32_t len) {
    for (uint32_t i = fs->local_count; i-- > 0;) {
        if (units_equal(fs->locals[i].name, fs->locals[i].len, name, len))
            return (int)i;
    }
    return -1;
}

static bool declare_local(JsCompiler *cx, const uint16_t *name, uint32_t len,
                          bool is_const, uint32_t pos, uint32_t scope_floor,
                          uint16_t *slot_out) {
    JsFuncState *fs = cx->cur;
    for (uint32_t i = scope_floor; i < fs->local_count; i++) {
        if (units_equal(fs->locals[i].name, fs->locals[i].len, name, len))
            return cerr(cx, pos, "identifier has already been declared in this scope");
    }
    if (fs->slot_count >= JS_MAX_LOCALS)
        return cerr(cx, pos, "too many local variables");
    if (fs->local_count == fs->local_cap) {
        uint32_t ncap = fs->local_cap ? fs->local_cap * 2 : 16;
        JsLocalVar *nl = js_arena_alloc(&cx->arena, (size_t)ncap * sizeof *nl);
        if (!nl)
            return cerr(cx, pos, "out of memory");
        if (fs->local_count)
            memcpy(nl, fs->locals, fs->local_count * sizeof *nl);
        fs->locals = nl;
        fs->local_cap = ncap;
    }
    JsLocalVar *v = &fs->locals[fs->local_count++];
    v->name = name;
    v->len = len;
    v->slot = fs->slot_count++;
    v->is_const = is_const;
    v->captured = false;
    if (fs->slot_count > fs->slot_max)
        fs->slot_max = fs->slot_count;
    if (slot_out)
        *slot_out = v->slot;
    return true;
}

static bool get_scratch(JsCompiler *cx, uint16_t *slot) {
    JsFuncState *fs = cx->cur;
    if (fs->scratch_slot < 0) {
        if (fs->slot_count >= JS_MAX_LOCALS)
            return cerr(cx, 0, "too many local variables");
        fs->scratch_slot = fs->slot_count++;
        if (fs->slot_count > fs->slot_max)
            fs->slot_max = fs->slot_count;
    }
    *slot = (uint16_t)fs->scratch_slot;
    return true;
}

/* ---- upvalue resolution ---- */

static int upval_add(JsCompiler *cx, JsFuncState *fs, uint16_t idx, bool from_local,
                     const uint16_t *name, uint32_t len, bool is_const) {
    JsFunctionCell *f = fs->fn;
    for (uint16_t i = 0; i < f->n_upvals; i++) {
        if (f->upvals[i].idx == idx && f->upvals[i].from_local == from_local)
            return i;
    }
    if (f->n_upvals == f->upval_cap) {
        uint16_t ncap = f->upval_cap ? f->upval_cap * 2 : 8;
        JsUpvalDesc *nd = js_realloc_raw(cx->vm, f->upvals,
                                         (size_t)f->upval_cap * sizeof(JsUpvalDesc),
                                         (size_t)ncap * sizeof(JsUpvalDesc));
        JsUpName *nn = js_arena_alloc(&cx->arena, (size_t)ncap * sizeof(JsUpName));
        if (!nd || !nn) {
            cerr(cx, 0, "out of memory");
            return -1;
        }
        if (f->n_upvals) {
            memcpy(nn, fs->upnames, f->n_upvals * sizeof(JsUpName));
        }
        f->upvals = nd;
        fs->upnames = nn;
        f->upval_cap = ncap;
        fs->upname_cap = ncap;
    }
    f->upvals[f->n_upvals].idx = idx;
    f->upvals[f->n_upvals].from_local = from_local ? 1 : 0;
    fs->upnames[f->n_upvals].name = name;
    fs->upnames[f->n_upvals].len = len;
    fs->upnames[f->n_upvals].is_const = is_const;
    return f->n_upvals++;
}

/* Returns upvalue index in fs capturing `name`, or -1. */
static int resolve_upvalue(JsCompiler *cx, JsFuncState *fs, const uint16_t *name,
                           uint32_t len, bool *is_const_out) {
    if (!fs->parent)
        return -1;
    int local = resolve_local_in(fs->parent, name, len);
    if (local >= 0) {
        fs->parent->locals[local].captured = true;
        *is_const_out = fs->parent->locals[local].is_const;
        return upval_add(cx, fs, fs->parent->locals[local].slot, true, name, len,
                         *is_const_out);
    }
    int up = resolve_upvalue(cx, fs->parent, name, len, is_const_out);
    if (up >= 0)
        return upval_add(cx, fs, (uint16_t)up, false, name, len, *is_const_out);
    return -1;
}

/* ---- declaration pre-scan ---- */

static JsModBinding *resolve_module_binding(JsCompiler *cx, const uint16_t *name,
                                            uint32_t len);

static bool pattern_declare(JsCompiler *cx, const JsAstNode *pat, bool is_const,
                            uint32_t scope_floor, bool tdz) {
    switch ((JsAstKind)pat->kind) {
    case JS_AST_IDENT: {
        /* REPL top-level: the binding is a persistent global, not a local */
        if (is_repl_top(cx))
            return true;
        /* module-top-level exported names live in the exports object, not a
         * local slot — skip declaring them here (SET_EXPORT initializes). */
        if (cx->module && cx->cur->is_module) {
            JsModBinding *b = resolve_module_binding(cx, pat->units, pat->len);
            if (b && b->kind == JS_MB_EXPORT)
                return true;
        }
        uint16_t slot;
        if (!declare_local(cx, pat->units, pat->len, is_const, pat->pos,
                           scope_floor, &slot))
            return false;
        return tdz ? emit_op_u16(cx, JS_OP_TDZ, slot, 0) : true;
    }
    case JS_AST_DEFAULT:
        return pattern_declare(cx, pat->a, is_const, scope_floor, tdz);
    case JS_AST_ARRAY_PATTERN:
        for (uint32_t i = 0; i < pat->count; i++) {
            const JsAstNode *el = pat->items[i];
            if (el->kind == JS_AST_HOLE)
                continue;
            if (el->kind == JS_AST_REST) {
                if (!pattern_declare(cx, el->a, is_const, scope_floor, tdz))
                    return false;
                continue;
            }
            if (!pattern_declare(cx, el, is_const, scope_floor, tdz))
                return false;
        }
        return true;
    case JS_AST_OBJECT_PATTERN:
        for (uint32_t i = 0; i < pat->count; i++) {
            const JsAstNode *prop = pat->items[i];
            if (prop->kind == JS_AST_REST) {
                if (!pattern_declare(cx, prop->a, is_const, scope_floor, tdz))
                    return false;
                continue;
            }
            if (!pattern_declare(cx, prop->b, is_const, scope_floor, tdz))
                return false;
        }
        return true;
    default:
        return cerr(cx, pat->pos, "invalid binding pattern");
    }
}

/* Hoists let/const (TDZ) and function-declaration names for one block. */
static bool prescan_declarations(JsCompiler *cx, JsAstNode *const *stmts,
                                 uint32_t count, uint32_t scope_floor) {
    for (uint32_t i = 0; i < count; i++) {
        const JsAstNode *s = stmts[i];
        /* unwrap `export <decl>` */
        if (s->kind == JS_AST_EXPORT_NAMED && s->a)
            s = s->a;
        if (s->kind == JS_AST_LET_DECL) {
            for (uint32_t d = 0; d < s->count; d++) {
                if (!pattern_declare(cx, s->items[d]->a, (s->flags & JS_F_CONST) != 0,
                                     scope_floor, true))
                    return false;
            }
        } else if (s->kind == JS_AST_FUNC_DECL) {
            /* REPL top-level function -> persistent global (not a local) */
            if (is_repl_top(cx))
                continue;
            /* exported functions become export bindings, not locals */
            if (cx->module && cx->cur->is_module) {
                JsModBinding *b = resolve_module_binding(cx, s->units, s->len);
                if (b && b->kind == JS_MB_EXPORT)
                    continue;
            }
            uint16_t slot;
            if (!declare_local(cx, s->units, s->len, false, s->pos, scope_floor, &slot))
                return false;
        }
    }
    return true;
}

/* ---- loops / finally exit ---- */

static JsLoopInfo *loop_push(JsCompiler *cx, bool is_loop, const uint16_t *label,
                             uint32_t label_len) {
    JsFuncState *fs = cx->cur;
    if (fs->loop_count >= JS_MAX_LOOPS) {
        cerr(cx, 0, "loops nested too deeply");
        return NULL;
    }
    JsLoopInfo *l = &fs->loops[fs->loop_count++];
    memset(l, 0, sizeof *l);
    l->is_loop = is_loop;
    l->label = label;
    l->label_len = label_len;
    l->entry_depth = fs->depth;
    l->continue_depth = fs->depth;
    l->continue_target = 0xFFFFFFFF;
    l->finally_floor = fs->finally_count;
    l->loopvar_slot = -1;
    return l;
}

static bool patch_list_add(JsCompiler *cx, JsPatchList *pl, uint32_t off) {
    if (pl->count == pl->cap) {
        uint32_t ncap = pl->cap ? pl->cap * 2 : 8;
        uint32_t *no = js_arena_alloc(&cx->arena, (size_t)ncap * sizeof *no);
        if (!no)
            return cerr(cx, 0, "out of memory");
        if (pl->count)
            memcpy(no, pl->offs, pl->count * sizeof *no);
        pl->offs = no;
        pl->cap = ncap;
    }
    pl->offs[pl->count++] = off;
    return true;
}

static void loop_pop_patch_breaks(JsCompiler *cx, JsLoopInfo *l) {
    for (uint32_t i = 0; i < l->breaks.count; i++)
        patch_here(cx, l->breaks.offs[i]);
    cx->cur->loop_count--;
}

static JsLoopInfo *find_loop(JsCompiler *cx, const uint16_t *label, uint32_t len,
                             bool need_loop) {
    JsFuncState *fs = cx->cur;
    for (int i = fs->loop_count; i-- > 0;) {
        JsLoopInfo *l = &fs->loops[i];
        if (need_loop && !l->is_loop)
            continue;
        if (!label) {
            if (l->is_label_only)
                continue;
            return l;
        }
        if (l->label && units_equal(l->label, l->label_len, label, len))
            return l;
    }
    return NULL;
}

static bool emit_pops_to(JsCompiler *cx, int to_depth) {
    int save = cx->cur->depth;
    while (cx->cur->depth > to_depth) {
        if (!emit_op(cx, JS_OP_POP, -1))
            return false;
    }
    cx->cur->depth = save;
    return true;
}

/* Routes an exit through active finally subroutines [floor, finally_count). */
static bool emit_finally_gosubs(JsCompiler *cx, int floor) {
    JsFuncState *fs = cx->cur;
    for (int i = fs->finally_count; i-- > floor;) {
        if (!emit_op(cx, JS_OP_GOSUB, 0))
            return false;
        uint32_t off = FN->code_len;
        if (!emit_u32(cx, 0xFFFFFFFF))
            return false;
        if (!patch_list_add(cx, &fs->finallys[i].gosubs, off))
            return false;
        (void)emit_op; /* GOSUB balances via RET_SUB */
    }
    return true;
}

/* ---- identifiers ---- */

/* Finds a module-level binding (export/import) by local name, or NULL. */
static JsModBinding *resolve_module_binding(JsCompiler *cx, const uint16_t *name,
                                            uint32_t len) {
    for (uint32_t i = 0; i < cx->mbind_count; i++) {
        if (units_equal(cx->mbind[i].name, cx->mbind[i].len, name, len))
            return &cx->mbind[i];
    }
    return NULL;
}

/* Emits access to a module binding; returns 0 not-a-binding, 1 emitted, -1 error. */
static int emit_module_load(JsCompiler *cx, const JsAstNode *n) {
    JsModBinding *b = resolve_module_binding(cx, n->units, n->len);
    if (!b)
        return 0;
    if (b->kind == JS_MB_EXPORT) {
        uint16_t c;
        if (!atom_const(cx, b->export_name, b->export_len, n->pos, &c))
            return -1;
        return emit_op_u16(cx, JS_OP_GET_EXPORT, c, +1) ? 1 : -1;
    }
    if (b->kind == JS_MB_IMPORT_NS)
        return emit_op_u16(cx, JS_OP_IMPORT_NS, b->import_index, +1) ? 1 : -1;
    return emit_op_u16(cx, JS_OP_GET_IMPORT, b->import_index, +1) ? 1 : -1;
}

static int emit_module_store(JsCompiler *cx, const JsAstNode *target) {
    JsModBinding *b = resolve_module_binding(cx, target->units, target->len);
    if (!b)
        return 0;
    if (b->kind != JS_MB_EXPORT)
        return cerr(cx, target->pos, "cannot assign to an imported binding") ? -1 : -1;
    if (b->is_const)
        return cerr(cx, target->pos, "assignment to constant variable") ? -1 : -1;
    uint16_t c;
    if (!atom_const(cx, b->export_name, b->export_len, target->pos, &c))
        return -1;
    return emit_op_u16(cx, JS_OP_SET_EXPORT, c, 0) ? 1 : -1;
}

static bool compile_ident_load(JsCompiler *cx, const JsAstNode *n) {
    note_pos(cx, n->pos);
    int idx = resolve_local_in(cx->cur, n->units, n->len);
    if (idx >= 0)
        return emit_op_u16(cx, JS_OP_GET_LOCAL, cx->cur->locals[idx].slot, +1);
    bool is_const;
    int up = resolve_upvalue(cx, cx->cur, n->units, n->len, &is_const);
    if (cx->err_msg)
        return false;
    if (up >= 0)
        return emit_op_u16(cx, JS_OP_GET_UPVAL, (uint16_t)up, +1);
    if (cx->module) {
        int m = emit_module_load(cx, n);
        if (m != 0)
            return m > 0;
    }
    uint16_t c;
    if (!atom_const(cx, n->units, n->len, n->pos, &c))
        return false;
    /* REPL: names not otherwise bound resolve to the lexical scope, else global */
    return emit_op_u16(cx, cx->repl ? JS_OP_GET_LEXICAL : JS_OP_GET_GLOBAL, c, +1);
}

static bool compile_ident_store(JsCompiler *cx, const JsAstNode *target) {
    note_pos(cx, target->pos);
    int idx = resolve_local_in(cx->cur, target->units, target->len);
    if (idx >= 0) {
        if (cx->cur->locals[idx].is_const)
            return cerr(cx, target->pos, "assignment to constant variable");
        return emit_op_u16(cx, JS_OP_SET_LOCAL, cx->cur->locals[idx].slot, 0);
    }
    bool is_const;
    int up = resolve_upvalue(cx, cx->cur, target->units, target->len, &is_const);
    if (cx->err_msg)
        return false;
    if (up >= 0) {
        if (is_const)
            return cerr(cx, target->pos, "assignment to constant variable");
        return emit_op_u16(cx, JS_OP_SET_UPVAL, (uint16_t)up, 0);
    }
    if (cx->module) {
        int m = emit_module_store(cx, target);
        if (m != 0)
            return m > 0;
    }
    uint16_t c;
    if (!atom_const(cx, target->units, target->len, target->pos, &c))
        return false;
    return emit_op_u16(cx, cx->repl ? JS_OP_SET_LEXICAL : JS_OP_SET_GLOBAL, c, 0);
}

/* ---- member chains (optional chaining aware) ---- */

static bool compile_member_chain(JsCompiler *cx, const JsAstNode *n,
                                 JsPatchList *opt) {
    if (n->kind != JS_AST_MEMBER)
        return compile_expr(cx, n);
    if (!compile_member_chain(cx, n->a, opt))
        return false;
    note_pos(cx, n->pos);
    if (n->flags & JS_F_OPTIONAL) {
        uint32_t patch;
        if (!emit_jump(cx, JS_OP_OPT_CHAIN, 0, &patch) ||
            !patch_list_add(cx, opt, patch))
            return false;
    }
    if (n->flags & JS_F_COMPUTED) {
        if (!compile_expr(cx, n->b))
            return false;
        return emit_op(cx, JS_OP_GET_PROP, -1);
    }
    uint16_t c;
    if (!atom_const(cx, n->units, n->len, n->pos, &c))
        return false;
    return emit_op_u16(cx, JS_OP_GET_PROP_ATOM, c, 0);
}

static bool compile_member_expr(JsCompiler *cx, const JsAstNode *n) {
    JsPatchList opt = {0};
    if (!compile_member_chain(cx, n, &opt))
        return false;
    for (uint32_t i = 0; i < opt.count; i++)
        patch_here(cx, opt.offs[i]);
    return true;
}

static bool compile_member_target_base(JsCompiler *cx, const JsAstNode *m) {
    if (!compile_expr(cx, m->a))
        return false;
    if (m->flags & JS_F_COMPUTED)
        return compile_expr(cx, m->b);
    return true;
}

/* ---- patterns ---- */

static bool compile_pattern_default(JsCompiler *cx, const JsAstNode *def,
                                    bool declaring) {
    uint32_t skip;
    if (!emit_op(cx, JS_OP_DUP, +1) || !emit_op(cx, JS_OP_UNDEFINED, +1) ||
        !emit_op(cx, JS_OP_STRICT_EQ, -1) ||
        !emit_jump(cx, JS_OP_JUMP_IF_FALSE, -1, &skip))
        return false;
    if (!emit_op(cx, JS_OP_POP, -1))
        return false;
    if (!compile_expr(cx, def->b))
        return false;
    patch_here(cx, skip);
    return compile_pattern_store(cx, def->a, declaring);
}

/* Emits the initializing write of an exported binding (bypasses const). */
static int emit_module_init(JsCompiler *cx, const JsAstNode *tgt) {
    JsModBinding *b = resolve_module_binding(cx, tgt->units, tgt->len);
    if (!b || b->kind != JS_MB_EXPORT)
        return 0;
    uint16_t c;
    if (!atom_const(cx, b->export_name, b->export_len, tgt->pos, &c))
        return -1;
    return emit_op_u16(cx, JS_OP_SET_EXPORT, c, 0) ? 1 : -1;
}

static bool store_target(JsCompiler *cx, const JsAstNode *tgt, bool declaring) {
    if (declaring) {
        int idx = resolve_local_in(cx->cur, tgt->units, tgt->len);
        if (idx < 0) {
            /* REPL top-level declaration -> persistent lexical binding */
            if (is_repl_top(cx)) {
                uint16_t c;
                if (!atom_const(cx, tgt->units, tgt->len, tgt->pos, &c))
                    return false;
                return emit_op_u16(cx, JS_OP_DEFINE_LEXICAL, c, 0) &&
                       emit8(cx, cx->decl_const ? 1 : 0) && emit_op(cx, JS_OP_POP, -1);
            }
            /* an exported top-level binding: initialize the export */
            if (cx->module && cx->cur->is_module) {
                int m = emit_module_init(cx, tgt);
                if (m > 0)
                    return emit_op(cx, JS_OP_POP, -1);
                if (m < 0)
                    return false;
            }
            return cerr(cx, tgt->pos, "internal: undeclared binding");
        }
        return emit_op_u16(cx, JS_OP_SET_LOCAL, cx->cur->locals[idx].slot, 0) &&
               emit_op(cx, JS_OP_POP, -1);
    }
    if (!compile_ident_store(cx, tgt))
        return false;
    return emit_op(cx, JS_OP_POP, -1);
}

static bool compile_pattern_store(JsCompiler *cx, const JsAstNode *pat,
                                  bool declaring) {
    switch ((JsAstKind)pat->kind) {
    case JS_AST_IDENT:
        return store_target(cx, pat, declaring);
    case JS_AST_MEMBER: {
        if (declaring)
            return cerr(cx, pat->pos, "invalid binding pattern");
        if (pat->flags & JS_F_COMPUTED) {
            if (!compile_expr(cx, pat->a) || !compile_expr(cx, pat->b))
                return false;
            if (!emit_op(cx, JS_OP_ROT3, 0) || !emit_op(cx, JS_OP_SET_PROP, -2))
                return false;
        } else {
            if (!compile_expr(cx, pat->a))
                return false;
            uint16_t c;
            if (!atom_const(cx, pat->units, pat->len, pat->pos, &c))
                return false;
            if (!emit_op(cx, JS_OP_SWAP, 0) ||
                !emit_op_u16(cx, JS_OP_SET_PROP_ATOM, c, -1))
                return false;
        }
        return emit_op(cx, JS_OP_POP, -1);
    }
    case JS_AST_DEFAULT:
        return compile_pattern_default(cx, pat, declaring);
    case JS_AST_ARRAY_PATTERN: {
        uint32_t i = 0;
        for (; i < pat->count; i++) {
            const JsAstNode *el = pat->items[i];
            if (el->kind == JS_AST_REST) {
                /* rest: collect elems [i..] into a new array */
                if (!emit_op(cx, JS_OP_DUP, +1) || !emit_number(cx, (double)i, el->pos) ||
                    !emit_op(cx, JS_OP_ARRAY_REST, -1))
                    return false;
                if (!compile_pattern_store(cx, el->a, declaring))
                    return false;
                break;
            }
            if (el->kind == JS_AST_HOLE)
                continue;
            if (!emit_op(cx, JS_OP_DUP, +1) || !emit_number(cx, (double)i, el->pos) ||
                !emit_op(cx, JS_OP_GET_PROP, -1))
                return false;
            if (!compile_pattern_store(cx, el, declaring))
                return false;
        }
        return emit_op(cx, JS_OP_POP, -1);
    }
    case JS_AST_OBJECT_PATTERN: {
        for (uint32_t i = 0; i < pat->count; i++) {
            const JsAstNode *prop = pat->items[i];
            if (prop->kind == JS_AST_REST)
                return cerr(cx, prop->pos, "object rest in destructuring is not supported yet");
            if (!emit_op(cx, JS_OP_DUP, +1))
                return false;
            if (prop->flags & JS_F_COMPUTED) {
                if (!compile_expr(cx, prop->a) || !emit_op(cx, JS_OP_GET_PROP, -1))
                    return false;
            } else {
                const JsAstNode *key = prop->a;
                if (key->kind == JS_AST_NUMBER) {
                    if (!emit_number(cx, key->number, key->pos) ||
                        !emit_op(cx, JS_OP_GET_PROP, -1))
                        return false;
                } else {
                    uint16_t c;
                    if (!atom_const(cx, key->units, key->len, key->pos, &c) ||
                        !emit_op_u16(cx, JS_OP_GET_PROP_ATOM, c, 0))
                        return false;
                }
            }
            if (!compile_pattern_store(cx, prop->b, declaring))
                return false;
        }
        return emit_op(cx, JS_OP_POP, -1);
    }
    default:
        return cerr(cx, pat->pos, "invalid assignment target");
    }
}

/* ---- operators ---- */

static JsOp binop_opcode(uint8_t op) {
    switch ((JsTokKind)op) {
    case JS_T_PLUS: return JS_OP_ADD;
    case JS_T_MINUS: return JS_OP_SUB;
    case JS_T_STAR: return JS_OP_MUL;
    case JS_T_SLASH: return JS_OP_DIV;
    case JS_T_PERCENT: return JS_OP_MOD;
    case JS_T_POW: return JS_OP_POW;
    case JS_T_AMP: return JS_OP_BITAND;
    case JS_T_BAR: return JS_OP_BITOR;
    case JS_T_CARET: return JS_OP_BITXOR;
    case JS_T_SHL: return JS_OP_SHL;
    case JS_T_SHR: return JS_OP_SHR;
    case JS_T_USHR: return JS_OP_USHR;
    case JS_T_EQEQ: return JS_OP_EQ;
    case JS_T_NE: return JS_OP_NEQ;
    case JS_T_EQEQEQ: return JS_OP_STRICT_EQ;
    case JS_T_STRICT_NE: return JS_OP_STRICT_NEQ;
    case JS_T_LT: return JS_OP_LT;
    case JS_T_GT: return JS_OP_GT;
    case JS_T_LE: return JS_OP_LE;
    case JS_T_GE: return JS_OP_GE;
    case JS_T_IN: return JS_OP_IN;
    default: return JS_OP_NOP;
    }
}

static JsOp compound_opcode(uint8_t op) {
    switch ((JsTokKind)op) {
    case JS_T_PLUS_EQ: return JS_OP_ADD;
    case JS_T_MINUS_EQ: return JS_OP_SUB;
    case JS_T_STAR_EQ: return JS_OP_MUL;
    case JS_T_SLASH_EQ: return JS_OP_DIV;
    case JS_T_PERCENT_EQ: return JS_OP_MOD;
    case JS_T_POW_EQ: return JS_OP_POW;
    case JS_T_SHL_EQ: return JS_OP_SHL;
    case JS_T_SHR_EQ: return JS_OP_SHR;
    case JS_T_USHR_EQ: return JS_OP_USHR;
    case JS_T_AMP_EQ: return JS_OP_BITAND;
    case JS_T_BAR_EQ: return JS_OP_BITOR;
    case JS_T_CARET_EQ: return JS_OP_BITXOR;
    default: return JS_OP_NOP;
    }
}

static bool is_logical_assign(uint8_t op) {
    return op == JS_T_ANDAND_EQ || op == JS_T_OROR_EQ || op == JS_T_QUESQUES_EQ;
}

static JsOp logical_peek_op(uint8_t op) {
    if (op == JS_T_ANDAND_EQ || op == JS_T_ANDAND) return JS_OP_JF_PEEK;
    if (op == JS_T_OROR_EQ || op == JS_T_OROR) return JS_OP_JT_PEEK;
    return JS_OP_JNN_PEEK;
}

static bool compile_assign(JsCompiler *cx, const JsAstNode *n) {
    const JsAstNode *t = n->a;
    note_pos(cx, n->pos);

    if (n->op == JS_T_EQ) {
        if (t->kind == JS_AST_IDENT)
            return compile_expr(cx, n->b) && compile_ident_store(cx, t);
        if (t->kind == JS_AST_MEMBER) {
            if (!compile_member_target_base(cx, t) || !compile_expr(cx, n->b))
                return false;
            if (t->flags & JS_F_COMPUTED)
                return emit_op(cx, JS_OP_SET_PROP, -2);
            uint16_t c;
            if (!atom_const(cx, t->units, t->len, t->pos, &c))
                return false;
            return emit_op_u16(cx, JS_OP_SET_PROP_ATOM, c, -1);
        }
        if (!compile_expr(cx, n->b) || !emit_op(cx, JS_OP_DUP, +1))
            return false;
        return compile_pattern_store(cx, t, false);
    }

    if (is_logical_assign(n->op)) {
        JsOp peek = logical_peek_op(n->op);
        if (t->kind == JS_AST_IDENT) {
            uint32_t end;
            if (!compile_ident_load(cx, t) || !emit_jump(cx, peek, 0, &end))
                return false;
            if (!emit_op(cx, JS_OP_POP, -1) || !compile_expr(cx, n->b) ||
                !compile_ident_store(cx, t))
                return false;
            patch_here(cx, end);
            return true;
        }
        if (t->kind == JS_AST_MEMBER && !(t->flags & JS_F_COMPUTED)) {
            uint16_t c;
            uint32_t sc, end;
            if (!atom_const(cx, t->units, t->len, t->pos, &c))
                return false;
            if (!compile_expr(cx, t->a) || !emit_op(cx, JS_OP_DUP, +1) ||
                !emit_op_u16(cx, JS_OP_GET_PROP_ATOM, c, 0))
                return false;
            if (!emit_jump(cx, peek, 0, &sc))
                return false;
            if (!emit_op(cx, JS_OP_POP, -1) || !compile_expr(cx, n->b) ||
                !emit_op_u16(cx, JS_OP_SET_PROP_ATOM, c, -1))
                return false;
            if (!emit_jump(cx, JS_OP_JUMP, 0, &end))
                return false;
            set_depth(cx, cx->cur->depth + 1);
            patch_here(cx, sc);
            if (!emit_op(cx, JS_OP_SWAP, 0) || !emit_op(cx, JS_OP_POP, -1))
                return false;
            patch_here(cx, end);
            return true;
        }
        if (t->kind == JS_AST_MEMBER) {
            uint32_t sc, end;
            if (!compile_expr(cx, t->a) || !compile_expr(cx, t->b) ||
                !emit_op(cx, JS_OP_DUP2, +2) || !emit_op(cx, JS_OP_GET_PROP, -1))
                return false;
            if (!emit_jump(cx, peek, 0, &sc))
                return false;
            if (!emit_op(cx, JS_OP_POP, -1) || !compile_expr(cx, n->b) ||
                !emit_op(cx, JS_OP_SET_PROP, -2))
                return false;
            if (!emit_jump(cx, JS_OP_JUMP, 0, &end))
                return false;
            set_depth(cx, cx->cur->depth + 2);
            patch_here(cx, sc);
            if (!emit_op(cx, JS_OP_ROT3, 0) || !emit_op(cx, JS_OP_POP, -1) ||
                !emit_op(cx, JS_OP_SWAP, 0) || !emit_op(cx, JS_OP_POP, -1))
                return false;
            patch_here(cx, end);
            return true;
        }
        return cerr(cx, t->pos, "invalid assignment target");
    }

    JsOp op = compound_opcode(n->op);
    if (op == JS_OP_NOP)
        return cerr(cx, n->pos, "unsupported assignment operator");
    if (t->kind == JS_AST_IDENT) {
        if (!compile_ident_load(cx, t) || !compile_expr(cx, n->b) ||
            !emit_op(cx, op, -1))
            return false;
        return compile_ident_store(cx, t);
    }
    if (t->kind == JS_AST_MEMBER && !(t->flags & JS_F_COMPUTED)) {
        uint16_t c;
        if (!atom_const(cx, t->units, t->len, t->pos, &c))
            return false;
        if (!compile_expr(cx, t->a) || !emit_op(cx, JS_OP_DUP, +1) ||
            !emit_op_u16(cx, JS_OP_GET_PROP_ATOM, c, 0) ||
            !compile_expr(cx, n->b) || !emit_op(cx, op, -1))
            return false;
        return emit_op_u16(cx, JS_OP_SET_PROP_ATOM, c, -1);
    }
    if (t->kind == JS_AST_MEMBER) {
        if (!compile_expr(cx, t->a) || !compile_expr(cx, t->b) ||
            !emit_op(cx, JS_OP_DUP2, +2) || !emit_op(cx, JS_OP_GET_PROP, -1) ||
            !compile_expr(cx, n->b) || !emit_op(cx, op, -1))
            return false;
        return emit_op(cx, JS_OP_SET_PROP, -2);
    }
    return cerr(cx, t->pos, "invalid assignment target");
}

static bool compile_update(JsCompiler *cx, const JsAstNode *n) {
    const JsAstNode *t = n->a;
    JsOp arith = n->op == JS_T_INC ? JS_OP_ADD : JS_OP_SUB;
    bool prefix = (n->flags & JS_F_PREFIX) != 0;
    note_pos(cx, n->pos);

    if (t->kind == JS_AST_IDENT) {
        if (!compile_ident_load(cx, t) || !emit_op(cx, JS_OP_POS, 0))
            return false;
        if (prefix)
            return emit_number(cx, 1, n->pos) && emit_op(cx, arith, -1) &&
                   compile_ident_store(cx, t);
        if (!emit_op(cx, JS_OP_DUP, +1) || !emit_number(cx, 1, n->pos) ||
            !emit_op(cx, arith, -1) || !compile_ident_store(cx, t))
            return false;
        return emit_op(cx, JS_OP_POP, -1);
    }
    if (t->kind != JS_AST_MEMBER)
        return cerr(cx, t->pos, "invalid increment/decrement target");

    bool computed = (t->flags & JS_F_COMPUTED) != 0;
    uint16_t c = 0;
    if (!computed && !atom_const(cx, t->units, t->len, t->pos, &c))
        return false;
    if (computed) {
        if (!compile_expr(cx, t->a) || !compile_expr(cx, t->b) ||
            !emit_op(cx, JS_OP_DUP2, +2) || !emit_op(cx, JS_OP_GET_PROP, -1) ||
            !emit_op(cx, JS_OP_POS, 0))
            return false;
    } else {
        if (!compile_expr(cx, t->a) || !emit_op(cx, JS_OP_DUP, +1) ||
            !emit_op_u16(cx, JS_OP_GET_PROP_ATOM, c, 0) || !emit_op(cx, JS_OP_POS, 0))
            return false;
    }
    if (!prefix) {
        uint16_t scratch;
        if (!get_scratch(cx, &scratch) || !emit_op_u16(cx, JS_OP_SET_LOCAL, scratch, 0))
            return false;
    }
    if (!emit_number(cx, 1, n->pos) || !emit_op(cx, arith, -1))
        return false;
    if (computed) {
        if (!emit_op(cx, JS_OP_SET_PROP, -2))
            return false;
    } else {
        if (!emit_op_u16(cx, JS_OP_SET_PROP_ATOM, c, -1))
            return false;
    }
    if (!prefix) {
        uint16_t scratch;
        if (!get_scratch(cx, &scratch) || !emit_op(cx, JS_OP_POP, -1) ||
            !emit_op_u16(cx, JS_OP_GET_LOCAL, scratch, +1))
            return false;
    }
    return true;
}

static bool compile_template(JsCompiler *cx, const JsAstNode *n) {
    if (!emit_string_const(cx, n->items[0]->units, n->items[0]->len, n->pos))
        return false;
    for (uint32_t i = 1; i < n->count; i += 2) {
        if (!compile_expr(cx, n->items[i]) || !emit_op(cx, JS_OP_TO_STRING, 0) ||
            !emit_op(cx, JS_OP_ADD, -1))
            return false;
        const JsAstNode *chunk = n->items[i + 1];
        if (chunk->len) {
            if (!emit_string_const(cx, chunk->units, chunk->len, chunk->pos) ||
                !emit_op(cx, JS_OP_ADD, -1))
                return false;
        }
    }
    return true;
}

static bool compile_object(JsCompiler *cx, const JsAstNode *n) {
    if (!emit_op(cx, JS_OP_NEW_OBJECT, +1))
        return false;
    for (uint32_t i = 0; i < n->count; i++) {
        const JsAstNode *prop = n->items[i];
        if (prop->kind == JS_AST_SPREAD) {
            if (!compile_expr(cx, prop->a) || !emit_op(cx, JS_OP_OBJ_SPREAD, -1))
                return false;
            continue;
        }
        if (prop->flags & JS_F_COMPUTED) {
            if (!compile_expr(cx, prop->a))
                return false;
        } else if (prop->a->kind == JS_AST_NUMBER) {
            if (!emit_number(cx, prop->a->number, prop->a->pos))
                return false;
        } else {
            if (!emit_string_const(cx, prop->a->units, prop->a->len, prop->a->pos))
                return false;
        }
        if (prop->flags & JS_F_METHOD) {
            if (!compile_function(cx, prop->b))
                return false;
        } else if (!compile_expr(cx, prop->b)) {
            return false;
        }
        if (!emit_op(cx, JS_OP_DEFINE_PROP, -2))
            return false;
    }
    return true;
}

static bool compile_array(JsCompiler *cx, const JsAstNode *n) {
    if (!emit_op(cx, JS_OP_NEW_ARRAY, +1) || !emit_u16(cx, 0))
        return false;
    for (uint32_t i = 0; i < n->count; i++) {
        const JsAstNode *el = n->items[i];
        if (el->kind == JS_AST_HOLE) {
            if (!emit_op(cx, JS_OP_UNDEFINED, +1) || !emit_op(cx, JS_OP_ARRAY_APPEND, -1))
                return false;
        } else if (el->kind == JS_AST_SPREAD) {
            if (!compile_expr(cx, el->a) || !emit_op(cx, JS_OP_ARRAY_SPREAD, -1))
                return false;
        } else {
            if (!compile_expr(cx, el) || !emit_op(cx, JS_OP_ARRAY_APPEND, -1))
                return false;
        }
    }
    return true;
}

/* ---- calls ---- */

static bool compile_args(JsCompiler *cx, JsAstNode *const *args, uint32_t count,
                         bool *varargs) {
    *varargs = false;
    for (uint32_t i = 0; i < count; i++) {
        if (args[i]->kind == JS_AST_SPREAD) {
            *varargs = true;
            break;
        }
    }
    if (!*varargs) {
        for (uint32_t i = 0; i < count; i++) {
            if (!compile_expr(cx, args[i]))
                return false;
        }
        return true;
    }
    /* build an argument array */
    if (!emit_op(cx, JS_OP_NEW_ARRAY, +1) || !emit_u16(cx, 0))
        return false;
    for (uint32_t i = 0; i < count; i++) {
        if (args[i]->kind == JS_AST_SPREAD) {
            if (!compile_expr(cx, args[i]->a) || !emit_op(cx, JS_OP_ARRAY_SPREAD, -1))
                return false;
        } else {
            if (!compile_expr(cx, args[i]) || !emit_op(cx, JS_OP_ARRAY_APPEND, -1))
                return false;
        }
    }
    return true;
}

static bool compile_call(JsCompiler *cx, const JsAstNode *n) {
    const JsAstNode *callee = n->a;
    note_pos(cx, n->pos);
    JsPatchList opt = {0};
    bool method = callee->kind == JS_AST_MEMBER;

    if (method) {
        /* leave [receiver, method] then swap -> [method, receiver] as this */
        if (!compile_member_chain(cx, callee->a, &opt))
            return false;
        if (callee->flags & JS_F_OPTIONAL) {
            uint32_t patch;
            if (!emit_jump(cx, JS_OP_OPT_CHAIN, 0, &patch) ||
                !patch_list_add(cx, &opt, patch))
                return false;
        }
        if (!emit_op(cx, JS_OP_DUP, +1)) /* [recv recv] */
            return false;
        if (callee->flags & JS_F_COMPUTED) {
            if (!compile_expr(cx, callee->b) || !emit_op(cx, JS_OP_GET_PROP, -1))
                return false;
        } else {
            uint16_t c;
            if (!atom_const(cx, callee->units, callee->len, callee->pos, &c) ||
                !emit_op_u16(cx, JS_OP_GET_PROP_ATOM, c, 0))
                return false;
        }
        /* [recv method] -> [method recv] */
        if (!emit_op(cx, JS_OP_SWAP, 0))
            return false;
    } else {
        if (!compile_member_chain(cx, callee, &opt)) /* value = callee */
            return false;
        if (!emit_op(cx, JS_OP_UNDEFINED, +1)) /* this = undefined */
            return false;
    }

    /* optional call: `f?.()` */
    uint32_t opt_call_patch = 0;
    bool has_opt_call = (n->flags & JS_F_OPTIONAL) != 0;
    if (has_opt_call) {
        if (!emit_jump(cx, JS_OP_OPT_CALL_CHECK, 0, &opt_call_patch))
            return false;
    }

    bool varargs;
    if (!compile_args(cx, n->items, n->count, &varargs))
        return false;
    if (varargs) {
        if (!emit_op(cx, JS_OP_CALL_VARARGS, -2)) /* [callee this arr] -> [res] */
            return false;
    } else {
        if (n->count > 255)
            return cerr(cx, n->pos, "too many call arguments");
        if (!emit_op(cx, JS_OP_CALL, -(1 + (int)n->count)) || /* pops this+args, callee->res */
            !emit8(cx, (uint8_t)n->count))
            return false;
    }

    if (has_opt_call) {
        uint32_t after;
        if (!emit_jump(cx, JS_OP_JUMP, 0, &after))
            return false;
        patch_at(cx, opt_call_patch, FN->code_len);
        if (!emit_op(cx, JS_OP_UNDEFINED, +1)) /* nullish callee -> undefined */
            return false;
        set_depth(cx, cx->cur->depth - 1); /* both paths converge at same depth */
        patch_here(cx, after);
        set_depth(cx, cx->cur->depth + 1);
    }
    for (uint32_t i = 0; i < opt.count; i++)
        patch_here(cx, opt.offs[i]);
    return true;
}

static bool compile_new(JsCompiler *cx, const JsAstNode *n) {
    note_pos(cx, n->pos);
    if (!compile_expr(cx, n->a)) /* callee value (member chains ok) */
        return false;
    if (!emit_op(cx, JS_OP_UNDEFINED, +1)) /* this placeholder; NEW overwrites it */
        return false;
    bool varargs;
    if (!compile_args(cx, n->items, n->count, &varargs))
        return false;
    if (varargs) {
        if (!emit_op(cx, JS_OP_NEW_VARARGS, -2)) /* [callee this arr] -> [res] */
            return false;
    } else {
        if (n->count > 255)
            return cerr(cx, n->pos, "too many constructor arguments");
        if (!emit_op(cx, JS_OP_NEW, -(1 + (int)n->count)) ||
            !emit8(cx, (uint8_t)n->count))
            return false;
    }
    return true;
}

/* ---- expressions ---- */

static bool compile_expr(JsCompiler *cx, const JsAstNode *n) {
    switch ((JsAstKind)n->kind) {
    case JS_AST_NUMBER:
        return emit_number(cx, n->number, n->pos);
    case JS_AST_STRING:
        return emit_string_const(cx, n->units, n->len, n->pos);
    case JS_AST_NULL_LIT:
        return emit_op(cx, JS_OP_NULL, +1);
    case JS_AST_BOOL_LIT:
        return emit_op(cx, (n->flags & JS_F_TRUE) ? JS_OP_TRUE : JS_OP_FALSE, +1);
    case JS_AST_THIS:
        return emit_op(cx, JS_OP_GET_THIS, +1);
    case JS_AST_IDENT:
        return compile_ident_load(cx, n);
    case JS_AST_TEMPLATE:
        return compile_template(cx, n);
    case JS_AST_ARRAY:
        return compile_array(cx, n);
    case JS_AST_OBJECT:
        return compile_object(cx, n);
    case JS_AST_MEMBER:
        return compile_member_expr(cx, n);
    case JS_AST_CALL:
        return compile_call(cx, n);
    case JS_AST_NEW:
        return compile_new(cx, n);
    case JS_AST_FUNC_EXPR:
        return compile_function(cx, n);
    case JS_AST_SEQUENCE:
        for (uint32_t i = 0; i < n->count; i++) {
            if (!compile_expr(cx, n->items[i]))
                return false;
            if (i + 1 < n->count && !emit_op(cx, JS_OP_POP, -1))
                return false;
        }
        return true;
    case JS_AST_COND: {
        uint32_t pelse, pend;
        if (!compile_expr(cx, n->a) || !emit_jump(cx, JS_OP_JUMP_IF_FALSE, -1, &pelse))
            return false;
        int d0 = cx->cur->depth;
        if (!compile_expr(cx, n->b) || !emit_jump(cx, JS_OP_JUMP, 0, &pend))
            return false;
        set_depth(cx, d0);
        patch_here(cx, pelse);
        if (!compile_expr(cx, n->c))
            return false;
        patch_here(cx, pend);
        return true;
    }
    case JS_AST_LOGICAL: {
        uint32_t end;
        if (!compile_expr(cx, n->a) || !emit_jump(cx, logical_peek_op(n->op), 0, &end) ||
            !emit_op(cx, JS_OP_POP, -1) || !compile_expr(cx, n->b))
            return false;
        patch_here(cx, end);
        return true;
    }
    case JS_AST_BINARY: {
        JsOp op = binop_opcode(n->op);
        if (op == JS_OP_NOP)
            return cerr(cx, n->pos, "unsupported operator");
        note_pos(cx, n->pos);
        return compile_expr(cx, n->a) && compile_expr(cx, n->b) && emit_op(cx, op, -1);
    }
    case JS_AST_UNARY:
        note_pos(cx, n->pos);
        switch ((JsTokKind)n->op) {
        case JS_T_MINUS:
            return compile_expr(cx, n->a) && emit_op(cx, JS_OP_NEG, 0);
        case JS_T_PLUS:
            return compile_expr(cx, n->a) && emit_op(cx, JS_OP_POS, 0);
        case JS_T_NOT:
            return compile_expr(cx, n->a) && emit_op(cx, JS_OP_NOT, 0);
        case JS_T_TILDE:
            return compile_expr(cx, n->a) && emit_op(cx, JS_OP_BITNOT, 0);
        case JS_T_VOID:
            return compile_expr(cx, n->a) && emit_op(cx, JS_OP_POP, -1) &&
                   emit_op(cx, JS_OP_UNDEFINED, +1);
        case JS_T_TYPEOF:
            if (n->a->kind == JS_AST_IDENT &&
                resolve_local_in(cx->cur, n->a->units, n->a->len) < 0) {
                bool ic;
                int up = resolve_upvalue(cx, cx->cur, n->a->units, n->a->len, &ic);
                if (cx->err_msg)
                    return false;
                if (up >= 0)
                    return emit_op_u16(cx, JS_OP_GET_UPVAL, (uint16_t)up, +1) &&
                           emit_op(cx, JS_OP_TYPEOF, 0);
                if (cx->module && resolve_module_binding(cx, n->a->units, n->a->len)) {
                    int m = emit_module_load(cx, n->a);
                    return m > 0 && emit_op(cx, JS_OP_TYPEOF, 0);
                }
                uint16_t c;
                if (!atom_const(cx, n->a->units, n->a->len, n->a->pos, &c))
                    return false;
                return emit_op_u16(cx, cx->repl ? JS_OP_GET_LEXICAL_SOFT : JS_OP_GET_GLOBAL_SOFT,
                                   c, +1) &&
                       emit_op(cx, JS_OP_TYPEOF, 0);
            }
            return compile_expr(cx, n->a) && emit_op(cx, JS_OP_TYPEOF, 0);
        case JS_T_DELETE: {
            const JsAstNode *m = n->a;
            if (m->kind != JS_AST_MEMBER)
                return compile_expr(cx, m) && emit_op(cx, JS_OP_POP, -1) &&
                       emit_op(cx, JS_OP_TRUE, +1);
            if (m->flags & JS_F_OPTIONAL)
                return cerr(cx, m->pos, "optional chaining in delete is not supported");
            if (!compile_expr(cx, m->a))
                return false;
            if (m->flags & JS_F_COMPUTED) {
                if (!compile_expr(cx, m->b))
                    return false;
            } else {
                if (!emit_string_const(cx, m->units, m->len, m->pos))
                    return false;
            }
            return emit_op(cx, JS_OP_DELETE_PROP, -1);
        }
        default:
            return cerr(cx, n->pos, "unsupported unary operator");
        }
    case JS_AST_UPDATE:
        return compile_update(cx, n);
    case JS_AST_ASSIGN:
        return compile_assign(cx, n);
    case JS_AST_AWAIT:
        note_pos(cx, n->pos);
        /* operand in, result out: net stack effect 0 */
        return compile_expr(cx, n->a) && emit_op(cx, JS_OP_AWAIT, 0);
    case JS_AST_IMPORT_CALL:
        note_pos(cx, n->pos);
        if (!compile_expr(cx, n->a))
            return false;
        /* options argument: evaluated (after the specifier, per spec order)
         * for side effects only, then discarded */
        if (n->b && !(compile_expr(cx, n->b) && emit_op(cx, JS_OP_POP, -1)))
            return false;
        /* specifier in, promise out: net stack effect 0 */
        return emit_op(cx, JS_OP_DYNAMIC_IMPORT, 0);
    case JS_AST_REGEX: {
#ifdef LAMASSU_HAS_REGEX
        /* early SyntaxError: validate the pattern now; the opcode recompiles
         * it at evaluation (a literal yields a fresh object each time) */
        const char *em = js_regexp_validate(cx->vm, n->units, n->len,
                                            n->units2, n->len2);
        if (em)
            return cerr(cx, n->pos, em);
        uint16_t csrc, cflags;
        if (!atom_const(cx, n->units, n->len, n->pos, &csrc) ||
            !atom_const(cx, n->units2, n->len2, n->pos, &cflags))
            return false;
        note_pos(cx, n->pos);
        return emit_op_u16(cx, JS_OP_NEW_REGEXP, csrc, +1) && emit_u16(cx, cflags);
#else
        return cerr(cx, n->pos, "regex support is not compiled into this build");
#endif
    }
    case JS_AST_SPREAD:
        return cerr(cx, n->pos, "spread is not valid here");
    default:
        return cerr(cx, n->pos, "unsupported expression");
    }
}

/* ---- functions ---- */

/* Binds parameters into locals; emits default/destructuring prologue. */
static bool bind_params(JsCompiler *cx, const JsAstNode *fnode, uint16_t *n_params,
                        bool *has_rest) {
    JsFuncState *fs = cx->cur;
    uint32_t count = fnode->count;
    *has_rest = false;
    uint16_t np = 0;

    /* Pass 1: one slot per positional parameter. */
    for (uint32_t i = 0; i < count; i++) {
        const JsAstNode *p = fnode->items[i];
        if (p->kind == JS_AST_REST) {
            *has_rest = true;
            if (p->a->kind != JS_AST_IDENT)
                return cerr(cx, p->pos, "rest parameter must be a simple identifier");
            uint16_t slot;
            if (!declare_local(cx, p->a->units, p->a->len, false, p->pos, 0, &slot))
                return false;
            np++;
            break;
        }
        const JsAstNode *name_node = p;
        if (p->kind == JS_AST_DEFAULT)
            name_node = p->a;
        if (name_node->kind == JS_AST_IDENT) {
            uint16_t slot;
            if (!declare_local(cx, name_node->units, name_node->len, false, p->pos, 0, &slot))
                return false;
        } else {
            /* anonymous slot for a destructuring parameter */
            uint16_t slot;
            if (!declare_local(cx, NULL, 0, false, p->pos, 0, &slot))
                return false;
        }
        np++;
    }
    *n_params = np;

    /* Pass 2: pre-declare names introduced by patterns (slots after params). */
    for (uint32_t i = 0; i < count; i++) {
        const JsAstNode *p = fnode->items[i];
        if (p->kind == JS_AST_REST)
            continue;
        const JsAstNode *target = p->kind == JS_AST_DEFAULT ? p->a : p;
        if (target->kind == JS_AST_ARRAY_PATTERN || target->kind == JS_AST_OBJECT_PATTERN) {
            if (!pattern_declare(cx, target, false, 0, false))
                return false;
        }
    }

    /* Pass 3: prologue for defaults and destructuring. */
    for (uint32_t i = 0; i < count; i++) {
        const JsAstNode *p = fnode->items[i];
        if (p->kind == JS_AST_REST)
            continue;
        const JsAstNode *target = p->kind == JS_AST_DEFAULT ? p->a : p;
        bool has_default = p->kind == JS_AST_DEFAULT;
        bool is_pattern = target->kind == JS_AST_ARRAY_PATTERN ||
                          target->kind == JS_AST_OBJECT_PATTERN;
        if (!has_default && !is_pattern)
            continue; /* plain ident: slot already holds the arg */
        if (!emit_op_u16(cx, JS_OP_GET_LOCAL, fs->locals[i].slot, +1))
            return false;
        if (has_default) {
            uint32_t skip;
            if (!emit_op(cx, JS_OP_DUP, +1) || !emit_op(cx, JS_OP_UNDEFINED, +1) ||
                !emit_op(cx, JS_OP_STRICT_EQ, -1) ||
                !emit_jump(cx, JS_OP_JUMP_IF_FALSE, -1, &skip))
                return false;
            if (!emit_op(cx, JS_OP_POP, -1) || !compile_expr(cx, p->b))
                return false;
            patch_here(cx, skip);
        }
        if (is_pattern) {
            if (!compile_pattern_store(cx, target, true))
                return false;
        } else { /* default on a plain ident: store back to its slot */
            if (!emit_op_u16(cx, JS_OP_SET_LOCAL, fs->locals[i].slot, 0) ||
                !emit_op(cx, JS_OP_POP, -1))
                return false;
        }
    }
    return true;
}

static bool compile_block_stmts(JsCompiler *cx, JsAstNode *const *stmts, uint32_t count);

static bool compile_function(JsCompiler *cx, const JsAstNode *fnode) {
    if (++cx->func_depth > 200)
        return cerr(cx, fnode->pos, "functions nested too deeply");

    JsGcCell *cell = js_gc_new_cell(cx->vm, JS_KIND_FUNCTION, sizeof(JsFunctionCell));
    if (!cell)
        return cerr(cx, fnode->pos, "out of memory");
    JsFunctionCell *fn = (JsFunctionCell *)cell;
    memset((char *)fn + sizeof(JsGcCell), 0, sizeof *fn - sizeof(JsGcCell));
    fn->module = cx->module; /* nested fns share the module (exports/imports) */

    JsFuncState fs;
    memset(&fs, 0, sizeof fs);
    fs.parent = cx->cur;
    fs.fn = fn;
    fs.fn_value = js_value_from_cell(cell);
    fs.scratch_slot = -1;
    fs.is_arrow = (fnode->flags & JS_F_ARROW) != 0;
    if (!js_gc_protect(cx->vm, &fs.fn_value))
        return cerr(cx, fnode->pos, "out of memory");
    /* Intern the name only after fn is rooted (interning may collect). */
    if (fnode->len && !fs.is_arrow) {
        JsValue nm = js_atom(cx->vm, fnode->units, fnode->len);
        if (js_is_string(nm))
            ((JsFunctionCell *)js_value_cell(fs.fn_value))->name = js_value_string(nm);
    }
    cx->cur = &fs;

    bool ok = true;
    uint16_t n_params = 0;
    bool has_rest = false;
    ok = bind_params(cx, fnode, &n_params, &has_rest);
    if (ok) {
        if (fnode->flags & JS_F_EXPR_BODY) { /* arrow with expression body */
            ok = compile_expr(cx, fnode->a) && emit_op(cx, JS_OP_RETURN, -1);
        } else {
            ok = compile_block_stmts(cx, fnode->a->items, fnode->a->count);
            if (ok)
                ok = emit_op(cx, JS_OP_UNDEFINED, +1) && emit_op(cx, JS_OP_RETURN, -1);
        }
    }

    fn->n_params = n_params;
    fn->fn_flags = (uint8_t)((fs.is_arrow ? JS_FN_ARROW : 0) |
                             (has_rest ? JS_FN_HAS_REST : 0) |
                             ((fnode->flags & JS_F_ASYNC) ? JS_FN_ASYNC : 0));
    fn->n_locals = fs.slot_max;
    fn->max_stack = (uint32_t)(fs.max_depth < 0 ? 0 : fs.max_depth) + 8;

    cx->cur = fs.parent;
    cx->func_depth--;
    if (!ok) {
        js_gc_unprotect(cx->vm, &fs.fn_value);
        return false;
    }

    /* Emit CLOSURE in the parent. */
    uint16_t cidx;
    if (!const_add(cx, js_value_from_cell(cell), &cidx)) {
        js_gc_unprotect(cx->vm, &fs.fn_value);
        return false;
    }
    js_gc_unprotect(cx->vm, &fs.fn_value);
    uint8_t flags = fs.is_arrow ? 1 : 0; /* bit0: capture `this` lexically */
    return emit_op_u16(cx, JS_OP_CLOSURE, cidx, +1) && emit8(cx, flags);
}

/* ---- statements ---- */

static bool compile_let_decl(JsCompiler *cx, const JsAstNode *n) {
    cx->decl_const = (n->flags & JS_F_CONST) != 0; /* for REPL DEFINE_LEXICAL */
    for (uint32_t i = 0; i < n->count; i++) {
        const JsAstNode *d = n->items[i];
        note_pos(cx, d->pos);
        if (d->b) {
            if (!compile_expr(cx, d->b))
                return false;
        } else if (!emit_op(cx, JS_OP_UNDEFINED, +1)) {
            return false;
        }
        if (!compile_pattern_store(cx, d->a, true))
            return false;
    }
    return true;
}

static bool hoist_functions(JsCompiler *cx, JsAstNode *const *stmts, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const JsAstNode *s = stmts[i];
        /* `export function f(){}` wraps the FUNC_DECL; unwrap it */
        bool exported = false;
        if (s->kind == JS_AST_EXPORT_NAMED && s->a && s->a->kind == JS_AST_FUNC_DECL) {
            s = s->a;
            exported = true;
        }
        if (s->kind != JS_AST_FUNC_DECL)
            continue;
        if (!compile_function(cx, s))
            return false;
        if (!exported && is_repl_top(cx)) {
            /* REPL top-level function -> persistent lexical binding (not const) */
            uint16_t c;
            if (!atom_const(cx, s->units, s->len, s->pos, &c) ||
                !emit_op_u16(cx, JS_OP_DEFINE_LEXICAL, c, 0) || !emit8(cx, 0) ||
                !emit_op(cx, JS_OP_POP, -1))
                return false;
            continue;
        }
        if (exported ||
            (cx->module && cx->cur->is_module && resolve_module_binding(cx, s->units, s->len) &&
             resolve_module_binding(cx, s->units, s->len)->kind == JS_MB_EXPORT)) {
            /* exported function: initialize the export binding */
            uint16_t c;
            JsModBinding *b = resolve_module_binding(cx, s->units, s->len);
            const uint16_t *ename = b ? b->export_name : s->units;
            uint32_t elen = b ? b->export_len : s->len;
            if (!atom_const(cx, ename, elen, s->pos, &c) ||
                !emit_op_u16(cx, JS_OP_SET_EXPORT, c, 0) || !emit_op(cx, JS_OP_POP, -1))
                return false;
            continue;
        }
        int idx = resolve_local_in(cx->cur, s->units, s->len);
        if (idx < 0)
            return cerr(cx, s->pos, "internal: function not pre-declared");
        if (!emit_op_u16(cx, JS_OP_SET_LOCAL, cx->cur->locals[idx].slot, 0) ||
            !emit_op(cx, JS_OP_POP, -1))
            return false;
    }
    return true;
}

static bool compile_block_stmts(JsCompiler *cx, JsAstNode *const *stmts, uint32_t count) {
    JsScopeMark m = scope_enter(cx);
    if (!prescan_declarations(cx, stmts, count, m.local_count))
        return false;
    if (!hoist_functions(cx, stmts, count))
        return false;
    for (uint32_t i = 0; i < count; i++) {
        if (!compile_stmt(cx, stmts[i]))
            return false;
    }
    return scope_leave(cx, m);
}

static bool compile_loop_body(JsCompiler *cx, const JsAstNode *body) {
    return compile_stmt(cx, body);
}

static bool compile_for_of(JsCompiler *cx, const JsAstNode *n, const uint16_t *label,
                           uint32_t label_len) {
    JsScopeMark m = scope_enter(cx);
    note_pos(cx, n->pos);
    if (!compile_expr(cx, n->b) || !emit_op(cx, JS_OP_ITER_NEW, +1))
        return false;

    JsLoopInfo *loop = loop_push(cx, true, label, label_len);
    if (!loop)
        return false;
    loop->entry_depth = cx->cur->depth - 2;
    loop->continue_depth = cx->cur->depth;
    uint32_t top = FN->code_len;
    loop->continue_target = top;

    uint32_t done;
    if (!emit_jump(cx, JS_OP_ITER_NEXT, +1, &done))
        return false;

    JsScopeMark body_scope = scope_enter(cx);
    loop->loopvar_slot = cx->cur->slot_count;
    if (n->flags & JS_F_DECL) {
        if (!pattern_declare(cx, n->a, (n->flags & JS_F_CONST) != 0, body_scope.local_count,
                             false) ||
            !compile_pattern_store(cx, n->a, true))
            return false;
    } else {
        if (!compile_pattern_store(cx, n->a, false))
            return false;
    }
    if (!compile_loop_body(cx, n->c))
        return false;
    if (!scope_leave(cx, body_scope))
        return false;
    if (!emit_jump_to(cx, JS_OP_JUMP, 0, top))
        return false;
    set_depth(cx, loop->entry_depth);
    patch_here(cx, done);
    loop_pop_patch_breaks(cx, loop);
    return scope_leave(cx, m);
}

static bool compile_for(JsCompiler *cx, const JsAstNode *n, const uint16_t *label,
                        uint32_t label_len) {
    JsScopeMark m = scope_enter(cx);
    note_pos(cx, n->pos);
    bool has_let = n->a && n->a->kind == JS_AST_LET_DECL;
    if (n->a) {
        if (has_let) {
            for (uint32_t d = 0; d < n->a->count; d++) {
                if (!pattern_declare(cx, n->a->items[d]->a, (n->a->flags & JS_F_CONST) != 0,
                                     m.local_count, false))
                    return false;
            }
            if (!compile_let_decl(cx, n->a))
                return false;
        } else {
            if (!compile_expr(cx, n->a->a) || !emit_op(cx, JS_OP_POP, -1))
                return false;
        }
    }
    JsLoopInfo *loop = loop_push(cx, true, label, label_len);
    if (!loop)
        return false;
    if (has_let)
        loop->loopvar_slot = m.slot_count;

    uint32_t top = FN->code_len;
    uint32_t exit = 0;
    bool has_test = n->b != NULL;
    if (has_test) {
        if (!compile_expr(cx, n->b) || !emit_jump(cx, JS_OP_JUMP_IF_FALSE, -1, &exit))
            return false;
    }
    if (!compile_loop_body(cx, n->d))
        return false;
    for (uint32_t i = 0; i < loop->continues.count; i++)
        patch_here(cx, loop->continues.offs[i]);
    /* fresh per-iteration binding for captured loop vars */
    if (has_let && loop->loopvar_slot >= 0) {
        if (!emit_op_u16(cx, JS_OP_CLOSE_UPVALS, (uint16_t)loop->loopvar_slot, 0))
            return false;
    }
    if (n->c) {
        if (!compile_expr(cx, n->c) || !emit_op(cx, JS_OP_POP, -1))
            return false;
    }
    if (!emit_jump_to(cx, JS_OP_JUMP, 0, top))
        return false;
    if (has_test)
        patch_here(cx, exit);
    loop_pop_patch_breaks(cx, loop);
    return scope_leave(cx, m);
}

static bool compile_while(JsCompiler *cx, const JsAstNode *n, bool is_do,
                          const uint16_t *label, uint32_t label_len) {
    JsLoopInfo *loop = loop_push(cx, true, label, label_len);
    if (!loop)
        return false;
    note_pos(cx, n->pos);
    if (is_do) {
        uint32_t top = FN->code_len;
        if (!compile_loop_body(cx, n->a))
            return false;
        for (uint32_t i = 0; i < loop->continues.count; i++)
            patch_here(cx, loop->continues.offs[i]);
        if (!compile_expr(cx, n->b) || !emit_jump_to(cx, JS_OP_JUMP_IF_TRUE, -1, top))
            return false;
    } else {
        uint32_t top = FN->code_len;
        loop->continue_target = top;
        uint32_t exit;
        if (!compile_expr(cx, n->a) || !emit_jump(cx, JS_OP_JUMP_IF_FALSE, -1, &exit))
            return false;
        if (!compile_loop_body(cx, n->b) || !emit_jump_to(cx, JS_OP_JUMP, 0, top))
            return false;
        patch_here(cx, exit);
    }
    loop_pop_patch_breaks(cx, loop);
    return true;
}

static bool compile_switch(JsCompiler *cx, const JsAstNode *n) {
    JsScopeMark m = scope_enter(cx);
    for (uint32_t i = 0; i < n->count; i++) {
        if (!prescan_declarations(cx, n->items[i]->items, n->items[i]->count, m.local_count))
            return false;
    }
    for (uint32_t i = 0; i < n->count; i++) {
        if (!hoist_functions(cx, n->items[i]->items, n->items[i]->count))
            return false;
    }
    note_pos(cx, n->pos);
    if (!compile_expr(cx, n->a))
        return false;

    JsLoopInfo *sw = loop_push(cx, false, NULL, 0);
    if (!sw)
        return false;
    sw->entry_depth = cx->cur->depth - 1;

    uint32_t *body_patches = js_arena_alloc(&cx->arena, (size_t)(n->count ? n->count : 1) *
                                                            sizeof(uint32_t));
    if (!body_patches)
        return cerr(cx, n->pos, "out of memory");
    int default_idx = -1;
    for (uint32_t i = 0; i < n->count; i++) {
        const JsAstNode *c = n->items[i];
        if (!c->a) {
            default_idx = (int)i;
            continue;
        }
        if (!compile_expr(cx, c->a) || !emit_jump(cx, JS_OP_CASE, -1, &body_patches[i]))
            return false;
    }
    uint32_t no_match;
    if (!emit_op(cx, JS_OP_POP, -1) || !emit_jump(cx, JS_OP_JUMP, 0, &no_match))
        return false;

    int body_depth = cx->cur->depth;
    for (uint32_t i = 0; i < n->count; i++) {
        const JsAstNode *c = n->items[i];
        set_depth(cx, body_depth);
        if ((int)i == default_idx)
            patch_here(cx, no_match);
        else
            patch_here(cx, body_patches[i]);
        for (uint32_t s = 0; s < c->count; s++) {
            if (!compile_stmt(cx, c->items[s]))
                return false;
        }
    }
    if (default_idx < 0)
        patch_here(cx, no_match);
    loop_pop_patch_breaks(cx, sw);
    return scope_leave(cx, m);
}

static bool compile_try(JsCompiler *cx, const JsAstNode *n) {
    JsFuncState *fs = cx->cur;
    bool has_catch = n->c != NULL;
    bool has_finally = n->d != NULL;
    int start_depth = fs->depth;

    JsFinallyInfo *fin = NULL;
    if (has_finally) {
        if (fs->finally_count >= JS_MAX_FINALLY)
            return cerr(cx, n->pos, "try/finally nested too deeply");
        fin = &fs->finallys[fs->finally_count++];
        memset(fin, 0, sizeof *fin);
    }

    uint32_t handler_patch;
    if (!emit_jump(cx, JS_OP_TRY_PUSH, 0, &handler_patch))
        return false;
    if (!compile_stmt(cx, n->a)) /* try block */
        return false;
    if (!emit_op(cx, JS_OP_TRY_POP, 0))
        return false;
    if (has_finally && !emit_finally_gosubs(cx, fs->finally_count - 1))
        return false;
    uint32_t end_normal;
    if (!emit_jump(cx, JS_OP_JUMP, 0, &end_normal))
        return false;

    /* Handler: thrown value on stack. */
    set_depth(cx, start_depth + 1);
    patch_here(cx, handler_patch);

    JsPatchList catch_ends = {0};
    if (has_catch) {
        uint32_t inner_handler = 0;
        bool guard_catch = has_finally;
        if (guard_catch) {
            if (!emit_jump(cx, JS_OP_TRY_PUSH, 0, &inner_handler))
                return false;
        }
        JsScopeMark cm = scope_enter(cx);
        if (n->b) { /* catch binding */
            if (!pattern_declare(cx, n->b, false, cm.local_count, false) ||
                !compile_pattern_store(cx, n->b, true))
                return false;
        } else {
            if (!emit_op(cx, JS_OP_POP, -1)) /* discard thrown value */
                return false;
        }
        if (!compile_stmt(cx, n->c))
            return false;
        if (!scope_leave(cx, cm))
            return false;
        if (guard_catch && !emit_op(cx, JS_OP_TRY_POP, 0))
            return false;
        if (has_finally && !emit_finally_gosubs(cx, fs->finally_count - 1))
            return false;
        uint32_t ce;
        if (!emit_jump(cx, JS_OP_JUMP, 0, &ce) || !patch_list_add(cx, &catch_ends, ce))
            return false;

        if (guard_catch) {
            /* exception during catch: run finally, rethrow */
            set_depth(cx, start_depth + 1);
            patch_here(cx, inner_handler);
        }
    }

    if (has_finally) {
        /* rethrow path: value on stack, run finally, throw */
        set_depth(cx, start_depth + 1);
        if (!emit_finally_gosubs(cx, fs->finally_count - 1) ||
            !emit_op(cx, JS_OP_THROW, -1))
            return false;
    }

    /* Emit finally body as a subroutine and patch all GOSUBs to it. */
    if (has_finally) {
        fs->finally_count--; /* finally body isn't guarded by itself */
        uint32_t body = FN->code_len;
        for (uint32_t i = 0; i < fin->gosubs.count; i++)
            patch_at(cx, fin->gosubs.offs[i], body);
        set_depth(cx, start_depth + 1); /* return address occupies one slot */
        if (!compile_stmt(cx, n->d) || !emit_op(cx, JS_OP_RET_SUB, -1))
            return false;
    }

    set_depth(cx, start_depth);
    patch_here(cx, end_normal);
    for (uint32_t i = 0; i < catch_ends.count; i++)
        patch_here(cx, catch_ends.offs[i]);
    return true;
}

static bool compile_stmt_labeled(JsCompiler *cx, const JsAstNode *n) {
    const JsAstNode *body = n->a;
    switch ((JsAstKind)body->kind) {
    case JS_AST_WHILE:
        return compile_while(cx, body, false, n->units, n->len);
    case JS_AST_DO_WHILE:
        return compile_while(cx, body, true, n->units, n->len);
    case JS_AST_FOR:
        return compile_for(cx, body, n->units, n->len);
    case JS_AST_FOR_OF:
        return compile_for_of(cx, body, n->units, n->len);
    default: {
        JsLoopInfo *l = loop_push(cx, false, n->units, n->len);
        if (!l)
            return false;
        l->is_label_only = true;
        if (!compile_stmt(cx, body))
            return false;
        loop_pop_patch_breaks(cx, l);
        return true;
    }
    }
}

static bool compile_stmt(JsCompiler *cx, const JsAstNode *n) {
    switch ((JsAstKind)n->kind) {
    case JS_AST_EMPTY:
    case JS_AST_FUNC_DECL: /* hoisted at block entry */
        return true;
    case JS_AST_BLOCK:
        return compile_block_stmts(cx, n->items, n->count);
    case JS_AST_EXPR_STMT:
        note_pos(cx, n->pos);
        if (!compile_expr(cx, n->a))
            return false;
        if (cx->cur->is_module)
            return emit_op_u16(cx, JS_OP_SET_LOCAL, JS_COMPLETION_SLOT, 0) &&
                   emit_op(cx, JS_OP_POP, -1);
        return emit_op(cx, JS_OP_POP, -1);
    case JS_AST_LET_DECL:
        return compile_let_decl(cx, n);
    case JS_AST_IF: {
        uint32_t pelse;
        note_pos(cx, n->pos);
        if (!compile_expr(cx, n->a) || !emit_jump(cx, JS_OP_JUMP_IF_FALSE, -1, &pelse))
            return false;
        if (!compile_stmt(cx, n->b))
            return false;
        if (n->c) {
            uint32_t pend;
            if (!emit_jump(cx, JS_OP_JUMP, 0, &pend))
                return false;
            patch_here(cx, pelse);
            if (!compile_stmt(cx, n->c))
                return false;
            patch_here(cx, pend);
        } else {
            patch_here(cx, pelse);
        }
        return true;
    }
    case JS_AST_WHILE:
        return compile_while(cx, n, false, NULL, 0);
    case JS_AST_DO_WHILE:
        return compile_while(cx, n, true, NULL, 0);
    case JS_AST_FOR:
        return compile_for(cx, n, NULL, 0);
    case JS_AST_FOR_OF:
        return compile_for_of(cx, n, NULL, 0);
    case JS_AST_SWITCH:
        return compile_switch(cx, n);
    case JS_AST_TRY:
        return compile_try(cx, n);
    case JS_AST_BREAK:
    case JS_AST_CONTINUE: {
        bool is_break = n->kind == JS_AST_BREAK;
        JsLoopInfo *l = find_loop(cx, n->len ? n->units : NULL, n->len, !is_break);
        if (!l)
            return cerr(cx, n->pos, is_break ? "no matching loop or switch for 'break'"
                                             : "no matching loop for 'continue'");
        note_pos(cx, n->pos);
        if (!emit_finally_gosubs(cx, l->finally_floor))
            return false;
        if (is_break) {
            if (!emit_pops_to(cx, l->entry_depth))
                return false;
            uint32_t patch;
            if (!emit_jump(cx, JS_OP_JUMP, 0, &patch))
                return false;
            return patch_list_add(cx, &l->breaks, patch);
        }
        if (!emit_pops_to(cx, l->continue_depth))
            return false;
        if (l->continue_target != 0xFFFFFFFF) {
            /* A direct-jump continue (for-of/for-in) bypasses the body scope's
             * scope_leave, which is where the per-iteration loop binding is
             * closed. Close it here too, or a closure captured before the
             * continue would share the next iteration's binding. (do/while set
             * a continue_target but have no per-iteration binding, so their
             * loopvar_slot is -1 and this is skipped.) */
            if (l->loopvar_slot >= 0 &&
                !emit_op_u16(cx, JS_OP_CLOSE_UPVALS, (uint16_t)l->loopvar_slot, 0))
                return false;
            return emit_jump_to(cx, JS_OP_JUMP, 0, l->continue_target);
        }
        uint32_t patch;
        if (!emit_jump(cx, JS_OP_JUMP, 0, &patch))
            return false;
        return patch_list_add(cx, &l->continues, patch);
    }
    case JS_AST_RETURN:
        if (cx->cur->is_module)
            return cerr(cx, n->pos, "'return' outside function");
        note_pos(cx, n->pos);
        if (n->a) {
            if (!compile_expr(cx, n->a))
                return false;
        } else if (!emit_op(cx, JS_OP_UNDEFINED, +1)) {
            return false;
        }
        if (!emit_finally_gosubs(cx, 0))
            return false;
        return emit_op(cx, JS_OP_RETURN, -1);
    case JS_AST_THROW:
        note_pos(cx, n->pos);
        return compile_expr(cx, n->a) && emit_op(cx, JS_OP_THROW, -1);
    case JS_AST_LABELED:
        return compile_stmt_labeled(cx, n);
    case JS_AST_IMPORT:
    case JS_AST_EXPORT_ALL:
        if (!cx->module)
            return cerr(cx, n->pos, JS_ERR_NEEDS_MODULE_LOADER);
        /* bindings/deps handled at analyze/link/eval time; no runtime code */
        return true;
    case JS_AST_EXPORT_NAMED:
        if (!cx->module)
            return cerr(cx, n->pos, JS_ERR_NEEDS_MODULE_LOADER);
        if (n->a) /* export <declaration> */
            return compile_stmt(cx, n->a);
        return true; /* export { ... }: bindings resolved by name */
    case JS_AST_EXPORT_DEFAULT: {
        if (!cx->module)
            return cerr(cx, n->pos, JS_ERR_NEEDS_MODULE_LOADER);
        /* exports.default = value */
        note_pos(cx, n->pos);
        if (n->a->kind == JS_AST_FUNC_EXPR || n->a->kind == JS_AST_FUNC_DECL) {
            if (!compile_function(cx, n->a))
                return false;
        } else if (!compile_expr(cx, n->a)) {
            return false;
        }
        uint16_t c;
        static const uint16_t def_name[] = {'d', 'e', 'f', 'a', 'u', 'l', 't'};
        if (!atom_const(cx, def_name, 7, n->pos, &c))
            return false;
        return emit_op_u16(cx, JS_OP_SET_EXPORT, c, 0) && emit_op(cx, JS_OP_POP, -1);
    }
    default:
        return cerr(cx, n->pos, "unsupported statement");
    }
}

/* ---- entry point ---- */

JsFunctionCell *js_compile_ast(JsContext *ctx, const JsAstNode *module, bool repl,
                               JsCompileError *err) {
    JsVm *vm = ctx->vm;
    JsCompiler cx;
    memset(&cx, 0, sizeof cx);
    cx.ctx = ctx;
    cx.vm = vm;
    cx.repl = repl;
    js_arena_init(&cx.arena, vm);

    JsGcCell *cell = js_gc_new_cell(vm, JS_KIND_FUNCTION, sizeof(JsFunctionCell));
    if (!cell) {
        err->msg = "out of memory";
        err->pos = 0;
        return NULL;
    }
    JsFunctionCell *fn = (JsFunctionCell *)cell;
    memset((char *)fn + sizeof(JsGcCell), 0, sizeof *fn - sizeof(JsGcCell));

    JsFuncState fs;
    memset(&fs, 0, sizeof fs);
    fs.fn = fn;
    fs.fn_value = js_value_from_cell(cell);
    fs.is_module = true;
    fs.scratch_slot = -1;
    fs.slot_count = 1; /* slot 0: completion value */
    fs.slot_max = 1;
    cx.cur = &fs;
    if (!js_gc_protect(vm, &fs.fn_value)) {
        err->msg = "out of memory";
        err->pos = 0;
        return NULL;
    }

    bool ok = compile_block_stmts(&cx, module->items, module->count);
    if (ok)
        ok = emit_op_u16(&cx, JS_OP_GET_LOCAL, JS_COMPLETION_SLOT, +1) &&
             emit_op(&cx, JS_OP_RETURN, -1);

    fn->n_locals = fs.slot_max;
    fn->max_stack = (uint32_t)(fs.max_depth < 0 ? 0 : fs.max_depth) + 8;
    fn->n_params = 0;
    /* top-level await makes the module body a suspendable (async) fiber */
    if (module->flags & JS_F_HAS_TLA)
        fn->fn_flags |= JS_FN_ASYNC;

    js_gc_unprotect(vm, &fs.fn_value);
    js_arena_free(&cx.arena);
    if (!ok) {
        err->msg = cx.err_msg ? cx.err_msg : "compile error";
        err->pos = cx.err_pos;
        return NULL;
    }
    return fn;
}

/* ---- module analysis + module-body compilation ---- */

static JsString *intern_atom(JsCompiler *cx, const uint16_t *u, uint32_t len) {
    JsValue a = js_atom(cx->vm, u, len);
    return js_is_string(a) ? js_value_string(a) : NULL;
}

/* Records a dependency specifier (deduplicated). Returns false on OOM. */
static bool mod_add_dep(JsCompiler *cx, JsModule *m, JsString *spec) {
    for (uint32_t i = 0; i < m->dep_spec_count; i++)
        if (m->dep_specs[i] == spec)
            return true;
    JsString **na = js_realloc_raw(cx->vm, m->dep_specs,
                                   (size_t)m->dep_spec_count * sizeof(JsString *),
                                   (size_t)(m->dep_spec_count + 1) * sizeof(JsString *));
    if (!na)
        return false;
    m->dep_specs = na;
    m->dep_specs[m->dep_spec_count++] = spec;
    return true;
}

static JsModuleImport *mod_add_import(JsCompiler *cx, JsModule *m, JsString *spec,
                                      JsString *imported, uint8_t kind) {
    JsModuleImport *na = js_realloc_raw(cx->vm, m->imports,
                                        (size_t)m->import_count * sizeof(JsModuleImport),
                                        (size_t)(m->import_count + 1) * sizeof(JsModuleImport));
    if (!na)
        return NULL;
    m->imports = na;
    JsModuleImport *imp = &m->imports[m->import_count];
    imp->specifier = spec;
    imp->imported_name = imported;
    imp->kind = kind;
    imp->source = NULL;
    m->import_count++;
    return imp;
}

static bool mod_add_star(JsCompiler *cx, JsModule *m, JsString *spec, uint8_t kind,
                         JsString *imported, JsString *exported) {
    JsStarExport *na = js_realloc_raw(cx->vm, m->stars,
                                      (size_t)m->star_count * sizeof(JsStarExport),
                                      (size_t)(m->star_count + 1) * sizeof(JsStarExport));
    if (!na)
        return false;
    m->stars = na;
    JsStarExport *s = &m->stars[m->star_count];
    s->specifier = spec;
    s->source = NULL;
    s->kind = kind;
    s->imported = imported;
    s->exported = exported;
    m->star_count++;
    return true;
}

static bool mbind_add(JsCompiler *cx, uint32_t *cap, JsModBinding b) {
    if (cx->mbind_count == *cap) {
        uint32_t ncap = *cap ? *cap * 2 : 8;
        JsModBinding *na = js_arena_alloc(&cx->arena, (size_t)ncap * sizeof(JsModBinding));
        if (!na)
            return false;
        if (cx->mbind_count)
            memcpy(na, cx->mbind, cx->mbind_count * sizeof(JsModBinding));
        cx->mbind = na;
        *cap = ncap;
    }
    cx->mbind[cx->mbind_count++] = b;
    return true;
}

/* Adds exported names from a declaration (let/const/function) to mbind. */
static bool mbind_add_export(JsCompiler *cx, uint32_t *cap, const uint16_t *local,
                             uint32_t local_len, const uint16_t *exp, uint32_t exp_len,
                             bool is_const) {
    JsModBinding b;
    b.name = local;
    b.len = local_len;
    b.kind = JS_MB_EXPORT;
    b.is_const = is_const;
    b.import_index = 0;
    b.export_name = exp ? exp : local;
    b.export_len = exp ? exp_len : local_len;
    return mbind_add(cx, cap, b);
}

/* Collects binding names of a declaration pattern into the export list. */
static bool export_pattern_names(JsCompiler *cx, uint32_t *cap, const JsAstNode *pat,
                                 bool is_const) {
    switch ((JsAstKind)pat->kind) {
    case JS_AST_IDENT:
        return mbind_add_export(cx, cap, pat->units, pat->len, NULL, 0, is_const);
    case JS_AST_DEFAULT:
        return export_pattern_names(cx, cap, pat->a, is_const);
    case JS_AST_ARRAY_PATTERN:
        for (uint32_t i = 0; i < pat->count; i++) {
            const JsAstNode *el = pat->items[i];
            if (el->kind == JS_AST_HOLE)
                continue;
            if (!export_pattern_names(cx, cap, el->kind == JS_AST_REST ? el->a : el, is_const))
                return false;
        }
        return true;
    case JS_AST_OBJECT_PATTERN:
        for (uint32_t i = 0; i < pat->count; i++) {
            const JsAstNode *prop = pat->items[i];
            if (!export_pattern_names(cx, cap, prop->kind == JS_AST_REST ? prop->a : prop->b,
                                      is_const))
                return false;
        }
        return true;
    default:
        return true;
    }
}

static bool analyze_module(JsCompiler *cx, const JsAstNode *module, JsModule *mod) {
    uint32_t cap = 0;
    for (uint32_t i = 0; i < module->count; i++) {
        const JsAstNode *n = module->items[i];
        switch ((JsAstKind)n->kind) {
        case JS_AST_IMPORT: {
            JsString *spec = intern_atom(cx, n->units, n->len);
            if (!spec || !mod_add_dep(cx, mod, spec))
                return cerr(cx, n->pos, "out of memory");
            for (uint32_t s = 0; s < n->count; s++) {
                const JsAstNode *sp = n->items[s];
                JsModBinding b;
                memset(&b, 0, sizeof b);
                b.name = sp->units2;
                b.len = sp->len2;
                if (sp->flags & JS_F_SPEC_NS) {
                    b.kind = JS_MB_IMPORT_NS;
                    if (!mod_add_import(cx, mod, spec, NULL, JS_IMPORT_NS))
                        return cerr(cx, n->pos, "out of memory");
                } else {
                    b.kind = JS_MB_IMPORT;
                    JsString *imported;
                    if (sp->flags & JS_F_SPEC_DEFAULT) {
                        static const uint16_t d[] = {'d','e','f','a','u','l','t'};
                        imported = intern_atom(cx, d, 7);
                    } else {
                        imported = intern_atom(cx, sp->units, sp->len);
                    }
                    if (!imported ||
                        !mod_add_import(cx, mod, spec, imported,
                                        (sp->flags & JS_F_SPEC_DEFAULT) ? JS_IMPORT_DEFAULT
                                                                        : JS_IMPORT_NAMED))
                        return cerr(cx, n->pos, "out of memory");
                }
                b.import_index = (uint16_t)(mod->import_count - 1);
                if (!mbind_add(cx, &cap, b))
                    return cerr(cx, n->pos, "out of memory");
            }
            break;
        }
        case JS_AST_EXPORT_ALL: {
            JsString *spec = intern_atom(cx, n->units, n->len);
            if (!spec || !mod_add_dep(cx, mod, spec))
                return cerr(cx, n->pos, "out of memory");
            if (n->len2) { /* export * as ns from 'm' */
                JsString *ns = intern_atom(cx, n->units2, n->len2);
                if (!ns || !mod_add_star(cx, mod, spec, JS_REEXP_NS, NULL, ns))
                    return cerr(cx, n->pos, "out of memory");
            } else { /* export * from 'm' */
                if (!mod_add_star(cx, mod, spec, JS_REEXP_ALL, NULL, NULL))
                    return cerr(cx, n->pos, "out of memory");
            }
            break;
        }
        case JS_AST_EXPORT_NAMED:
            if (n->a) {
                const JsAstNode *d = n->a;
                if (d->kind == JS_AST_LET_DECL) {
                    bool is_const = (d->flags & JS_F_CONST) != 0;
                    for (uint32_t k = 0; k < d->count; k++)
                        if (!export_pattern_names(cx, &cap, d->items[k]->a, is_const))
                            return false;
                } else if (d->kind == JS_AST_FUNC_DECL) {
                    if (!mbind_add_export(cx, &cap, d->units, d->len, NULL, 0, false))
                        return false;
                }
            } else if (n->len) { /* export { x as y } from 'm' (re-export) */
                JsString *spec = intern_atom(cx, n->units, n->len);
                if (!spec || !mod_add_dep(cx, mod, spec))
                    return cerr(cx, n->pos, "out of memory");
                for (uint32_t s = 0; s < n->count; s++) {
                    const JsAstNode *sp = n->items[s];
                    JsString *imp = intern_atom(cx, sp->units, sp->len);
                    JsString *exp = intern_atom(cx, sp->units2, sp->len2);
                    if (!imp || !exp ||
                        !mod_add_star(cx, mod, spec, JS_REEXP_NAMED, imp, exp))
                        return cerr(cx, n->pos, "out of memory");
                }
            } else { /* export { a, b as c } (local) */
                for (uint32_t s = 0; s < n->count; s++) {
                    const JsAstNode *sp = n->items[s];
                    if (!mbind_add_export(cx, &cap, sp->units, sp->len, sp->units2, sp->len2,
                                          false))
                        return false;
                }
            }
            break;
        default:
            break;
        }
    }
    return true;
}

JsFunctionCell *js_compile_module_body(JsContext *ctx, const JsAstNode *module,
                                       JsModule *mod, JsCompileError *err) {
    JsVm *vm = ctx->vm;
    JsCompiler cx;
    memset(&cx, 0, sizeof cx);
    cx.ctx = ctx;
    cx.vm = vm;
    cx.module = mod;
    js_arena_init(&cx.arena, vm);

    JsGcCell *cell = js_gc_new_cell(vm, JS_KIND_FUNCTION, sizeof(JsFunctionCell));
    if (!cell) {
        err->msg = "out of memory";
        err->pos = 0;
        return NULL;
    }
    JsFunctionCell *fn = (JsFunctionCell *)cell;
    memset((char *)fn + sizeof(JsGcCell), 0, sizeof *fn - sizeof(JsGcCell));
    fn->module = mod;

    JsFuncState fs;
    memset(&fs, 0, sizeof fs);
    fs.fn = fn;
    fs.fn_value = js_value_from_cell(cell);
    fs.is_module = true;
    fs.scratch_slot = -1;
    fs.slot_count = 1;
    fs.slot_max = 1;
    cx.cur = &fs;
    if (!js_gc_protect(vm, &fs.fn_value)) {
        err->msg = "out of memory";
        err->pos = 0;
        return NULL;
    }

    bool ok = analyze_module(&cx, module, mod);
    if (ok)
        ok = compile_block_stmts(&cx, module->items, module->count);
    if (ok)
        ok = emit_op_u16(&cx, JS_OP_GET_LOCAL, JS_COMPLETION_SLOT, +1) &&
             emit_op(&cx, JS_OP_RETURN, -1);

    fn->n_locals = fs.slot_max;
    fn->max_stack = (uint32_t)(fs.max_depth < 0 ? 0 : fs.max_depth) + 8;
    fn->n_params = 0;
    if (module->flags & JS_F_HAS_TLA)
        fn->fn_flags |= JS_FN_ASYNC;

    js_gc_unprotect(vm, &fs.fn_value);
    js_arena_free(&cx.arena);
    if (!ok) {
        err->msg = cx.err_msg ? cx.err_msg : "compile error";
        err->pos = cx.err_pos;
        return NULL;
    }
    return fn;
}
