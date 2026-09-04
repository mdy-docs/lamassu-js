/*
 * Case conversion, the way the language defines it.
 *
 * `String.prototype.toLowerCase` and `toUpperCase` are specified as Unicode
 * Default Case Conversion — not as "add or subtract 32 for A-Z", which is what
 * this used to do. That was wrong for every language that is not English, and
 * silently: `'Ä'.toLowerCase()` came back `'Ä'`, and code that lowercases a
 * title before comparing it simply stopped matching.
 *
 * Three things the simple arithmetic cannot do, all of them required:
 *
 *   ASTRAL CHARACTERS. A string is UTF-16 units, and a character above U+FFFF
 *   is two of them. Mapping unit by unit takes a surrogate for a character.
 *
 *   MAPPINGS THAT CHANGE LENGTH. `'ß'.toUpperCase()` is `'SS'` and
 *   `'İ'.toLowerCase()` is `'i̇'` — one character becoming two. These are
 *   SpecialCasing's unconditional entries.
 *
 *   FINAL SIGMA. Greek capital sigma lowercases to `ς` at the end of a word
 *   and `σ` inside one, so `'ΟΔΟΣ ΣΤΟ'` is `'οδος στο'`. It is the one
 *   context-dependent rule in the language-independent path, and it needs the
 *   Cased and Case_Ignorable properties to decide.
 *
 * The tables are baru-re's generated UCD data, which this build already links
 * for the regex engine's `\p{...}`. Referenced directly, so the linker keeps
 * only what is used.
 */
#include <stdbool.h>
#include <stdint.h>

#include "ucd.h"

#include "../js_bytecode.h"

static bool in_ranges(const UCDRange *ranges, int count, uint32_t cp) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].start) hi = mid - 1;
        else if (cp > ranges[mid].end) lo = mid + 1;
        else return true;
    }
    return false;
}

bool js_case_is_cased(uint32_t cp) {
    return in_ranges(ucd_bin_Cased_ranges,
                     (int)(sizeof ucd_bin_Cased_ranges / sizeof(UCDRange)), cp);
}

bool js_case_is_ignorable(uint32_t cp) {
    return in_ranges(ucd_bin_Case_Ignorable_ranges,
                     (int)(sizeof ucd_bin_Case_Ignorable_ranges / sizeof(UCDRange)), cp);
}

static uint32_t simple(const SimpleCaseMapping *table, int count, uint32_t cp) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (table[mid].cp == cp) return table[mid].mapping;
        if (table[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return cp;
}

static const SpecialCaseMapping *special(uint32_t cp) {
    /* Small and sorted; a linear scan would do, but the table is sorted so a
     * binary search costs nothing extra to write. */
    int lo = 0, hi = (int)(sizeof UCD_SPECIAL_CASING / sizeof(SpecialCaseMapping)) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (UCD_SPECIAL_CASING[mid].cp == cp) return &UCD_SPECIAL_CASING[mid];
        if (UCD_SPECIAL_CASING[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/*
 * Map one code point, writing 1-3 code points into `out` and returning how
 * many. `final_sigma` is only consulted for U+03A3 and only when lowercasing;
 * the caller decides it, because it depends on what surrounds the character
 * rather than on the character.
 */
int js_case_map(uint32_t cp, bool to_upper, bool final_sigma, uint32_t *out) {
    if (!to_upper && cp == 0x03A3) {
        out[0] = final_sigma ? 0x03C2 : 0x03C3;
        return 1;
    }

    const SpecialCaseMapping *s = special(cp);
    if (s) {
        const uint32_t *from = to_upper ? s->upper : s->lower;
        uint8_t len = to_upper ? s->upper_len : s->lower_len;
        for (uint8_t i = 0; i < len && i < 3; i++) out[i] = from[i];
        return len > 3 ? 3 : len;
    }

    out[0] = to_upper
        ? simple(UCD_SIMPLE_UPPERCASE,
                 (int)(sizeof UCD_SIMPLE_UPPERCASE / sizeof(SimpleCaseMapping)), cp)
        : simple(UCD_SIMPLE_LOWERCASE,
                 (int)(sizeof UCD_SIMPLE_LOWERCASE / sizeof(SimpleCaseMapping)), cp);
    return 1;
}
