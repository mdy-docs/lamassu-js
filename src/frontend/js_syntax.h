/*
 * Syntax front end: arena, lexer, AST, parser.
 *
 * The AST is transient — arena-allocated, consumed by the compiler, then
 * freed all at once. Nodes never reference GC cells (identifier/string
 * payloads point into the source or the arena), so parsing never interacts
 * with the collector. Diagnostic messages are static ASCII templates (the
 * one carve-out from the no-C-strings rule); they are widened to UTF-16
 * only when surfaced as JS values.
 */
#ifndef JS_SYNTAX_H
#define JS_SYNTAX_H

#include "lamassu_internal.h"

/* ---- arena ---- */

typedef struct JsArenaChunk JsArenaChunk;

typedef struct JsArena {
    JsVm *vm;
    JsArenaChunk *chunks;
} JsArena;

void  js_arena_init(JsArena *a, JsVm *vm);
void *js_arena_alloc(JsArena *a, size_t size); /* 8-aligned; NULL on OOM */
void  js_arena_free(JsArena *a);

/* ---- tokens ---- */

typedef enum JsTokKind {
    JS_T_EOF = 0,
    JS_T_IDENT,
    JS_T_RESERVED, /* strict-reserved / unsupported keyword; units carried */
    JS_T_NUM,
    JS_T_STR,
    JS_T_TEMPLATE, /* one cooked chunk; u.str.cont means "${ follows" */
    JS_T_REGEX,

    JS_T_AWAIT, JS_T_BREAK, JS_T_CASE, JS_T_CATCH, JS_T_CONST,
    JS_T_CONTINUE, JS_T_DEFAULT, JS_T_DELETE, JS_T_DO, JS_T_ELSE,
    JS_T_EXPORT, JS_T_FINALLY, JS_T_FOR, JS_T_FUNCTION, JS_T_IF,
    JS_T_IMPORT, JS_T_IN, JS_T_INSTANCEOF, JS_T_LET, JS_T_NEW,
    JS_T_NULL, JS_T_RETURN, JS_T_SWITCH, JS_T_THIS, JS_T_THROW,
    JS_T_TRUE, JS_T_FALSE, JS_T_TRY, JS_T_TYPEOF, JS_T_VOID, JS_T_WHILE,

    JS_T_LBRACE, JS_T_RBRACE, JS_T_LPAREN, JS_T_RPAREN,
    JS_T_LBRACK, JS_T_RBRACK, JS_T_SEMI, JS_T_COMMA,
    JS_T_DOT, JS_T_DOTDOTDOT, JS_T_ARROW,
    JS_T_QUES, JS_T_QUESDOT, JS_T_QUESQUES, JS_T_QUESQUES_EQ,
    JS_T_COLON,
    JS_T_EQ, JS_T_PLUS, JS_T_MINUS, JS_T_STAR, JS_T_SLASH, JS_T_PERCENT,
    JS_T_POW,
    JS_T_PLUS_EQ, JS_T_MINUS_EQ, JS_T_STAR_EQ, JS_T_SLASH_EQ,
    JS_T_PERCENT_EQ, JS_T_POW_EQ,
    JS_T_LT, JS_T_GT, JS_T_LE, JS_T_GE,
    JS_T_EQEQ, JS_T_NE, JS_T_EQEQEQ, JS_T_STRICT_NE,
    JS_T_SHL, JS_T_SHR, JS_T_USHR,
    JS_T_SHL_EQ, JS_T_SHR_EQ, JS_T_USHR_EQ,
    JS_T_AMP, JS_T_BAR, JS_T_CARET,
    JS_T_AMP_EQ, JS_T_BAR_EQ, JS_T_CARET_EQ,
    JS_T_ANDAND, JS_T_OROR, JS_T_ANDAND_EQ, JS_T_OROR_EQ,
    JS_T_NOT, JS_T_TILDE, JS_T_INC, JS_T_DEC,
} JsTokKind;

typedef struct JsToken {
    JsTokKind kind;
    uint32_t pos, end; /* offsets in source units */
    bool newline_before;
    union {
        double number;
        struct {
            const uint16_t *units; /* ident/reserved/keyword name, string, template chunk */
            uint32_t len;
            bool cont; /* JS_T_TEMPLATE: chunk ends with "${" */
        } str;
        struct {
            const uint16_t *pattern;
            uint32_t pattern_len;
            const uint16_t *flags;
            uint32_t flags_len;
        } regex;
    } u;
} JsToken;

typedef struct JsLexer {
    JsArena *arena;
    const uint16_t *src;
    uint32_t len, pos;
    JsToken tok;
    bool failed;
    const char *err_msg;
    uint32_t err_pos;
} JsLexer;

void js_lexer_init(JsLexer *lx, JsArena *arena, const uint16_t *src, size_t len);
bool js_lexer_next(JsLexer *lx);
bool js_lexer_rescan_regex(JsLexer *lx);    /* tok must be SLASH or SLASH_EQ */
bool js_lexer_rescan_template(JsLexer *lx); /* tok must be RBRACE */

void js_source_line_col(const uint16_t *src, size_t len, uint32_t pos,
                        uint32_t *line, uint32_t *col); /* 1-based */

/* ---- AST ---- */

typedef enum JsAstKind {
    /* statements / module items */
    JS_AST_MODULE = 1, JS_AST_BLOCK, JS_AST_EMPTY, JS_AST_EXPR_STMT,
    JS_AST_LET_DECL, JS_AST_DECLARATOR, JS_AST_IF, JS_AST_FOR, JS_AST_FOR_OF,
    JS_AST_WHILE, JS_AST_DO_WHILE, JS_AST_SWITCH, JS_AST_SWITCH_CASE,
    JS_AST_BREAK, JS_AST_CONTINUE, JS_AST_RETURN, JS_AST_THROW, JS_AST_TRY,
    JS_AST_LABELED, JS_AST_FUNC_DECL,
    JS_AST_IMPORT, JS_AST_IMPORT_SPEC,
    JS_AST_EXPORT_NAMED, JS_AST_EXPORT_DEFAULT, JS_AST_EXPORT_ALL,
    JS_AST_EXPORT_SPEC,
    /* expressions */
    JS_AST_IDENT, JS_AST_NUMBER, JS_AST_STRING, JS_AST_TEMPLATE, JS_AST_REGEX,
    JS_AST_NULL_LIT, JS_AST_BOOL_LIT, JS_AST_THIS,
    JS_AST_ARRAY, JS_AST_OBJECT, JS_AST_PROPERTY,
    JS_AST_FUNC_EXPR, JS_AST_CALL, JS_AST_NEW, JS_AST_MEMBER,
    JS_AST_UNARY, JS_AST_UPDATE, JS_AST_BINARY, JS_AST_LOGICAL, JS_AST_COND,
    JS_AST_ASSIGN, JS_AST_SEQUENCE, JS_AST_SPREAD, JS_AST_AWAIT,
    JS_AST_IMPORT_CALL, /* dynamic import(): a = specifier, b = options or NULL */
    JS_AST_HOLE,
    JS_AST_COVER, /* transient: parenthesized list before arrow/expr decision */
    /* patterns */
    JS_AST_ARRAY_PATTERN, JS_AST_OBJECT_PATTERN, JS_AST_REST, JS_AST_DEFAULT,
} JsAstKind;

/* flags — meaning depends on kind */
#define JS_F_CONST       0x01 /* LET_DECL; FOR_OF with JS_F_DECL */
#define JS_F_DECL        0x02 /* FOR_OF: binding is a declaration */
#define JS_F_ASYNC       0x01 /* FUNC_* */
#define JS_F_ARROW       0x02 /* FUNC_EXPR */
#define JS_F_EXPR_BODY   0x04 /* FUNC_EXPR: arrow with expression body */
#define JS_F_COMPUTED    0x01 /* MEMBER, PROPERTY */
#define JS_F_OPTIONAL    0x02 /* MEMBER, CALL: ?. */
#define JS_F_SHORTHAND   0x04 /* PROPERTY */
#define JS_F_METHOD      0x08 /* PROPERTY */
#define JS_F_COVER_INIT  0x10 /* PROPERTY: shorthand with default ({a = 1}) */
#define JS_F_PROP_ASYNC  0x20 /* PROPERTY: async method */
#define JS_F_PREFIX      0x01 /* UPDATE */
#define JS_F_TRUE        0x01 /* BOOL_LIT */
#define JS_F_HAS_TLA     0x01 /* MODULE: uses top-level await */
#define JS_F_SPEC_DEFAULT 0x01 /* IMPORT_SPEC */
#define JS_F_SPEC_NS      0x02 /* IMPORT_SPEC */
#define JS_F_COVER_SPREAD 0x01 /* COVER */
#define JS_F_COVER_TRAIL  0x02 /* COVER: trailing comma */
#define JS_F_COVER_ASYNC  0x04 /* COVER: `async (...)` form */
#define JS_F_PAREN        0x80 /* any expression: was parenthesized */

/*
 * Deliberately fat node (one struct, no unions): the AST is transient and
 * arena-freed as a whole, so simplicity beats size. Field use by kind is
 * documented next to each kind's builder in js_parser.c.
 */
typedef struct JsAstNode JsAstNode;
struct JsAstNode {
    uint8_t kind;
    uint8_t flags;
    uint8_t op; /* JsTokKind operator for UNARY/UPDATE/BINARY/LOGICAL/ASSIGN */
    uint32_t pos;
    double number;
    const uint16_t *units;  uint32_t len;  /* name / string / label / source */
    const uint16_t *units2; uint32_t len2; /* second name (regex flags, spec local, export alias) */
    JsAstNode *a, *b, *c, *d;
    JsAstNode **items; uint32_t count;
};

/* ---- parser ---- */

typedef struct JsParseResult {
    JsAstNode *module;   /* NULL on error */
    const char *err_msg; /* static ASCII template */
    uint32_t err_pos;    /* offset in source units */
} JsParseResult;

bool js_parse_module(JsArena *arena, const uint16_t *src, size_t len,
                     JsParseResult *out);

#endif /* JS_SYNTAX_H */
