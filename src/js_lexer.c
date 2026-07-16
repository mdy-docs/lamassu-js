#include "js_bytecode.h" /* js_make_double */
#include "js_syntax.h"

/*
 * UTF-16 lexer. Always strict / module goal: legacy octal literals and
 * escapes are errors, `await`/`let`/`yield` are always reserved. The
 * parser drives context-dependent rescans (regex vs division, template
 * continuations after `}`).
 */

static uint16_t cur(const JsLexer *lx) {
    return lx->pos < lx->len ? lx->src[lx->pos] : 0;
}

static uint16_t at(const JsLexer *lx, uint32_t i) {
    return lx->pos + i < lx->len ? lx->src[lx->pos + i] : 0;
}

static bool lex_fail(JsLexer *lx, uint32_t pos, const char *msg) {
    if (!lx->failed) {
        lx->failed = true;
        lx->err_msg = msg;
        lx->err_pos = pos;
    }
    return false;
}

static bool is_line_term(uint16_t u) {
    return u == '\n' || u == '\r' || u == 0x2028 || u == 0x2029;
}

static bool is_space(uint16_t u) {
    return u == ' ' || u == '\t' || u == 0x0B || u == 0x0C || u == 0xA0 ||
           u == 0xFEFF;
}

static bool is_digit(uint16_t u) { return u >= '0' && u <= '9'; }

/* Permissive identifier policy: any unit >= 0xA0 is accepted. */
static bool is_id_start(uint16_t u) {
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '$' ||
           u == '_' || u >= 0xA0;
}

static bool is_id_part(uint16_t u) { return is_id_start(u) || is_digit(u); }

static int hex_val(uint16_t u) {
    if (u >= '0' && u <= '9') return u - '0';
    if (u >= 'a' && u <= 'f') return u - 'a' + 10;
    if (u >= 'A' && u <= 'F') return u - 'A' + 10;
    return -1;
}

void js_lexer_init(JsLexer *lx, JsArena *arena, const uint16_t *src, size_t len) {
    lx->arena = arena;
    lx->src = src;
    lx->len = (uint32_t)len;
    lx->pos = 0;
    lx->failed = false;
    lx->err_msg = NULL;
    lx->err_pos = 0;
    lx->tok.kind = JS_T_EOF;
    lx->tok.pos = lx->tok.end = 0;
    lx->tok.newline_before = false;
}

void js_source_line_col(const uint16_t *src, size_t len, uint32_t pos,
                        uint32_t *line, uint32_t *col) {
    uint32_t ln = 1, c = 1;
    if (pos > len)
        pos = (uint32_t)len;
    for (uint32_t i = 0; i < pos; i++) {
        if (is_line_term(src[i])) {
            if (src[i] == '\r' && i + 1 < pos && src[i + 1] == '\n')
                i++;
            ln++;
            c = 1;
        } else {
            c++;
        }
    }
    *line = ln;
    *col = c;
}

/* Skips whitespace and comments; reports whether a line terminator passed. */
static bool skip_space(JsLexer *lx, bool *newline) {
    for (;;) {
        uint16_t u = cur(lx);
        if (is_space(u)) {
            lx->pos++;
        } else if (is_line_term(u)) {
            *newline = true;
            lx->pos++;
        } else if (u == '/' && at(lx, 1) == '/') {
            lx->pos += 2;
            while (lx->pos < lx->len && !is_line_term(cur(lx)))
                lx->pos++;
        } else if (u == '/' && at(lx, 1) == '*') {
            uint32_t start = lx->pos;
            lx->pos += 2;
            for (;;) {
                if (lx->pos >= lx->len)
                    return lex_fail(lx, start, "unterminated comment");
                if (cur(lx) == '*' && at(lx, 1) == '/') {
                    lx->pos += 2;
                    break;
                }
                if (is_line_term(cur(lx)))
                    *newline = true;
                lx->pos++;
            }
        } else {
            return true;
        }
    }
}

/* ---- keywords ---- */

typedef struct { const char *name; JsTokKind kind; } JsKeyword;

static const JsKeyword js_keywords[] = {
    {"await", JS_T_AWAIT},     {"break", JS_T_BREAK},   {"case", JS_T_CASE},
    {"catch", JS_T_CATCH},     {"const", JS_T_CONST},   {"continue", JS_T_CONTINUE},
    {"default", JS_T_DEFAULT}, {"delete", JS_T_DELETE}, {"do", JS_T_DO},
    {"else", JS_T_ELSE},       {"export", JS_T_EXPORT}, {"finally", JS_T_FINALLY},
    {"for", JS_T_FOR},         {"function", JS_T_FUNCTION}, {"if", JS_T_IF},
    {"import", JS_T_IMPORT},   {"in", JS_T_IN},         {"instanceof", JS_T_INSTANCEOF},
    {"let", JS_T_LET},         {"new", JS_T_NEW},       {"null", JS_T_NULL},
    {"return", JS_T_RETURN},   {"switch", JS_T_SWITCH}, {"this", JS_T_THIS},
    {"throw", JS_T_THROW},     {"true", JS_T_TRUE},     {"false", JS_T_FALSE},
    {"try", JS_T_TRY},         {"typeof", JS_T_TYPEOF}, {"void", JS_T_VOID},
    {"while", JS_T_WHILE},
};

/* Reserved (strict mode) or deliberately unsupported words. */
static const char *const js_reserved[] = {
    "var", "class", "extends", "super", "with", "enum", "yield", "static",
    "implements", "interface", "package", "private", "protected", "public",
    "debugger",
};

static bool units_eq_ascii(const uint16_t *u, uint32_t len, const char *s) {
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] == '\0' || u[i] != (uint16_t)(unsigned char)s[i])
            return false;
    }
    return s[len] == '\0';
}

static void lex_ident(JsLexer *lx) {
    uint32_t start = lx->pos;
    while (lx->pos < lx->len && is_id_part(cur(lx)))
        lx->pos++;
    const uint16_t *units = lx->src + start;
    uint32_t len = lx->pos - start;
    lx->tok.kind = JS_T_IDENT;
    lx->tok.u.str.units = units;
    lx->tok.u.str.len = len;
    lx->tok.u.str.cont = false;
    for (size_t i = 0; i < sizeof js_keywords / sizeof js_keywords[0]; i++) {
        if (units_eq_ascii(units, len, js_keywords[i].name)) {
            lx->tok.kind = js_keywords[i].kind;
            return;
        }
    }
    for (size_t i = 0; i < sizeof js_reserved / sizeof js_reserved[0]; i++) {
        if (units_eq_ascii(units, len, js_reserved[i])) {
            lx->tok.kind = JS_T_RESERVED;
            return;
        }
    }
}

/* ---- numbers (double conversion shared via js_number.c) ---- */

static bool lex_number(JsLexer *lx) {
    uint32_t start = lx->pos;
    double value = 0.0;

    if (cur(lx) == '0' && (at(lx, 1) == 'x' || at(lx, 1) == 'X' ||
                           at(lx, 1) == 'o' || at(lx, 1) == 'O' ||
                           at(lx, 1) == 'b' || at(lx, 1) == 'B')) {
        uint16_t marker = at(lx, 1);
        int radix = marker == 'x' || marker == 'X' ? 16
                    : marker == 'o' || marker == 'O' ? 8 : 2;
        lx->pos += 2;
        bool any = false;
        for (;;) {
            int d = hex_val(cur(lx));
            if (d < 0 || d >= radix)
                break;
            value = value * radix + d;
            lx->pos++;
            any = true;
        }
        if (!any)
            return lex_fail(lx, start, "invalid number literal");
    } else {
        uint64_t mant = 0;
        int digits = 0, exp_adjust = 0, exp_part = 0;
        if (cur(lx) == '0' && is_digit(at(lx, 1)))
            return lex_fail(lx, start, "legacy octal literals are not allowed");
        while (is_digit(cur(lx))) {
            if (digits < 19) {
                mant = mant * 10 + (uint64_t)(cur(lx) - '0');
                if (mant)
                    digits++;
            } else {
                exp_adjust++;
            }
            lx->pos++;
        }
        if (cur(lx) == '.') {
            lx->pos++;
            while (is_digit(cur(lx))) {
                if (digits < 19) {
                    mant = mant * 10 + (uint64_t)(cur(lx) - '0');
                    if (mant)
                        digits++;
                    exp_adjust--;
                }
                lx->pos++;
            }
        }
        if (cur(lx) == 'e' || cur(lx) == 'E') {
            lx->pos++;
            bool neg = false;
            if (cur(lx) == '+' || cur(lx) == '-') {
                neg = cur(lx) == '-';
                lx->pos++;
            }
            if (!is_digit(cur(lx)))
                return lex_fail(lx, lx->pos, "missing exponent digits");
            while (is_digit(cur(lx))) {
                if (exp_part < 100000)
                    exp_part = exp_part * 10 + (cur(lx) - '0');
                lx->pos++;
            }
            if (neg)
                exp_part = -exp_part;
        }
        value = js_make_double(mant, exp_part + exp_adjust);
    }

    if (cur(lx) == '_')
        return lex_fail(lx, lx->pos, "numeric separators are not supported");
    if (cur(lx) == 'n')
        return lex_fail(lx, lx->pos, "BigInt literals are not supported");
    if (is_id_start(cur(lx)) || is_digit(cur(lx)))
        return lex_fail(lx, lx->pos, "unexpected character after number");

    lx->tok.kind = JS_T_NUM;
    lx->tok.u.number = value;
    return true;
}

/* ---- strings / templates ---- */

/*
 * Cooks one escape sequence at lx->pos (which points at the backslash)
 * into out/outlen. Returns false on error.
 */
static bool cook_escape(JsLexer *lx, uint16_t *out, uint32_t *outlen) {
    uint32_t bs = lx->pos;
    lx->pos++; /* backslash */
    uint16_t e = cur(lx);
    *outlen = 1;
    if (is_line_term(e)) { /* line continuation */
        lx->pos++;
        if (e == '\r' && cur(lx) == '\n')
            lx->pos++;
        *outlen = 0;
        return true;
    }
    lx->pos++;
    switch (e) {
    case 'n': out[0] = '\n'; return true;
    case 't': out[0] = '\t'; return true;
    case 'r': out[0] = '\r'; return true;
    case 'b': out[0] = '\b'; return true;
    case 'f': out[0] = '\f'; return true;
    case 'v': out[0] = '\v'; return true;
    case '0':
        if (is_digit(cur(lx)))
            return lex_fail(lx, bs, "octal escapes are not allowed");
        out[0] = 0;
        return true;
    case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        return lex_fail(lx, bs, "octal escapes are not allowed");
    case 'x': {
        int h = hex_val(cur(lx)), l = hex_val(at(lx, 1));
        if (h < 0 || l < 0)
            return lex_fail(lx, bs, "invalid \\x escape");
        out[0] = (uint16_t)(h * 16 + l);
        lx->pos += 2;
        return true;
    }
    case 'u': {
        if (cur(lx) == '{') {
            lx->pos++;
            uint32_t cp = 0;
            bool any = false;
            while (hex_val(cur(lx)) >= 0) {
                cp = cp * 16 + (uint32_t)hex_val(cur(lx));
                if (cp > 0x10FFFF)
                    return lex_fail(lx, bs, "code point out of range");
                lx->pos++;
                any = true;
            }
            if (!any || cur(lx) != '}')
                return lex_fail(lx, bs, "invalid \\u{...} escape");
            lx->pos++;
            if (cp > 0xFFFF) {
                cp -= 0x10000;
                out[0] = (uint16_t)(0xD800 + (cp >> 10));
                out[1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
                *outlen = 2;
            } else {
                out[0] = (uint16_t)cp;
            }
            return true;
        }
        uint32_t cp = 0;
        for (int i = 0; i < 4; i++) {
            int h = hex_val(cur(lx));
            if (h < 0)
                return lex_fail(lx, bs, "invalid \\u escape");
            cp = cp * 16 + (uint32_t)h;
            lx->pos++;
        }
        out[0] = (uint16_t)cp;
        return true;
    }
    default:
        out[0] = e; /* identity escape */
        return true;
    }
}

static bool lex_string(JsLexer *lx) {
    uint32_t start = lx->pos;
    uint16_t quote = cur(lx);
    lx->pos++;

    /* First pass: find the end, so the cook buffer can be sized. */
    uint32_t scan = lx->pos;
    for (;;) {
        if (scan >= lx->len)
            return lex_fail(lx, start, "unterminated string");
        uint16_t u = lx->src[scan];
        if (u == quote)
            break;
        if (u == '\\') {
            scan++;
            if (scan < lx->len && lx->src[scan] == '\r' &&
                scan + 1 < lx->len && lx->src[scan + 1] == '\n')
                scan++;
            if (scan >= lx->len)
                return lex_fail(lx, start, "unterminated string");
            scan++;
        } else if (is_line_term(u)) {
            return lex_fail(lx, start, "unterminated string");
        } else {
            scan++;
        }
    }

    uint32_t raw_len = scan - lx->pos;
    uint16_t *buf = js_arena_alloc(lx->arena, (size_t)(raw_len + 2) * sizeof(uint16_t));
    if (!buf)
        return lex_fail(lx, start, "out of memory");
    uint32_t n = 0;
    while (cur(lx) != quote) {
        if (cur(lx) == '\\') {
            uint32_t got;
            if (!cook_escape(lx, buf + n, &got))
                return false;
            n += got;
        } else {
            buf[n++] = cur(lx);
            lx->pos++;
        }
    }
    lx->pos++; /* closing quote */
    lx->tok.kind = JS_T_STR;
    lx->tok.u.str.units = buf;
    lx->tok.u.str.len = n;
    lx->tok.u.str.cont = false;
    return true;
}

/*
 * Cooks one template chunk starting at lx->pos (just after ` or }).
 * Ends at an unescaped ` (cont=false) or ${ (cont=true).
 */
static bool lex_template_chunk(JsLexer *lx, uint32_t err_from) {
    /* First pass: find the terminator. */
    uint32_t scan = lx->pos;
    for (;;) {
        if (scan >= lx->len)
            return lex_fail(lx, err_from, "unterminated template literal");
        uint16_t u = lx->src[scan];
        if (u == '`')
            break;
        if (u == '$' && scan + 1 < lx->len && lx->src[scan + 1] == '{')
            break;
        if (u == '\\') {
            scan += 2;
            if (scan > lx->len)
                return lex_fail(lx, err_from, "unterminated template literal");
        } else {
            scan++;
        }
    }

    uint32_t raw_len = scan - lx->pos;
    uint16_t *buf = js_arena_alloc(lx->arena, (size_t)(raw_len + 2) * sizeof(uint16_t));
    if (!buf)
        return lex_fail(lx, err_from, "out of memory");
    uint32_t n = 0;
    for (;;) {
        uint16_t u = cur(lx);
        if (u == '`') {
            lx->pos++;
            lx->tok.u.str.cont = false;
            break;
        }
        if (u == '$' && at(lx, 1) == '{') {
            lx->pos += 2;
            lx->tok.u.str.cont = true;
            break;
        }
        if (u == '\\') {
            uint32_t got;
            if (!cook_escape(lx, buf + n, &got))
                return false;
            n += got;
        } else if (u == '\r') { /* normalize CR / CRLF to LF */
            buf[n++] = '\n';
            lx->pos++;
            if (cur(lx) == '\n')
                lx->pos++;
        } else {
            buf[n++] = u;
            lx->pos++;
        }
    }
    lx->tok.kind = JS_T_TEMPLATE;
    lx->tok.u.str.units = buf;
    lx->tok.u.str.len = n;
    return true;
}

bool js_lexer_rescan_template(JsLexer *lx) {
    /* Current token must be the '}' closing a ${...} substitution. */
    lx->tok.pos = lx->tok.end; /* chunk starts right after '}' */
    lx->pos = lx->tok.end;
    if (!lex_template_chunk(lx, lx->tok.pos))
        return false;
    lx->tok.end = lx->pos;
    lx->tok.newline_before = false;
    return true;
}

bool js_lexer_rescan_regex(JsLexer *lx) {
    /* Current token is SLASH or SLASH_EQ; rescan from its start. */
    uint32_t start = lx->tok.pos;
    uint32_t p = start + 1;
    bool in_class = false;
    for (;;) {
        if (p >= lx->len || is_line_term(lx->src[p]))
            return lex_fail(lx, start, "unterminated regex literal");
        uint16_t u = lx->src[p];
        if (u == '\\') {
            p++;
            if (p >= lx->len || is_line_term(lx->src[p]))
                return lex_fail(lx, start, "unterminated regex literal");
            p++;
        } else if (u == '[') {
            in_class = true;
            p++;
        } else if (u == ']') {
            in_class = false;
            p++;
        } else if (u == '/' && !in_class) {
            break;
        } else {
            p++;
        }
    }
    lx->tok.kind = JS_T_REGEX;
    lx->tok.u.regex.pattern = lx->src + start + 1;
    lx->tok.u.regex.pattern_len = p - start - 1;
    p++; /* closing slash */
    uint32_t fstart = p;
    while (p < lx->len && is_id_part(lx->src[p]))
        p++;
    lx->tok.u.regex.flags = lx->src + fstart;
    lx->tok.u.regex.flags_len = p - fstart;
    lx->pos = p;
    lx->tok.end = p;
    return true;
}

/* ---- punctuators + main dispatch ---- */

static bool lex_punct(JsLexer *lx) {
    uint16_t a = cur(lx), b = at(lx, 1), c = at(lx, 2), d = at(lx, 3);
    JsTokKind k;
    uint32_t n = 1;
    switch (a) {
    case '{': k = JS_T_LBRACE; break;
    case '}': k = JS_T_RBRACE; break;
    case '(': k = JS_T_LPAREN; break;
    case ')': k = JS_T_RPAREN; break;
    case '[': k = JS_T_LBRACK; break;
    case ']': k = JS_T_RBRACK; break;
    case ';': k = JS_T_SEMI; break;
    case ',': k = JS_T_COMMA; break;
    case ':': k = JS_T_COLON; break;
    case '~': k = JS_T_TILDE; break;
    case '.':
        if (b == '.' && c == '.') { k = JS_T_DOTDOTDOT; n = 3; }
        else k = JS_T_DOT;
        break;
    case '?':
        if (b == '?' && c == '=') { k = JS_T_QUESQUES_EQ; n = 3; }
        else if (b == '?') { k = JS_T_QUESQUES; n = 2; }
        else if (b == '.' && !is_digit(c)) { k = JS_T_QUESDOT; n = 2; }
        else k = JS_T_QUES;
        break;
    case '=':
        if (b == '=' && c == '=') { k = JS_T_EQEQEQ; n = 3; }
        else if (b == '=') { k = JS_T_EQEQ; n = 2; }
        else if (b == '>') { k = JS_T_ARROW; n = 2; }
        else k = JS_T_EQ;
        break;
    case '!':
        if (b == '=' && c == '=') { k = JS_T_STRICT_NE; n = 3; }
        else if (b == '=') { k = JS_T_NE; n = 2; }
        else k = JS_T_NOT;
        break;
    case '+':
        if (b == '+') { k = JS_T_INC; n = 2; }
        else if (b == '=') { k = JS_T_PLUS_EQ; n = 2; }
        else k = JS_T_PLUS;
        break;
    case '-':
        if (b == '-') { k = JS_T_DEC; n = 2; }
        else if (b == '=') { k = JS_T_MINUS_EQ; n = 2; }
        else k = JS_T_MINUS;
        break;
    case '*':
        if (b == '*' && c == '=') { k = JS_T_POW_EQ; n = 3; }
        else if (b == '*') { k = JS_T_POW; n = 2; }
        else if (b == '=') { k = JS_T_STAR_EQ; n = 2; }
        else k = JS_T_STAR;
        break;
    case '/':
        if (b == '=') { k = JS_T_SLASH_EQ; n = 2; }
        else k = JS_T_SLASH;
        break;
    case '%':
        if (b == '=') { k = JS_T_PERCENT_EQ; n = 2; }
        else k = JS_T_PERCENT;
        break;
    case '<':
        if (b == '<' && c == '=') { k = JS_T_SHL_EQ; n = 3; }
        else if (b == '<') { k = JS_T_SHL; n = 2; }
        else if (b == '=') { k = JS_T_LE; n = 2; }
        else k = JS_T_LT;
        break;
    case '>':
        if (b == '>' && c == '>' && d == '=') { k = JS_T_USHR_EQ; n = 4; }
        else if (b == '>' && c == '>') { k = JS_T_USHR; n = 3; }
        else if (b == '>' && c == '=') { k = JS_T_SHR_EQ; n = 3; }
        else if (b == '>') { k = JS_T_SHR; n = 2; }
        else if (b == '=') { k = JS_T_GE; n = 2; }
        else k = JS_T_GT;
        break;
    case '&':
        if (b == '&' && c == '=') { k = JS_T_ANDAND_EQ; n = 3; }
        else if (b == '&') { k = JS_T_ANDAND; n = 2; }
        else if (b == '=') { k = JS_T_AMP_EQ; n = 2; }
        else k = JS_T_AMP;
        break;
    case '|':
        if (b == '|' && c == '=') { k = JS_T_OROR_EQ; n = 3; }
        else if (b == '|') { k = JS_T_OROR; n = 2; }
        else if (b == '=') { k = JS_T_BAR_EQ; n = 2; }
        else k = JS_T_BAR;
        break;
    case '^':
        if (b == '=') { k = JS_T_CARET_EQ; n = 2; }
        else k = JS_T_CARET;
        break;
    default:
        return lex_fail(lx, lx->pos, "unexpected character");
    }
    lx->tok.kind = k;
    lx->pos += n;
    return true;
}

bool js_lexer_next(JsLexer *lx) {
    if (lx->failed)
        return false;
    bool nl = false;
    if (!skip_space(lx, &nl))
        return false;
    lx->tok.newline_before = nl;
    lx->tok.pos = lx->pos;

    bool ok;
    uint16_t u = cur(lx);
    if (lx->pos >= lx->len) {
        lx->tok.kind = JS_T_EOF;
        ok = true;
    } else if (is_id_start(u)) {
        lex_ident(lx);
        ok = true;
    } else if (is_digit(u) || (u == '.' && is_digit(at(lx, 1)))) {
        ok = lex_number(lx);
    } else if (u == '"' || u == '\'') {
        ok = lex_string(lx);
    } else if (u == '`') {
        lx->pos++;
        ok = lex_template_chunk(lx, lx->tok.pos);
    } else if (u == '\\') {
        ok = lex_fail(lx, lx->pos, "unicode escapes in identifiers are not supported");
    } else {
        ok = lex_punct(lx);
    }
    lx->tok.end = lx->pos;
    return ok;
}
