/* glassine: the writing paper (GlassineOS/shell spec 006 FR-010..FR-013a; the shell.write
 * boards of GlassineOS/ui): a sheet laid over the app at the field's cursor, as the input
 * method's popup. One guide line to write on; three controls in the top right corner
 * (Backspace, keys, put away); while text is pending, a row along the top edge with the
 * alternatives (the read text in the middle), a correction control and a discard control;
 * while correcting, an edit field and Use in that row instead; a short note at the bottom after the paper learned something.
 *
 * This is the model and its geometry (no drawing, no Wayland): the ink written on it, the
 * readings, and where everything is. osk.c runs it; keyboard.c draws it. */
#ifndef PAPER_H
#define PAPER_H

#include <stdbool.h>
#include <stdint.h>

#define PAPER_CANDS 3             /* the read text and two alternatives */
#define PAPER_TEXT_MAX 256        /* bytes of a reading kept */
#define PAPER_NOTE_MAX 160
#define PAPER_PAUSE_MS 800        /* a pause after the last stroke: the reading shows */
#define PAPER_NOTE_MS 2600        /* the note after a lesson */
#define PAPER_POINTS_MAX 20000    /* points of one stroke kept (a longer one ends there) */
#define PAPER_STROKES_MAX 512     /* strokes on the paper (more: the oldest pending text commits) */

struct paper_pt { float x, y, p, tx, ty; int32_t t; };   /* tilt in degrees */

struct paper_stroke {
    struct paper_pt *pt;
    int n, cap;
    int num;                      /* the recognizer's number for it (-1: not known yet) */
    bool pressure;                /* its points have a pressure */
    bool tilt;                    /* ... and a tilt (only with a pressure, as the contract has it) */
    char id[16];                  /* the id of its stroke request ("": not sent) */
};

enum paper_kind {
    PI_CHIP,                      /* a reading (cand: its index, 0 = the best) */
    PI_FIX,                       /* the correction control (a pencil) */
    PI_DROP,                      /* discard the reading and its ink */
    PI_BACK,                      /* Backspace */
    PI_KEYS,                      /* switch to keys */
    PI_CLOSE,                     /* put the paper away */
    PI_EDIT,                      /* the edit field (correcting) */
    PI_USE,                       /* Use: commit the corrected text */
    PI_NOTE,                      /* the note */
};

#define PAPER_ITEMS 12

struct paper_item {
    enum paper_kind kind;
    int x, y, w, h;
    int cand;                     /* PI_CHIP: the reading's index (0 = the best) */
};

struct paper {
    int w, h;                     /* the surface (px) */
    int ctl;                      /* a control's size and the top row's height */
    struct paper_stroke *s;       /* the ink, oldest first */
    int ns, cap;
    bool drawing;                 /* the last stroke is still being written */
    int ncand;
    char cand[PAPER_CANDS][PAPER_TEXT_MAX];  /* best first */
    bool pending;                 /* the reading shows (pending text, the row, dim ink) */
    int pending_n;                /* the strokes it reads: the first ones (later ones: writing on) */
    bool fixing;                  /* correcting: the edit field and the keys */
    char fix[PAPER_TEXT_MAX];
    char note[PAPER_NOTE_MAX];
};

void paper_init(struct paper *p);
void paper_free(struct paper *p);
void paper_size(struct paper *p, int w, int h);

/* the ink */
struct paper_stroke *paper_stroke_new(struct paper *p);
bool paper_point(struct paper_stroke *s, float x, float y, float pressure, int32_t t);
bool paper_point_tilt(struct paper_stroke *s, float x, float y, float pressure, float tx, float ty, int32_t t);
void paper_remove(struct paper *p, int index);
void paper_clear(struct paper *p);
int paper_find_num(const struct paper *p, int num);     /* the stroke with that number (-1) */
int paper_find_id(const struct paper *p, const char *id);

/* the layout: the guide line's y, the items, the item at x, y (-1: none) */
int paper_guide_y(const struct paper *p);
int paper_items(const struct paper *p, struct paper_item *out);
int paper_hit(const struct paper *p, int x, int y, struct paper_item *out);
/* the display order of the readings in the row: the best in the middle */
int paper_chip_order(int ncand, int slot);

/* The units the recognizer gets (docs/recognizer.md of GlassineOS/handwriting). With the
 * screen's scale known (px per mm > 0): the surface's px, sent as device:PER_MM, so the size
 * of the ink is compared in millimetres with the person's letters from any app. Without it:
 * em (paper_em px is 1), as the keyboard always sent. paper_unit: px per unit. */
double paper_em(const struct paper *p);
double paper_unit(const struct paper *p, double per_mm);
/* the area's line for the recognizer: the paper is one line, its guide line (paper_guide_y)
 * the line letters sit on (BASE). LINE is half the paper's height: the lines before and
 * after the guide line fall outside the writing area (in the top row, below the edge), so
 * a mark is placed by the guide line unless it is written more than half a line away;
 * and without the person's letters the recognizer takes small letters as 0.2 lines, which
 * at half the paper keeps a small lone letter from counting as a dot (the whole paper
 * would not). At the keyboard's 300 px the samples app's line too (150 px). */
double paper_line(const struct paper *p);
/* the area request's value: "X0,Y0,X1,Y1,LINE,ROT,BASE" in the units of paper_unit */
void paper_area_text(const struct paper *p, double per_mm, int rot, char *out, size_t n);
/* a stroke's points as the contract writes them ("x,y,t[,p[,tx,ty]] ..."; tilt when the
 * stroke has it), in the units of paper_unit. Returns a malloc'd string, its length in *len. */
char *paper_points_text(const struct paper *p, const struct paper_stroke *s, bool pressure, double per_mm,
                        size_t *len);

/* the ink's width (surface px) at a pressure, as the compositor's live preview draws it
 * (the paper's brush: PAPER_INK_MIN times the base width at no pressure to PAPER_INK_MAX
 * at full, linear); pressure < 0: not known, the base width */
#define PAPER_INK_MAX 1.4
double paper_ink_at(double base, double pressure);
/* the report rate of the ink on the paper (points per second: 1000 over the median step
 * of time, as the samples app reckons it; 0 with fewer than 8 steps) */
int paper_rate(const struct paper *p);

#define PAPER_NOTE_LEARNED "Noted. This paper learns your handwriting, on this device only."

#endif
