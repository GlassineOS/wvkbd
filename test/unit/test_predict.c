/* Host test of the predictor client (predict.c) against fake-predict.py: a working
 * predictor, a slow one, one that crashes, one that sends garbage, one that answers with
 * stale ids, one that hangs, one that speaks version 1, a missing one; with
 * GLASSINE_PREDICT set, the real glassine-predict too (no base model: learned words). */
#define _GNU_SOURCE
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../../predict.h"
#include "t.h"

static struct pc_words got;
static int calls;
static int64_t got_ms;
static int64_t now_ms(void);
static void on_words(void *d, const struct pc_words *w) { got = *w; calls++; got_ms = now_ms(); }

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* the keyboard's loop for ms milliseconds */
static void run(struct pc *c, int ms)
{
    int64_t end = now_ms() + ms;
    for (int64_t now; (now = now_ms()) < end;) {
        struct pollfd fds[2];
        int n = pc_poll_fds(c, fds);
        int64_t d = pc_next_deadline(c);
        int64_t until = d >= 0 && d < end ? d : end;
        int wait = until > now ? (int)(until - now) : 0;
        if (n) poll(fds, n, wait); else usleep(wait * 1000);
        now = now_ms();
        if (n) pc_handle(c, fds, n, now);
        pc_tick(c, now);
    }
}

static char *fake[] = { "python3", "-u", NULL, NULL };

static void with_mode(const char *mode)
{
    setenv("FAKE_MODE", mode, 1);
    calls = 0;
    memset(&got, 0, sizeof got);
}

static bool predict(struct pc *c, const char *before, const char *word)
{
    return pc_predict(c, "en", "normal", before, strlen(before), word, strlen(word), now_ms());
}

int main(int argc, char **argv)
{
    static char script[512];
    snprintf(script, sizeof script, "%s/../fake-predict.py", argc > 1 ? argv[1] : ".");
    fake[2] = script;
    struct pc c;

    /* a working predictor */
    with_mode("ok");
    pc_init(&c, fake, on_words, NULL);
    CHECK("not started before the first request", c.pid == 0 && c.restarts == 0);
    int64_t t0 = now_ms();
    CHECK("predict sent", predict(&c, "I say ", "hel"));
    run(&c, 600);
    CHECK_INT("ok: three words", got.n, 3);
    CHECK_STR("ok: the best first", got.word[0], "hela");
    CHECK_STR("ok: its language (v2)", got.lang[0], "xx");
    CHECK_INT("ok: contract version 2", c.version, 2);
    CHECK("ok: answered within 300 ms (the fake starts in that time too)", got_ms - t0 < 300);
    /* only the newest request's answer counts */
    calls = 0;
    predict(&c, "", "a");
    predict(&c, "", "ab");
    run(&c, 300);
    CHECK_STR("newest request wins", got.word[0], "aba");
    CHECK("an older answer was not shown", calls == 1);
    CHECK("learn sent", pc_learn(&c, "en", "normal", "I ", 2, "say", 3, "", now_ms()));
    CHECK("remove sent", pc_remove(&c, "en", "teh", now_ms()));
    run(&c, 200);
    pc_stop(&c);

    /* a slow one: the list empties after PC_SLOW_MS, the late answer to the newest is shown */
    with_mode("slow");
    setenv("FAKE_DELAY", "0.9", 1);
    pc_init(&c, fake, on_words, NULL);
    predict(&c, "", "x");
    t0 = now_ms();
    got.n = 9;
    run(&c, PC_SLOW_MS + 150);
    CHECK_INT("slow: emptied", got.n, 0);
    CHECK("slow: the request never blocked", now_ms() - t0 < PC_SLOW_MS + 400);
    run(&c, 900);
    CHECK_INT("slow: the newest answer still shown when it comes", got.n, 3);
    pc_stop(&c);

    /* a crash: empty, then started again after the backoff */
    with_mode("crash");
    pc_init(&c, fake, on_words, NULL);
    got.n = 9;
    predict(&c, "", "x");
    run(&c, 300);
    CHECK_INT("crash: empty", got.n, 0);
    CHECK("crash: forgotten", c.pid == 0 && c.to < 0);
    CHECK("crash: no restart before the backoff", !predict(&c, "", "y") && c.restarts == 1);
    run(&c, PC_BACKOFF_MS + 50);
    with_mode("ok");
    CHECK("crash: started again after the backoff", predict(&c, "", "y") && c.restarts == 2);
    run(&c, 300);
    CHECK_INT("crash: words again", got.n, 3);
    pc_stop(&c);
    /* repeated crashes: the backoff doubles */
    with_mode("crash");
    pc_init(&c, fake, on_words, NULL);
    predict(&c, "", "x");
    run(&c, 200);
    int b1 = c.backoff_ms;
    run(&c, PC_BACKOFF_MS + 50);
    predict(&c, "", "x");
    run(&c, 200);
    CHECK("crash: the backoff doubles", c.backoff_ms == 2 * b1);
    pc_stop(&c);

    /* garbage: nothing shown, the client keeps going; then an empty list */
    with_mode("garbage");
    pc_init(&c, fake, on_words, NULL);
    calls = 0;
    predict(&c, "", "x");
    run(&c, 200);
    CHECK_INT("garbage: nothing taken as words", calls, 0);
    run(&c, PC_SLOW_MS);
    CHECK("garbage: then empty", calls == 1 && got.n == 0);
    CHECK("garbage: still running", c.pid > 0);
    pc_stop(&c);

    /* stale ids: never shown */
    with_mode("stale");
    pc_init(&c, fake, on_words, NULL);
    calls = 0;
    predict(&c, "", "a");
    run(&c, 100);
    predict(&c, "", "b");
    run(&c, 200);
    CHECK("stale: an older or unknown id is not shown", calls == 0 || got.n == 0);
    pc_stop(&c);

    /* hang: empty after PC_SLOW_MS, stopped and restarted after PC_HUNG_MS */
    with_mode("hang");
    pc_init(&c, fake, on_words, NULL);
    t0 = now_ms();
    predict(&c, "", "x");
    run(&c, PC_SLOW_MS + 100);
    CHECK_INT("hang: empty", got.n, 0);
    int64_t d = pc_next_deadline(&c);
    CHECK("hang: a deadline to give up on it", d > 0 && d - t0 <= PC_HUNG_MS + 50);
    pid_t p = c.pid;
    run(&c, PC_HUNG_MS - PC_SLOW_MS);
    CHECK("hang: stopped", c.pid == 0 && p > 0);
    pc_stop(&c);

    /* version 1 */
    with_mode("v1");
    pc_init(&c, fake, on_words, NULL);
    predict(&c, "", "x");
    run(&c, 300);
    CHECK("v1: words without languages", got.n == 3 && !strcmp(got.word[1], "xb") && !got.lang[0][0]);
    pc_stop(&c);

    /* missing */
    char *missing[] = { "glassine-predict-not-there", NULL };
    pc_init(&c, missing, on_words, NULL);
    CHECK("missing: nothing sent, no crash", !predict(&c, "", "x"));
    pc_stop(&c);

    /* the real predictor, when built (GLASSINE_PREDICT=path): a word learned twice is suggested */
    const char *real = getenv("GLASSINE_PREDICT");
    if (real && *real) {
        static char dir[600];            /* (the Makefile's TMPDIR: inside the checkout) */
        snprintf(dir, sizeof dir, "%s/predict-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : ".");
        if (mkdtemp(dir)) setenv("XDG_DATA_HOME", dir, 1);
        setenv("XDG_CONFIG_HOME", getenv("XDG_DATA_HOME"), 1);
        setenv("GLASSINE_PREDICT_MODELS", getenv("XDG_DATA_HOME"), 1);   /* no model */
        char *rv[] = { (char *)real, NULL };
        pc_init(&c, rv, on_words, NULL);
        pc_learn(&c, "en", "normal", "", 0, "glassine", 8, "", now_ms());
        pc_learn(&c, "en", "normal", "the ", 4, "glassine", 8, "", now_ms());
        predict(&c, "the ", "gla");
        run(&c, 500);
        CHECK_INT("real predictor: contract version 2", c.version, 2);
        CHECK("real predictor: a learned word is suggested", got.n >= 1 && !strcmp(got.word[0], "glassine"));
        CHECK_STR("real predictor: with its language", got.lang[0], "en");
        pc_remove(&c, "en", "glassine", now_ms());
        predict(&c, "the ", "glas");
        run(&c, 500);
        CHECK("real predictor: a removed word is gone", got.n == 0 || strcmp(got.word[0], "glassine"));
        pc_stop(&c);
        usleep(200000);
    } else {
        printf("skip the real predictor (GLASSINE_PREDICT unset)\n");
    }
    return T_DONE();
}
