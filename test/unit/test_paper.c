/* Host test of handwriting (GlassineOS/shell spec 006 FR-010..FR-013a): the recognizer
 * client (hw.c) against fake-recognizer.py (slow, crashing, idle exit, stale replies,
 * garbage, hanging, missing) and the writing paper in osk.c with a stand-in compositor
 * (the osk_ops below keep the field's text and its pending text, as an app would):
 * strokes and reads, the pending text after the pause, writing on, alternatives and the
 * lessons they give, the correction, the gestures, the private-field rule (nothing reaches
 * the recognizer), the keys and put-away controls, focus changes, a restarted recognizer
 * getting the ink again. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../osk.h"
#include "t.h"

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* ---- the stand-in compositor and app ---- */
static char app[4096];          /* the field's text (the cursor at its end) */
static char pre[512];           /* its pending text */
static int edits, backspaces, draws, hides;
static enum osk_view view = OSK_VIEW_KEYS;
static int views;

static void apply(uint32_t del, const char *text)
{
    size_t n = strlen(app);
    if (del > n) del = n;
    app[n - del] = 0;
    strncat(app, text, sizeof app - strlen(app) - 1);
}
static void op_im_send(void *d, uint32_t del, const char *text) { apply(del, text); pre[0] = 0; edits++; }
static void op_strip(void *d) {}
static void op_keys(void *d, enum osk_keys k) {}
static void op_shift(void *d, bool on) {}
static void op_hide(void *d) { hides++; }
static void op_pen(void *d) {}
static void op_focus(void *d, int s, uint32_t p) {}
static void op_preedit(void *d, uint32_t del, const char *text, const char *p)
{
    apply(del, text);
    snprintf(pre, sizeof pre, "%s", p);
    edits++;
}
static void op_backspaces(void *d, int n)
{
    backspaces += n;
    for (int i = 0; i < n; i++) {           /* the app's Backspace: one character */
        size_t l = strlen(app);
        while (l > 0 && (app[l - 1] & 0xC0) == 0x80) l--;
        if (l) app[l - 1] = 0;
    }
}
static void op_view(void *d, enum osk_view v) { view = v; views++; }
static void op_changed(void *d) { draws++; }
static const struct osk_ops ops = {
    .im_send = op_im_send, .strip_changed = op_strip, .keys = op_keys, .shift = op_shift,
    .hide = op_hide, .handwriting = op_pen, .focus = op_focus, .im_preedit = op_preedit,
    .backspaces = op_backspaces, .view = op_view, .paper_changed = op_changed,
};

static char logpath[600], confdir[600], confpath[700], script[600];
static char *fake[] = { "python3", "-u", script, NULL };
static char *missing[] = { "/nonexistent/glassine-handwriting", NULL };
static struct osk o;

static int log_count(const char *verb)
{
    FILE *f = fopen(logpath, "r");
    if (!f) return 0;
    static char l[1 << 20];
    int n = 0;
    size_t vl = strlen(verb);
    while (fgets(l, sizeof l, f)) if (!strncmp(l, verb, vl) && l[vl] == '\t') n++;
    fclose(f);
    return n;
}

static bool log_has(const char *s)
{
    FILE *f = fopen(logpath, "r");
    if (!f) return false;
    static char l[1 << 20];
    bool r = false;
    while (fgets(l, sizeof l, f)) if (strstr(l, s)) r = true;
    fclose(f);
    return r;
}

static long log_size(void)
{
    struct stat st;
    return stat(logpath, &st) ? 0 : (long)st.st_size;
}

static void run(struct osk *k, int ms)
{
    int64_t end = now_ms() + ms;
    for (int64_t now; (now = now_ms()) < end;) {
        struct pollfd fds[2];
        int n = osk_poll_fds(k, fds);
        int64_t d = osk_next_deadline(k);
        int64_t until = d >= 0 && d < end ? d : end;
        int wait = until > now ? (int)(until - now) : 0;
        if (n) poll(fds, n, wait); else usleep(wait * 1000);
        now = now_ms();
        if (n) osk_handle(k, fds, n, now);
        osk_tick(k, now);
    }
}

static void conf(const char *text)
{
    FILE *f = fopen(confpath, "w");
    fputs(text, f);
    fclose(f);
}

static void fresh(const char *mode, char **cmd)
{
    unlink(logpath);
    setenv("FAKE_MODE", mode, 1);
    osk_init(&o, &ops, NULL, NULL, true, false);
    osk_handwriting(&o, cmd);
    snprintf(o.langs, sizeof o.langs, "en");
    osk_paper_size(&o, 800, 300);       /* 1 em = 90 px */
    app[0] = pre[0] = 0;
    edits = backspaces = draws = hides = views = 0;
    view = OSK_VIEW_KEYS;
}

static void focus(uint32_t purpose, uint32_t hints, const char *text, bool surrounding)
{
    osk_im_activate(&o);
    osk_im_content_type(&o, hints, purpose);
    snprintf(app, sizeof app, "%s", text);
    if (surrounding) osk_im_surrounding(&o, app, strlen(app), strlen(app));
    osk_im_cause(&o, IM_CAUSE_OTHER);
    osk_im_done(&o, now_ms());
}

static void unfocus(void)
{
    osk_im_deactivate(&o);
    osk_im_done(&o, now_ms());
}

/* a pen stroke through the points (surface px) */
static void pen(const int *xy, int n)
{
    static uint32_t t = 1000;
    osk_pen_down(&o, xy[0], xy[1], -1, t, now_ms());
    for (int i = 1; i < n; i++) osk_pen_motion(&o, xy[2 * i], xy[2 * i + 1], -1, t += 8);
    osk_pen_up(&o, now_ms());
    t += 100;
}
static void write_letter(int x)      /* an ordinary stroke */
{
    int p[] = { x, 140, x + 20, 190, x + 40, 150 };
    pen(p, 3);
}

/* the chip showing reading i (0 = the best) */
static bool chip(int i, int *x, int *y)
{
    struct paper_item it[PAPER_ITEMS];
    int n = paper_items(&o.pp, it);
    for (int k = 0; k < n; k++)
        if (it[k].kind == PI_CHIP && it[k].cand == i) { *x = it[k].x + 2; *y = it[k].y + 2; return true; }
    return false;
}
static bool item(enum paper_kind kind, int *x, int *y)
{
    struct paper_item it[PAPER_ITEMS];
    int n = paper_items(&o.pp, it);
    for (int k = 0; k < n; k++)
        if (it[k].kind == kind) { *x = it[k].x + 2; *y = it[k].y + 2; return true; }
    return false;
}

#define PAUSE 200                       /* handwriting_pause in the test's predict.conf */

static void test_geometry(void)
{
    struct paper p;
    paper_init(&p);
    paper_size(&p, 800, 300);
    struct paper_item it[PAPER_ITEMS];
    int n = paper_items(&p, it);
    CHECK_INT("geometry: the bare paper has the two controls", n, 2);
    CHECK("geometry: keys then put away, in the top right corner",
          it[0].kind == PI_KEYS && it[1].kind == PI_CLOSE && it[1].x + it[1].w <= 800 && it[1].y < 20);
    p.pending = true;
    p.ncand = 3;
    n = paper_items(&p, it);
    CHECK_INT("geometry: pending: three chips, the pencil, the controls", n, 6);
    CHECK("geometry: the best reading in the middle", it[0].cand == 1 && it[1].cand == 0 && it[2].cand == 2);
    CHECK("geometry: the pencil after the chips", it[3].kind == PI_FIX && it[3].x > it[2].x);
    CHECK("geometry: the row stays clear of the controls", it[3].x + it[3].w <= it[4].x);
    p.fixing = true;
    n = paper_items(&p, it);
    CHECK("geometry: correcting: the edit field and Use", it[0].kind == PI_EDIT && it[1].kind == PI_USE);
    CHECK("geometry: the guide line in the lower part", paper_guide_y(&p) > p.ctl * 2 && paper_guide_y(&p) < 300);
    struct paper_stroke *s = paper_stroke_new(&p);
    paper_point(s, 90, 180, 0, 0);
    paper_point(s, 90, 180, 0, 4);      /* no move: not kept */
    paper_point(s, 180, 90, 0.5, 8);
    size_t len;
    char *t = paper_points_text(&p, s, false, &len);
    CHECK_STR("geometry: points in em (the writing line is 1)", t, "1.0000,2.0000,0 2.0000,1.0000,8");
    free(t);
    paper_free(&p);
}

static void test_client(void)
{
    /* missing: no replies, a backoff, typing unaffected */
    fresh("ok", missing);
    focus(IM_P_NORMAL, 0, "", true);
    CHECK("missing: the paper still opens (ink, no readings)", osk_paper_open(&o, now_ms()));
    write_letter(100);
    run(&o, PAUSE + 100);
    CHECK("missing: no pending text", !o.pp.pending && !pre[0]);
    CHECK("missing: counted as a failure, started again later", o.hw.crashes >= 1 && o.hw.restart_ms > now_ms() - 1000);
    osk_finish(&o);

    /* a stroke longer than the predictor's 64 KB line limit */
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    static int big[2 * 6000];
    for (int i = 0; i < 6000; i++) { big[2 * i] = 50 + i / 10; big[2 * i + 1] = 140 + i % 50; }
    pen(big, 6000);
    run(&o, PAUSE + 300);
    CHECK("a long stroke (> 64 KB): sent whole and read", o.pp.pending && !strcmp(pre, "w1") && log_size() > 65536);
    osk_finish(&o);

    /* the idle exit (status 0): a stop, not a crash; the ink is sent again */
    conf("handwriting_pause = 1000\n");    /* the exit comes before the reading shows */
    fresh("idle", fake);
    setenv("FAKE_IDLE", "0.2", 1);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    run(&o, 500);
    CHECK("idle exit: a stop without a backoff", o.hw.idle_stops == 1 && o.hw.crashes == 0 && !hw_running(&o.hw));
    write_letter(160);
    run(&o, 1200);
    CHECK_INT("idle exit: started again at once", o.hw.restarts, 2);
    CHECK_INT("idle exit: the session begun again", log_count("begin"), 2);
    CHECK_STR("idle exit: the reading has all the ink", pre, "w2");
    conf("handwriting_pause = 200\n");
    osk_finish(&o);

    /* a crash at the read: started again after the backoff, the ink sent again */
    fresh("crash", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    run(&o, 200);
    CHECK("crash: counted, with a backoff", o.hw.crashes == 1 && o.hw.restart_ms > now_ms());
    setenv("FAKE_MODE", "ok", 1);
    run(&o, HW_BACKOFF_MS + PAUSE + 300);
    CHECK("crash: the read owed is asked again after the backoff", o.hw.restarts == 2 && !strcmp(pre, "w1"));
    CHECK_INT("crash: the stroke sent again", log_count("stroke"), 2);
    osk_finish(&o);

    /* slow: nothing blocks; the reading shows when it is there */
    fresh("slow", fake);
    setenv("FAKE_DELAY", "0.6", 1);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    int64_t t0 = now_ms();
    write_letter(100);
    CHECK("slow: the pen never waits", now_ms() - t0 < 50);
    run(&o, PAUSE + 50);
    CHECK("slow: no reading before its answer", !o.pp.pending);
    run(&o, 800);
    CHECK_STR("slow: shown when it arrives", pre, "w1");
    osk_finish(&o);

    /* stale: an answer to an older id is never shown */
    fresh("stale", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    run(&o, 50);
    write_letter(200);
    run(&o, PAUSE + 200);
    CHECK_STR("stale: only the newest read's answer counts", pre, "w2");
    CHECK("stale: no candidate from the old one", strcmp(o.pp.cand[1], "stale") && strcmp(o.pp.cand[0], "stale"));
    osk_finish(&o);

    /* garbage: nothing shown, nothing breaks */
    fresh("garbage", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    run(&o, PAUSE + 300);
    CHECK("garbage: no pending text", !o.pp.pending && !pre[0]);
    CHECK("garbage: still running", hw_running(&o.hw));
    osk_finish(&o);

    /* hang: stopped after HW_HUNG_MS and started again */
    fresh("hang", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    run(&o, HW_HUNG_MS + 200);
    CHECK("hang: stopped as a crash", o.hw.crashes == 1 && !hw_running(&o.hw));
    osk_finish(&o);
}

static void test_writing(void)
{
    int x, y;
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "", true);
    CHECK("offered in a normal field", osk_paper_offered(&o) && osk_field_empty(&o));
    CHECK("a pen tap into the empty field opens the paper", osk_show_pen(&o, now_ms()) && view == OSK_VIEW_PAPER);
    osk_paper_size(&o, 800, 300);
    CHECK("the recognizer starts with the paper", hw_running(&o.hw) && log_count("begin") == 0 /* not written yet */ + 0);
    write_letter(100);
    run(&o, 50);
    CHECK_INT("a stroke is sent at once", log_count("stroke"), 1);
    CHECK_INT("... and read", log_count("read"), 1);
    CHECK("no pending text before the pause", !o.pp.pending && !pre[0]);
    write_letter(160);
    run(&o, PAUSE + 100);
    CHECK_STR("after the pause: the reading is the pending text", pre, "w2");
    CHECK("... the field's text unchanged", !app[0]);
    CHECK("... the ink dims (all of it was read)", o.pp.pending && o.pp.pending_n == 2);
    CHECK("... the best and two alternatives", o.pp.ncand == 3 && !strcmp(o.pp.cand[1], "a2"));
    CHECK("... begun with the field and em units", log_has("begin\th") && log_has("\t-\tnormal\tem\t"));

    /* writing on commits the reading as it was (no lesson) */
    write_letter(300);
    run(&o, 100);
    CHECK_STR("writing on: the reading commits, a space after it", app, "w2 ");
    CHECK_INT("writing on: no lesson", log_count("confirm"), 0);
    CHECK_INT("writing on: only the new ink stays", o.pp.ns, 1);
    CHECK("writing on: the read ink left the session", log_has("remove\t") || log_has("clear\t"));
    run(&o, PAUSE + 100);
    CHECK_STR("writing on: the new ink read alone", pre, "w1");

    /* an alternative: a lesson, with the note when it was kept */
    CHECK("the chips are there", chip(1, &x, &y));
    CHECK("tapping a chip is a control", osk_paper_tap(&o, x, y, now_ms()));
    CHECK_STR("an alternative picked goes into the field", app, "w2 a1 ");
    CHECK("... the pending text gone", !pre[0] && !o.pp.pending);
    run(&o, 100);
    CHECK("... a lesson: confirm picked", log_has("\tpicked\ta1"));
    CHECK_STR("... the note says so", o.pp.note, PAPER_NOTE_LEARNED);
    CHECK_INT("... the ink clears", o.pp.ns, 0);
    run(&o, PAPER_NOTE_MS + 50);
    CHECK("... the note goes", !o.pp.note[0]);

    /* the best reading tapped: committed, no lesson */
    write_letter(100);
    run(&o, PAUSE + 100);
    chip(0, &x, &y);
    osk_paper_tap(&o, x, y, now_ms());
    CHECK_STR("the read text tapped: committed", app, "w2 a1 w1 ");
    CHECK_INT("... no lesson", log_count("confirm"), 1);

    /* the correction: the keys edit it, the pending text follows, Enter uses it */
    write_letter(100);
    write_letter(150);
    run(&o, PAUSE + 100);
    item(PI_FIX, &x, &y);
    osk_paper_tap(&o, x, y, now_ms());
    CHECK("correcting: the paper and the keys", view == OSK_VIEW_PAPER_KEYS && o.pp.fixing);
    CHECK("correcting: the strip shows the readings", o.s.words.n == 3 && !strcmp(o.s.words.word[0], "w2"));
    osk_backspace(&o, now_ms());
    osk_text(&o, "x", now_ms());
    CHECK_STR("correcting: the keys edit the reading", o.pp.fix, "wx");
    CHECK_STR("correcting: the pending text follows", pre, "wx");
    CHECK_STR("correcting: the field unchanged", app, "w2 a1 w1 ");
    CHECK("correcting: other keys are held back", osk_key_capture(&o, 105, now_ms()));
    osk_strip_size(&o, 800, 50);
    osk_strip_down(&o, 400, 25, now_ms());     /* the middle word: the best reading */
    osk_strip_up(&o, now_ms());
    CHECK_STR("correcting: a reading from the strip", o.pp.fix, "w2");
    osk_text(&o, "z", now_ms());
    CHECK("correcting: Enter uses it", osk_key_capture(&o, 28, now_ms()));
    CHECK_STR("... committed", app, "w2 a1 w1 w2z ");
    CHECK("... back to the paper", view == OSK_VIEW_PAPER && !o.pp.fixing);
    run(&o, 100);
    CHECK("... a lesson: corrected", log_has("\tcorrected\tw2z"));
    CHECK_STR("... the note", o.pp.note, PAPER_NOTE_LEARNED);

    /* the keys control: the pending text commits, the keys come */
    write_letter(100);
    run(&o, PAUSE + 100);
    item(PI_KEYS, &x, &y);
    osk_paper_tap(&o, x, y, now_ms());
    CHECK_STR("keys: the pending text commits", app, "w2 a1 w1 w2z w1 ");
    CHECK("keys: the keys come, the paper goes", view == OSK_VIEW_KEYS && !o.paper_up);
    run(&o, 100);
    CHECK("keys: the session ended", log_count("end") == 1);

    /* the pen key in the strip opens it again; put away commits and hides */
    osk_visible(&o, true, now_ms());
    osk_strip_size(&o, 800, 50);
    osk_strip_down(&o, 10, 25, now_ms());
    CHECK("the pen key opens the paper in any field", o.paper_up && view == OSK_VIEW_PAPER);
    osk_visible(&o, false, now_ms());
    write_letter(100);
    run(&o, PAUSE + 100);
    item(PI_CLOSE, &x, &y);
    osk_paper_tap(&o, x, y, now_ms());
    CHECK_STR("put away: the pending text commits", app, "w2 a1 w1 w2z w1 w1 ");
    CHECK("put away: all of it goes, as the hide key", view == OSK_VIEW_HIDDEN && !o.paper_up);
    osk_finish(&o);

    /* a pen tap into a field with text: the keys */
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "hello", true);
    CHECK("a pen tap into a field with text: no paper", !osk_show_pen(&o, now_ms()) && !o.paper_up);
    osk_finish(&o);

    /* an email field: no space after the reading */
    fresh("ok", fake);
    focus(IM_P_EMAIL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    run(&o, PAUSE + 100);
    osk_paper_end(&o, now_ms());
    CHECK_STR("email: the reading without a space", app, "w1");
    osk_finish(&o);

    /* the field goes while text is pending: nothing reaches another field */
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    run(&o, PAUSE + 100);
    edits = 0;
    unfocus();
    CHECK("field gone: the pending text is dropped, not committed", edits == 0 && !o.pp.ns && !o.pp.pending);
    CHECK("field gone: the paper goes, the keys come", !o.paper_up && view == OSK_VIEW_KEYS);
    osk_finish(&o);
}

static void test_gestures(void)
{
    int strike[] = { 60, 160, 300, 161, 560, 160 };
    int scratch[] = { 100, 150, 160, 170, 100, 150, 160, 170, 100, 150, 160, 170 };
    int caret[] = { 140, 200, 160, 160, 180, 200 };

    /* strike-through with nothing on the paper: the last word goes (surrounding text) */
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "see you at", true);
    osk_paper_open(&o, now_ms());
    pen(strike, 3);
    run(&o, 150);
    CHECK_STR("strike, empty paper: the last word deleted", app, "see you ");
    CHECK_INT("... through the input method", backspaces, 0);
    CHECK_INT("... the gesture is no ink", o.pp.ns, 0);
    osk_finish(&o);

    /* no surrounding text: Backspace for the last word the paper committed */
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "", false);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    run(&o, PAUSE + 100);
    osk_paper_end(&o, now_ms());
    osk_paper_open(&o, now_ms());
    CHECK_STR("no surrounding text: the reading committed", app, "w1 ");
    pen(strike, 3);
    run(&o, 150);
    CHECK_INT("strike, no surrounding text: Backspace for that word", backspaces, 3);
    CHECK_STR("... gone from the field", app, "");
    osk_finish(&o);

    /* strike-through over pending ink: those strokes go, the rest is read again */
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    write_letter(620);
    run(&o, PAUSE + 100);
    CHECK_STR("before the strike", pre, "w2");
    pen(strike, 3);
    run(&o, 200);
    CHECK_STR("strike over pending ink: read again without it", pre, "w1");
    CHECK_INT("... one stroke left", o.pp.ns, 1);
    CHECK("... removed from the session", log_has("remove\t"));
    CHECK_STR("... nothing committed", app, "");
    osk_finish(&o);

    /* scratch-out of pending text: discarded */
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(110);
    run(&o, PAUSE + 100);
    pen(scratch, 6);
    run(&o, 200);
    CHECK("scratch over pending text: discarded", !pre[0] && !o.pp.pending && o.pp.ns == 0);
    CHECK_STR("... nothing committed", app, "");
    osk_finish(&o);

    /* caret: a space */
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "ab", true);
    osk_paper_open(&o, now_ms());
    pen(caret, 3);
    run(&o, 150);
    CHECK_STR("caret, empty paper: a space", app, "ab ");
    write_letter(100);
    write_letter(160);
    run(&o, PAUSE + 100);
    pen(caret, 3);
    run(&o, 200);
    CHECK_STR("caret over pending text: a space inside it", app, "ab w 2 ");
    osk_finish(&o);
}

static void test_private(void)
{
    static const struct { uint32_t purpose, hints; const char *name; } priv[] = {
        { IM_P_PASSWORD, 0, "password" }, { IM_P_PIN, 0, "pin" },
        { IM_P_NORMAL, IM_H_SENSITIVE_DATA, "sensitive_data" }, { IM_P_NORMAL, IM_H_HIDDEN_TEXT, "hidden_text" },
    };
    char name[128];
    for (unsigned i = 0; i < sizeof priv / sizeof *priv; i++) {
        fresh("ok", fake);
        focus(priv[i].purpose, priv[i].hints, "", true);
        snprintf(name, sizeof name, "private %s: no paper, not even after a pen tap", priv[i].name);
        CHECK(name, !osk_paper_open(&o, now_ms()) && !osk_show_pen(&o, now_ms()) && !o.paper_up);
        osk_visible(&o, true, now_ms());
        snprintf(name, sizeof name, "private %s: no pen key", priv[i].name);
        CHECK(name, !o.s.pen);
        osk_pen_down(&o, 10, 10, -1, 1, now_ms());
        osk_pen_up(&o, now_ms());
        run(&o, PAUSE);
        snprintf(name, sizeof name, "private %s: the recognizer never started, nothing written", priv[i].name);
        CHECK(name, o.hw.restarts == 0 && log_size() == 0);
        osk_finish(&o);
    }
    /* the field becomes private while the paper is up: the paper goes, nothing more is sent */
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_paper_open(&o, now_ms());
    write_letter(100);
    run(&o, 100);
    long before = log_size();
    osk_im_content_type(&o, 0, IM_P_PASSWORD);
    osk_im_done(&o, now_ms());
    CHECK("turned private: the paper goes", !o.paper_up && view == OSK_VIEW_KEYS);
    osk_pen_down(&o, 10, 10, -1, 1, now_ms());
    osk_pen_up(&o, now_ms());
    run(&o, PAUSE);
    long after = log_size();
    CHECK("turned private: only the session's end was written", after - before < 32 && !log_has("\tpassword"));
    osk_finish(&o);

    /* handwriting off in the settings: no paper, no pen key */
    conf("handwriting = off\n");
    fresh("ok", fake);
    focus(IM_P_NORMAL, 0, "", true);
    osk_visible(&o, true, now_ms());
    CHECK("handwriting off: no paper, no pen key", !osk_paper_open(&o, now_ms()) && !o.s.pen && log_size() == 0);
    osk_finish(&o);
    conf("handwriting_pause = 200\n");
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    snprintf(script, sizeof script, "%s/../fake-recognizer.py", argc > 1 ? argv[1] : ".");
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
    snprintf(logpath, sizeof logpath, "%s/recognizer.log", tmp);
    setenv("FAKE_LOG", logpath, 1);
    snprintf(confdir, sizeof confdir, "%s/config-hw", tmp);
    mkdir(confdir, 0700);
    setenv("XDG_CONFIG_HOME", confdir, 1);
    snprintf(confpath, sizeof confpath, "%s/glassine", confdir);
    mkdir(confpath, 0700);
    snprintf(confpath, sizeof confpath, "%s/glassine/predict.conf", confdir);
    conf("handwriting_pause = 200\n");
    test_geometry();
    test_client();
    test_writing();
    test_gestures();
    test_private();
    unlink(logpath);
    return T_DONE();
}
