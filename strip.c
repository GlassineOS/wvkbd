/* glassine: the suggestion strip's model and geometry (see strip.h) */
#include <stdio.h>
#include <string.h>
#include "strip.h"

/* one key column of a ten-key row: the pen and hide keys line up with the keys below */
static int unit(const struct strip *s) { return s->w / 10; }

static struct strip_item item(enum strip_kind k, int x, int w, const struct strip *s)
{
    return (struct strip_item){ .kind = k, .x = x, .y = 0, .w = w, .h = s->h, .word = -1 };
}

int strip_items(const struct strip *s, struct strip_item *out)
{
    int n = 0, u = unit(s);
    int x0 = u, x1 = s->w - u;          /* the middle part, between pen and hide */
    if (s->pen && s->mode != STRIP_PRIVATE) out[n++] = item(SI_PEN, 0, u, s);
    switch (s->mode) {
    case STRIP_WORDS: {
        int k = s->words.n > PC_WORDS ? PC_WORDS : s->words.n;
        /* the likeliest in the middle: 2nd, 1st, 3rd; fewer share the width */
        static const int order3[] = { 1, 0, 2 }, order2[] = { 0, 1 }, order1[] = { 0 };
        const int *ord = k == 3 ? order3 : k == 2 ? order2 : order1;
        for (int i = 0; i < k; i++) {
            int a = x0 + (x1 - x0) * i / k, b = x0 + (x1 - x0) * (i + 1) / k;
            struct strip_item it = item(SI_WORD, a, b - a, s);
            it.word = ord[i];
            it.best = ord[i] == 0;
            out[n++] = it;
        }
        break;
    }
    case STRIP_ASK: {
        int bw = u * 3 / 2;             /* Keep and Remove: a key and a half each */
        if (x1 - x0 < 3 * bw) bw = (x1 - x0) / 3;
        out[n++] = item(SI_ASK_TEXT, x0, x1 - x0 - 2 * bw, s);
        out[n++] = item(SI_KEEP, x1 - 2 * bw, bw, s);
        out[n++] = item(SI_REMOVE, x1 - bw, bw, s);
        break;
    }
    case STRIP_NOTE:
        out[n++] = item(SI_NOTE, x0, x1 - x0, s);
        break;
    case STRIP_PRIVATE:
        out[n++] = item(SI_LOCK_NOTE, x0, x1 - x0, s);
        break;
    }
    out[n++] = item(SI_HIDE, x1, s->w - x1, s);
    return n;
}

int strip_hit(const struct strip *s, int x, int y, struct strip_item *out)
{
    struct strip_item it[STRIP_ITEMS];
    int n = strip_items(s, it);
    if (y < 0 || y >= s->h) return -1;
    for (int i = 0; i < n; i++) {
        if (x < it[i].x || x >= it[i].x + it[i].w) continue;
        if (it[i].kind == SI_ASK_TEXT || it[i].kind == SI_NOTE || it[i].kind == SI_LOCK_NOTE) return -1;
        *out = it[i];
        return i;
    }
    return -1;
}

void strip_ask_text(const char *word, char *out, size_t n)
{
    snprintf(out, n, "Remove “%s” from suggestions?", word);
}

void strip_removed_text(const char *word, char *out, size_t n)
{
    snprintf(out, n, "Removed “%s” · not suggested or learned again", word);
}

bool strip_same(const struct strip *a, const struct strip *b)
{
    if (a->w != b->w || a->h != b->h || a->pen != b->pen || a->mode != b->mode) return false;
    switch (a->mode) {
    case STRIP_WORDS:
        if (a->words.n != b->words.n) return false;
        for (int i = 0; i < a->words.n; i++) if (strcmp(a->words.word[i], b->words.word[i])) return false;
        return true;
    case STRIP_ASK: return !strcmp(a->ask, b->ask);
    case STRIP_NOTE: return !strcmp(a->note, b->note);
    case STRIP_PRIVATE: return true;
    }
    return false;
}
