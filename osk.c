/* glassine: the keyboard as the input method (see osk.h) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "osk.h"
#include "textrules.h"

#define KEY_ENTER_ 28
#define KEY_KPENTER_ 96

static void refresh(struct osk *o, int64_t now);
static void pick_second(struct osk *o, int64_t now);

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
    }
    fclose(fp);
}

void osk_finish(struct osk *o)
{
    pc_stop(&o->pc);
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
    n.pen = o->pen_on && o->handwriting && o->f.active && !imf_private(&o->f);
    if (imf_private(&o->f)) n.mode = STRIP_PRIVATE;
    else if (o->s.ask[0]) n.mode = STRIP_ASK;
    else if (o->s.note[0]) n.mode = STRIP_NOTE;
    else n.mode = STRIP_WORDS;
    if (n.mode == STRIP_WORDS && want_predictor(o) && o->visible) n.words = o->words;
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
    if (r) after_edit(o, now);
    if (o->pick_until && imf_settled(&o->f)) pick_second(o, now);
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

bool osk_text(struct osk *o, const char *text, int64_t now)
{
    if (!o->f.active || !text || !*text) return false;
    uint32_t c;
    tr_dec(text, strlen(text), &c);
    if (tr_separator(c)) finish_word(o, now);
    if (o->ops->im_send) o->ops->im_send(o->d, 0, text);
    imf_edit(&o->f, 0, text, now);
    after_edit(o, now);
    return true;
}

bool osk_backspace(struct osk *o, int64_t now)
{
    if (!o->f.active) return false;
    size_t n = imf_backspace_bytes(&o->f);
    if (!n) {                           /* the key does it: the own view follows */
        imf_key_backspace(&o->f);
        after_edit(o, now);
        return false;
    }
    if (o->ops->im_send) o->ops->im_send(o->d, (uint32_t)n, "");
    imf_edit(&o->f, n, "", now);
    o->bs_down = true;
    o->bs_next = now + OSK_REPEAT_DELAY_MS;
    after_edit(o, now);
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
 * cursor; with none there (a next word) right after a letter, a digit or punctuation
 * that ends something (after_space), a space goes before it. A space follows it, except
 * in an email field (it holds addresses) and in a URL field where the text being typed
 * looks like an address (address_like): a URL field is also where a browser takes a
 * search, and a plain word there gets its space. */
static void pick(struct osk *o, const char *word, const char *lang, int64_t now)
{
    if (!o->f.active || imf_private(&o->f) || !*word) return;
    size_t len, ws = imf_word_start(&o->f);
    const char *b = imf_before(&o->f, &len);
    bool addr = o->f.purpose == IM_P_EMAIL || (o->f.purpose == IM_P_URL && address_like(b, ws));
    bool lead = false;
    if (ws == len && len && !addr) {
        uint32_t c;
        size_t p = tr_prev(b, len);
        tr_dec(b + p, len - p, &c);
        lead = tr_letter(c) || (c >= '0' && c <= '9') || after_space(c);
    }
    char w[PC_WORD_MAX + 1];
    snprintf(w, sizeof w, "%s", word);
    if (ws < len) {                     /* a capital typed stays a capital */
        uint32_t p, c;
        tr_dec(b + ws, len - ws, &p);
        int wl = tr_dec(w, strlen(w), &c);
        char up[4];
        int ul = tr_enc(tr_upper(c), up);
        if (tr_is_upper(p) && !tr_is_upper(c) && ul == wl) memcpy(w, up, ul);
    }
    if (lead) {                         /* learned after the space it gets */
        char ctx[IM_VIEW_MAX + 2];
        memcpy(ctx, b, len);
        ctx[len] = ' ';
        learn(o, ctx, len + 1, w, strlen(w), lang, now);
        memset(ctx, 0, len + 1);
    } else {
        learn(o, b, ws, w, strlen(w), lang, now);
    }
    char text[PC_WORD_MAX + 3];
    snprintf(text, sizeof text, "%s%s%s", lead ? " " : "", w, addr ? "" : " ");
    uint32_t del = (uint32_t)(len - ws);
    if (o->f.surrounding && o->f.selection && del) {
        /* a selection (a browser's address bar selects its own completion after the
         * typed text): apps resolve a delete around a selection differently, but every
         * app replaces a selection with inserted text. So first the text alone, over the
         * selection; once the app reported that, the typed word and the text are deleted
         * and the text inserted again, before a plain cursor */
        if (o->ops->im_send) o->ops->im_send(o->d, 0, text);
        imf_edit(&o->f, 0, text, now);
        snprintf(o->pick_text, sizeof o->pick_text, "%s", text);
        o->pick_del = del + (uint32_t)strlen(text);
        o->pick_until = now + OSK_PICK_WAIT_MS;
        o->sync_at = now + OSK_SYNC_MS;     /* some apps report only when asked */
        after_edit(o, now);
        return;
    }
    if (o->ops->im_send) o->ops->im_send(o->d, del, text);
    imf_edit(&o->f, del, text, now);
    after_edit(o, now);
}

/* the second step of a pick over a selection: the app took the first (its report
 * matches the view, no selection); the view still ends with the typed word and the
 * picked text; otherwise (or after OSK_PICK_WAIT_MS) it is dropped */
static void pick_second(struct osk *o, int64_t now)
{
    size_t len, t = strlen(o->pick_text);
    const char *b = imf_before(&o->f, &len);
    bool ok = o->pick_until && now < o->pick_until && imf_settled(&o->f) && len >= o->pick_del &&
              t <= o->pick_del && !memcmp(b + len - t, o->pick_text, t);
    o->pick_until = 0;
    if (ok) {
        if (o->ops->im_send) o->ops->im_send(o->d, o->pick_del, o->pick_text);
        imf_edit(&o->f, o->pick_del, o->pick_text, now);
        after_edit(o, now);
    }
    memset(o->pick_text, 0, sizeof o->pick_text);
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
        if (o->ops->handwriting) o->ops->handwriting(o->d);
        break;
    case SI_HIDE:
        if (o->ops->hide) o->ops->hide(o->d);
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
    char w[PC_WORD_MAX], l[PC_LANG_MAX];
    snprintf(w, sizeof w, "%s", o->s.words.word[i]);
    snprintf(l, sizeof l, "%s", o->s.words.lang[i]);
    pick(o, w, l, now);
}

int osk_poll_fds(struct osk *o, struct pollfd *fds)
{
    return pc_poll_fds(&o->pc, fds);
}

void osk_handle(struct osk *o, const struct pollfd *fds, int n, int64_t now)
{
    pc_handle(&o->pc, fds, n, now);
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
    if (o->sync_at) d = earliest(d, o->sync_at);
    if (o->pick_until) d = earliest(d, o->pick_until);
    return d;
}

void osk_tick(struct osk *o, int64_t now)
{
    pc_tick(&o->pc, now);
    if (o->sync_at && now >= o->sync_at) {
        /* the app reported late: an empty commit makes it report its newest text (and
         * resolve a following delete against that); again after each late answer, spaced,
         * until the own edits arrive or IM_INFLIGHT_MS passed */
        o->sync_at = 0;
        if (imf_late(&o->f, now) || (o->pick_until && !imf_settled(&o->f))) {
            imf_asked(&o->f);
            if (o->ops->im_send) o->ops->im_send(o->d, 0, "");
            if (o->pick_until) o->sync_at = now + OSK_SYNC_MS;
        }
    }
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
        size_t n = imf_backspace_bytes(&o->f);
        if (!n) { o->bs_down = false; return; }
        if (o->ops->im_send) o->ops->im_send(o->d, (uint32_t)n, "");
        imf_edit(&o->f, n, "", now);
        o->bs_next = now + OSK_REPEAT_MS;
        after_edit(o, now);
        o->bs_down = true;
    }
}
