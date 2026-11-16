/* Host test of the pen on the writing paper (pen.c into osk.c): synthetic pen sequences
 * replayed as the compositor sends them, as a tablet tool (frames) and as the emulated
 * pointer, with the pen hovering before it touches, the digitizer's jump as the tip lifts,
 * pressure and tilt. The strokes must start at the contact point and end at the last
 * report with the tip down (no hook at either end), keep pressure and tilt (tablet), and
 * reach the recognizer in the units the samples app writes (device:PER_MM:RATE). The same
 * sequences through the old pointer handling (the start at the truncated position, the
 * lift-off kept) give the figures to compare: the first and the last step over the
 * stroke's median step, and the strokes that turn back within their first points.
 * All ink here is synthesised. */
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../osk.h"
#include "../../pen.h"
#include "t.h"

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static int backspaces;
static void op_im_send(void *d, uint32_t del, const char *text) { if (del) backspaces++; }
static void op_strip(void *d) {}
static void op_keys(void *d, enum osk_keys k) {}
static void op_shift(void *d, bool on) {}
static void op_hide(void *d) {}
static void op_pen(void *d) {}
static void op_focus(void *d, int s, uint32_t p) {}
static void op_preedit(void *d, uint32_t del, const char *text, const char *p) {}
static void op_backspaces(void *d, int n) { backspaces += n; }
static void op_view(void *d, enum osk_view v) {}
static void op_changed(void *d) {}
static const struct osk_ops ops = {
    .im_send = op_im_send, .strip_changed = op_strip, .keys = op_keys, .shift = op_shift,
    .hide = op_hide, .handwriting = op_pen, .focus = op_focus, .im_preedit = op_preedit,
    .backspaces = op_backspaces, .view = op_view, .paper_changed = op_changed,
};

static char logpath[600], confdir[600], confpath[700], script[600];
static char *fake[] = { "python3", "-u", script, NULL };
static char *missing[] = { "/nonexistent/glassine-handwriting", NULL };
static struct osk o;

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

/* the last stroke request's points (empty: none) */
static const char *last_stroke(void)
{
    static char l[1 << 20], keep[1 << 20];
    keep[0] = 0;
    FILE *f = fopen(logpath, "r");
    if (!f) return keep;
    while (fgets(l, sizeof l, f)) {
        if (strncmp(l, "stroke\t", 7)) continue;
        char *p = strrchr(l, '\t');
        snprintf(keep, sizeof keep, "%s", p ? p + 1 : "");
    }
    fclose(f);
    keep[strcspn(keep, "\n")] = 0;
    return keep;
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
    for (int64_t end2 = now_ms() + 3000; k->hw.waiting > 0 && hw_running(&k->hw) && now_ms() < end2;)
        run(k, 10);
}

static int fresh_rate;                  /* the pen's rate known from earlier ink */
static void fresh(char **cmd, double per_mm)
{
    unlink(logpath);
    setenv("FAKE_MODE", "ok", 1);
    osk_init(&o, &ops, NULL, NULL, true, false);
    osk_handwriting(&o, cmd);
    snprintf(o.langs, sizeof o.langs, "en");
    osk_paper_size(&o, 800, 300);
    osk_paper_scale(&o, per_mm);
    osk_im_activate(&o);
    osk_im_content_type(&o, 0, IM_P_NORMAL);
    osk_im_surrounding(&o, "", 0, 0);
    osk_im_cause(&o, IM_CAUSE_OTHER);
    osk_im_done(&o, now_ms());
    o.rate = fresh_rate;
    osk_paper_open(&o, now_ms());
    backspaces = 0;
}

/* ---- synthetic ink ---- */

static uint32_t rng = 12345;
static double rnd(void)                 /* 0..1 */
{
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 8) & 0xFFFFFF) / (double)0x1000000;
}

#define MAXP 200
struct spt { double x, y, p, tx, ty; uint32_t t; };
struct sstroke {
    int nh;                             /* hover reports before the contact */
    struct spt hover[8];
    int n;                              /* reports with the tip down, the contact first */
    struct spt pt[MAXP];
    struct spt lift;                    /* the report as the tip leaves the glass */
};

static uint32_t clock_t0 = 5000;

/* a letter-like stroke: a curve with eased speed (~333 reports/s, steps of 2-4 ms),
 * pressure rising from the touch and falling to the end, tilt; the pen hovers in from
 * some direction and the digitizer jumps 2-3 median steps as the tip lifts */
static void gen(struct sstroke *s)
{
    memset(s, 0, sizeof *s);
    double x = 120 + rnd() * 520, y = 130 + rnd() * 110;
    double a = rnd() * 2 * M_PI, turn = (rnd() - 0.5) * 0.12, vmax = 0.12 + rnd() * 0.2;    /* px per ms: a letter of ~5 mm */
    double tx0 = 20 + rnd() * 30, ty0 = -20 + rnd() * 40;
    int n = 30 + (int)(rnd() * 60);
    uint32_t t = clock_t0;
    double ha = rnd() * 2 * M_PI;       /* the hover comes in along ha */
    s->nh = 6;
    for (int i = 0; i < s->nh; i++) {
        int k = s->nh - i;
        s->hover[i] = (struct spt){ x - cos(ha) * k * 0.8, y - sin(ha) * k * 0.8, 0, tx0, ty0, t };
        t += 3;
    }
    double steps = 0;
    for (int i = 0; i < n; i++) {
        double u = (double)i / (n - 1);
        s->pt[i] = (struct spt){ x, y, 0.15 + 0.5 * pow(sin(M_PI * u), 0.6), tx0 + 4 * u, ty0 - 3 * u, t };
        uint32_t dt = 2 + (uint32_t)(rnd() * 3);
        double v = 0.03 + vmax * pow(sin(M_PI * u), 0.8);     /* the pen lands, then speeds up */
        x += cos(a) * v * dt;
        y += sin(a) * v * dt;
        steps += v * dt;
        a += turn * dt;
        t += dt;
    }
    s->n = n;
    double med = steps / n, la = rnd() * 2 * M_PI, lj = 2 + rnd();
    struct spt *e = &s->pt[n - 1];
    s->lift = (struct spt){ e->x + cos(la) * lj * med, e->y + sin(la) * lj * med, 0.02, e->tx, e->ty, t };
    clock_t0 = t + 400;
}

/* ---- the replays ---- */

/* the old pointer handling: the press at the truncated position of the latest motion,
 * every motion while pressed (the lift-off's too), no pressure */
static void replay_old(const struct sstroke *s)
{
    int cx = -1, cy = -1;
    for (int i = 0; i < s->nh; i++) { cx = (int)s->hover[i].x; cy = (int)s->hover[i].y; }
    cx = (int)s->pt[0].x;               /* the contact's motion, then the press */
    cy = (int)s->pt[0].y;
    osk_pen_down(&o, cx, cy, -1, s->pt[0].t, now_ms());
    for (int i = 1; i < s->n; i++) osk_pen_motion(&o, s->pt[i].x, s->pt[i].y, -1, s->pt[i].t);
    osk_pen_motion(&o, s->lift.x, s->lift.y, -1, s->lift.t);
    osk_pen_up(&o, now_ms());
}

/* the emulated pointer as sway sends it: a motion at each report (the contact's and the
 * lift-off's with the press's and the release's time), the press and the release */
static void replay_ptr(struct pen *pn, const struct sstroke *s, bool contact_motion)
{
    for (int i = 0; i < s->nh; i++) pen_ptr_motion(pn, s->hover[i].x, s->hover[i].y, s->hover[i].t);
    /* the contact's own motion with the press's time, or hovering onto it before */
    pen_ptr_motion(pn, s->pt[0].x, s->pt[0].y, s->pt[0].t - (contact_motion ? 0 : 1));
    pen_ptr_button(pn, true, s->pt[0].t, now_ms());
    for (int i = 1; i < s->n; i++) pen_ptr_motion(pn, s->pt[i].x, s->pt[i].y, s->pt[i].t);
    pen_ptr_motion(pn, s->lift.x, s->lift.y, s->lift.t);
    pen_ptr_button(pn, false, s->lift.t, now_ms());
    pen_ptr_motion(pn, s->lift.x + 3, s->lift.y - 4, s->lift.t + 3);     /* hover after */
}

static void tool_frame(struct pen *pn, const struct spt *q, bool motion, bool down, bool up)
{
    if (motion) pen_tool_motion(pn, q->x, q->y);
    pen_tool_pressure(pn, q->p);
    pen_tool_tilt(pn, q->tx, q->ty);
    if (down) pen_tool_down(pn);
    if (up) pen_tool_up(pn);
    pen_tool_frame(pn, q->t, now_ms());
}

/* the tablet tool as the compositor sends it: each report a motion with its pressure and
 * tilt, the contact's followed by the down, the lift-off's by the up (one libinput event,
 * so one frame). batch 0: a frame per report; else a busy compositor's frames of 1 to 3
 * reports (all a loop turn handled) */
static void replay_tool_b(struct pen *pn, const struct sstroke *s, bool contact_motion, int batch)
{
    struct spt ev[MAXP + 16];
    char kind[MAXP + 16];               /* r: a report, d: down after it, u: up after it, D: down alone */
    int n = 0;
    for (int i = 0; i < s->nh; i++) {
        ev[n] = s->hover[i];
        ev[n].p = 0;
        kind[n++] = 'r';
    }
    if (!contact_motion) {              /* hovering onto the contact point, then the tip down */
        ev[n] = s->pt[0];
        ev[n].p = 0;
        ev[n].t -= 1;
        kind[n++] = 'r';
        ev[n] = s->pt[0];
        kind[n++] = 'D';
    } else {
        ev[n] = s->pt[0];
        kind[n++] = 'd';
    }
    for (int i = 1; i < s->n; i++) { ev[n] = s->pt[i]; kind[n++] = 'r'; }
    ev[n] = s->lift;
    kind[n++] = 'u';
    ev[n] = s->lift;
    ev[n].x += 3;
    ev[n].t += 3;
    ev[n].p = 0;
    kind[n++] = 'r';
    int left = batch ? 1 + (int)(rnd() * 3) : 1;
    for (int i = 0; i < n; i++) {
        if (kind[i] == 'D') {           /* the touch changes the pressure and tilt, not the position */
            pen_tool_pressure(pn, ev[i].p);
            pen_tool_tilt(pn, ev[i].tx, ev[i].ty);
            pen_tool_down(pn);
        } else {
            pen_tool_motion(pn, ev[i].x, ev[i].y);
            pen_tool_pressure(pn, ev[i].p);
            pen_tool_tilt(pn, ev[i].tx, ev[i].ty);
            if (kind[i] == 'd') pen_tool_down(pn);
            if (kind[i] == 'u') pen_tool_up(pn);
        }
        if (--left == 0 || i == n - 1) {
            pen_tool_frame(pn, ev[i].t, now_ms());
            left = batch ? 1 + (int)(rnd() * 3) : 1;
        }
    }
}

static void replay_tool(struct pen *pn, const struct sstroke *s, bool contact_motion)
{
    replay_tool_b(pn, s, contact_motion, 0);
}

/* a toolkit with tablet support (GTK-style) as an app such as the samples app gets the pen: the press at the contact, a motion per report,
 * the release at the latest motion's position (the lift-off frame's motion goes into the
 * release event) */
static void replay_app(const struct sstroke *s)
{
    osk_pen_down(&o, s->pt[0].x, s->pt[0].y, s->pt[0].p, s->pt[0].t, now_ms());
    for (int i = 1; i < s->n; i++) osk_pen_motion(&o, s->pt[i].x, s->pt[i].y, s->pt[i].p, s->pt[i].t);
    osk_pen_up(&o, now_ms());
}

/* ---- the figures ---- */

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(double *v, int n)
{
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof *v, cmpd);
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static double step(const struct paper_stroke *st, int i)
{
    return hypot(st->pt[i].x - st->pt[i - 1].x, st->pt[i].y - st->pt[i - 1].y);
}

/* the stroke's first and last step over its median step; true: it turns back within its
 * first points (its first step against the way of the next three) */
static bool measure(const struct paper_stroke *st, double *first, double *last)
{
    int n = st->n;
    double d[PAPER_POINTS_MAX];
    for (int i = 1; i < n; i++) d[i - 1] = step(st, i);
    double m = median(d, n - 1);
    *first = m > 0 ? step(st, 1) / m : 0;
    *last = m > 0 ? step(st, n - 1) / m : 0;
    int k = n - 1 < 4 ? n - 1 : 4;
    double ax = st->pt[1].x - st->pt[0].x, ay = st->pt[1].y - st->pt[0].y;
    double bx = st->pt[k].x - st->pt[1].x, by = st->pt[k].y - st->pt[1].y;
    return ax * bx + ay * by < 0;
}

struct figures { double first, last, back; bool hooks; };

#define NSTROKES 200
static struct sstroke ink[NSTROKES];

enum way { OLD, PTR, TOOL, APP };

static struct figures replay_all(enum way w, bool contact_motion)
{
    double f[NSTROKES], l[NSTROKES];
    int back = 0;
    bool hooks = false;
    struct pen pn;
    pen_init(&pn, &o);
    pn.has_pressure = pn.has_tilt = true;
    for (int i = 0; i < NSTROKES; i++) {
        fresh(missing, 4.4749);         /* no readings: the ink stays as written */
        if (w == OLD) replay_old(&ink[i]);
        else if (w == PTR) replay_ptr(&pn, &ink[i], contact_motion);
        else if (w == TOOL) {
            pen_tool_enter(&pn);
            replay_tool(&pn, &ink[i], contact_motion);
            pen_tool_leave(&pn, now_ms());
        } else replay_app(&ink[i]);
        const struct paper_stroke *st = &o.pp.s[0];
        if (measure(st, &f[i], &l[i])) back++;
        if (f[i] > 1.5 || l[i] > 1.5) hooks = true;
        osk_finish(&o);
    }
    return (struct figures){ median(f, NSTROKES), median(l, NSTROKES), 100.0 * back / NSTROKES, hooks || back };
}

static void test_figures(void)
{
    for (int i = 0; i < NSTROKES; i++) gen(&ink[i]);
    struct figures old = replay_all(OLD, true), ptr = replay_all(PTR, true), tool = replay_all(TOOL, true),
                   app = replay_all(APP, true);
    printf("figures (%d synthetic strokes; median over strokes of the step over the stroke's median step):\n",
           NSTROKES);
    printf("  %-26s first %.2f  last %.2f  turn back %.0f %%\n", "before (pointer, old)", old.first, old.last, old.back);
    printf("  %-26s first %.2f  last %.2f  turn back %.0f %%\n", "after: pointer fallback", ptr.first, ptr.last, ptr.back);
    printf("  %-26s first %.2f  last %.2f  turn back %.0f %%\n", "after: tablet tool", tool.first, tool.last, tool.back);
    printf("  %-26s first %.2f  last %.2f  turn back %.0f %%\n", "toolkit model (GTK-style)", app.first, app.last, app.back);
    CHECK("figures: the old handling has the hooks (the replay shows the fault)", old.hooks);
    CHECK("tablet: no stroke starts or ends with a jump over 1.5 median steps, none turns back", !tool.hooks);
    CHECK("pointer: no stroke starts or ends with a jump over 1.5 median steps, none turns back", !ptr.hooks);
    CHECK("tablet: the strokes of the toolkit model (press at the contact, a motion per report, release)",
          fabs(tool.first - app.first) < 1e-9 && fabs(tool.last - app.last) < 1e-9 && tool.back == app.back);
    struct figures ptr_nc = replay_all(PTR, false), tool_nc = replay_all(TOOL, false);
    CHECK("pointer, the press without its own motion: still no hooks", !ptr_nc.hooks);
    CHECK("tablet, the down frame without a motion: still no hooks", !tool_nc.hooks);
}

/* the points themselves: the contact first, the last report with the tip down last */
static void test_points(void)
{
    struct sstroke s;
    gen(&s);
    struct pen pn;
    pen_init(&pn, &o);
    pn.has_pressure = pn.has_tilt = true;

    fresh(missing, 4.4749);
    pen_tool_enter(&pn);
    replay_tool(&pn, &s, true);
    struct paper_stroke *st = &o.pp.s[0];
    CHECK_INT("tablet: one stroke", o.pp.ns, 1);
    CHECK_INT("tablet: every report with the tip down, no hover, no lift-off", st->n, s.n);
    CHECK("tablet: starts at the contact (fractional)",
          st->pt[0].x == (float)s.pt[0].x && st->pt[0].y == (float)s.pt[0].y);
    CHECK("tablet: ends at the last report with the tip down",
          st->pt[st->n - 1].x == (float)s.pt[s.n - 1].x && st->pt[st->n - 1].y == (float)s.pt[s.n - 1].y);
    CHECK("tablet: the pressure kept (the touch's own)", st->pressure && fabsf(st->pt[0].p - 0.15f) < 1e-6);
    CHECK("tablet: the tilt kept", st->tilt && fabsf(st->pt[0].tx - (float)s.pt[0].tx) < 1e-4);
    /* a busy compositor: several reports in a frame, the contact with the next, the last
     * with the lift-off: the same points */
    pen_tool_leave(&pn, now_ms());
    for (int k = 0; k < 20; k++) {
        struct sstroke sb;
        gen(&sb);
        osk_finish(&o);
        fresh(missing, 4.4749);
        struct pen pb;
        pen_init(&pb, &o);
        pb.has_pressure = pb.has_tilt = true;
        pen_tool_enter(&pb);
        replay_tool_b(&pb, &sb, k % 2, 1);
        pen_tool_leave(&pb, now_ms());
        struct paper_stroke *b = o.pp.ns ? &o.pp.s[0] : NULL;
        bool same = o.pp.ns == 1 && b->n == sb.n;
        if (!same) printf("# batch %d: %d strokes, %d points, want %d\n", k, o.pp.ns, b ? b->n : 0, sb.n);
        for (int i = 0; same && i < sb.n; i++)
            same = b->pt[i].x == (float)sb.pt[i].x && b->pt[i].y == (float)sb.pt[i].y &&
                   fabsf(b->pt[i].p - (float)sb.pt[i].p) < 1e-6 && fabsf(b->pt[i].tx - (float)sb.pt[i].tx) < 1e-4;
        for (int i = 0; !same && b && i < sb.n && i < b->n; i++)
            if (b->pt[i].x != (float)sb.pt[i].x || fabsf(b->pt[i].p - (float)sb.pt[i].p) >= 1e-6 ||
                fabsf(b->pt[i].tx - (float)sb.pt[i].tx) >= 1e-4) {
                printf("# batch %d: point %d: %g,%g p %g tx %g, want %g,%g p %g tx %g\n", k, i, b->pt[i].x, b->pt[i].y,
                       b->pt[i].p, b->pt[i].tx, sb.pt[i].x, sb.pt[i].y, sb.pt[i].p, sb.pt[i].tx);
                break;
            }
        if (!same) { CHECK("tablet: frames of several reports give every report with the tip down", false); break; }
        if (k == 19) CHECK("tablet: frames of several reports give every report with the tip down", true);
    }
    osk_finish(&o);
    fresh(missing, 4.4749);
    pen_init(&pn, &o);
    pn.has_pressure = pn.has_tilt = true;
    pen_tool_enter(&pn);
    replay_tool(&pn, &s, true);
    /* hovering on: nothing more */
    for (int i = 0; i < s.nh; i++) tool_frame(&pn, &s.hover[i], true, false, false);
    CHECK_INT("tablet: hover writes nothing", o.pp.ns, 1);
    /* the pen comes as a tool: the pointer the compositor might also send doesn't write */
    struct pen pp;
    pen_init(&pp, &o);
    replay_ptr(&pp, &s, true);
    CHECK_INT("tablet: no second stroke from the pointer while the tool is over the paper", o.pp.ns, 1);
    pen_tool_leave(&pn, now_ms());
    replay_ptr(&pp, &s, true);
    CHECK_INT("pointer: writes again once the tool has left", o.pp.ns, 2);
    st = &o.pp.s[1];
    CHECK_INT("pointer: every report with the tip down, the lift-off held back", st->n, s.n);
    CHECK("pointer: starts at the contact (fractional, not the hover)",
          st->pt[0].x == (float)s.pt[0].x && st->pt[0].y == (float)s.pt[0].y);
    CHECK("pointer: ends at the last report with the tip down",
          st->pt[st->n - 1].x == (float)s.pt[s.n - 1].x && st->pt[st->n - 1].y == (float)s.pt[s.n - 1].y);
    CHECK("pointer: no pressure", !st->pressure && !st->tilt);
    osk_finish(&o);

    /* a release at another time than the last motion: that motion is ink */
    fresh(missing, 4.4749);
    pen_init(&pp, &o);
    pen_ptr_motion(&pp, 100.25, 150.5, 10);
    pen_ptr_button(&pp, true, 10, now_ms());
    pen_ptr_motion(&pp, 101.5, 151, 13);
    pen_ptr_motion(&pp, 103, 152, 16);
    pen_ptr_button(&pp, false, 19, now_ms());
    CHECK_INT("pointer: a release after the last motion keeps it", o.pp.s[0].n, 3);
    osk_finish(&o);

    /* the eraser end doesn't write; a tap on a control taps it */
    fresh(missing, 4.4749);
    pen_init(&pn, &o);
    pn.has_pressure = true;
    pn.eraser = true;
    pen_tool_enter(&pn);
    replay_tool(&pn, &s, true);
    CHECK_INT("tablet: the eraser end doesn't write", o.pp.ns, 0);
    pen_tool_leave(&pn, now_ms());
    pn.eraser = false;
    struct paper_item it[PAPER_ITEMS];
    int n = paper_items(&o.pp, it), bx = 0, by = 0;
    for (int k = 0; k < n; k++) if (it[k].kind == PI_BACK) { bx = it[k].x + 4; by = it[k].y + 4; }
    osk_im_surrounding(&o, "ab", 2, 2);
    osk_im_done(&o, now_ms());
    pen_tool_enter(&pn);
    struct spt q = { bx + 0.5, by + 0.5, 0.3, 0, 0, 100 };
    tool_frame(&pn, &q, true, true, false);
    q.t += 3;
    tool_frame(&pn, &q, true, false, true);
    pen_tool_leave(&pn, now_ms());
    CHECK("tablet: a tap on Backspace is Backspace, no ink", o.pp.ns == 0 && backspaces >= 1);
    osk_finish(&o);
}

/* what reaches the recognizer: px with the screen's size and the pen's rate, the pressure
 * and the tilt with every point (as the samples app sends them) */
static void test_units(void)
{
    struct sstroke s;
    gen(&s);
    struct pen pn;
    pen_init(&pn, &o);
    pn.has_pressure = pn.has_tilt = true;
    fresh(fake, 4.4749);
    run(&o, 1);
    CHECK("units: begun with px per mm in its shortest form", log_has("\tdevice:4.4749\t"));
    pen_tool_enter(&pn);
    replay_tool(&pn, &s, true);
    run(&o, 100);
    const char *pts = last_stroke();
    char first[200];
    snprintf(first, sizeof first, "%s", pts);
    first[strcspn(first, " ")] = 0;
    int commas = 0;
    for (const char *c = first; *c; c++) commas += *c == ',';
    CHECK_INT("units: a point is x,y,t,p,tx,ty", commas, 5);
    char want[64];
    snprintf(want, sizeof want, "%.4f,%.4f,0,0.15,", s.pt[0].x, s.pt[0].y);
    CHECK("units: the first point is the contact with its pressure", !strncmp(first, want, strlen(want)));
    int npts = *pts ? 1 : 0;
    for (const char *c = pts; *c; c++) npts += *c == ' ';
    CHECK_INT("units: the points sent are the stroke's", npts, s.n);
    CHECK("units: the pen's rate measured (~333 reports/s)", o.rate >= 250 && o.rate <= 500);
    pen_tool_leave(&pn, now_ms());
    osk_finish(&o);
    /* the next session: the rate in its units */
    int rate = o.rate;
    fresh_rate = rate;
    fresh(fake, 9);
    fresh_rate = 0;
    run(&o, 1);
    char u[64];
    snprintf(u, sizeof u, "\tdevice:9:%d\t", rate);
    CHECK("units: a later session: device:PER_MM:RATE", log_has(u));
    osk_finish(&o);
    char b[64];
    osk_units_text(4.47494, 0, b, sizeof b);
    CHECK_STR("units: 4 places", b, "device:4.4749");
    osk_units_text(8.5, 333, b, sizeof b);
    CHECK_STR("units: shortest form with the rate", b, "device:8.5:333");
    osk_units_text(0, 333, b, sizeof b);
    CHECK_STR("units: no scale: em", b, "em");
}

/* the paper's own ink is as wide as the compositor's preview draws it */
static void test_width(void)
{
    CHECK("width: no pressure: the base width", paper_ink_at(3, -1) == 3);
    CHECK("width: no pressure on the pen: the brush's minimum", paper_ink_at(3, 0) == 3);
    CHECK("width: full pressure: the brush's maximum", fabs(paper_ink_at(3, 1) - 3 * PAPER_INK_MAX) < 1e-9);
    CHECK("width: linear between (pressure exponent 1)", fabs(paper_ink_at(3, 0.5) - 3 * (1 + (PAPER_INK_MAX - 1) / 2)) < 1e-9);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    snprintf(script, sizeof script, "%s/../fake-recognizer.py", argc > 1 ? argv[1] : ".");
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
    snprintf(logpath, sizeof logpath, "%s/recognizer-pen.log", tmp);
    setenv("FAKE_LOG", logpath, 1);
    snprintf(confdir, sizeof confdir, "%s/config-pen", tmp);
    mkdir(confdir, 0700);
    setenv("XDG_CONFIG_HOME", confdir, 1);
    snprintf(confpath, sizeof confpath, "%s/glassine", confdir);
    mkdir(confpath, 0700);
    test_figures();
    test_points();
    test_units();
    test_width();
    unlink(logpath);
    return T_DONE();
}
