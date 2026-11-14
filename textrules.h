/* glassine: the text rules the input method needs: UTF-8 steps, what a word is (the
 * predictor's rule, docs/predictor.md of GlassineOS/shell: letters with apostrophes
 * between them; a word still being typed keeps a trailing apostrophe), sentence starts
 * and the first letter's case. No locale: the rules are fixed here. */
#ifndef TEXTRULES_H
#define TEXTRULES_H

#include <stddef.h>
#include <stdint.h>

/* one code point from s[0..n); its length (1 for a bad byte, read as U+FFFD) */
static inline int tr_dec(const char *str, size_t n, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)str;
    unsigned c = s[0];
    int len;
    uint32_t v;
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) { len = 2; v = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; v = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }
    if ((size_t)len > n) { *cp = 0xFFFD; return 1; }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return len;
}

static inline int tr_enc(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F)); return 4;
}

/* the start of the code point that ends at s[n] (n > 0) */
static inline size_t tr_prev(const char *s, size_t n)
{
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80 && n - i < 4) i--;
    return i;
}

/* letters: ASCII, the alphabetic blocks below General Punctuation (Latin, Greek,
 * Cyrillic, ... with their combining marks) and the CJK/Hangul range */
static inline int tr_letter(uint32_t c)
{
    if (c < 0x80) return (c | 32) >= 'a' && (c | 32) <= 'z';
    if (c < 0xC0 || c == 0xD7 || c == 0xF7 || c == 0xFFFD) return 0;
    if (c < 0x2000) return 1;
    if (c >= 0x3040 && c < 0xD800) return 1;
    return 0;
}

static inline int tr_apostrophe(uint32_t c) { return c == '\'' || c == 0x2019 || c == 0x2BC; }

/* the start of the word being typed at the end of s[0..n): n when there is none */
static inline size_t tr_word_start(const char *s, size_t n)
{
    size_t r = n;                         /* the run of letters and apostrophes at the end */
    while (r > 0) {
        size_t p = tr_prev(s, r);
        uint32_t c;
        tr_dec(s + p, r - p, &c);
        if (!tr_letter(c) && !tr_apostrophe(c)) break;
        r = p;
    }
    size_t i = r, start = n;
    int prev_letter = 0;
    while (i < n) {
        uint32_t c, d = 0;
        int l = tr_dec(s + i, n - i, &c);
        if (tr_letter(c)) {
            if (start == n) start = i;
            prev_letter = 1;
        } else {                          /* an apostrophe */
            if (i + l < n) tr_dec(s + i + l, n - i - l, &d);
            if (!(prev_letter && (i + l == n || tr_letter(d)))) start = n;
            prev_letter = 0;
        }
        i += l;
    }
    return start;
}

/* a code point that ends the word before it (a space, punctuation; not a letter, an
 * apostrophe or a digit) */
static inline int tr_separator(uint32_t c)
{
    return !tr_letter(c) && !tr_apostrophe(c) && !(c >= '0' && c <= '9');
}

/* the text s[0..n) ends at a sentence start: empty, after a line break, or after . ! ?
 * and at least one space */
static inline int tr_sentence_start(const char *s, size_t n)
{
    if (n == 0) return 1;
    if (s[n - 1] == '\n') return 1;
    size_t i = n;
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) i--;
    if (i == n) return 0;
    if (i == 0) return 1;
    return s[i - 1] == '.' || s[i - 1] == '!' || s[i - 1] == '?' || s[i - 1] == '\n';
}

/* upper and lower case of the Latin, Greek and Cyrillic letters (as the predictor) */
static inline uint32_t tr_upper(uint32_t c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131) return 'I';
        if (c == 0x130 || c == 0x138 || c == 0x149 || c == 0x17F || c == 0x178) return c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

static inline uint32_t tr_lower(uint32_t c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130) return 'i';
        if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
        if (c == 0x178) return 0xFF;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        return (c & 1) ? c : c + 1;
    }
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c == 0x1E9E) return 0xDF;
    return c;
}

static inline int tr_is_upper(uint32_t c) { return tr_lower(c) != c; }

#endif
