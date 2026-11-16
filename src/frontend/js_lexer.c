#include "js_bytecode.h" /* js_make_double */
#include "js_syntax.h"

/*
 * UTF-16 lexer. Always strict / module goal: legacy octal literals and
 * escapes are errors (noted on the token, for the parser to raise where V8
 * does), `await`/`let`/`yield` are always reserved. The
 * parser drives context-dependent rescans (regex vs division, template
 * continuations after `}`).
 */

static uint16_t cur(const JsLexer *lx) {
    return lx->pos < lx->len ? lx->src[lx->pos] : 0;
}

static uint16_t at(const JsLexer *lx, uint32_t i) {
    return lx->pos + i < lx->len ? lx->src[lx->pos + i] : 0;
}

static void lex_late(JsLexer *lx, uint32_t pos, uint32_t end, const char *msg) {
    lx->late_msg = msg; /* the last in a token is the one V8 keeps */
    lx->late_pos = pos;
    lx->late_end = end;
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
    lx->late_msg = NULL;
    lx->late_pos = lx->late_end = 0;
    lx->tok.kind = JS_T_EOF;
    lx->tok.pos = lx->tok.end = 0;
    lx->tok.newline_before = false;
}

/* js_source_line_col lives in the runtime now (public API, lamassu.h). */

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
                    return lex_fail(lx, start, "Invalid or unexpected token");
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

bool js_lexer_is_reserved_word(const uint16_t *units, uint32_t len) {
    for (size_t i = 0; i < sizeof js_keywords / sizeof js_keywords[0]; i++)
        if (units_eq_ascii(units, len, js_keywords[i].name))
            return true;
    for (size_t i = 0; i < sizeof js_reserved / sizeof js_reserved[0]; i++)
        if (units_eq_ascii(units, len, js_reserved[i]))
            return true;
    return false;
}

/* A \u escape at *pos in an identifier: its code point (and *pos past it); -1 when it is
 * malformed (*BAD: where V8 points, the backslash, or in `\u{...}` the character that is no hex
 * digit), -2 when it names no code point, -3 when the backslash starts no \u escape at all */
static int32_t ident_escape(const JsLexer *lx, uint32_t *pos, uint32_t *bad) {
    uint32_t p = *pos;
    *bad = p;
    if (p + 1 >= lx->len || lx->src[p] != '\\' || lx->src[p + 1] != 'u')
        return -3;
    p += 2;
    uint32_t cp = 0;
    if (p < lx->len && lx->src[p] == '{') {
        p++;
        bool any = false;
        while (p < lx->len && hex_val(lx->src[p]) >= 0) {
            cp = cp * 16 + (uint32_t)hex_val(lx->src[p++]);
            if (cp > 0x10FFFF)
                return -2;
            any = true;
        }
        if (!any || p >= lx->len || lx->src[p] != '}') {
            *bad = p;
            return -1;
        }
        p++;
    } else {
        for (int i = 0; i < 4; i++) {
            if (p >= lx->len || hex_val(lx->src[p]) < 0)
                return -1;
            cp = cp * 16 + (uint32_t)hex_val(lx->src[p++]);
        }
    }
    *pos = p;
    return (int32_t)cp;
}

/* An identifier with \u escapes in it, from START: decoded into the arena. False with V8's error
 * for an escape that is malformed or does not spell an identifier character. */
static bool lex_escaped_ident(JsLexer *lx, uint32_t start) {
    uint32_t n = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint16_t *out = NULL;
        if (pass == 1 && !(out = js_arena_alloc(lx->arena, (n ? n : 1) * sizeof *out)))
            return lex_fail(lx, start, "out of memory");
        uint32_t p = start, k = 0;
        bool first = true;
        while (p < lx->len) {
            uint16_t u = lx->src[p];
            int32_t cp;
            uint32_t at_pos = p, bad;
            if (u == '\\') {
                cp = ident_escape(lx, &p, &bad);
                if (cp == -3) /* V8 points this one at the whole identifier */
                    return lex_fail(lx, start, "Invalid or unexpected token");
                if (cp < 0)
                    return lex_fail(lx, cp == -2 ? at_pos : bad,
                                    cp == -2 ? "Undefined Unicode code-point" : "Invalid Unicode escape sequence");
                bool ok = !(cp >= 0xD800 && cp <= 0xDFFF) &&
                          (cp > 0xFFFF || (first ? is_id_start((uint16_t)cp) : is_id_part((uint16_t)cp)));
                if (!ok) /* well formed, but no identifier character */
                    return lex_fail(lx, start, "Invalid or unexpected token");
            } else if (first ? is_id_start(u) : is_id_part(u)) {
                cp = u;
                p++;
            } else {
                break;
            }
            first = false;
            if (cp > 0xFFFF) {
                if (out) {
                    out[k] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
                    out[k + 1] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
                }
                k += 2;
            } else {
                if (out)
                    out[k] = (uint16_t)cp;
                k++;
            }
        }
        if (pass == 0) {
            n = k;
        } else {
            lx->pos = p;
            /* an identifier even when it spells a keyword: the parser decides. `await` alone
             * stays the word it spells, as in V8: an escaped one is refused only where it would
             * be the operator, and is the reserved word everywhere else */
            lx->tok.kind = units_eq_ascii(out, n, "await") ? JS_T_AWAIT : JS_T_IDENT;
            lx->tok.escaped = true;
            lx->tok.u.str.units = out;
            lx->tok.u.str.len = n;
            lx->tok.u.str.cont = false;
        }
    }
    return true;
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

/* A numeric separator at lx->pos, inside a run of RADIX digits that has at least one digit: true
 * (and stepped over) when a digit follows, else false with V8's error. */
static bool lex_separator(JsLexer *lx, int radix) {
    uint16_t next = at(lx, 1);
    if (next == '_')
        return lex_fail(lx, lx->pos + 1, "Only one underscore is allowed as numeric separator");
    int d = hex_val(next);
    if (d < 0 || d >= radix)
        return lex_fail(lx, lx->pos, "Numeric separators are not allowed at the end of numeric literals");
    lx->pos++;
    return true;
}

static bool lex_number(JsLexer *lx) {
    uint32_t start = lx->pos;
    double value = 0.0;
    bool separated = false, leading = false;

    if (cur(lx) == '0' && (at(lx, 1) == 'x' || at(lx, 1) == 'X' ||
                           at(lx, 1) == 'o' || at(lx, 1) == 'O' ||
                           at(lx, 1) == 'b' || at(lx, 1) == 'B')) {
        uint16_t marker = at(lx, 1);
        int radix = marker == 'x' || marker == 'X' ? 16
                    : marker == 'o' || marker == 'O' ? 8 : 2;
        lx->pos += 2;
        bool any = false;
        for (;;) {
            if (cur(lx) == '_' && any) {
                if (!lex_separator(lx, radix))
                    return false;
                continue;
            }
            int d = hex_val(cur(lx));
            if (d < 0 || d >= radix)
                break;
            value = value * radix + d;
            lx->pos++;
            any = true;
        }
        if (!any)
            return lex_fail(lx, start, "Invalid or unexpected token");
    } else {
        uint64_t mant = 0;
        int digits = 0, exp_adjust = 0, exp_part = 0;
        bool dropped = false; /* digits past the 19th: the whole text decides the rounding */
        if (cur(lx) == '0' && is_digit(at(lx, 1))) {
            /* Not strict mode's, but V8 reads them as the numbers they once were and reports
             * them where the function ends, 08 and 09-style ones (decimal all the same) under
             * another name than the octal-looking ones */
            uint32_t i = 1;
            for (; is_digit(at(lx, i)); i++)
                if (at(lx, i) >= '8')
                    leading = true;
            if (at(lx, i) == '_' || at(lx, i) == 'n') /* V8's places for them */
                return lex_fail(lx, leading && at(lx, i) == '_' ? start + i : start,
                                "Invalid or unexpected token");
            if (!leading) {
                for (uint32_t k = 1; k < i; k++)
                    value = value * 8 + (at(lx, k) - '0');
                lx->pos = start + i;
                if (is_id_start(cur(lx)) || is_digit(cur(lx)))
                    return lex_fail(lx, start, "Invalid or unexpected token");
                lex_late(lx, start, lx->pos, "Octal literals are not allowed in strict mode.");
                lx->tok.kind = JS_T_NUM;
                lx->tok.u.number = value;
                return true;
            }
        }
        if (cur(lx) == '0' && at(lx, 1) == '_')
            return lex_fail(lx, lx->pos + 1, "Numeric separator can not be used after leading 0.");
        while (is_digit(cur(lx)) || (cur(lx) == '_' && lx->pos > start)) {
            if (cur(lx) == '_') {
                if (!lex_separator(lx, 10))
                    return false;
                separated = true;
                continue;
            }
            if (digits < 19) {
                mant = mant * 10 + (uint64_t)(cur(lx) - '0');
                if (mant)
                    digits++;
            } else {
                exp_adjust++;
                dropped = true;
            }
            lx->pos++;
        }
        if (cur(lx) == '.') {
            lx->pos++;
            uint32_t frac_start = lx->pos;
            while (is_digit(cur(lx)) || (cur(lx) == '_' && lx->pos > frac_start)) {
                if (cur(lx) == '_') {
                    if (!lex_separator(lx, 10))
                        return false;
                    separated = true;
                    continue;
                }
                if (digits < 19) {
                    mant = mant * 10 + (uint64_t)(cur(lx) - '0');
                    if (mant)
                        digits++;
                    exp_adjust--;
                } else {
                    dropped = true;
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
                return lex_fail(lx, lx->pos, "Invalid or unexpected token");
            while (is_digit(cur(lx)) || cur(lx) == '_') {
                if (cur(lx) == '_') {
                    if (!lex_separator(lx, 10))
                        return false;
                    continue;
                }
                if (exp_part < 100000)
                    exp_part = exp_part * 10 + (cur(lx) - '0');
                lx->pos++;
            }
            if (neg)
                exp_part = -exp_part;
        }
        if (dropped && separated) { /* the whole text decides: without its separators */
            uint32_t n = lx->pos - start, k = 0;
            uint16_t *clean = js_arena_alloc(lx->arena, n * sizeof *clean);
            if (!clean)
                return lex_fail(lx, start, "out of memory");
            for (uint32_t i = 0; i < n; i++)
                if (lx->src[start + i] != '_')
                    clean[k++] = lx->src[start + i];
            value = js_k_decimal(clean, k);
        } else {
            value = dropped ? js_k_decimal(lx->src + start, lx->pos - start)
                            : js_make_double(mant, exp_part + exp_adjust);
        }
    }

    if (leading) {
        if (cur(lx) == 'n' || is_id_start(cur(lx)) || is_digit(cur(lx)))
            return lex_fail(lx, start, "Invalid or unexpected token");
        lex_late(lx, start, lx->pos, "Decimals with leading zeros are not allowed in strict mode.");
    }
    if (cur(lx) == 'n') { /* a BigInt: an integer, so no point, no exponent */
        for (uint32_t i = start; i < lx->pos; i++) {
            uint16_t u = lx->src[i];
            bool hex = lx->pos - start > 1 && (lx->src[start + 1] == 'x' || lx->src[start + 1] == 'X');
            if (u == '.' || (!hex && (u == 'e' || u == 'E')))
                return lex_fail(lx, start, "Invalid or unexpected token");
        }
        uint32_t n = lx->pos - start, k = 0;
        uint16_t *text = js_arena_alloc(lx->arena, (n ? n : 1) * sizeof *text);
        if (!text)
            return lex_fail(lx, start, "out of memory");
        for (uint32_t i = 0; i < n; i++)
            if (lx->src[start + i] != '_')
                text[k++] = lx->src[start + i];
        lx->pos++; /* n */
        if (is_id_start(cur(lx)) || is_digit(cur(lx)))
            return lex_fail(lx, lx->pos, "Invalid or unexpected token");
        lx->tok.kind = JS_T_BIGINT;
        lx->tok.u.str.units = text;
        lx->tok.u.str.len = k;
        lx->tok.u.str.cont = false;
        return true;
    }
    if (is_id_start(cur(lx)) || is_digit(cur(lx)))
        return lex_fail(lx, lx->pos, "Invalid or unexpected token");

    lx->tok.kind = JS_T_NUM;
    lx->tok.u.number = value;
    return true;
}

/* ---- strings / templates ---- */

/*
 * Cooks one escape sequence at lx->pos (which points at the backslash)
 * into out/outlen. Returns false on error.
 */
static bool cook_escape(JsLexer *lx, uint16_t *out, uint32_t *outlen, bool in_template) {
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
    if (e >= '1' && e <= '9' ? true : e == '0' && is_digit(cur(lx))) {
        /* An octal escape, or \8 and \9, which once stood for the digit. A template refuses
         * them at once; in a string V8 goes on and reports them where the function ends. */
        bool digit = e >= '8';
        if (in_template)
            return lex_fail(lx, digit ? bs : bs + 1,
                            digit ? "\\8 and \\9 are not allowed in template strings."
                                  : "Octal escape sequences are not allowed in template strings.");
        lex_late(lx, digit ? bs : bs + 1, lx->pos,
                 digit ? "\\8 and \\9 are not allowed in strict mode."
                       : "Octal escape sequences are not allowed in strict mode.");
        out[0] = e;
        return true;
    }
    switch (e) {
    case 'n': out[0] = '\n'; return true;
    case 't': out[0] = '\t'; return true;
    case 'r': out[0] = '\r'; return true;
    case 'b': out[0] = '\b'; return true;
    case 'f': out[0] = '\f'; return true;
    case 'v': out[0] = '\v'; return true;
    case '0':
        out[0] = 0;
        return true;
    case 'x': {
        int h = hex_val(cur(lx)), l = hex_val(at(lx, 1));
        if (h < 0 || l < 0)
            return lex_fail(lx, bs, "Invalid hexadecimal escape sequence");
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
                    return lex_fail(lx, bs, "Undefined Unicode code-point");
                lx->pos++;
                any = true;
            }
            if (!any || cur(lx) != '}') /* V8 points at the character that is no hex digit */
                return lex_fail(lx, lx->pos, "Invalid Unicode escape sequence");
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
                return lex_fail(lx, bs, "Invalid Unicode escape sequence");
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
            return lex_fail(lx, start, "Invalid or unexpected token");
        uint16_t u = lx->src[scan];
        if (u == quote)
            break;
        if (u == '\\') {
            scan++;
            if (scan < lx->len && lx->src[scan] == '\r' &&
                scan + 1 < lx->len && lx->src[scan + 1] == '\n')
                scan++;
            if (scan >= lx->len)
                return lex_fail(lx, start, "Invalid or unexpected token");
            scan++;
        } else if (u == '\n' || u == '\r') {
            /* U+2028 and U+2029 may stand in a string literal (ES2019, so JSON text is a
             * subset of the language); only CR and LF end one */
            return lex_fail(lx, start, "Invalid or unexpected token");
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
            if (!cook_escape(lx, buf + n, &got, false))
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
            return lex_fail(lx, err_from, "Unexpected end of input");
        uint16_t u = lx->src[scan];
        if (u == '`')
            break;
        if (u == '$' && scan + 1 < lx->len && lx->src[scan + 1] == '{')
            break;
        if (u == '\\') {
            scan += 2;
            if (scan > lx->len)
                return lex_fail(lx, err_from, "Unexpected end of input");
        } else {
            scan++;
        }
    }

    uint32_t raw_len = scan - lx->pos;
    uint16_t *buf = js_arena_alloc(lx->arena, (size_t)(raw_len + 2) * sizeof(uint16_t));
    uint16_t *raw = js_arena_alloc(lx->arena, (size_t)(raw_len + 1) * sizeof(uint16_t));
    if (!buf || !raw)
        return lex_fail(lx, err_from, "out of memory");
    uint32_t rn = 0;
    for (uint32_t i = lx->pos; i < scan; i++) { /* raw: the source, CR and CRLF made LF */
        uint16_t u = lx->src[i];
        if (u == '\r') {
            raw[rn++] = '\n';
            if (i + 1 < scan && lx->src[i + 1] == '\n')
                i++;
        } else {
            raw[rn++] = u;
        }
    }
    lx->tok.u.str.raw = raw;
    lx->tok.u.str.raw_len = rn;
    lx->tok.u.str.bad_escape = NULL;
    lx->tok.u.str.bad_pos = 0;
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
            uint32_t got, bs = lx->pos;
            if (!cook_escape(lx, buf + n, &got, true)) {
                /* kept, not thrown: a tagged template's strings have undefined for it; the
                 * parser raises it for an untagged one */
                if (!lx->tok.u.str.bad_escape) {
                    lx->tok.u.str.bad_escape = lx->err_msg;
                    lx->tok.u.str.bad_pos = lx->err_pos;
                }
                lx->failed = false;
                lx->err_msg = NULL;
                lx->pos = bs + 2 <= scan ? bs + 2 : scan;
                continue;
            }
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
    lx->late_msg = NULL;
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
            return lex_fail(lx, start, "Invalid regular expression: missing /");
        uint16_t u = lx->src[p];
        if (u == '\\') {
            p++;
            if (p >= lx->len || is_line_term(lx->src[p]))
                return lex_fail(lx, start, "Invalid regular expression: missing /");
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
        return lex_fail(lx, lx->pos, "Invalid or unexpected token");
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
    lx->tok.escaped = false;
    lx->late_msg = NULL;

    bool ok;
    uint16_t u = cur(lx);
    if (lx->pos >= lx->len) {
        lx->tok.kind = JS_T_EOF;
        ok = true;
    } else if (u == '\\' && at(lx, 1) != 'u') {
        ok = lex_fail(lx, lx->pos, "Invalid or unexpected token");
    } else if (is_id_start(u) || u == '\\') {
        uint32_t p = lx->pos;
        while (p < lx->len && is_id_part(lx->src[p]))
            p++;
        if (p < lx->len && lx->src[p] == '\\') {
            ok = lex_escaped_ident(lx, lx->pos);
        } else {
            lex_ident(lx);
            ok = true;
        }
    } else if (is_digit(u) || (u == '.' && is_digit(at(lx, 1)))) {
        ok = lex_number(lx);
    } else if (u == '"' || u == '\'') {
        ok = lex_string(lx);
    } else if (u == '`') {
        lx->pos++;
        ok = lex_template_chunk(lx, lx->tok.pos);
    } else {
        ok = lex_punct(lx);
    }
    lx->tok.end = lx->pos;
    return ok;
}
