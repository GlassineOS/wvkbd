/* glassine: the suggestion strip above the keys (GlassineOS/shell spec 006 FR-004,
 * FR-008a, FR-009; the EinkKeyboard design of GlassineOS/ui): one row, the pen key over
 * the first key column, up to three words with the likeliest in the middle, the hide
 * key over the last column. Instead of the words it can ask whether to remove one
 * (Keep, Remove), show a short note, or, in a private field, a lock and a line saying
 * nothing is suggested or learned. Its height never changes, so the keys never move.
 *
 * This is the model and its geometry (no drawing): keyboard.c draws the items
 * strip_items gives, and only the strip's own rectangle. */
#ifndef STRIP_H
#define STRIP_H

#include <stdbool.h>
#include <stdint.h>
#include "predict.h"

enum strip_mode { STRIP_WORDS, STRIP_ASK, STRIP_NOTE, STRIP_PRIVATE };

/* what an item is */
enum strip_kind {
    SI_PEN, SI_WORD, SI_HIDE, SI_ASK_TEXT, SI_KEEP, SI_REMOVE, SI_NOTE, SI_LOCK_NOTE,
};

#define STRIP_TEXT_MAX 320
#define STRIP_ITEMS 8

struct strip_item {
    enum strip_kind kind;
    int x, y, w, h;
    int word;                   /* SI_WORD: the index in the list (0 = the likeliest) */
    bool best;                  /* SI_WORD: the likeliest (drawn bold) */
};

struct strip {
    int w, h;                   /* the strip's size (the surface's width, its own height) */
    bool pen;                   /* the pen key is offered */
    enum strip_mode mode;
    struct pc_words words;
    char ask[PC_WORD_MAX];      /* STRIP_ASK: the word */
    char note[STRIP_TEXT_MAX];  /* STRIP_NOTE: the text */
};

/* the items, left to right; their count */
int strip_items(const struct strip *s, struct strip_item *out);
/* the item at x, y (-1: none); out gets it */
int strip_hit(const struct strip *s, int x, int y, struct strip_item *out);
/* the question the strip asks (STRIP_ASK), and the note after a removal */
void strip_ask_text(const char *word, char *out, size_t n);
void strip_removed_text(const char *word, char *out, size_t n);
#define STRIP_PRIVATE_TEXT "Private field · nothing is suggested or learned"
/* two strips look the same (no redraw needed) */
bool strip_same(const struct strip *a, const struct strip *b);

#endif
