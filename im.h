/* glassine: the focused text field as the input method sees it (zwp_input_method_v2):
 * its content type, and the keyboard's view of the text before the cursor. No Wayland
 * here: main.c feeds the protocol's events in, so the rules can be tested on the host.
 *
 * The view: what the app reports as surrounding text (the part before the cursor), or,
 * when it reports none, what was typed into the field since it was focused. The
 * keyboard's own edits change the view at once (the app's report comes later). Apps
 * report late in different ways: one report per edit, a report of the text half-way
 * through an edit (the delete done, the insert not yet), the text from before the edit
 * first and the new text after it, or always one edit behind. So while own edits are on
 * their way (until IM_INFLIGHT_MS after the last one), a report is judged by its text:
 * the text the field had before one of them (or half-way through it) is a late report
 * and is ignored; the text of the view confirms it; any other text is the app's own
 * change and replaces the view. After a late report the keyboard asks for a fresh one
 * (imf_late): some apps send their newest text only in answer to the input method's
 * next commit, and resolve a delete against the text they last sent. A change the app reports as made by other
 * means drops the keyboard's view. In a private field (imf_private) there is no view at
 * all: nothing typed there is kept. */
#ifndef IM_H
#define IM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* zwp_text_input_v3.content_purpose */
enum {
    IM_P_NORMAL, IM_P_ALPHA, IM_P_DIGITS, IM_P_NUMBER, IM_P_PHONE, IM_P_URL, IM_P_EMAIL,
    IM_P_NAME, IM_P_PASSWORD, IM_P_PIN, IM_P_DATE, IM_P_TIME, IM_P_DATETIME, IM_P_TERMINAL,
};
/* zwp_text_input_v3.content_hint */
enum {
    IM_H_COMPLETION = 0x1, IM_H_SPELLCHECK = 0x2, IM_H_AUTO_CAPITALIZATION = 0x4,
    IM_H_LOWERCASE = 0x8, IM_H_UPPERCASE = 0x10, IM_H_TITLECASE = 0x20,
    IM_H_HIDDEN_TEXT = 0x40, IM_H_SENSITIVE_DATA = 0x80, IM_H_LATIN = 0x100,
    IM_H_MULTILINE = 0x200,
};
/* zwp_text_input_v3.change_cause */
enum { IM_CAUSE_INPUT_METHOD = 0, IM_CAUSE_OTHER = 1 };

#define IM_VIEW_MAX 4096        /* bytes of text before the cursor kept */
#define IM_INFLIGHT_MS 500      /* an own edit trusted over the app's older report */
#define IM_OLD_MAX 8            /* texts of the field superseded by own edits on their way */

/* what a done changed (imf_done) */
enum { IM_FOCUS_ON = 1, IM_FOCUS_OFF = 2, IM_TYPE = 4, IM_VIEW = 8 };

struct im_field {
    bool active;                /* a field is focused (applied state) */
    uint32_t purpose, hints;
    bool surrounding;           /* the app reports surrounding text */
    bool selection;             /* text is selected (anchor != cursor) */
    size_t sel_start;           /* where it starts in the view (len: at the cursor or after) */
    char before[IM_VIEW_MAX + 1];
    size_t len;
    /* the texts before the cursor the field had during own edits on their way (a hash
     * and length each): a report of one of them is late */
    struct { uint64_t h; size_t len; } old[IM_OLD_MAX];
    int n_old;
    int64_t last_edit_ms;
    bool late;                  /* the last report was late (imf_late) */
    uint32_t serial;            /* done events received (the commit serial) */

    struct {                    /* pending, applied on done */
        bool activate, deactivate, text, type;
        char *t;
        uint32_t cursor, anchor, hints, purpose, cause;
    } p;
    bool first_done;            /* the first done since activate is still to come */
};

void imf_init(struct im_field *f);
void imf_free(struct im_field *f);
/* the protocol's events */
void imf_activate(struct im_field *f);
void imf_deactivate(struct im_field *f);
void imf_surrounding(struct im_field *f, const char *text, uint32_t cursor, uint32_t anchor);
void imf_cause(struct im_field *f, uint32_t cause);
void imf_content_type(struct im_field *f, uint32_t hints, uint32_t purpose);
int imf_done(struct im_field *f, int64_t now_ms);    /* IM_* bits */

/* the field's rules (spec 006 FR-007) */
bool imf_private(const struct im_field *f);         /* password, pin, sensitive, hidden */
bool imf_suggests(const struct im_field *f);        /* suggestions and learning wanted */
bool imf_keypad(const struct im_field *f);          /* digits, number, phone, pin */
/* "purpose,hint,hint" with the protocol's names (the predictor's FIELD) */
void imf_field_name(const struct im_field *f, char *out, size_t n);

/* the keyboard's own edits: del bytes before the cursor removed, then text inserted */
void imf_edit(struct im_field *f, size_t del, const char *text, int64_t now_ms);
/* a key event the view can't follow (an arrow, Enter, Tab ...): a view of its own
 * typing is dropped */
void imf_lost(struct im_field *f);
/* Backspace sent as a key event: a view of its own typing loses its last character */
void imf_key_backspace(struct im_field *f);

/* the last report was late (own edits still on their way): the caller sends an empty
 * commit (spaced, see osk.c), which makes the app report its newest text; then
 * imf_asked. A report after IM_INFLIGHT_MS is taken as it is, so an edit the app refused
 * doesn't keep the view wrong */
bool imf_late(const struct im_field *f, int64_t now_ms);
void imf_asked(struct im_field *f);
/* the app's last report matched the view: no own edit on its way, no selection */
bool imf_settled(const struct im_field *f);
/* the app reports its text and own edits are on their way (unconfirmed) */
bool imf_unconfirmed(const struct im_field *f);

/* the view */
const char *imf_before(const struct im_field *f, size_t *len);
size_t imf_word_start(const struct im_field *f);    /* the word being typed: before[start..len) */
bool imf_sentence_start(const struct im_field *f);
/* bytes Backspace removes through the input method (0: send the key instead) */
size_t imf_backspace_bytes(const struct im_field *f);

#endif
