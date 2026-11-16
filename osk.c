/* glassine: the keyboard as the input method (see osk.h) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "osk.h"
#include "textrules.h"

#define KEY_ENTER_ 28
#define KEY_KPENTER_ 96

static void refresh(struct osk *o, int64_t now);
static void hw_got_line(void *d, char **f, int n, int64_t now);
static void hw_stopped(void *d, bool crashed);
static bool fixing(const struct osk *o);
static void fix_set(struct osk *o, const char *text);
static void fix_leave(struct osk *o);
static void pick_second(struct osk *o, int64_t now);
static void release(struct osk *o, int64_t now, bool force);
static void pick(struct osk *o, const char *word, const char *lang, int64_t now);

static void got_words(void *d, const struct pc_words *w)
{
    struct osk *o = d;
    o->words = *w;
    refresh(o, 0);
}

void osk_init(struct osk *o, const struct osk_ops *ops, void *d, char **predictor, bool strip, bool pen)
{
    memset(o, 0, sizeof *o);
    o->ops = ops;
    o->d = d;
    imf_init(&o->f);
    pc_init(&o->pc, predictor, got_words, o);
    o->strip_on = strip;
    o->predict_on = strip && predictor && predictor[0];
    o->pen_on = pen;
    o->suggestions = o->handwriting = true;
    o->held = -1;
    hw_init(&o->hw, NULL, hw_got_line, hw_stopped, o);
    paper_init(&o->pp);
    o->pause_ms = PAPER_PAUSE_MS;
    o->caret_at = -1;
}

void osk_handwriting(struct osk *o, char **recognizer)
{
    hw_init(&o->hw, recognizer, hw_got_line, hw_stopped, o);
    o->hw_on = recognizer && recognizer[0];
}

/* off, false, 0: off; anything else (or no line at all): on */
static bool conf_on(const char *v)
{
    return strcasecmp(v, "off") && strcasecmp(v, "false") && strcmp(v, "0");
}

void osk_read_settings(struct osk *o, const char *path)
{
    char def[1024];
    o->suggestions = o->handwriting = true;
    o->pause_ms = PAPER_PAUSE_MS;
    if (!path) {
        const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
        if (x && *x == '/') snprintf(def, sizeof def, "%s/glassine/predict.conf", x);
        else if (h && *h) snprintf(def, sizeof def, "%s/.config/glassine/predict.conf", h);
        else return;
        path = def;
    }
    FILE *fp = fopen(path, "re");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof line, fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        char *k = line, *v = eq + 1, *e = eq;
        while (*k == ' ' || *k == '\t') k++;
        while (e > k && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        e = v + strlen(v);
        while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
        *e = 0;
        if (!strcmp(k, "suggestions")) o->suggestions = conf_on(v);   /* the last line wins */
        else if (!strcmp(k, "handwriting")) o->handwriting = conf_on(v);
        else if (!strcmp(k, "handwriting_pause")) {       /* ms, 200..5000 */
            int ms = atoi(v);
            o->pause_ms = ms < 200 ? PAPER_PAUSE_MS : ms > 5000 ? 5000 : ms;
        }
    }
    fclose(fp);
}

void osk_finish(struct osk *o)
{
    pc_stop(&o->pc);
    hw_stop(&o->hw);
    paper_free(&o->pp);
    imf_free(&o->f);
}

/* THE privacy rule (FR-007): the predictor hears of a field only when this says so */
static bool want_predictor(const struct osk *o)
{
    return o->predict_on && o->suggestions && o->f.active && !imf_private(&o->f) && imf_suggests(&o->f);
}

static void refresh(struct osk *o, int64_t now)
{
    struct strip n = o->s;
    (void)now;
    n.pen = (o->pen_on || o->hw_on) && o->handwriting && o->f.active && !imf_private(&o->f);
    if (imf_private(&o->f)) n.mode = STRIP_PRIVATE;
    else if (o->s.ask[0]) n.mode = STRIP_ASK;
    else if (o->s.note[0]) n.mode = STRIP_NOTE;
    else n.mode = STRIP_WORDS;
    if (n.mode == STRIP_WORDS && fixing(o)) {          /* correcting: the readings */
        memset(&n.words, 0, sizeof n.words);
        for (int i = 0; i < o->pp.ncand && i < PC_WORDS; i++) {
            snprintf(n.words.word[i], PC_WORD_MAX, "%s", o->pp.cand[i]);
            n.words.n++;
        }
    } else if (n.mode == STRIP_WORDS && want_predictor(o) && o->visible) n.words = o->words;
    else memset(&n.words, 0, sizeof n.words);
    if (strip_same(&o->s, &n)) { o->s = n; return; }
    o->s = n;
    if (o->strip_on && o->visible && o->ops->strip_changed) o->ops->strip_changed(o->d);
}

static uint64_t fnv(uint64_t h, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 0x100000001b3ULL;
    return (h ^ 0xff) * 0x100000001b3ULL;
}

/* ask for the words of the text before the cursor (once per context; force: again) */
static void ask_words(struct osk *o, int64_t now, bool force)
{
    if (!o->visible) return;            /* FR-014: nothing while hidden */
    if (fixing(o)) return;              /* the strip shows the readings */
    if (!want_predictor(o)) {
        o->words.n = 0;
        o->asked = 0;
        pc_forget(&o->pc);
        refresh(o, now);
        return;
    }
    size_t len, ws = imf_word_start(&o->f);
    const char *b = imf_before(&o->f, &len);
    char fld[160];
    imf_field_name(&o->f, fld, sizeof fld);
    uint64_t h = 0xcbf29ce484222325ULL;
    h = fnv(h, o->langs, strlen(o->langs));
    h = fnv(h, fld, strlen(fld));
    h = fnv(h, b, len);
    h = fnv(h, (const char *)&ws, sizeof ws);
    if (!force && h == o->asked) return;
    if (pc_predict(&o->pc, o->langs, fld, b, ws, b + ws, len - ws, now)) {
        o->asked = h;
    } else {
        o->asked = 0;
        o->words.n = 0;
        refresh(o, now);
    }
}

/* where a word starts, for the learned mark: the text before it (its last 256 bytes, so a
 * field that reports only part of a long text still gives the same place) */
static uint64_t word_place(const char *b, size_t ws)
{
    size_t from = ws > 256 ? ws - 256 : 0;
    return fnv(0xcbf29ce484222325ULL, b + from, ws - from);
}

static bool learned_here(const struct osk *o, size_t ws, const char *word, size_t wl)
{
    size_t len;
    const char *b = imf_before(&o->f, &len);
    return o->learned[0] && strlen(o->learned) == wl && !memcmp(o->learned, word, wl) &&
           o->learned_at == word_place(b, ws);
}

/* learn word, typed or picked after the text b[0..ws) */
static void learn(struct osk *o, const char *b, size_t ws, const char *word, size_t wl, const char *picked, int64_t now)
{
    if (!want_predictor(o)) return;
    char fld[160];
    imf_field_name(&o->f, fld, sizeof fld);
    pc_learn(&o->pc, o->langs, fld, b, ws, word, wl, picked, now);
    if (wl < sizeof o->learned) {
        memcpy(o->learned, word, wl);
        o->learned[wl] = 0;
        o->learned_at = word_place(b, ws);
    } else {
        o->learned[0] = 0;
    }
}

/* the word before the cursor is finished (a space, punctuation, Enter after it); a word
 * learned already at this place (picked, or finished before a Backspace) isn't again */
static void finish_word(struct osk *o, int64_t now)
{
    size_t len, ws = imf_word_start(&o->f);
    const char *b = imf_before(&o->f, &len);
    if (ws < len && !learned_here(o, ws, b + ws, len - ws)) learn(o, b, ws, b + ws, len - ws, "", now);
}

static void update_shift(struct osk *o)
{
    bool on = o->f.active && !imf_private(&o->f) && !imf_keypad(&o->f) &&
              (o->f.hints & IM_H_AUTO_CAPITALIZATION) && imf_sentence_start(&o->f);
    if (on == o->autoshift) return;
    o->autoshift = on;
    if (o->ops->shift) o->ops->shift(o->d, on);
}

/* after an edit: a question or note in the strip goes, new words are asked for */
static void after_edit(struct osk *o, int64_t now)
{
    o->s.ask[0] = 0;
    o->s.note[0] = 0;
    o->note_until = 0;
    update_shift(o);
    ask_words(o, now, false);
    refresh(o, now);
}

static enum osk_keys keys_for(const struct im_field *f)
{
    if (!f->active) return OSK_LETTERS;
    switch (f->purpose) {
    case IM_P_PIN: return OSK_PIN;
    case IM_P_DIGITS: case IM_P_NUMBER: return OSK_NUMBER;
    case IM_P_PHONE: return OSK_PHONE;
    case IM_P_EMAIL: return OSK_EMAIL;
    case IM_P_URL: return OSK_URL;
    }
    return OSK_LETTERS;
}

void osk_im_activate(struct osk *o) { imf_activate(&o->f); }
void osk_im_deactivate(struct osk *o) { imf_deactivate(&o->f); }
void osk_im_surrounding(struct osk *o, const char *t, uint32_t c, uint32_t a) { imf_surrounding(&o->f, t, c, a); }
void osk_im_cause(struct osk *o, uint32_t c) { imf_cause(&o->f, c); }
void osk_im_content_type(struct osk *o, uint32_t h, uint32_t p) { imf_content_type(&o->f, h, p); }
uint32_t osk_serial(const struct osk *o) { return o->f.serial; }
void osk_im_new(struct osk *o) { o->f.serial = 0; }

static void paper_field(struct osk *o, int r, int64_t now);

void osk_im_done(struct osk *o, int64_t now)
{
    int r = imf_done(&o->f, now);
    if (imf_late(&o->f, now) && !o->sync_at) o->sync_at = now + OSK_SYNC_MS;
    if (r & IM_VIEW) o->learned[0] = 0; /* changed by other means: another place */
    if (r & (IM_FOCUS_OFF | IM_FOCUS_ON)) {
        o->words.n = 0;
        o->asked = 0;
        o->learned[0] = 0;
        o->held = -1;
        o->bs_down = false;
        o->sync_at = 0;
        o->pick_until = 0;
        memset(o->pick_text, 0, sizeof o->pick_text);
        memset(o->pick_tail, 0, sizeof o->pick_tail);
        memset(o->queue, 0, sizeof o->queue);   /* nothing typed stays in memory */
        o->queued = 0;
        o->queue_until = 0;
        pc_forget(&o->pc);
    }
    if (r & IM_FOCUS_OFF) {
        if (o->ops->focus && !(r & IM_FOCUS_ON)) o->ops->focus(o->d, 0, 0);
        if (!(r & IM_FOCUS_ON) && o->ops->keys) o->ops->keys(o->d, OSK_LETTERS);
    }
    if (r & IM_FOCUS_ON) {
        osk_read_settings(o, o->settings_path);
        if (o->ops->focus) o->ops->focus(o->d, 1, o->f.purpose);
    } else if (o->f.active && o->ops->focus) {
        o->ops->focus(o->d, 2, o->f.purpose);
    }
    if ((r & IM_TYPE) && o->f.active && o->ops->keys) o->ops->keys(o->d, keys_for(&o->f));
    if (r & (IM_FOCUS_ON | IM_FOCUS_OFF | IM_TYPE)) paper_field(o, r, now);
    if (r) after_edit(o, now);
    if (o->pick_until && imf_settled(&o->f)) pick_second(o, now);
    if (o->queued) release(o, now, false);
}

void osk_visible(struct osk *o, bool on, int64_t now)
{
    if (on == o->visible) return;
    o->visible = on;
    o->held = -1;
    o->bs_down = false;
    o->s.ask[0] = 0;
    if (on) {
        osk_read_settings(o, o->settings_path);
        o->asked = 0;
        ask_words(o, now, true);
    } else {
        pc_forget(&o->pc);
        o->asked = 0;
    }
    refresh(o, now);
}

void osk_languages(struct osk *o, const char *langs, int64_t now)
{
    if (!strcmp(o->langs, langs ? langs : "")) return;
    snprintf(o->langs, sizeof o->langs, "%s", langs ? langs : "");
    ask_words(o, now, false);
}

static void text_now(struct osk *o, const char *text, int64_t now)
{
    uint32_t c;
    tr_dec(text, strlen(text), &c);
    if (tr_separator(c)) finish_word(o, now);
    if (o->ops->im_send) o->ops->im_send(o->d, 0, text);
    imf_edit(&o->f, 0, text, now);
    after_edit(o, now);
}

/* one character deleted through the input method (none: nothing to delete) */
static void backspace_now(struct osk *o, int64_t now)
{
    size_t n = imf_backspace_bytes(&o->f);
    if (!n) return;
    if (o->ops->im_send) o->ops->im_send(o->d, (uint32_t)n, "");
    imf_edit(&o->f, n, "", now);
    after_edit(o, now);
}

/* Holding edits back. Some apps resolve a delete against the text they last reported
 * and drop it when that is older than the field (they report one edit behind). So a
 * delete (Backspace, a pick that replaces something) waits while own edits are on their
 * way, until the app's report confirms them (the empty commits of osk_tick ask for it) or
 * OSK_QUEUE_WAIT_MS passed; edits after a held one wait behind it, in order. */
static void hold(struct osk *o, int kind, const char *text, const char *lang, int64_t now)
{
    if (o->queued == OSK_QUEUE_MAX) release(o, now, true);
    struct osk_op *op = &o->queue[o->queued++];
    op->kind = kind;
    snprintf(op->text, sizeof op->text, "%s", text ? text : "");
    snprintf(op->lang, sizeof op->lang, "%s", lang ? lang : "");
    if (!o->queue_until) o->queue_until = now + OSK_QUEUE_WAIT_MS;
    if (!o->sync_at) o->sync_at = now + OSK_SYNC_MS;
}

/* the held edits that can go now, in order: text always, a delete once the field is
 * confirmed (or, force, the first one in any case) */
static void release(struct osk *o, int64_t now, bool force)
{
    int i = 0;
    for (; i < o->queued; i++) {
        struct osk_op *op = &o->queue[i];
        if (op->kind != OSK_OP_TEXT && imf_unconfirmed(&o->f) && !force) break;
        if (op->kind != OSK_OP_TEXT) force = false;
        if (!o->f.active) continue;
        if (op->kind == OSK_OP_TEXT) text_now(o, op->text, now);
        else if (op->kind == OSK_OP_BACKSPACE) backspace_now(o, now);
        else pick(o, op->text, op->lang, now);
    }
    memmove(o->queue, o->queue + i, sizeof o->queue[0] * (o->queued - i));
    memset(o->queue + (o->queued - i), 0, sizeof o->queue[0] * i);
    o->queued -= i;
    o->queue_until = 0;
    if (o->queued) {
        o->queue_until = now + OSK_QUEUE_WAIT_MS;
        if (!o->sync_at) o->sync_at = now + OSK_SYNC_MS;
    }
}

bool osk_text(struct osk *o, const char *text, int64_t now)
{
    if (!o->f.active || !text || !*text) return false;
    if (fixing(o)) {                    /* correcting a reading: the keys edit it */
        char t[PAPER_TEXT_MAX];
        snprintf(t, sizeof t, "%s%s", o->pp.fix, text);
        fix_set(o, t);
        return true;
    }
    if (o->queued) hold(o, OSK_OP_TEXT, text, NULL, now);
    else text_now(o, text, now);
    return true;
}

bool osk_backspace(struct osk *o, int64_t now)
{
    if (!o->f.active) return false;
    if (fixing(o)) {
        char t[PAPER_TEXT_MAX];
        snprintf(t, sizeof t, "%s", o->pp.fix);
        if (*t) t[tr_prev(t, strlen(t))] = 0;
        fix_set(o, t);
        return true;
    }
    size_t n = imf_backspace_bytes(&o->f);
    if (!n && !o->queued) {             /* the key does it: the own view follows */
        imf_key_backspace(&o->f);
        after_edit(o, now);
        return false;
    }
    if (o->queued || imf_unconfirmed(&o->f)) hold(o, OSK_OP_BACKSPACE, NULL, NULL, now);
    else backspace_now(o, now);
    o->bs_down = true;
    o->bs_next = now + OSK_REPEAT_DELAY_MS;
    return true;
}

void osk_backspace_up(struct osk *o)
{
    o->bs_down = false;
}

void osk_key(struct osk *o, uint32_t code, int64_t now)
{
    if (!o->f.active) return;
    if (code == KEY_ENTER_ || code == KEY_KPENTER_) finish_word(o, now);
    o->learned[0] = 0;                  /* the field may change by itself now (a sent message) */
    imf_lost(&o->f);
    after_edit(o, now);
}

bool osk_shifted(const struct osk *o)
{
    return o->autoshift;
}

/* the text being typed looks like an address: the text since the last space before the
 * word being typed has a . / : or @ */
static bool address_like(const char *b, size_t ws)
{
    size_t i = ws;
    while (i > 0 && b[i - 1] != ' ' && b[i - 1] != '\t' && b[i - 1] != '\n') i--;
    for (; i < ws; i++) if (strchr("./:@", b[i])) return true;
    return false;
}

/* punctuation a word follows with a space between: what ends a clause or closes a
 * bracket or a typographic quote (not an opening one, a dash, a slash or a straight
 * quote, which open as often as they close) */
static bool after_space(uint32_t c)
{
    return (c < 0x80 && strchr(",.;:!?)]}", (int)c) && c) || c == 0x201D || c == 0x2019 ||
           c == 0xBB || c == 0x2026;
}

/* a word picked from the strip, as one edit: it replaces the word being typed at the
 * cursor; with none there (a next word) right after a digit or punctuation that ends
 * something (after_space), a space goes before it (right after a letter there always is
 * a word being typed). A space follows it, except in an email field (it holds
 * addresses) and in a URL field where the text being typed looks like an address
 * (address_like): a URL field is also where a browser takes a search, and a plain word
 * there gets its space.
 *
 * With a selection, the word is the one right before the selection, and the selection
 * goes: every app replaces a selection with inserted text, but apps resolve a delete
 * around a selection differently (a browser's address bar selects its own completion
 * after the typed text and then deletes from the wrong end). So the text goes in alone
 * first, over the selection; once the app reported that, the typed word and the text
 * are replaced by the text, before a plain cursor (pick_second). */
static void pick(struct osk *o, const char *word, const char *lang, int64_t now)
{
    if (!o->f.active || imf_private(&o->f) || !*word) return;
    size_t len;
    const char *b = imf_before(&o->f, &len);
    bool sel = o->f.surrounding && o->f.selection;
    size_t end = sel ? o->f.sel_start : len;   /* the word ends here */
    size_t ws = tr_word_start(b, end);
    bool addr = o->f.purpose == IM_P_EMAIL || (o->f.purpose == IM_P_URL && address_like(b, ws));
    bool lead = false;
    if (ws == end && end && !addr) {
        uint32_t c;
        size_t p = tr_prev(b, end);
        tr_dec(b + p, end - p, &c);
        lead = (c >= '0' && c <= '9') || after_space(c);
    }
    char w[PC_WORD_MAX + 1];
    snprintf(w, sizeof w, "%s", word);
    if (ws < end) {                     /* a capital typed stays a capital */
        uint32_t p, c;
        tr_dec(b + ws, end - ws, &p);
        int wl = tr_dec(w, strlen(w), &c);
        char up[4];
        int ul = tr_enc(tr_upper(c), up);
        if (tr_is_upper(p) && !tr_is_upper(c) && ul == wl) memcpy(w, up, ul);
    }
    if (lead) {                         /* learned after the space it gets */
        char ctx[IM_VIEW_MAX + 2];
        memcpy(ctx, b, end);
        ctx[end] = ' ';
        learn(o, ctx, end + 1, w, strlen(w), lang, now);
        memset(ctx, 0, end + 1);
    } else {
        learn(o, b, ws, w, strlen(w), lang, now);
    }
    char text[PC_WORD_MAX + 3];
    snprintf(text, sizeof text, "%s%s%s", lead ? " " : "", w, addr ? "" : " ");
    uint32_t del = (uint32_t)(end - ws);
    if (sel) {
        if (del) {                      /* the second step: the typed word and the text */
            snprintf(o->pick_tail, sizeof o->pick_tail, "%.*s%s", (int)del, b + ws, text);
            snprintf(o->pick_text, sizeof o->pick_text, "%s", text);
            o->pick_until = now + OSK_PICK_WAIT_MS;
            o->sync_at = now + OSK_SYNC_MS; /* some apps report only when asked */
        }
        if (o->ops->im_send) o->ops->im_send(o->d, 0, text);
        imf_edit(&o->f, len - end, text, now);   /* the view: the selected part replaced */
        after_edit(o, now);
        return;
    }
    if (o->ops->im_send) o->ops->im_send(o->d, del, text);
    imf_edit(&o->f, del, text, now);
    after_edit(o, now);
}

/* the second step of a pick over a selection, once the app reported the first (its
 * report matches the view, no selection) and the view still ends with the typed word
 * and the text; otherwise (or after OSK_PICK_WAIT_MS) it is dropped */
static void pick_second(struct osk *o, int64_t now)
{
    size_t len, t = strlen(o->pick_tail);
    const char *b = imf_before(&o->f, &len);
    bool ok = o->pick_until && now < o->pick_until && imf_settled(&o->f) && len >= t &&
              !memcmp(b + len - t, o->pick_tail, t);
    o->pick_until = 0;
    if (ok) {
        if (o->ops->im_send) o->ops->im_send(o->d, (uint32_t)t, o->pick_text);
        imf_edit(&o->f, t, o->pick_text, now);
        after_edit(o, now);
    }
    memset(o->pick_tail, 0, sizeof o->pick_tail);
    memset(o->pick_text, 0, sizeof o->pick_text);
}

/* a pick now would delete (a word being typed, a selection) */
static bool pick_deletes(const struct osk *o)
{
    size_t len;
    const char *b = imf_before(&o->f, &len);
    if (o->f.selection) return true;
    return tr_word_start(b, len) < len;
}

static void remove_word(struct osk *o, int64_t now)
{
    char w[PC_WORD_MAX];
    snprintf(w, sizeof w, "%s", o->s.ask);
    o->s.ask[0] = 0;
    if (!*w) return;
    if (want_predictor(o)) pc_remove(&o->pc, o->langs, w, now);
    struct pc_words keep = { 0 };       /* gone from the strip at once */
    for (int i = 0; i < o->words.n; i++) {
        if (!strcmp(o->words.word[i], w)) continue;
        snprintf(keep.word[keep.n], PC_WORD_MAX, "%s", o->words.word[i]);
        snprintf(keep.lang[keep.n], PC_LANG_MAX, "%s", o->words.lang[i]);
        keep.n++;
    }
    o->words = keep;
    strip_removed_text(w, o->s.note, sizeof o->s.note);
    o->note_until = now + OSK_NOTE_MS;
    ask_words(o, now, true);            /* the next list comes without it */
    refresh(o, now);
}

void osk_strip_size(struct osk *o, int w, int h)
{
    o->s.w = w;
    o->s.h = h;
}

bool osk_strip_down(struct osk *o, int x, int y, int64_t now)
{
    if (!o->strip_on || y < 0 || y >= o->s.h) return false;
    struct strip_item it;
    o->held = -1;
    if (strip_hit(&o->s, x, y, &it) < 0) return true;
    switch (it.kind) {
    case SI_WORD:
        o->held = it.word;
        o->held_item = it;
        o->hold_at = now + OSK_HOLD_MS;
        break;
    case SI_PEN:
        if (fixing(o)) fix_leave(o);                    /* back to the paper */
        else if (o->hw_on) osk_paper_open(o, now);
        else if (o->ops->handwriting) o->ops->handwriting(o->d);
        break;
    case SI_HIDE:
        if (fixing(o)) fix_leave(o);
        else if (o->ops->hide) o->ops->hide(o->d);
        break;
    case SI_KEEP:
        o->s.ask[0] = 0;
        refresh(o, now);
        break;
    case SI_REMOVE:
        remove_word(o, now);
        break;
    default:
        break;
    }
    return true;
}

void osk_strip_motion(struct osk *o, int x, int y)
{
    struct strip_item it;
    if (o->held < 0) return;
    if (strip_hit(&o->s, x, y, &it) < 0 || it.kind != SI_WORD || it.word != o->held) o->held = -1;
}

void osk_strip_up(struct osk *o, int64_t now)
{
    int i = o->held;
    o->held = -1;
    if (i < 0 || o->s.mode != STRIP_WORDS || i >= o->s.words.n) return;
    if (fixing(o)) {                    /* a reading picked while correcting: the edit's text */
        fix_set(o, o->s.words.word[i]);
        return;
    }
    char w[PC_WORD_MAX], l[PC_LANG_MAX];
    snprintf(w, sizeof w, "%s", o->s.words.word[i]);
    snprintf(l, sizeof l, "%s", o->s.words.lang[i]);
    if (o->queued || (imf_unconfirmed(&o->f) && pick_deletes(o))) hold(o, OSK_OP_PICK, w, l, now);
    else pick(o, w, l, now);
}

int osk_poll_fds(struct osk *o, struct pollfd *fds)
{
    int n = pc_poll_fds(&o->pc, fds);
    return n + hw_poll_fds(&o->hw, fds + n);
}

void osk_handle(struct osk *o, const struct pollfd *fds, int n, int64_t now)
{
    for (int i = 0; i < n; i++) {
        if (fds[i].fd == o->pc.from) pc_handle(&o->pc, fds + i, 1, now);
        else if (fds[i].fd == o->hw.fd) hw_handle(&o->hw, fds + i, 1, now);
    }
}

static int64_t earliest(int64_t a, int64_t b)
{
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
}

int64_t osk_next_deadline(const struct osk *o)
{
    int64_t d = pc_next_deadline(&o->pc);
    if (o->held >= 0) d = earliest(d, o->hold_at);
    if (o->note_until) d = earliest(d, o->note_until);
    if (o->bs_down) d = earliest(d, o->bs_next);
    d = earliest(d, hw_next_deadline(&o->hw));
    if (o->pause_at) d = earliest(d, o->pause_at);
    if (o->paper_note_until) d = earliest(d, o->paper_note_until);
    if (o->pbs_down) d = earliest(d, o->pbs_next);
    if (o->near_at) d = earliest(d, o->near_at);
    if (o->need_read && o->paper_up) d = earliest(d, o->hw.restart_ms);
    if (o->sync_at) d = earliest(d, o->sync_at);
    if (o->pick_until) d = earliest(d, o->pick_until);
    if (o->queued) d = earliest(d, o->queue_until);
    return d;
}

static void paper_tick(struct osk *o, int64_t now);

void osk_tick(struct osk *o, int64_t now)
{
    pc_tick(&o->pc, now);
    hw_tick(&o->hw, now);
    paper_tick(o, now);
    if (o->sync_at && now >= o->sync_at) {
        /* the app reported late: an empty commit makes it report its newest text (and
         * resolve a following delete against that); again after each late answer, spaced,
         * until the own edits arrive or a report after IM_INFLIGHT_MS is taken as it is */
        o->sync_at = 0;
        if (imf_late(&o->f, now) || (o->pick_until && !imf_settled(&o->f)) || o->queued) {
            imf_asked(&o->f);
            if (o->ops->im_send) o->ops->im_send(o->d, 0, "");
            if (o->pick_until || o->queued) o->sync_at = now + OSK_SYNC_MS;
        }
    }
    if (o->queued && now >= o->queue_until) release(o, now, true);
    if (o->pick_until && now >= o->pick_until) pick_second(o, now);
    if (o->held >= 0 && now >= o->hold_at) {
        int i = o->held;
        o->held = -1;
        if (o->s.mode == STRIP_WORDS && i < o->s.words.n) {
            snprintf(o->s.ask, sizeof o->s.ask, "%s", o->s.words.word[i]);
            refresh(o, now);
        }
    }
    if (o->note_until && now >= o->note_until) {
        o->note_until = 0;
        o->s.note[0] = 0;
        refresh(o, now);
    }
    if (o->bs_down && now >= o->bs_next) {
        if (!imf_backspace_bytes(&o->f) && !o->queued) { o->bs_down = false; return; }
        if (o->queued || imf_unconfirmed(&o->f)) hold(o, OSK_OP_BACKSPACE, NULL, NULL, now);
        else backspace_now(o, now);
        o->bs_next = now + OSK_REPEAT_MS;
    }
}

/* ---- handwriting (FR-010..FR-013a) ------------------------------------------------- */

static bool fixing(const struct osk *o)
{
    return o->paper_up && o->pp.fixing;
}

/* THE privacy rule for handwriting (FR-007): the recognizer hears of a field, and the
 * paper opens on it, only when this says so */
static bool want_recognizer(const struct osk *o)
{
    return o->hw_on && o->handwriting && o->f.active && !imf_private(&o->f);
}

bool osk_paper_offered(const struct osk *o)
{
    return want_recognizer(o);
}

bool osk_field_empty(const struct osk *o)
{
    return imf_empty(&o->f);
}

static void paper_draw(struct osk *o)
{
    if (o->paper_up && o->ops->paper_changed) o->ops->paper_changed(o->d);
}

static void set_view(struct osk *o, enum osk_view v)
{
    if (o->ops->view) o->ops->view(o->d, v);
}

static void preedit(struct osk *o, const char *t)
{
    if (o->f.active && o->ops->im_preedit) o->ops->im_preedit(o->d, 0, "", t);
}

static void readings_clear(struct osk *o)
{
    o->pp.ncand = 0;
    memset(o->pp.cand, 0, sizeof o->pp.cand);
    o->pp.pending = false;
    o->pp.pending_n = 0;
    memset(o->pending_text, 0, sizeof o->pending_text);
    o->read_fresh = false;
    o->want_read[0] = 0;
    o->need_read = false;
    o->pause_at = 0;
    o->caret_at = -1;
}

static bool send_stroke(struct osk *o, struct paper_stroke *st, int64_t now)
{
    size_t n;
    char *pts = paper_points_text(&o->pp, st, st->pressure, o->session_per_mm, &n);
    if (!pts) return false;
    bool ok = hw_request_raw(&o->hw, now, st->id, "stroke", o->sid, pts, n);
    memset(pts, 0, n);
    free(pts);
    if (!ok) st->id[0] = 0;
    return ok;
}

/* the finished strokes not sent on the recognizer's current run */
static void send_unsent(struct osk *o, int64_t now)
{
    for (int i = 0; i < o->pp.ns; i++) {
        if (o->pp.drawing && i == o->pp.ns - 1) break;
        if (!o->pp.s[i].id[0] && !send_stroke(o, &o->pp.s[i], now)) return;
    }
}

/* the screen's turn as glassine-rotd applied it (0, 90, 180, 270; 0 when not known), as
 * the samples app reads it */
static int rotation(void)
{
    FILE *f = fopen(OSK_ROTATION_FILE, "re");
    if (!f) return 0;
    int r = -1;
    if (fscanf(f, "%d", &r) != 1) r = -1;
    fclose(f);
    return r == 90 || r == 180 || r == 270 ? r : 0;
}

/* the writing area of the session: the paper, its line and its guide line (BASE), so the
 * recognizer tells a comma on the line from an apostrophe above it. A recognizer that
 * doesn't know BASE answers bad-area: ignored, the session goes on without an area. */
static void send_area(struct osk *o, int64_t now)
{
    if (o->pp.w <= 0 || o->pp.h <= 0) return;
    char a[160];
    paper_area_text(&o->pp, o->session_per_mm, rotation(), a, sizeof a);
    hw_request(&o->hw, now, NULL, "area", o->sid, a, (char *)NULL);
    o->area_w = o->pp.w;
    o->area_h = o->pp.h;
}

/* a session on the recognizer's current run (begun again after it went away: the ink is
 * sent again, so a lesson or a reading still has all of it) */
static bool session(struct osk *o, int64_t now)
{
    if (!want_recognizer(o)) return false;
    if (o->session && o->session_gen == o->hw.gen && hw_running(&o->hw)) {
        /* the paper changed its size (the screen turned): its area again */
        if (o->pp.w != o->area_w || o->pp.h != o->area_h) send_area(o, now);
        return true;
    }
    o->session = false;
    snprintf(o->sid, sizeof o->sid, "s%lu", ++o->nsession % 100000000UL);
    char fld[160];
    imf_field_name(&o->f, fld, sizeof fld);
    size_t len;
    const char *b = imf_before(&o->f, &len);
    size_t from = len > 1024 ? len - 1024 : 0;  /* the last 1 KB of the text before */
    while (from < len && ((unsigned char)b[from] & 0xC0) == 0x80) from++;
    char before[1028];
    memcpy(before, b + from, len - from);
    before[len - from] = 0;
    /* the units: the surface's px with their size and the pen's rate, else em */
    char units[64];
    osk_units_text(o->per_mm, o->rate, units, sizeof units);
    bool ok = hw_request(&o->hw, now, NULL, "begin", o->sid, "-", fld, units, before, (char *)NULL);
    memset(before, 0, sizeof before);
    if (!ok) return false;
    o->session = true;
    o->session_gen = o->hw.gen;
    o->session_per_mm = o->per_mm;
    send_area(o, now);
    for (int i = 0; i < o->pp.ns; i++) {
        o->pp.s[i].num = -1;
        o->pp.s[i].id[0] = 0;
    }
    send_unsent(o, now);
    return true;
}

static void session_end(struct osk *o, int64_t now)
{
    if (o->session && o->session_gen == o->hw.gen && hw_running(&o->hw))
        hw_request(&o->hw, now, NULL, "end", o->sid, (char *)NULL);
    o->session = false;
}

/* read the ink (after each stroke; the newest read's answer is the one that counts) */
static void ask_read(struct osk *o, int64_t now)
{
    o->read_fresh = false;
    o->want_read[0] = 0;
    if (!o->pp.ns) return;
    if (!session(o, now) ||
        !hw_request(&o->hw, now, o->want_read, "read", o->sid, "3", (char *)NULL)) {
        o->want_read[0] = 0;
        o->need_read = want_recognizer(o);      /* again when the recognizer is back */
        return;
    }
    o->need_read = false;
}

/* strokes gone from the paper and the session, by the recognizer's numbers */
static void remove_nums(struct osk *o, const int *nums, int n, int64_t now)
{
    char list[1024];
    size_t k = 0;
    list[0] = 0;
    for (int j = 0; j < n; j++) {
        int i = paper_find_num(&o->pp, nums[j]);
        if (i < 0) continue;
        if (i < o->pp.pending_n) o->pp.pending_n--;
        paper_remove(&o->pp, i);
        if (k + 16 < sizeof list) k += (size_t)snprintf(list + k, sizeof list - k, "%s%d", k ? "," : "", nums[j]);
    }
    if (k && o->session && o->session_gen == o->hw.gen)
        hw_request(&o->hw, now, NULL, "remove", o->sid, list, (char *)NULL);
}

/* the first n strokes go (their text was committed or discarded) */
static void drop_first(struct osk *o, int n, int64_t now)
{
    if (n >= o->pp.ns) {
        if (o->session && o->session_gen == o->hw.gen)
            hw_request(&o->hw, now, NULL, "clear", o->sid, (char *)NULL);
        paper_clear(&o->pp);
        return;
    }
    int nums[PAPER_STROKES_MAX], k = 0;
    for (int i = 0; i < n; i++) if (o->pp.s[i].num >= 0) nums[k++] = o->pp.s[i].num;
    for (int i = 0; i < n; i++) paper_remove(&o->pp, 0);
    o->pp.pending_n = 0;
    char list[4096];
    size_t l = 0;
    for (int i = 0; i < k && l + 16 < sizeof list; i++) l += (size_t)snprintf(list + l, sizeof list - l, "%s%d", i ? "," : "", nums[i]);
    if (l && o->session && o->session_gen == o->hw.gen)
        hw_request(&o->hw, now, NULL, "remove", o->sid, list, (char *)NULL);
}

/* text into the field after del bytes before the cursor go (the pending text goes). With
 * a deletion, the pending text goes in an edit of its own first: Chromium's address bar
 * drops a deletion that comes with its composition's end */
static void put_text(struct osk *o, size_t del, const char *text, int64_t now)
{
    if (!o->f.active) return;
    if (del && o->ops->im_preedit) o->ops->im_preedit(o->d, 0, "", "");
    if (o->ops->im_preedit) o->ops->im_preedit(o->d, (uint32_t)del, text, "");
    if (del || *text) imf_edit(&o->f, del, text, now);
    if (*text) snprintf(o->last_hw, sizeof o->last_hw, "%s", text);
    o->hw_space = 0;
}

static bool starts_with(const char *s, const char *p)
{
    for (; *p; s++, p++) if ((*s | 0x20) != *p) return false;
    return true;
}

/* a URL field also takes searches (a browser's address bar): the field's text b[0..n)
 * with text after it looks like an address once it has a . / : or @, or starts with a
 * scheme ("http") or "www"; plain words are a search */
static bool url_address(const char *b, size_t n, const char *text)
{
    char all[IM_VIEW_MAX + PAPER_TEXT_MAX + 2];
    size_t i = 0;
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n')) i++;
    snprintf(all, sizeof all, "%.*s%s", (int)(n - i), b + i, text);
    bool addr = strpbrk(all, "./:@") || starts_with(all, "http") || starts_with(all, "www");
    memset(all, 0, sizeof all);
    return addr;
}

/* the reading goes into the field: a space before it where one is missing (see lead), text, a space after it (not in an email field, not in
 * a URL field once its text looks like an address: url_address; with a selection, the
 * text before it, which the reading replaces); source: the lesson (picked, corrected,
 * confirmed; NULL: none: a reading committed as it was read teaches nothing). A reading
 * that starts with . / : or @ right after the space the paper put after a search in a
 * URL field ("example", then ".com"), with nothing edited since, takes that space back;
 * not with a selection (apps resolve a deletion around one differently, see pick) or
 * without the field's text (the deletion needs it). The ink it was read from clears; ink
 * written after it stays. */
static void commit_reading(struct osk *o, const char *text, const char *source, int64_t now)
{
    int n = o->pp.pending ? o->pp.pending_n : o->pp.ns;
    /* a lesson needs exactly the ink of the reading in the session */
    if (source && n == o->pp.ns && *text && session(o, now)) {
        send_unsent(o, now);
        if (!hw_request(&o->hw, now, o->confirm_id, "confirm", o->sid, source, text, (char *)NULL))
            o->confirm_id[0] = 0;
    }
    char t[PAPER_TEXT_MAX + 2];
    size_t tl = strlen(text), len, del = 0;
    const char *b = imf_before(&o->f, &len);
    bool sel = o->f.surrounding && o->f.selection;
    size_t end = sel ? o->f.sel_start : len;
    bool url = o->f.purpose == IM_P_URL, space = tl && text[tl - 1] != ' ' && o->f.purpose != IM_P_EMAIL;
    if (url && *text && strchr("./:@", text[0]) && o->hw_space == len && len && !sel && o->f.surrounding &&
        b[len - 1] == ' ') {
        size_t hl = strlen(o->last_hw);
        if (hl <= len && !memcmp(b + len - hl, o->last_hw, hl)) del = 1;
    }
    if (url && space) space = !url_address(b, end - del, text);
    /* right after a word, a number or punctuation that ends something, a reading that
     * starts with a letter or digit gets a space before it, in prose and name fields and
     * in a URL field's search; not after a space (the caret's, too), a bracket or quote
     * that opens, a dash or a slash */
    bool lead = false;
    int pu = o->f.purpose;
    if (*text && !del && end && (pu == IM_P_NORMAL || pu == IM_P_ALPHA || pu == IM_P_NAME || pu == IM_P_URL)) {
        uint32_t c, f;
        size_t p = tr_prev(b, end);
        tr_dec(b + p, end - p, &c);
        tr_dec(text, tl, &f);
        lead = (tr_letter(c) || (c >= '0' && c <= '9') || after_space(c)) && (tr_letter(f) || (f >= '0' && f <= '9'));
        if (lead && pu == IM_P_URL) lead = !url_address(b, end, text);
    }
    snprintf(t, sizeof t, "%s%s%s", lead ? " " : "", text, space ? " " : "");
    put_text(o, del, t, now);
    if (url && space) imf_before(&o->f, &o->hw_space);
    memset(t, 0, sizeof t);
    drop_first(o, n, now);
    readings_clear(o);
    o->pp.fixing = false;
    memset(o->pp.fix, 0, sizeof o->pp.fix);
    if (o->pp.ns) ask_read(o, now);
    after_edit(o, now);
    paper_draw(o);
}

static void show_pending(struct osk *o)
{
    if (!o->pp.ncand || !o->read_fresh || o->pp.drawing || !o->pp.cand[0][0]) return;
    o->pp.pending = true;
    o->pp.pending_n = o->pp.ns;
    snprintf(o->pending_text, sizeof o->pending_text, "%s", o->pp.cand[0]);
    preedit(o, o->pending_text);
    paper_draw(o);
}

/* the reading and the ink it was read from go; nothing reaches the field */
static void discard_pending(struct osk *o, int64_t now)
{
    drop_first(o, o->pp.pending_n, now);
    preedit(o, "");
    readings_clear(o);
}

/* where the last word of s[0..n) starts: back over the spaces after it, then to the space
 * before it (a word as written: "3pm," goes as a whole) */
static size_t written_word_start(const char *s, size_t n)
{
    size_t e = n;
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    while (e > 0 && s[e - 1] != ' ' && s[e - 1] != '\t' && s[e - 1] != '\n') e--;
    return e;
}

/* the last word before the cursor (and the spaces after it) goes (FR-013) */
static void delete_last_word(struct osk *o, int64_t now)
{
    size_t len;
    const char *b = imf_before(&o->f, &len);
    size_t ws = written_word_start(b, len), del = len - ws;
    if (!del && !o->f.surrounding && o->last_hw[0]) {     /* nothing known: the paper's last text */
        b = o->last_hw;
        len = strlen(b);
        ws = written_word_start(b, len);
        del = len - ws;
    }
    if (!del) return;
    if (o->f.surrounding) {
        if (o->ops->im_preedit) o->ops->im_preedit(o->d, (uint32_t)del, "", "");
    } else if (o->ops->backspaces) {
        int cps = 0;
        for (size_t i = ws; i < len; i++) cps += ((unsigned char)b[i] & 0xC0) != 0x80;
        o->ops->backspaces(o->d, cps);
    }
    if (b != o->last_hw) imf_edit(&o->f, del, "", now);
    o->last_hw[0] = 0;
    after_edit(o, now);
}

/* text with a space before its k-th character (code points) */
static void space_at(const char *text, int k, char *out, size_t n)
{
    size_t i = 0, len = strlen(text);
    for (int c = 0; i < len && c < k; c++) i += (size_t)tr_dec(text + i, len - i, &(uint32_t){ 0 });
    if (k < 0 || i > len) i = len;
    snprintf(out, n, "%.*s %s", (int)i, text, text + i);
}

static void gesture(struct osk *o, char **f, int n, int64_t now)
{
    if (n < 8) return;
    const char *kind = f[3];
    int g = paper_find_num(&o->pp, atoi(f[4]));
    if (g < 0 && o->pp.ns && !o->pp.drawing) g = o->pp.ns - 1;
    if (g >= 0) {                       /* the gesture isn't writing: gone from the paper too */
        if (g < o->pp.pending_n) o->pp.pending_n--;
        paper_remove(&o->pp, g);
    }
    int cov[256], nc = 0;
    if (strcmp(f[6], "-"))
        for (char *p = f[6]; *p && nc < 256; ) {
            cov[nc++] = atoi(p);
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
    bool others = o->pp.ns > 0;
    if (!strcmp(kind, "scratch")) {     /* scratched out: the pending text is discarded */
        if (nc && o->pp.pending) {
            discard_pending(o, now);
        } else if (nc) {
            remove_nums(o, cov, nc, now);
        }
        if (o->pp.ns) ask_read(o, now);
        else if (!o->pp.pending) readings_clear(o);
    } else if (!strcmp(kind, "strike")) {
        if (nc) {                       /* struck through: those characters go */
            remove_nums(o, cov, nc, now);
            if (o->pp.ns) {
                ask_read(o, now);
                o->pause_at = now;      /* the new reading shows as soon as it is there */
            } else {
                preedit(o, "");
                readings_clear(o);
            }
        } else if (!others) {
            delete_last_word(o, now);
        } else {
            ask_read(o, now);
        }
    } else if (!strcmp(kind, "caret")) { /* a space */
        int k = f[7][0] == '@' ? atoi(f[7] + 1) : -1;
        if (!others) {
            put_text(o, 0, " ", now);
            after_edit(o, now);
        } else if (o->pp.pending) {
            char t[PAPER_TEXT_MAX + 2];
            space_at(o->pending_text, k, t, sizeof t);
            commit_reading(o, t, NULL, now);
        } else {
            o->caret_at = k < 0 ? 0 : k;
            ask_read(o, now);
        }
    }
    paper_draw(o);
}

static void hw_got_line(void *d, char **f, int n, int64_t now)
{
    struct osk *o = d;
    const char *verb = f[0], *id = f[1];
    if (!strcmp(verb, "stroke")) {
        int i = n >= 4 && !strcmp(f[2], "ok") ? paper_find_id(&o->pp, id) : -1;
        if (i >= 0) o->pp.s[i].num = atoi(f[3]);
        return;
    }
    if (!strcmp(verb, "confirm")) {     /* a lesson kept: the note says so */
        if (!o->confirm_id[0] || strcmp(id, o->confirm_id)) return;
        o->confirm_id[0] = 0;
        if (n >= 4 && !strcmp(f[2], "ok") && o->paper_up) {
            snprintf(o->pp.note, sizeof o->pp.note, "%s", PAPER_NOTE_LEARNED);
            o->paper_note_until = now + PAPER_NOTE_MS;
            paper_draw(o);
        }
        return;
    }
    if (!strcmp(verb, "error")) {
        if (n >= 3 && !strcmp(f[2], "unknown-session")) o->session = false;
        if (o->want_read[0] && !strcmp(id, o->want_read)) o->want_read[0] = 0;
        if (o->confirm_id[0] && !strcmp(id, o->confirm_id)) o->confirm_id[0] = 0;
        return;
    }
    if (!strcmp(verb, "begin")) {
        if (n >= 3 && strcmp(f[2], "ok")) o->session = false;   /* private: nothing kept */
        return;
    }
    if (strcmp(verb, "read") || !o->want_read[0] || strcmp(id, o->want_read)) return;  /* drop, older */
    o->want_read[0] = 0;
    if (n >= 8 && !strcmp(f[2], "gesture")) { gesture(o, f, n, now); return; }
    if (n < 4 || strcmp(f[2], "text")) {   /* empty, or not a reply of this version */
        o->pp.ncand = 0;
        o->read_fresh = true;
        return;
    }
    int k = atoi(f[3]);
    if (k < 0 || n != 4 + 3 * k) return;
    if (o->pp.pending && o->pp.ns > o->pp.pending_n) {
        /* writing on after the reading showed: that reading commits, the new ink is read alone */
        char t[PAPER_TEXT_MAX];
        snprintf(t, sizeof t, "%s", o->pending_text);
        commit_reading(o, t, NULL, now);
        memset(t, 0, sizeof t);
        return;
    }
    int c = 0;
    char cand[PAPER_CANDS][PAPER_TEXT_MAX];
    for (int i = 0; i < k && c < PAPER_CANDS; i++) {
        char *t = f[4 + 3 * i];
        if (!hw_unesc(t) || !*t || strlen(t) >= PAPER_TEXT_MAX) continue;
        bool dup = false;
        for (int j = 0; j < c; j++) dup |= !strcmp(cand[j], t);
        if (!dup) snprintf(cand[c++], PAPER_TEXT_MAX, "%s", t);
    }
    memcpy(o->pp.cand, cand, sizeof cand);
    memset(cand, 0, sizeof cand);
    o->pp.ncand = c;
    o->read_fresh = true;
    if (o->caret_at >= 0 && c) {        /* a caret was waiting for the reading */
        char t[PAPER_TEXT_MAX + 2];
        space_at(o->pp.cand[0], o->caret_at, t, sizeof t);
        o->caret_at = -1;
        commit_reading(o, t, NULL, now);
        return;
    }
    if (o->pp.pending) {                /* a new reading of the same ink (after a strike) */
        if (!c) {
            preedit(o, "");
            o->pp.pending = false;
        } else {
            o->pp.pending_n = o->pp.ns;
            snprintf(o->pending_text, sizeof o->pending_text, "%s", o->pp.cand[0]);
            preedit(o, o->pending_text);
        }
        paper_draw(o);
    } else if (!o->pause_at) {
        show_pending(o);
    }
}

static void hw_stopped(void *d, bool crashed)
{
    struct osk *o = d;
    o->session = false;
    o->confirm_id[0] = 0;
    if (o->want_read[0]) {              /* a reading was on its way: asked again */
        o->want_read[0] = 0;
        o->need_read = o->paper_up && o->pp.ns > 0;
    }
}

static void paper_reset(struct osk *o)
{
    paper_clear(&o->pp);
    readings_clear(o);
    o->pp.fixing = false;
    memset(o->pp.fix, 0, sizeof o->pp.fix);
    o->pp.note[0] = 0;
    o->paper_note_until = 0;
    o->confirm_id[0] = 0;
    o->pbs_down = false;
}

bool osk_paper_open(struct osk *o, int64_t now)
{
    if (!want_recognizer(o)) return false;
    if (o->paper_up) return true;
    paper_reset(o);
    o->paper_up = true;
    session(o, now);                    /* the recognizer starts now: the first reading is quick */
    set_view(o, OSK_VIEW_PAPER);
    return true;
}

bool osk_show_pen(struct osk *o, int64_t now)
{
    return imf_empty(&o->f) && osk_paper_open(o, now);
}

void osk_paper_end(struct osk *o, int64_t now)
{
    if (!o->paper_up) return;
    if (o->pp.pending && o->f.active && o->pending_text[0]) {
        char t[PAPER_TEXT_MAX];
        snprintf(t, sizeof t, "%s", o->pending_text);
        commit_reading(o, t, NULL, now);
        memset(t, 0, sizeof t);
    }
    session_end(o, now);
    paper_reset(o);
    o->paper_up = false;
    refresh(o, now);
}

/* the field went or changed: what was pending can't reach it any more */
static void paper_field(struct osk *o, int r, int64_t now)
{
    if (!o->paper_up) return;
    if (r & IM_FOCUS_OFF) {
        session_end(o, now);
        paper_reset(o);
    }
    if (!want_recognizer(o)) {          /* no field, or a private one: no paper */
        session_end(o, now);
        paper_reset(o);
        o->paper_up = false;
        set_view(o, OSK_VIEW_KEYS);
        return;
    }
    paper_draw(o);
}

void osk_paper_size(struct osk *o, int w, int h)
{
    paper_size(&o->pp, w, h);           /* a session's area follows at its next request */
}

int osk_paper_strokes(const struct osk *o)
{
    return o->pp.ns;
}

void osk_units_text(double per_mm, int rate, char *out, size_t n)
{
    if (per_mm <= 0) {
        snprintf(out, n, "em");
        return;
    }
    /* PER_MM to 4 places in its shortest form (as the samples app writes it), RATE when known */
    char v[32];
    snprintf(v, sizeof v, "%.4f", per_mm);
    char *e = v + strlen(v);
    while (e > v && e[-1] == '0') *--e = 0;
    if (e > v && e[-1] == '.') *--e = 0;
    if (rate > 0) snprintf(out, n, "device:%s:%d", v, rate);
    else snprintf(out, n, "device:%s", v);
}

void osk_paper_scale(struct osk *o, double per_mm)
{
    o->per_mm = per_mm > 0 && per_mm < 1000 ? per_mm : 0;
}

void osk_pen_down_tilt(struct osk *o, double x, double y, double pressure, double tx, double ty, uint32_t t,
                       int64_t now)
{
    if (!o->paper_up || fixing(o) || o->pp.drawing) return;
    o->pause_at = 0;
    struct paper_stroke *st = paper_stroke_new(&o->pp);
    if (!st) return;
    if (o->pp.ns == 1) o->t0 = t;
    st->pressure = pressure >= 0;
    st->tilt = st->pressure && !isnan(tx) && !isnan(ty);
    paper_point_tilt(st, (float)x, (float)y, (float)(pressure < 0 ? 0 : pressure), st->tilt ? (float)tx : 0,
                     st->tilt ? (float)ty : 0, (int32_t)(t - (uint32_t)o->t0));
    o->pp.drawing = true;
}

void osk_pen_motion_tilt(struct osk *o, double x, double y, double pressure, double tx, double ty, uint32_t t)
{
    if (!o->paper_up || !o->pp.drawing || !o->pp.ns) return;
    struct paper_stroke *st = &o->pp.s[o->pp.ns - 1];
    bool tilt = st->tilt && !isnan(tx) && !isnan(ty);
    paper_point_tilt(st, (float)x, (float)y, (float)(pressure < 0 ? 0 : pressure), tilt ? (float)tx : 0,
                     tilt ? (float)ty : 0, (int32_t)(t - (uint32_t)o->t0));
}

void osk_pen_down(struct osk *o, double x, double y, double pressure, uint32_t t, int64_t now)
{
    osk_pen_down_tilt(o, x, y, pressure, NAN, NAN, t, now);
}

void osk_pen_motion(struct osk *o, double x, double y, double pressure, uint32_t t)
{
    osk_pen_motion_tilt(o, x, y, pressure, NAN, NAN, t);
}

void osk_pen_up(struct osk *o, int64_t now)
{
    if (!o->paper_up || !o->pp.drawing) return;
    o->pp.drawing = false;
    int rate = paper_rate(&o->pp);      /* the pen's rate, for the next session's units */
    if (rate > 0) o->rate = rate;
    if (session(o, now)) send_unsent(o, now);
    ask_read(o, now);
    o->pause_at = now + o->pause_ms;
}

static void fix_set(struct osk *o, const char *text)
{
    snprintf(o->pp.fix, sizeof o->pp.fix, "%s", text);
    preedit(o, o->pp.fix);              /* the pending text follows the edit */
    paper_draw(o);
}

static void fix_leave(struct osk *o)
{
    o->pp.fixing = false;
    memset(o->pp.fix, 0, sizeof o->pp.fix);
    preedit(o, o->pending_text);
    set_view(o, OSK_VIEW_PAPER);
    refresh(o, 0);
    paper_draw(o);
}

static void fix_use(struct osk *o, int64_t now)
{
    char t[PAPER_TEXT_MAX];
    const char *s = o->pp.fix;
    while (*s == ' ') s++;
    snprintf(t, sizeof t, "%s", *s ? s : o->pending_text);
    size_t l = strlen(t);
    while (l && t[l - 1] == ' ') t[--l] = 0;
    const char *source = strcmp(t, o->pending_text) ? "corrected" : "confirmed";
    commit_reading(o, t, source, now);
    memset(t, 0, sizeof t);
    set_view(o, OSK_VIEW_PAPER);
    refresh(o, now);
}

/* the paper's Backspace: the pending text's last character (the reading and its ink go
 * with the last one), else, while correcting, the edit's, else the character before the
 * cursor as the keys' Backspace. false: nothing left to delete (a repeat stops) */
static bool paper_backspace(struct osk *o, int64_t now)
{
    if (!o->f.active) return false;
    if (fixing(o)) {
        if (!o->pp.fix[0]) return false;
        char t[PAPER_TEXT_MAX];
        snprintf(t, sizeof t, "%s", o->pp.fix);
        t[tr_prev(t, strlen(t))] = 0;
        fix_set(o, t);
        return true;
    }
    if (o->pp.pending && o->pending_text[0]) {
        size_t l = strlen(o->pending_text);
        o->pending_text[tr_prev(o->pending_text, l)] = 0;
        if (!o->pending_text[0]) {
            discard_pending(o, now);
            if (o->pp.ns) ask_read(o, now);
        } else {
            /* the edited reading is the only one left (the alternatives were for the ink) */
            snprintf(o->pp.cand[0], sizeof o->pp.cand[0], "%s", o->pending_text);
            for (int i = 1; i < PAPER_CANDS; i++) memset(o->pp.cand[i], 0, sizeof o->pp.cand[i]);
            o->pp.ncand = 1;
            o->want_read[0] = 0;        /* a reading still on its way is for the ink, not the edit */
            preedit(o, o->pending_text);
        }
        paper_draw(o);
        return true;
    }
    if (imf_backspace_bytes(&o->f)) {
        if (o->queued || imf_unconfirmed(&o->f)) hold(o, OSK_OP_BACKSPACE, NULL, NULL, now);
        else backspace_now(o, now);
        return true;
    }
    if (o->queued) {
        hold(o, OSK_OP_BACKSPACE, NULL, NULL, now);
        return true;
    }
    if (o->f.surrounding) return false;     /* the field says it is empty before the cursor */
    if (!o->ops->backspaces) return false;
    o->ops->backspaces(o->d, 1);            /* a field without its text: the key */
    imf_key_backspace(&o->f);
    after_edit(o, now);
    return true;
}

void osk_paper_release(struct osk *o)
{
    o->pbs_down = false;
}

void osk_pen_near(struct osk *o, int64_t now)
{
    if (!o->visible || o->paper_up || !want_recognizer(o)) return;
    o->near_at = now + OSK_PEN_NEAR_MS;
}

void osk_pen_on_keys(struct osk *o, bool on)
{
    o->pen_on_keys = on;
}

bool osk_paper_tap(struct osk *o, int x, int y, int64_t now)
{
    struct paper_item it;
    if (!o->paper_up || paper_hit(&o->pp, x, y, &it) < 0) return false;
    switch (it.kind) {
    case PI_CHIP:
        if (o->pp.pending && it.cand >= 0 && it.cand < o->pp.ncand) {
            char t[PAPER_TEXT_MAX];
            snprintf(t, sizeof t, "%s", o->pp.cand[it.cand]);
            /* an alternative picked: a lesson; the read text tapped: as it was read */
            commit_reading(o, t, it.cand ? "picked" : NULL, now);
            memset(t, 0, sizeof t);
        }
        break;
    case PI_FIX:
        if (!o->pp.pending) break;
        o->pp.fixing = true;
        o->pause_at = 0;
        snprintf(o->pp.fix, sizeof o->pp.fix, "%s", o->pending_text);
        set_view(o, OSK_VIEW_PAPER_KEYS);
        refresh(o, now);
        paper_draw(o);
        break;
    case PI_DROP:
        if (!o->pp.pending || fixing(o)) break;
        discard_pending(o, now);
        if (o->pp.ns) ask_read(o, now);
        paper_draw(o);
        break;
    case PI_BACK:
        if (paper_backspace(o, now)) {
            o->pbs_down = true;
            o->pbs_next = now + OSK_REPEAT_DELAY_MS;
        }
        break;
    case PI_USE:
        fix_use(o, now);
        break;
    case PI_KEYS:
        osk_paper_end(o, now);
        set_view(o, OSK_VIEW_KEYS);
        break;
    case PI_CLOSE:
        osk_paper_end(o, now);
        set_view(o, OSK_VIEW_HIDDEN);
        break;
    default:
        break;
    }
    return true;
}

bool osk_key_capture(struct osk *o, uint32_t code, int64_t now)
{
    if (!fixing(o)) return false;
    if (code == KEY_ENTER_ || code == KEY_KPENTER_) fix_use(o, now);
    return true;                        /* no key reaches the app while a reading is corrected */
}

static void paper_tick(struct osk *o, int64_t now)
{
    if (o->near_at && now >= o->near_at) {
        /* the pen came near: over the keys it is there to tap them, elsewhere to write */
        o->near_at = 0;
        if (o->visible && !o->paper_up && !o->pen_on_keys) osk_paper_open(o, now);
    }
    if (!o->paper_up) return;
    if (o->pbs_down && now >= o->pbs_next) {
        if (paper_backspace(o, now)) o->pbs_next = now + OSK_REPEAT_MS;
        else o->pbs_down = false;
    }
    if (o->pause_at && now >= o->pause_at) {
        o->pause_at = 0;
        if (!o->pp.pending) show_pending(o);
    }
    if (o->paper_note_until && now >= o->paper_note_until) {
        o->paper_note_until = 0;
        o->pp.note[0] = 0;
        paper_draw(o);
    }
    if (o->need_read && now >= o->hw.restart_ms) ask_read(o, now);
}
