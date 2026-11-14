/* glassine: the focused text field (see im.h) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "im.h"
#include "textrules.h"

void imf_init(struct im_field *f)
{
    memset(f, 0, sizeof *f);
}

void imf_free(struct im_field *f)
{
    free(f->p.t);
    f->p.t = NULL;
}

static void view_clear(struct im_field *f)
{
    memset(f->before, 0, sizeof f->before);     /* nothing of a field stays in memory */
    f->len = 0;
    f->selection = false;
    f->sel_start = 0;
    memset(f->old, 0, sizeof f->old);
    f->n_old = 0;
    f->late = false;
}

/* the part of s[0..n) a view keeps: its last IM_VIEW_MAX bytes, cut at a code point */
static const char *view_tail(const char *s, size_t *n)
{
    if (*n > IM_VIEW_MAX) {
        size_t cut = *n - IM_VIEW_MAX;
        while (cut < *n && ((unsigned char)s[cut] & 0xC0) == 0x80) cut++;
        s += cut;
        *n -= cut;
    }
    return s;
}

static uint64_t text_hash(const char *s, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001b3ULL;
    return h;
}

/* the field had s[0..n) during an own edit on its way */
static void old_add(struct im_field *f, const char *s, size_t n)
{
    s = view_tail(s, &n);
    if (f->n_old == IM_OLD_MAX) {
        memmove(f->old, f->old + 1, sizeof f->old[0] * (IM_OLD_MAX - 1));
        f->n_old--;
    }
    f->old[f->n_old].h = text_hash(s, n);
    f->old[f->n_old].len = n;
    f->n_old++;
}

static bool old_has(const struct im_field *f, const char *s, size_t n)
{
    s = view_tail(s, &n);
    uint64_t h = text_hash(s, n);
    for (int i = 0; i < f->n_old; i++) if (f->old[i].len == n && f->old[i].h == h) return true;
    return false;
}

/* the view becomes s[0..n): its last IM_VIEW_MAX bytes, cut at a code point */
static void view_set(struct im_field *f, const char *s, size_t n)
{
    s = view_tail(s, &n);
    memmove(f->before, s, n);
    f->before[n] = 0;
    f->len = n;
}

void imf_activate(struct im_field *f)
{
    f->p.activate = true;
    f->p.deactivate = false;
    /* activate resets the pending state */
    f->p.text = f->p.type = false;
    f->p.hints = 0;
    f->p.purpose = IM_P_NORMAL;
    f->p.cause = IM_CAUSE_INPUT_METHOD;
}

void imf_deactivate(struct im_field *f)
{
    f->p.deactivate = true;
    f->p.activate = false;
}

void imf_surrounding(struct im_field *f, const char *text, uint32_t cursor, uint32_t anchor)
{
    free(f->p.t);
    f->p.t = strdup(text ? text : "");
    size_t n = f->p.t ? strlen(f->p.t) : 0;
    f->p.cursor = cursor > n ? (uint32_t)n : cursor;
    f->p.anchor = anchor > n ? (uint32_t)n : anchor;
    f->p.text = f->p.t != NULL;
}

void imf_cause(struct im_field *f, uint32_t cause)
{
    f->p.cause = cause;
}

void imf_content_type(struct im_field *f, uint32_t hints, uint32_t purpose)
{
    f->p.hints = hints;
    f->p.purpose = purpose;
    f->p.type = true;
}

int imf_done(struct im_field *f, int64_t now)
{
    int r = 0;
    f->serial++;
    if (f->p.deactivate) {
        f->p.deactivate = false;
        if (f->active) r |= IM_FOCUS_OFF;
        f->active = false;
        f->purpose = IM_P_NORMAL;
        f->hints = 0;
        f->surrounding = false;
        view_clear(f);
    }
    if (f->p.activate) {
        f->p.activate = false;
        if (f->active) r |= IM_FOCUS_OFF;           /* another field, at once */
        f->active = true;
        f->first_done = true;
        f->surrounding = false;
        f->purpose = IM_P_NORMAL;
        f->hints = 0;
        view_clear(f);
        r |= IM_FOCUS_ON | IM_TYPE | IM_VIEW;
    }
    if (!f->active) goto out;
    if (f->p.type && (f->p.hints != f->hints || f->p.purpose != f->purpose)) {
        bool was_private = imf_private(f);
        f->hints = f->p.hints;
        f->purpose = f->p.purpose;
        r |= IM_TYPE;
        if (imf_private(f) && !was_private) { view_clear(f); r |= IM_VIEW; }
    }
    if (f->first_done) {                            /* no surrounding text now: none ever */
        f->first_done = false;
        f->surrounding = f->p.text;
    }
    if (imf_private(f)) goto out;                    /* nothing of a private field is kept */
    if (f->surrounding && f->p.text) {
        size_t c = f->p.cursor;
        bool sel = f->p.anchor != f->p.cursor;
        bool waiting = f->n_old > 0 && now - f->last_edit_ms < IM_INFLIGHT_MS;
        size_t vn = c;
        const char *vt = view_tail(f->p.t, &vn);
        bool same = vn == f->len && !memcmp(f->before, vt, vn);
        if (f->p.cause != IM_CAUSE_OTHER && waiting && !same && old_has(f, f->p.t, c)) {
            f->late = true;                         /* ignored: a fresh one is asked for */
        } else {
            f->n_old = 0;                           /* the own edits arrived, or the */
            f->late = false;                        /* app changed the text itself */
            if (!same || sel != f->selection) r |= IM_VIEW;
            view_set(f, f->p.t, c);
            f->selection = sel;
            size_t cut = c - f->len;                /* the view starts at report byte cut */
            uint32_t a = f->p.anchor;
            f->sel_start = !sel || a > c ? f->len : a > cut ? a - cut : 0;
        }
    } else if (!f->surrounding && f->p.cause == IM_CAUSE_OTHER && f->len) {
        view_clear(f);                              /* changed by other means: unknown now */
        r |= IM_VIEW;
    }
out:
    if (f->p.t) memset(f->p.t, 0, strlen(f->p.t));
    free(f->p.t);
    f->p.t = NULL;
    f->p.text = f->p.type = false;
    f->p.cause = IM_CAUSE_INPUT_METHOD;
    return r;
}

bool imf_private(const struct im_field *f)
{
    return f->active && (f->purpose == IM_P_PASSWORD || f->purpose == IM_P_PIN ||
                         (f->hints & (IM_H_SENSITIVE_DATA | IM_H_HIDDEN_TEXT)));
}

bool imf_suggests(const struct im_field *f)
{
    if (!f->active || imf_private(f)) return false;
    switch (f->purpose) {
    case IM_P_DIGITS: case IM_P_NUMBER: case IM_P_PHONE:
    case IM_P_DATE: case IM_P_TIME: case IM_P_DATETIME:
        return false;
    }
    return true;
}

bool imf_keypad(const struct im_field *f)
{
    return f->active && (f->purpose == IM_P_DIGITS || f->purpose == IM_P_NUMBER ||
                         f->purpose == IM_P_PHONE || f->purpose == IM_P_PIN);
}

void imf_field_name(const struct im_field *f, char *out, size_t n)
{
    static const char *const purposes[] = {
        "normal", "alpha", "digits", "number", "phone", "url", "email", "name",
        "password", "pin", "date", "time", "datetime", "terminal",
    };
    static const char *const hints[] = {
        "completion", "spellcheck", "auto_capitalization", "lowercase", "uppercase",
        "titlecase", "hidden_text", "sensitive_data", "latin", "multiline",
    };
    size_t o = 0;
    /* a purpose this keyboard has no name for: one the predictor doesn't know either */
    o += snprintf(out, n, "%s", f->purpose < sizeof purposes / sizeof *purposes ? purposes[f->purpose] : "unknown");
    for (unsigned i = 0; i < sizeof hints / sizeof *hints && o < n; i++)
        if (f->hints & (1u << i)) o += snprintf(out + o, n - o, ",%s", hints[i]);
    if (o < n && (f->hints >> 10)) snprintf(out + o, n - o, ",unknown");
}

void imf_edit(struct im_field *f, size_t del, const char *text, int64_t now)
{
    if (!f->active || imf_private(f)) return;
    if (del > f->len) del = f->len;
    size_t n = f->len - del, t = strlen(text);
    if (f->surrounding) {               /* the texts a late report can still show */
        old_add(f, f->before, f->len);
        if (del && t) old_add(f, f->before, n);
    }
    char buf[IM_VIEW_MAX * 2 + 2];
    if (t > IM_VIEW_MAX) t = IM_VIEW_MAX;
    memcpy(buf, f->before, n);
    memcpy(buf + n, text, t);
    view_set(f, buf, n + t);
    memset(buf, 0, n + t);
    f->selection = false;
    f->sel_start = f->len;
    if (f->surrounding) {
        f->last_edit_ms = now;
    }
}

void imf_lost(struct im_field *f)
{
    if (!f->surrounding) view_clear(f);
}

void imf_key_backspace(struct im_field *f)
{
    if (f->surrounding || !f->len) return;
    f->len = tr_prev(f->before, f->len);
    memset(f->before + f->len, 0, sizeof f->before - f->len);
}

bool imf_late(const struct im_field *f, int64_t now)
{
    (void)now;
    return f->late && f->n_old > 0;
}

void imf_asked(struct im_field *f)
{
    f->late = false;
}

bool imf_unconfirmed(const struct im_field *f)
{
    return f->active && f->surrounding && f->n_old > 0;
}

bool imf_settled(const struct im_field *f)
{
    return f->active && f->surrounding && f->n_old == 0 && !f->selection;
}

const char *imf_before(const struct im_field *f, size_t *len)
{
    *len = f->len;
    return f->before;
}

size_t imf_word_start(const struct im_field *f)
{
    return tr_word_start(f->before, f->len);
}

bool imf_sentence_start(const struct im_field *f)
{
    return tr_sentence_start(f->before, f->len);
}

size_t imf_backspace_bytes(const struct im_field *f)
{
    if (!f->active || imf_private(f) || !f->surrounding || f->selection || !f->len) return 0;
    return f->len - tr_prev(f->before, f->len);
}
