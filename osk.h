/* glassine: the keyboard as the input method (GlassineOS/shell spec 006): the field
 * (im.h), the predictor (predict.h) and the strip (strip.h) together. No Wayland here:
 * main.c passes the protocol's events in and gets its requests back through osk_ops, so
 * all of it runs in host tests.
 *
 * Typing: with a field active, a key with a text meaning is committed text (osk_text),
 * Backspace deletes through the input method where the field reports its text
 * (osk_backspace), other keys stay key events (osk_key tells about them). Without a
 * field, everything is key events and nothing here acts.
 *
 * Privacy (FR-007): in a private field (imf_private) nothing is sent to the predictor,
 * kept in the view or learned; the strip shows the lock note. That rule is applied in
 * one place, want_predictor().
 *
 * Handwriting (FR-010..FR-013a): the writing paper (paper.h) and the recognizer (hw.h).
 * The paper replaces the keys (the keys again while a reading is corrected); pen strokes
 * on it go to the recognizer one by one, each followed by a read; after a pause the
 * newest reading shows as pending text (preedit) and along the paper's top edge. A pick
 * of an alternative or a correction is confirmed to the recognizer (a lesson); writing
 * on, the keys or putting the paper away commits the read text as it is. In a private
 * field there is no paper and nothing reaches the recognizer: want_recognizer(). */
#ifndef OSK_H
#define OSK_H

#include "hw.h"
#include "im.h"
#include "paper.h"
#include "predict.h"
#include "strip.h"

/* the keys a field's purpose asks for (osk_ops.purpose) */
enum osk_keys { OSK_LETTERS, OSK_EMAIL, OSK_URL, OSK_NUMBER, OSK_PHONE, OSK_PIN };

/* what is on screen (osk_ops.view): the keys, the paper, both (correcting), nothing */
enum osk_view { OSK_VIEW_KEYS, OSK_VIEW_PAPER, OSK_VIEW_PAPER_KEYS, OSK_VIEW_HIDDEN };

#define OSK_HOLD_MS 500           /* holding a word: the strip asks to remove it */
#define OSK_NOTE_MS 2600          /* the note after a removal */
#define OSK_REPEAT_DELAY_MS 500   /* a held Backspace repeats ... */
#define OSK_REPEAT_MS 80          /* ... this often */
#ifndef OSK_ROTATION_FILE
#define OSK_ROTATION_FILE "/run/glassine-rotation"  /* glassine-rotd: the screen's turn */
#endif
#define OSK_PEN_NEAR_MS 150       /* the pen came near: where it is (over the keys or not) by then */
#define OSK_SYNC_MS 30            /* after a late report: the empty commit asking for a fresh one */
#define OSK_PICK_WAIT_MS 300      /* a pick over a selection: its second step waits this long */
#define OSK_QUEUE_MAX 64          /* edits held while a delete waits for the app's text */
#define OSK_QUEUE_WAIT_MS 150     /* ... at most this long each */

/* an edit held back (see osk.c, hold) */
enum { OSK_OP_TEXT, OSK_OP_BACKSPACE, OSK_OP_PICK };
struct osk_op {
    int kind;
    char text[PC_WORD_MAX];     /* the text typed or the word picked */
    char lang[PC_LANG_MAX];     /* a picked word's language */
};

struct osk_ops {
    /* delete del bytes before the cursor, insert text, apply both as one edit */
    void (*im_send)(void *d, uint32_t del, const char *text);
    void (*strip_changed)(void *d);         /* draw the strip again (only its rectangle) */
    void (*keys)(void *d, enum osk_keys k); /* the field wants these keys */
    void (*shift)(void *d, bool on);        /* a capital at a sentence start (labels) */
    void (*hide)(void *d);                  /* the strip's hide key */
    void (*handwriting)(void *d);           /* the strip's pen key */
    void (*focus)(void *d, int state, uint32_t purpose); /* 1 on, 0 off, 2 changed */
    /* handwriting: delete del bytes before the cursor, insert text, show preedit as the
     * pending text ("" none), as one edit */
    void (*im_preedit)(void *d, uint32_t del, const char *text, const char *preedit);
    void (*backspaces)(void *d, int n);     /* Backspace key events (no surrounding text) */
    void (*view)(void *d, enum osk_view v); /* show this instead (HIDDEN: put away, as the hide key) */
    void (*paper_changed)(void *d);         /* draw the paper again */
};

struct osk {
    const struct osk_ops *ops;
    void *d;
    struct im_field f;
    struct pc pc;
    struct strip s;
    bool strip_on;              /* a strip at all (--strip) */
    bool predict_on;            /* a predictor command was given */
    bool pen_on;                /* a handwriting hook was given */
    bool suggestions;           /* predict.conf: suggestions (off: empty strip, nothing sent) */
    bool handwriting;           /* predict.conf: handwriting (off: no pen key) */
    const char *settings_path;  /* NULL: the default (see osk_read_settings) */
    bool visible;
    bool autoshift;
    char langs[64];             /* the layout's language (the predictor's setting wins) */
    struct pc_words words;      /* the latest list (the strip may show something else) */
    uint64_t asked;             /* the request the words are for (a hash; 0: none) */
    int held;                   /* the strip item down (-1 none) */
    struct strip_item held_item;
    int64_t hold_at, note_until;
    /* the word learned last and where (a hash of the text before it): finishing the
     * same word there again (Backspace back into it, then a space or Enter) learns
     * nothing, so a word counts once however often it is finished; forgotten after
     * any other key and when the field changes by other means */
    char learned[PC_WORD_MAX + 2];
    uint64_t learned_at;
    bool bs_down;
    int64_t bs_next;
    int64_t sync_at;            /* an empty commit is due (the app reported late; 0: none) */
    /* the second step of a pick over a selection (see pick in osk.c): pick_tail (the
     * typed word and the text) replaced by pick_text, once the app reported the first;
     * until pick_until (0: none) */
    int64_t pick_until;
    char pick_tail[2 * PC_WORD_MAX + 8], pick_text[PC_WORD_MAX + 3];
    struct osk_op queue[OSK_QUEUE_MAX];
    int queued;
    int64_t queue_until;        /* the first held edit goes then in any case */

    /* handwriting */
    struct hw hw;
    bool hw_on;                 /* a recognizer command was given (--handwriting) */
    struct paper pp;
    bool paper_up;              /* the paper is on screen */
    int pause_ms;               /* predict.conf handwriting_pause (default PAPER_PAUSE_MS) */
    bool session;               /* begun on the recognizer's current run */
    unsigned session_gen;
    char sid[16];
    unsigned long nsession;
    int64_t t0;                 /* the time origin of the session's points */
    double per_mm;              /* the paper's px per mm (osk_paper_scale; 0: not known, em) */
    double session_per_mm;      /* ... as the session was begun with */
    int area_w, area_h;         /* the paper's size the session's area was sent for */
    char want_read[16];         /* the newest read ("": none waiting) */
    bool need_read;             /* a read is owed (the recognizer was away) */
    bool read_fresh;            /* the readings are for the ink as it is */
    int64_t pause_at;           /* the pause after the last stroke ends then (0: none) */
    int caret_at;               /* a caret over the ink: a space before that character (-1) */
    char confirm_id[16];        /* the lesson whose answer shows the note */
    int64_t paper_note_until;
    char last_hw[PAPER_TEXT_MAX];  /* the text the paper committed last (strike, no surrounding) */
    size_t hw_space;            /* ... and it ended in a space the paper added (a search in a URL
                                 * field): the field's text before the cursor then (bytes; 0: none) */
    char pending_text[PAPER_TEXT_MAX];  /* the reading in the field as pending text */
    bool pbs_down;              /* the paper's Backspace is held */
    int64_t pbs_next;
    int64_t near_at;            /* the pen came near: the paper then, unless it is over the keys (0: none) */
    bool pen_on_keys;           /* the pointer (the pen hovering) is over the keys */
};

void osk_init(struct osk *o, const struct osk_ops *ops, void *d, char **predictor,
              bool strip, bool pen);
void osk_handwriting(struct osk *o, char **recognizer);  /* the recognizer's command */
void osk_finish(struct osk *o);

/* the input method's events (zwp_input_method_v2) */
void osk_im_activate(struct osk *o);
void osk_im_deactivate(struct osk *o);
void osk_im_surrounding(struct osk *o, const char *text, uint32_t cursor, uint32_t anchor);
void osk_im_cause(struct osk *o, uint32_t cause);
void osk_im_content_type(struct osk *o, uint32_t hints, uint32_t purpose);
void osk_im_done(struct osk *o, int64_t now);
uint32_t osk_serial(const struct osk *o);   /* the done events of the current object */
void osk_im_new(struct osk *o);             /* a new input method object: serial from 0 */

void osk_visible(struct osk *o, bool on, int64_t now);
/* the keyboard's keys in predict.conf (suggestions, handwriting), read again at each
 * field focus and each show (from o->settings_path); path NULL:
 * $XDG_CONFIG_HOME/glassine/predict.conf, else ~/.config/glassine/predict.conf */
void osk_read_settings(struct osk *o, const char *path);
void osk_languages(struct osk *o, const char *langs, int64_t now);

/* typing: true when it went through the input method (no key event then) */
bool osk_text(struct osk *o, const char *text, int64_t now);
bool osk_backspace(struct osk *o, int64_t now);         /* Backspace pressed */
void osk_backspace_up(struct osk *o);                    /* and released */
void osk_key(struct osk *o, uint32_t code, int64_t now); /* any other key event sent */
bool osk_shifted(const struct osk *o);                   /* a capital at a sentence start */

/* the strip (surface coordinates; the strip is at the top) */
void osk_strip_size(struct osk *o, int w, int h);
bool osk_strip_down(struct osk *o, int x, int y, int64_t now);  /* true: in the strip */
void osk_strip_motion(struct osk *o, int x, int y);
void osk_strip_up(struct osk *o, int64_t now);

/* handwriting (all no-ops where it isn't offered) */
bool osk_paper_offered(const struct osk *o);    /* a field, not private, handwriting on */
bool osk_field_empty(const struct osk *o);
/* open the paper (the pen key, a pen tap into an empty field): false if not offered */
bool osk_paper_open(struct osk *o, int64_t now);
/* the session's show after a pen tap: the paper when offered and the field is empty */
bool osk_show_pen(struct osk *o, int64_t now);
/* the paper goes (the session hides the keyboard, the field went): pending text commits
 * when a field can take it, the ink goes; no view change is asked for */
void osk_paper_end(struct osk *o, int64_t now);
void osk_paper_size(struct osk *o, int w, int h);
/* the screen's scale: surface px per mm (0: not known). The recognizer then compares the
 * ink's size in millimetres; a new value counts from the next session. */
void osk_paper_scale(struct osk *o, double per_mm);
/* the pen on the paper (surface px; pressure 0..1, -1 unknown; t ms of the event) */
void osk_pen_down(struct osk *o, double x, double y, double pressure, uint32_t t, int64_t now);
void osk_pen_motion(struct osk *o, double x, double y, double pressure, uint32_t t);
void osk_pen_up(struct osk *o, int64_t now);
/* a tap (a finger, or the pen on a control): true when it hit a control */
bool osk_paper_tap(struct osk *o, int x, int y, int64_t now);
/* that finger or pen lifted (a held Backspace stops) */
void osk_paper_release(struct osk *o);
/* the pen came into range somewhere (the session tells): with the keys up, the paper
 * opens after OSK_PEN_NEAR_MS unless the pen is over the keys by then (osk_pen_on_keys) */
void osk_pen_near(struct osk *o, int64_t now);
void osk_pen_on_keys(struct osk *o, bool on);
/* a key without text while correcting (Enter uses the text): true when taken */
bool osk_key_capture(struct osk *o, uint32_t code, int64_t now);

/* the event loop */
int osk_poll_fds(struct osk *o, struct pollfd *fds);    /* at most 2 */
void osk_handle(struct osk *o, const struct pollfd *fds, int n, int64_t now);
int64_t osk_next_deadline(const struct osk *o);          /* -1: none */
void osk_tick(struct osk *o, int64_t now);

#endif
