/* Host test of the input method's rules: the text rules (textrules.h), the field's view
 * (im.c), the strip's layout (strip.c) and the keyboard as input method (osk.c) with a
 * stand-in compositor (the osk_ops below record what would go to the app) and
 * fake-predict.py logging every line it reads: private fields send nothing to the
 * predictor (one check per purpose and hint), the purpose picks the keys, a pick
 * replaces the word being typed in one edit, hold-to-remove. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../osk.h"
#include "../../textrules.h"
#include "t.h"

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* ---- the stand-in compositor and app ---- */
static char app[4096];          /* the field's text (the cursor at its end) */
static int edits, strip_draws, keys_calls, shift_calls, hides, pens;
static enum osk_keys last_keys;
static bool shift_on;
static int focus_state = -1;
static char last_send[512];

static void op_im_send(void *d, uint32_t del, const char *text)
{
    size_t n = strlen(app);
    if (del > n) del = n;
    app[n - del] = 0;
    strncat(app, text, sizeof app - strlen(app) - 1);
    snprintf(last_send, sizeof last_send, "%u|%s", del, text);
    edits++;
}
static void op_strip(void *d) { strip_draws++; }
static void op_keys(void *d, enum osk_keys k) { last_keys = k; keys_calls++; }
static void op_shift(void *d, bool on) { shift_on = on; shift_calls++; }
static void op_hide(void *d) { hides++; }
static void op_pen(void *d) { pens++; }
static void op_focus(void *d, int s, uint32_t p) { focus_state = s; }
static const struct osk_ops ops = { op_im_send, op_strip, op_keys, op_shift, op_hide, op_pen, op_focus };

static char logpath[600];
static char *fake[] = { "python3", "-u", NULL, NULL };

static long log_size(void)
{
    struct stat st;
    return stat(logpath, &st) ? 0 : (long)st.st_size;
}

static bool log_has(const char *s)
{
    FILE *f = fopen(logpath, "r");
    if (!f) return false;
    char l[8192];
    bool r = false;
    while (fgets(l, sizeof l, f)) if (strstr(l, s)) r = true;
    fclose(f);
    return r;
}

/* the learn lines for word (its sixth field) in the log */
static int log_learns(const char *word)
{
    FILE *f = fopen(logpath, "r");
    if (!f) return 0;
    char l[8192];
    int n = 0;
    while (fgets(l, sizeof l, f)) {
        if (strncmp(l, "learn\t", 6)) continue;
        char *p = l;
        for (int i = 0; i < 5 && p; i++) if ((p = strchr(p, '\t'))) p++;
        if (!p) continue;
        size_t wl = strcspn(p, "\t\n");
        if (wl == strlen(word) && !strncmp(p, word, wl)) n++;
    }
    fclose(f);
    return n;
}

/* the loop for ms milliseconds */
static void run(struct osk *o, int ms)
{
    int64_t end = now_ms() + ms;
    for (int64_t now; (now = now_ms()) < end;) {
        struct pollfd fds[2];
        int n = osk_poll_fds(o, fds);
        int64_t d = osk_next_deadline(o);
        int64_t until = d >= 0 && d < end ? d : end;
        int wait = until > now ? (int)(until - now) : 0;
        if (n) poll(fds, n, wait); else usleep(wait * 1000);
        now = now_ms();
        if (n) osk_handle(o, fds, n, now);
        osk_tick(o, now);
    }
}

/* the app focuses a field: activate, its content type and (unless -1) its text */
static void focus(struct osk *o, uint32_t purpose, uint32_t hints, const char *text)
{
    osk_im_activate(o);
    osk_im_content_type(o, hints, purpose);
    if (text) {
        snprintf(app, sizeof app, "%s", text);
        osk_im_surrounding(o, app, strlen(app), strlen(app));
    } else {
        app[0] = 0;
    }
    osk_im_cause(o, IM_CAUSE_OTHER);
    osk_im_done(o, now_ms());
}

/* the app reports its text back after an edit */
static void report(struct osk *o, uint32_t purpose, uint32_t hints)
{
    osk_im_content_type(o, hints, purpose);
    osk_im_surrounding(o, app, strlen(app), strlen(app));
    osk_im_done(o, now_ms());
}

static void type(struct osk *o, const char *s, bool reports, uint32_t purpose, uint32_t hints)
{
    for (const char *p = s; *p;) {
        char c[5] = { 0 };
        uint32_t cp;
        int l = tr_dec(p, strlen(p), &cp);
        memcpy(c, p, l);
        p += l;
        osk_text(o, c, now_ms());
        if (reports) report(o, purpose, hints);
    }
}

static void unfocus(struct osk *o)
{
    osk_im_deactivate(o);
    osk_im_done(o, now_ms());
}

static void test_text(void)
{
    const char *s;
    s = "Hello wor";     CHECK_INT("word: the last word", tr_word_start(s, strlen(s)), 6);
    s = "Hello ";        CHECK_INT("word: none after a space", tr_word_start(s, strlen(s)), strlen(s));
    s = "say don'";      CHECK_INT("word: a trailing apostrophe stays", tr_word_start(s, strlen(s)), 4);
    s = "it's";          CHECK_INT("word: an inner apostrophe", tr_word_start(s, strlen(s)), 0);
    s = "x 'tis";        CHECK_INT("word: a leading apostrophe isn't", tr_word_start(s, strlen(s)), 3);
    s = "abc1";          CHECK_INT("word: a digit ends it", tr_word_start(s, strlen(s)), strlen(s));
    s = "Grüße";         CHECK_INT("word: umlauts are letters", tr_word_start(s, strlen(s)), 0);
    s = "ana@exam";      CHECK_INT("word: after @", tr_word_start(s, strlen(s)), 4);
    CHECK("sentence: empty", tr_sentence_start("", 0));
    CHECK("sentence: after '. '", tr_sentence_start("Hi. ", 4));
    CHECK("sentence: not after 'Hi '", !tr_sentence_start("Hi ", 3));
    CHECK("sentence: not right after '.'", !tr_sentence_start("Hi.", 3));
    CHECK("sentence: after a line break", tr_sentence_start("Hi\n", 3));
    uint32_t c;
    tr_dec("ü", 2, &c);
    CHECK("case: ü up is Ü", tr_upper(c) == 0xDC && tr_is_upper(0xDC) && !tr_is_upper(c));
}

static void test_im(void)
{
    struct im_field f;
    imf_init(&f);
    size_t n;
    /* surrounding text: the app's view, an own edit trusted until it is reported */
    imf_activate(&f);
    imf_surrounding(&f, "Hello world", 5, 5);
    imf_content_type(&f, 0, IM_P_NORMAL);
    int r = imf_done(&f, 1000);
    CHECK("im: focus on", r & IM_FOCUS_ON);
    CHECK("im: surrounding text supported", f.surrounding);
    CHECK_STR("im: the text before the cursor", imf_before(&f, &n), "Hello");
    imf_edit(&f, 0, "!", 1000);
    CHECK_STR("im: an own edit counts at once", imf_before(&f, &n), "Hello!");
    imf_edit(&f, 0, "?", 1001);
    imf_surrounding(&f, "Hello! world", 6, 6);         /* the report of the first edit */
    imf_done(&f, 1010);
    CHECK_STR("im: an older report doesn't undo an edit on its way", imf_before(&f, &n), "Hello!?");
    imf_surrounding(&f, "Hello!? world", 7, 7);
    imf_done(&f, 1020);
    CHECK_STR("im: the report of the last edit", imf_before(&f, &n), "Hello!?");
    imf_edit(&f, 0, "x", 2000);
    imf_surrounding(&f, "Hello!? world", 7, 7);         /* never reported: given up later */
    imf_done(&f, 2000 + IM_INFLIGHT_MS + 1);
    CHECK_STR("im: an edit never reported is given up", imf_before(&f, &n), "Hello!?");
    imf_surrounding(&f, "Goodbye", 3, 3);
    imf_cause(&f, IM_CAUSE_OTHER);
    imf_edit(&f, 0, "z", 3000);
    imf_done(&f, 3001);
    CHECK_STR("im: a change by other means wins", imf_before(&f, &n), "Goo");
    imf_surrounding(&f, "Goodbye", 1, 4);
    imf_done(&f, 4000);
    CHECK("im: a selection: Backspace is a key", imf_backspace_bytes(&f) == 0);
    imf_surrounding(&f, "Grüß", 6, 6);
    imf_done(&f, 5000);
    CHECK_INT("im: Backspace deletes one character's bytes", imf_backspace_bytes(&f), 2);
    imf_deactivate(&f);
    CHECK("im: focus off", imf_done(&f, 6000) & IM_FOCUS_OFF);
    CHECK("im: nothing kept after focus off", f.len == 0 && !f.active);

    /* no surrounding text: what was typed since the focus */
    imf_activate(&f);
    imf_content_type(&f, 0, IM_P_NORMAL);
    imf_done(&f, 7000);
    CHECK("im: no surrounding text reported", !f.surrounding);
    imf_edit(&f, 0, "hi th", 7001);
    CHECK_STR("im: own typing", imf_before(&f, &n), "hi th");
    CHECK("im: no surrounding: Backspace stays a key", imf_backspace_bytes(&f) == 0);
    imf_key_backspace(&f);
    CHECK_STR("im: the key's Backspace followed", imf_before(&f, &n), "hi t");
    imf_cause(&f, IM_CAUSE_OTHER);
    imf_done(&f, 7002);
    CHECK_INT("im: changed by other means: own view dropped", f.len, 0);
    imf_edit(&f, 0, "ab", 7003);
    imf_lost(&f);
    CHECK_INT("im: an arrow key: own view dropped", f.len, 0);

    /* field names and the private rule */
    char b[160];
    imf_content_type(&f, IM_H_AUTO_CAPITALIZATION | IM_H_SENSITIVE_DATA, IM_P_NORMAL);
    imf_done(&f, 7004);
    imf_field_name(&f, b, sizeof b);
    CHECK_STR("im: the predictor's FIELD", b, "normal,auto_capitalization,sensitive_data");
    CHECK("im: sensitive is private", imf_private(&f) && !imf_suggests(&f));
    imf_content_type(&f, 0, IM_P_TERMINAL);
    imf_done(&f, 7005);
    CHECK("im: terminal suggests", imf_suggests(&f) && !imf_private(&f));
    imf_content_type(&f, 0, IM_P_NUMBER);
    imf_done(&f, 7006);
    CHECK("im: number: no suggestions, a keypad", !imf_suggests(&f) && imf_keypad(&f));
    imf_content_type(&f, 0, 99);
    imf_done(&f, 7007);
    imf_field_name(&f, b, sizeof b);
    CHECK_STR("im: an unknown purpose", b, "unknown");
    imf_free(&f);
}

static void test_strip(void)
{
    struct strip s = { .w = 800, .h = 50, .pen = true, .mode = STRIP_WORDS };
    struct strip_item it[STRIP_ITEMS];
    int n = strip_items(&s, it);
    CHECK("strip: empty: pen and hide keep their places", n == 2 && it[0].kind == SI_PEN && it[1].kind == SI_HIDE);
    CHECK("strip: pen over the first key column", it[0].x == 0 && it[0].w == 80);
    CHECK("strip: hide over the last", it[1].x == 720 && it[1].w == 80 && it[1].h == 50);
    s.words.n = 3;
    strcpy(s.words.word[0], "best");
    strcpy(s.words.word[1], "second");
    strcpy(s.words.word[2], "third");
    n = strip_items(&s, it);
    CHECK("strip: three words between", n == 5 && it[1].kind == SI_WORD && it[3].kind == SI_WORD);
    CHECK("strip: the likeliest in the middle", it[2].word == 0 && it[2].best && it[1].word == 1 && it[3].word == 2);
    CHECK("strip: words share the width", it[1].x == 80 && it[1].w == 213 && it[3].x + it[3].w == 720);
    s.words.n = 1;
    n = strip_items(&s, it);
    CHECK("strip: one word takes the middle part", n == 3 && it[1].x == 80 && it[1].w == 640);
    s.pen = false;
    n = strip_items(&s, it);
    CHECK("strip: no pen key: its place stays empty", n == 2 && it[0].kind == SI_WORD && it[0].x == 80);
    struct strip_item h;
    CHECK("strip: hit a word", strip_hit(&s, 100, 10, &h) >= 0 && h.kind == SI_WORD);
    CHECK("strip: the empty pen place hits nothing", strip_hit(&s, 10, 10, &h) < 0);
    CHECK("strip: below it hits nothing", strip_hit(&s, 100, 60, &h) < 0);
    s.mode = STRIP_ASK;
    strcpy(s.ask, "teh");
    n = strip_items(&s, it);
    CHECK("strip: asking: the question, Keep, Remove, hide", n == 4 && it[0].kind == SI_ASK_TEXT &&
          it[1].kind == SI_KEEP && it[2].kind == SI_REMOVE && it[3].kind == SI_HIDE && it[2].x + it[2].w == 720);
    char q[200];
    strip_ask_text("teh", q, sizeof q);
    CHECK_STR("strip: the question", q, "Remove “teh” from suggestions?");
    s.mode = STRIP_PRIVATE;
    s.pen = true;
    n = strip_items(&s, it);
    CHECK("strip: private: the lock note, no pen key", n == 2 && it[0].kind == SI_LOCK_NOTE && it[1].kind == SI_HIDE);
    struct strip a = s, b = s;
    CHECK("strip: the same strip needs no redraw", strip_same(&a, &b));
    b.mode = STRIP_WORDS;
    CHECK("strip: a changed one does", !strip_same(&a, &b));
}

static struct osk o;

static void fresh(const char *mode)
{
    setenv("FAKE_MODE", mode, 1);
    unlink(logpath);
    osk_init(&o, &ops, NULL, fake, true, true);
    osk_strip_size(&o, 800, 50);
    osk_visible(&o, true, now_ms());
    osk_languages(&o, "en", now_ms());
    edits = strip_draws = keys_calls = hides = pens = 0;
}

/* FR-007: nothing about a private field reaches the predictor; one field each */
static void private_field(const char *name, uint32_t purpose, uint32_t hints)
{
    char t[160];
    fresh("ok");
    focus(&o, purpose, hints, "");
    type(&o, "secret words, more", true, purpose, hints);
    osk_key(&o, 28, now_ms());                  /* Enter */
    osk_backspace(&o, now_ms());
    osk_strip_down(&o, 300, 10, now_ms());      /* where a word would be */
    run(&o, 50);
    osk_strip_up(&o, now_ms());
    run(&o, 200);
    snprintf(t, sizeof t, "private %s: nothing written to the predictor", name);
    CHECK(t, log_size() == 0 && o.pc.restarts == 0);
    snprintf(t, sizeof t, "private %s: the strip shows the lock note, no pen key", name);
    CHECK(t, o.s.mode == STRIP_PRIVATE && !o.s.pen && o.s.words.n == 0);
    snprintf(t, sizeof t, "private %s: nothing kept of the text", name);
    CHECK(t, o.f.len == 0);
    snprintf(t, sizeof t, "private %s: the text still reaches the app", name);
    CHECK(t, strstr(app, "secret words") != NULL);
    osk_finish(&o);
}

static void test_osk(void)
{
    /* typing in a normal field: committed text, words for the word being typed */
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    CHECK("osk: focus reported", focus_state == 1);
    CHECK("osk: letters for a normal field", last_keys == OSK_LETTERS);
    type(&o, "I say hel", true, IM_P_NORMAL, 0);
    CHECK_STR("osk: typed text committed", app, "I say hel");
    run(&o, 300);
    CHECK("osk: the predictor heard the context", log_has("predict\tk") && log_has("\ten\tnormal\tI say \thel"));
    CHECK_INT("osk: three words on the strip", o.s.words.n, 3);
    CHECK("osk: the strip drawn on its own", strip_draws > 0);
    CHECK("osk: a finished word is learned", log_has("learn\tk") && log_has("\tI \tsay\t"));
    /* a pick: the partial word replaced, a space after, one edit; learned with its language */
    struct strip_item it[STRIP_ITEMS];
    int n = strip_items(&o.s, it), mid = -1;
    for (int i = 0; i < n; i++) if (it[i].kind == SI_WORD && it[i].best) mid = i;
    int e0 = edits;
    osk_strip_down(&o, it[mid].x + 5, 10, now_ms());
    osk_strip_up(&o, now_ms());
    CHECK_STR("osk: the pick replaces the word being typed", app, "I say hela ");
    CHECK_STR("osk: as one edit (delete 3, insert)", last_send, "3|hela ");
    CHECK_INT("osk: one edit only", edits - e0, 1);
    run(&o, 100);
    CHECK("osk: a pick is learned with PICKED", log_has("learn\tk") && log_has("\tI say \thela\txx"));
    report(&o, IM_P_NORMAL, 0);
    /* a capital typed stays */
    type(&o, "Wo", true, IM_P_NORMAL, 0);
    run(&o, 200);
    n = strip_items(&o.s, it);
    for (int i = 0; i < n; i++) if (it[i].kind == SI_WORD && it[i].best) mid = i;
    osk_strip_down(&o, it[mid].x + 5, 10, now_ms());
    osk_strip_up(&o, now_ms());
    CHECK_STR("osk: the picked word keeps the typed capital", app, "I say hela Woa ");
    report(&o, IM_P_NORMAL, 0);
    /* Backspace through the input method, one character */
    osk_backspace(&o, now_ms());
    osk_backspace_up(&o);
    CHECK_STR("osk: Backspace deletes through the input method", last_send, "1|");
    report(&o, IM_P_NORMAL, 0);
    /* hold-to-remove */
    type(&o, " te", true, IM_P_NORMAL, 0);
    run(&o, 200);
    n = strip_items(&o.s, it);
    for (int i = 0; i < n; i++) if (it[i].kind == SI_WORD && it[i].best) mid = i;
    e0 = edits;
    osk_strip_down(&o, it[mid].x + 5, 10, now_ms());
    run(&o, OSK_HOLD_MS + 50);
    CHECK("osk: holding a word asks", o.s.mode == STRIP_ASK && !strcmp(o.s.ask, "tea"));
    osk_strip_up(&o, now_ms());
    CHECK_INT("osk: the hold typed nothing", edits, e0);
    n = strip_items(&o.s, it);
    int keep = -1, rem = -1;
    for (int i = 0; i < n; i++) { if (it[i].kind == SI_KEEP) keep = i; if (it[i].kind == SI_REMOVE) rem = i; }
    osk_strip_down(&o, it[keep].x + 2, 10, now_ms());
    osk_strip_up(&o, now_ms());
    CHECK("osk: Keep: the words again", o.s.mode == STRIP_WORDS && o.s.words.n == 3);
    osk_strip_down(&o, it[mid].x + 5, 10, now_ms());
    run(&o, OSK_HOLD_MS + 50);
    osk_strip_up(&o, now_ms());
    osk_strip_down(&o, it[rem].x + 2, 10, now_ms());
    osk_strip_up(&o, now_ms());
    CHECK("osk: Remove: the note", o.s.mode == STRIP_NOTE && strstr(o.s.note, "Removed “tea”"));
    bool gone = true;
    for (int i = 0; i < o.words.n; i++) if (!strcmp(o.words.word[i], "tea")) gone = false;
    CHECK("osk: Remove: the word gone at once", gone);
    run(&o, 100);
    CHECK("osk: Remove: sent to the predictor", log_has("remove\tk") && log_has("\ten\ttea"));
    run(&o, OSK_NOTE_MS);
    CHECK("osk: the note goes by itself", o.s.mode == STRIP_WORDS);
    /* the hide and pen keys */
    osk_strip_down(&o, 790, 10, now_ms());
    osk_strip_up(&o, now_ms());
    osk_strip_down(&o, 5, 10, now_ms());
    osk_strip_up(&o, now_ms());
    CHECK("osk: the hide and pen keys", hides == 1 && pens == 1);
    /* hidden: nothing asked */
    long sz = log_size();
    osk_visible(&o, false, now_ms());
    type(&o, "x", false, IM_P_NORMAL, 0);
    run(&o, 100);
    CHECK("osk: hidden: the predictor isn't asked", !log_has("\tte\tx\n") && log_size() == sz);
    osk_visible(&o, true, now_ms());
    unfocus(&o);
    CHECK("osk: focus off reported, letters again", focus_state == 0 && last_keys == OSK_LETTERS);
    CHECK("osk: no field: no words", o.s.words.n == 0 && !o.s.pen);
    CHECK("osk: no field: keys stay key events", !osk_text(&o, "a", now_ms()));
    osk_finish(&o);

    /* auto capitalization */
    fresh("ok");
    focus(&o, IM_P_NORMAL, IM_H_AUTO_CAPITALIZATION, "");
    CHECK("osk: a capital at the start", osk_shifted(&o));
    type(&o, "H", true, IM_P_NORMAL, IM_H_AUTO_CAPITALIZATION);
    CHECK("osk: then none", !osk_shifted(&o));
    type(&o, "i. ", true, IM_P_NORMAL, IM_H_AUTO_CAPITALIZATION);
    CHECK("osk: again after a sentence end", osk_shifted(&o));
    unfocus(&o);
    focus(&o, IM_P_NORMAL, 0, "");
    CHECK("osk: no hint: no automatic capital", !osk_shifted(&o));
    osk_finish(&o);

    /* the purpose picks the keys */
    struct { uint32_t p; enum osk_keys k; const char *name; } map[] = {
        { IM_P_NUMBER, OSK_NUMBER, "number: the number pad" }, { IM_P_DIGITS, OSK_NUMBER, "digits: the number pad" },
        { IM_P_PHONE, OSK_PHONE, "phone: the phone pad" }, { IM_P_PIN, OSK_PIN, "pin: the PIN pad" },
        { IM_P_EMAIL, OSK_EMAIL, "email: letters, the address row" }, { IM_P_URL, OSK_URL, "url: letters, the web row" },
        { IM_P_NORMAL, OSK_LETTERS, "normal: letters" }, { IM_P_PASSWORD, OSK_LETTERS, "password: letters" },
        { IM_P_TERMINAL, OSK_LETTERS, "terminal: letters" },
    };
    fresh("ok");
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
        char t[80];
        focus(&o, map[i].p, 0, "");
        snprintf(t, sizeof t, "purpose %s", map[i].name);
        CHECK(t, last_keys == map[i].k);
        unfocus(&o);
    }
    run(&o, 100);
    long sz0 = log_size();
    focus(&o, IM_P_NUMBER, 0, "1");
    type(&o, "23", true, IM_P_NUMBER, 0);
    run(&o, 100);
    CHECK("purpose number: no suggestions asked", log_size() == sz0 && o.s.words.n == 0);
    unfocus(&o);
    /* an address: a pick adds no space */
    focus(&o, IM_P_EMAIL, 0, "ana@exam");
    run(&o, 200);
    n = strip_items(&o.s, it);
    for (int i = 0; i < n; i++) if (it[i].kind == SI_WORD && it[i].best) mid = i;
    osk_strip_down(&o, it[mid].x + 5, 10, now_ms());
    osk_strip_up(&o, now_ms());
    CHECK_STR("email: the pick completes the word, no space", app, "ana@exama");
    osk_finish(&o);

    /* no surrounding text: the own typing is the context */
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, NULL);
    type(&o, "so th", false, IM_P_NORMAL, 0);
    run(&o, 200);
    CHECK("no surrounding: words for what was typed", log_has("\tso \tth") && o.s.words.n == 3);
    osk_finish(&o);

    /* a missing predictor: typing goes on, the strip stays empty */
    char *missing[] = { "glassine-predict-not-there", NULL };
    osk_init(&o, &ops, NULL, missing, true, false);
    osk_strip_size(&o, 800, 50);
    osk_visible(&o, true, now_ms());
    focus(&o, IM_P_NORMAL, 0, "");
    app[0] = 0;
    type(&o, "still types", true, IM_P_NORMAL, 0);
    run(&o, 100);
    CHECK_STR("missing predictor: typing works", app, "still types");
    CHECK("missing predictor: no words, no pen key without a hook", o.s.words.n == 0 && !o.s.pen);
    osk_finish(&o);

    /* FR-007, one check per purpose and hint */
    private_field("password", IM_P_PASSWORD, 0);
    private_field("pin", IM_P_PIN, 0);
    private_field("sensitive_data", IM_P_NORMAL, IM_H_SENSITIVE_DATA);
    private_field("hidden_text", IM_P_NORMAL, IM_H_HIDDEN_TEXT);
    /* a field that turns private while focused: nothing after that */
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    type(&o, "ab", true, IM_P_NORMAL, 0);
    run(&o, 100);
    long before = log_size();
    report(&o, IM_P_NORMAL, IM_H_SENSITIVE_DATA);
    type(&o, "cd ef ", true, IM_P_NORMAL, IM_H_SENSITIVE_DATA);
    run(&o, 200);
    CHECK("private after focus: nothing more written", log_size() == before && !log_has("cd"));
    osk_finish(&o);
}

/* a word is learned once per finish: Backspace back into a learned word (the space
 * after a pick or after a typed word deleted), then Enter or a space, learns nothing more */

/* ---- late reports and picks: what real toolkits send (recorded under sway) ---- */

/* the app reports text (cursor and anchor in bytes) with a cause */
static void say(struct osk *o, const char *text, uint32_t cursor, uint32_t anchor, uint32_t cause)
{
    osk_im_surrounding(o, text, cursor, anchor);
    osk_im_cause(o, cause);
    osk_im_done(o, now_ms());
}

/* tap a word on the strip (the strip set to just this word) */
static void tap_word(struct osk *o, const char *w)
{
    o->s.mode = STRIP_WORDS;
    memset(&o->s.words, 0, sizeof o->s.words);
    snprintf(o->s.words.word[0], PC_WORD_MAX, "%s", w);
    snprintf(o->s.words.lang[0], PC_LANG_MAX, "en");
    o->s.words.n = 1;
    o->held = 0;
    osk_strip_up(o, now_ms());
}

/* the sends since the last call: sync commits ("0|") counted apart */
static int sends_since(int e0)
{
    return edits - e0;
}

static void test_late_reports(void)
{
    size_t n;
    int e0;
    /* GTK: after a pick, a report half-way (the delete done) and then the new text */
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    type(&o, "hello", true, IM_P_NORMAL, 0);
    tap_word(&o, "Hello");
    CHECK_STR("gtk: the pick replaces the typed word", last_send, "5|Hello ");
    say(&o, "", 0, 0, IM_CAUSE_INPUT_METHOD);
    CHECK_STR("gtk: the half-way report is late", imf_before(&o.f, &n), "Hello ");
    say(&o, "Hello ", 6, 6, IM_CAUSE_INPUT_METHOD);
    tap_word(&o, "world");
    CHECK_STR("gtk: a next word: one space between", app, "Hello world ");
    osk_finish(&o);

    /* Firefox: the text from before the edit first (cause input method), then the new
     * text (cause other); after a delete only the old text, twice */
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    const char *steps[] = { "", "h", "he", "hel", "hell", "hello" };
    for (int i = 1; i < 6; i++) {
        char c[2] = { steps[i][i - 1], 0 };
        osk_text(&o, c, now_ms());
        say(&o, steps[i - 1], i - 1, i - 1, IM_CAUSE_INPUT_METHOD);
        say(&o, steps[i], i, i, IM_CAUSE_OTHER);
    }
    snprintf(app, sizeof app, "hello");
    CHECK_STR("firefox: the view follows the typing", imf_before(&o.f, &n), "hello");
    tap_word(&o, "Hello");
    say(&o, "hello", 5, 5, IM_CAUSE_INPUT_METHOD);
    say(&o, "hello", 5, 5, IM_CAUSE_INPUT_METHOD);
    CHECK_STR("firefox: the old text after a pick is late", imf_before(&o.f, &n), "Hello ");
    tap_word(&o, "world");
    CHECK_STR("firefox: pick, then a next word: no word deleted", app, "Hello world ");
    osk_finish(&o);

    /* Chromium: every report one edit behind, the newest text only after the next
     * commit (the sync); with auto capitalization */
    fresh("ok");
    focus(&o, IM_P_NORMAL, IM_H_AUTO_CAPITALIZATION, "");
    osk_text(&o, "H", now_ms());
    e0 = edits;
    say(&o, "", 0, 0, IM_CAUSE_INPUT_METHOD);
    CHECK("chromium: a late report: no capital again", !osk_shifted(&o));
    CHECK_STR("chromium: ... the view kept", imf_before(&o.f, &n), "H");
    CHECK_INT("chromium: ... nothing sent at once", sends_since(e0), 0);
    run(&o, OSK_SYNC_MS + 20);
    CHECK_STR("chromium: ... then a fresh report asked for (an empty commit)", last_send, "0|");
    CHECK_INT("chromium: ... once", sends_since(e0), 1);
    say(&o, "", 0, 0, IM_CAUSE_INPUT_METHOD);
    run(&o, OSK_SYNC_MS + 20);
    CHECK_INT("chromium: late again: asked again, spaced", sends_since(e0), 2);
    say(&o, "H", 1, 1, IM_CAUSE_INPUT_METHOD);
    run(&o, OSK_SYNC_MS + 20);
    CHECK_INT("chromium: the edit arrived: no more asking", sends_since(e0), 2);
    osk_text(&o, "e", now_ms());
    say(&o, "H", 1, 1, IM_CAUSE_INPUT_METHOD);
    say(&o, "He", 2, 2, IM_CAUSE_INPUT_METHOD);
    CHECK_STR("chromium: typing followed", imf_before(&o.f, &n), "He");
    e0 = edits;
    osk_text(&o, "y", now_ms());
    run(&o, IM_INFLIGHT_MS + 50);
    say(&o, "He", 2, 2, IM_CAUSE_INPUT_METHOD);
    run(&o, OSK_SYNC_MS + 20);
    CHECK_INT("chromium: a report after IM_INFLIGHT_MS: the field's text, nothing asked", sends_since(e0), 1);
    CHECK_STR("chromium: ... taken", imf_before(&o.f, &n), "He");
    osk_finish(&o);

    /* a browser address bar: a URL field with an inline completion selected after the
     * typed text (cursor 4, anchor 7) */
    fresh("ok");
    focus(&o, IM_P_URL, 0, "");
    type(&o, "hell", false, IM_P_URL, 0);
    say(&o, "hellweg", 4, 7, IM_CAUSE_INPUT_METHOD);
    CHECK_STR("omnibox: the typed text before the cursor", imf_before(&o.f, &n), "hell");
    snprintf(app, sizeof app, "hell");
    tap_word(&o, "Hello");
    CHECK_STR("omnibox: a pick over a selection: first the text alone, over it", last_send, "0|Hello ");
    snprintf(app, sizeof app, "hellHello ");
    say(&o, "hellweg", 4, 7, IM_CAUSE_INPUT_METHOD);  /* late */
    CHECK_STR("omnibox: ... a late report: nothing more yet", last_send, "0|Hello ");
    run(&o, OSK_SYNC_MS + 20);
    CHECK_STR("omnibox: ... the app asked for its text", last_send, "0|");
    say(&o, "hellHello ", 10, 10, IM_CAUSE_INPUT_METHOD);
    CHECK_STR("omnibox: ... then the typed word and the text replaced by the text", last_send, "10|Hello ");
    CHECK_STR("omnibox: ... the field", app, "Hello ");
    say(&o, "Hello ", 6, 6, IM_CAUSE_INPUT_METHOD);
    tap_word(&o, "World");
    CHECK_STR("omnibox: a next word: one space between", app, "Hello World ");
    osk_finish(&o);

    /* a pick over a selection whose first step the app never reports: no second step */
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    type(&o, "wor", true, IM_P_NORMAL, 0);
    say(&o, "word", 3, 4, IM_CAUSE_INPUT_METHOD);
    tap_word(&o, "world");
    e0 = edits;
    run(&o, OSK_PICK_WAIT_MS + 50);
    CHECK("selection: unreported: no second step", strncmp(last_send, "9|", 2) && o.pick_until == 0);
    osk_finish(&o);

    /* the space rules */
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "Hello");
    osk_text(&o, ",", now_ms());
    report(&o, IM_P_NORMAL, 0);
    tap_word(&o, "world");
    CHECK_STR("space: after a comma one before the word", app, "Hello, world ");
    osk_finish(&o);
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "say (");
    tap_word(&o, "world");
    CHECK_STR("space: none after an opening bracket", app, "say (world ");
    osk_finish(&o);
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "route 66");
    tap_word(&o, "is");
    CHECK_STR("space: a next word after a digit gets one before it", app, "route 66 is ");
    osk_finish(&o);
    fresh("ok");
    focus(&o, IM_P_URL, 0, "github.c");
    tap_word(&o, "com");
    CHECK_STR("space: an address in a URL field: none after", app, "github.com");
    osk_finish(&o);
    fresh("ok");
    focus(&o, IM_P_URL, 0, "https://exam");
    tap_word(&o, "example");
    CHECK_STR("space: a scheme makes it an address", app, "https://example");
    osk_finish(&o);
    fresh("ok");
    focus(&o, IM_P_URL, 0, "weather in ber");
    tap_word(&o, "Berlin");
    CHECK_STR("space: a search in a URL field gets its space", app, "weather in Berlin ");
    osk_finish(&o);
    fresh("ok");
    focus(&o, IM_P_EMAIL, 0, "ana");
    tap_word(&o, "anna");
    CHECK_STR("space: an email field: none", app, "anna");
    osk_finish(&o);
}

static void test_learn_once(void)
{
    struct strip_item it[STRIP_ITEMS];
    int n, mid = -1;
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    type(&o, "I say hel", true, IM_P_NORMAL, 0);
    run(&o, 200);
    n = strip_items(&o.s, it);
    for (int i = 0; i < n; i++) if (it[i].kind == SI_WORD && it[i].best) mid = i;
    osk_strip_down(&o, it[mid].x + 5, 10, now_ms());
    osk_strip_up(&o, now_ms());
    report(&o, IM_P_NORMAL, 0);
    CHECK_STR("learn once: picked", app, "I say hela ");
    osk_backspace(&o, now_ms());
    osk_backspace_up(&o);
    report(&o, IM_P_NORMAL, 0);
    CHECK_STR("learn once: the space deleted", app, "I say hela");
    osk_key(&o, 28, now_ms());
    run(&o, 100);
    CHECK_INT("learn once: a picked word, Backspace, Enter: one learn", log_learns("hela"), 1);
    snprintf(app, sizeof app, "I say hela\n");        /* the app took the Enter */
    report(&o, IM_P_NORMAL, 0);
    /* a typed word, its space deleted, a space again */
    type(&o, "tea ", true, IM_P_NORMAL, 0);
    osk_backspace(&o, now_ms());
    osk_backspace_up(&o);
    report(&o, IM_P_NORMAL, 0);
    type(&o, " ", true, IM_P_NORMAL, 0);
    run(&o, 100);
    CHECK_INT("learn once: a typed word, Backspace, space: one learn", log_learns("tea"), 1);
    /* the same word at another place is another use */
    type(&o, "tea ", true, IM_P_NORMAL, 0);
    run(&o, 100);
    CHECK_INT("learn once: the word again further on: learned again", log_learns("tea"), 2);
    /* the learned word changed: the new word is learned */
    osk_backspace(&o, now_ms());
    osk_backspace_up(&o);
    report(&o, IM_P_NORMAL, 0);
    type(&o, "s ", true, IM_P_NORMAL, 0);
    run(&o, 100);
    CHECK_INT("learn once: a changed word is learned", log_learns("teas"), 1);
    /* the app empties the field (a sent message): the same word there again counts */
    snprintf(app, sizeof app, "%s", "");
    osk_im_cause(&o, IM_CAUSE_OTHER);
    report(&o, IM_P_NORMAL, 0);
    type(&o, "tea ", true, IM_P_NORMAL, 0);
    snprintf(app, sizeof app, "%s", "");
    osk_im_cause(&o, IM_CAUSE_OTHER);
    report(&o, IM_P_NORMAL, 0);
    type(&o, "tea ", true, IM_P_NORMAL, 0);
    run(&o, 100);
    CHECK_INT("learn once: the field emptied by the app, the word again: learned", log_learns("tea"), 4);
    /* no surrounding text: Enter, then the same word: learned again (the place is unknown) */
    unfocus(&o);
    focus(&o, IM_P_NORMAL, 0, NULL);
    type(&o, "tea", false, IM_P_NORMAL, 0);
    osk_key(&o, 28, now_ms());
    type(&o, "tea", false, IM_P_NORMAL, 0);
    osk_key(&o, 28, now_ms());
    run(&o, 100);
    CHECK_INT("learn once: no surrounding text, Enter, the word again: learned", log_learns("tea"), 6);
    /* another field: the mark is gone */
    unfocus(&o);
    focus(&o, IM_P_NORMAL, 0, "");
    type(&o, "I say hela ", true, IM_P_NORMAL, 0);
    run(&o, 100);
    CHECK_INT("learn once: a new focus learns again", log_learns("hela"), 2);
    osk_finish(&o);
}

/* predict.conf's keys for the keyboard: suggestions and handwriting (on when unset) */
static char confdir[600], confpath[700];
static void conf(const char *text)
{
    FILE *fp = fopen(confpath, "w");
    if (fp) { fputs(text, fp); fclose(fp); }
}

static void test_settings(void)
{
    snprintf(confpath, sizeof confpath, "%s/glassine", confdir);
    mkdir(confpath, 0700);
    snprintf(confpath, sizeof confpath, "%s/glassine/predict.conf", confdir);

    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    CHECK("settings: no file: suggestions and the pen key on", o.suggestions && o.handwriting && o.s.pen);
    run(&o, 200);                       /* (its predictor's lines written before the next) */
    osk_finish(&o);

    conf("languages = en,de\nsuggestions = off\n");
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    type(&o, "I say hel", true, IM_P_NORMAL, 0);
    osk_key(&o, 28, now_ms());
    run(&o, 300);
    CHECK("suggestions off: nothing written to the predictor", log_size() == 0 && o.pc.restarts == 0);
    CHECK("suggestions off: an empty strip, the pen key stays", o.s.mode == STRIP_WORDS && o.s.words.n == 0 && o.s.pen);
    CHECK_STR("suggestions off: typing works", app, "I say hel");
    osk_finish(&o);

    conf("  handwriting=FALSE\n");
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    type(&o, "so th", true, IM_P_NORMAL, 0);
    run(&o, 300);
    CHECK("handwriting off: no pen key", !o.s.pen);
    CHECK("handwriting off: suggestions still on", o.s.words.n == 3);
    osk_finish(&o);

    /* the last line of a key wins; read again at the next focus */
    conf("suggestions = 0\nhandwriting = 0\nsuggestions = on\n");
    fresh("ok");
    focus(&o, IM_P_NORMAL, 0, "");
    CHECK("settings: the last line wins", o.suggestions && !o.handwriting);
    conf("suggestions = off\n");
    focus(&o, IM_P_NORMAL, 0, "");
    CHECK("settings: read again at the next focus", !o.suggestions && o.handwriting && o.s.pen);
    osk_finish(&o);
    unlink(confpath);
}

int main(int argc, char **argv)
{
    static char script[512];
    snprintf(script, sizeof script, "%s/../fake-predict.py", argc > 1 ? argv[1] : ".");
    fake[2] = script;
    snprintf(logpath, sizeof logpath, "%s/fake.log", getenv("TMPDIR") ? getenv("TMPDIR") : ".");
    setenv("FAKE_LOG", logpath, 1);
    /* never the real settings: an empty config home */
    snprintf(confdir, sizeof confdir, "%s/config", getenv("TMPDIR") ? getenv("TMPDIR") : ".");
    mkdir(confdir, 0700);
    setenv("XDG_CONFIG_HOME", confdir, 1);
    test_text();
    test_im();
    test_strip();
    test_osk();
    test_late_reports();
    test_learn_once();
    test_settings();
    unlink(logpath);
    return T_DONE();
}
