/* glassine: the pen on the writing paper (see pen.h) */
#include <math.h>
#include <string.h>
#include "osk.h"
#include "pen.h"

static int tools_near;          /* tools over the paper */

void pen_init(struct pen *pen, struct osk *o)
{
    memset(pen, 0, sizeof *pen);
    pen->o = o;
    pen->tx = pen->ty = NAN;
}

void pen_reset(struct pen *pen)
{
    pen->writing = pen->ctl = pen->held = false;
    pen->f_motion = pen->f_down = pen->f_up = false;
    pen->nf = 0;
}

/* the press: a control, else a stroke from (x, y) */
static int press(struct pen *pen, double x, double y, double p, double tx, double ty, uint32_t t,
                 int64_t now, bool writes)
{
    if (pen->writing || pen->ctl) return 0;
    if (osk_paper_tap(pen->o, (int)x, (int)y, now)) {
        pen->ctl = true;
        return 0;
    }
    if (!writes) return 0;
    int ns = osk_paper_strokes(pen->o);
    osk_pen_down_tilt(pen->o, x, y, p, tx, ty, t, now);
    if (osk_paper_strokes(pen->o) == ns) return 0;      /* the paper takes no ink now */
    pen->writing = true;
    return PEN_INK | PEN_DOWN;
}

static int release(struct pen *pen, int64_t now)
{
    int r = 0;
    if (pen->writing) {
        pen->writing = false;
        osk_pen_up(pen->o, now);
        r |= PEN_UP;
    }
    if (pen->ctl) osk_paper_release(pen->o);
    pen->ctl = false;
    return r;
}

/* ---- the tablet tool ---- */

/* the frame's events in their order: a report is a motion with the pressure and tilt that
 * follow it (the compositor sends them so); several reports can share a frame */
static void item(struct pen *pen, int kind)
{
    if (pen->nf < PEN_FRAME_MAX) {
        pen->f[pen->nf] = (struct pen_ev){ kind, pen->x, pen->y, pen->p, pen->tx, pen->ty };
        pen->nf++;
    } else if (kind == PEN_EV_MOTION) {     /* a flood: the frame's latest report replaces its last */
        pen->f[PEN_FRAME_MAX - 1] = (struct pen_ev){ kind, pen->x, pen->y, pen->p, pen->tx, pen->ty };
    }
}

/* the open report (the frame's last, a motion) takes a pressure or tilt sent after it */
static struct pen_ev *open_report(struct pen *pen)
{
    return pen->nf && pen->f[pen->nf - 1].kind == PEN_EV_MOTION ? &pen->f[pen->nf - 1] : NULL;
}

void pen_tool_motion(struct pen *pen, double x, double y)
{
    pen->x = x;
    pen->y = y;
    pen->f_motion = true;
    item(pen, PEN_EV_MOTION);
}

void pen_tool_pressure(struct pen *pen, double p)
{
    pen->p = p < 0 ? 0 : p > 1 ? 1 : p;
    struct pen_ev *e = open_report(pen);
    if (e) e->p = pen->p;
}

void pen_tool_tilt(struct pen *pen, double tx, double ty)
{
    pen->tx = tx;
    pen->ty = ty;
    struct pen_ev *e = open_report(pen);
    if (e) { e->tx = tx; e->ty = ty; }
}

void pen_tool_down(struct pen *pen)
{
    pen->f_down = true;
    item(pen, PEN_EV_DOWN);
}

void pen_tool_up(struct pen *pen)
{
    pen->f_up = true;
    item(pen, PEN_EV_UP);
}

/* the frame: its reports in order with the frame's time. The tip's down starts the stroke
 * at the report it came with (the contact; without one, where the tool is), a report with
 * the tip down is a point, and the report the up came with (the lift-off) is not kept. */
int pen_tool_frame(struct pen *pen, uint32_t t, int64_t now)
{
    int r = 0;
    bool tilt = pen->has_tilt && pen->has_pressure;
    /* the position before the frame's first report: where the tool was */
    double cx = pen->lx, cy = pen->ly;
    for (int i = 0; i < pen->nf; i++) {
        struct pen_ev *e = &pen->f[i];
        double p = pen->has_pressure ? e->p : -1;
        double tx = tilt && !isnan(e->tx) && !isnan(e->ty) ? e->tx : NAN;
        double ty = isnan(tx) ? NAN : e->ty;
        switch (e->kind) {
        case PEN_EV_MOTION:
            cx = e->x;
            cy = e->y;
            if (pen->writing && !(i + 1 < pen->nf && pen->f[i + 1].kind == PEN_EV_UP)) {
                osk_pen_motion_tilt(pen->o, e->x, e->y, p, tx, ty, t);
                r |= PEN_INK;
            }
            break;
        case PEN_EV_DOWN:               /* where the tool is, with the pressure and tilt it has then */
            r |= press(pen, cx, cy, p, tx, ty, t, now, !pen->eraser);
            break;
        case PEN_EV_UP:
            r |= release(pen, now);
            break;
        }
    }
    pen->lx = pen->x;
    pen->ly = pen->y;
    pen->nf = 0;
    pen->f_motion = pen->f_down = pen->f_up = false;
    return r;
}

void pen_tool_drop(struct pen *pen)
{
    pen->lx = pen->x;
    pen->ly = pen->y;
    pen->nf = 0;
    pen->f_motion = pen->f_down = pen->f_up = false;
}

void pen_tool_enter(struct pen *pen)
{
    if (!pen->near) tools_near++;
    pen->near = true;
    pen->writing = pen->ctl = false;    /* the frame being gathered stays (it may hold the down) */
}

int pen_tool_leave(struct pen *pen, int64_t now)
{
    if (pen->near) tools_near--;
    pen->near = false;
    pen->f_motion = pen->f_down = pen->f_up = false;
    pen->nf = 0;
    return release(pen, now);
}

/* ---- the emulated pointer ---- */

static int commit_held(struct pen *pen)
{
    if (!pen->held) return 0;
    pen->held = false;
    osk_pen_motion(pen->o, pen->hx, pen->hy, -1, pen->ht);
    return PEN_INK;
}

int pen_ptr_motion(struct pen *pen, double x, double y, uint32_t t)
{
    int r = 0;
    if (tools_near) return 0;
    pen->px = x;
    pen->py = y;
    pen->pt = t;
    if (!pen->writing) return 0;            /* hover: not kept */
    r |= commit_held(pen);
    pen->held = true;
    pen->hx = x;
    pen->hy = y;
    pen->ht = t;
    return r;
}

int pen_ptr_button(struct pen *pen, bool down, uint32_t t, int64_t now)
{
    if (tools_near && !pen->writing && !pen->ctl) return 0;
    if (down) {
        pen->held = false;
        return press(pen, pen->px, pen->py, -1, NAN, NAN, t, now, true);
    }
    int r = 0;
    /* a motion at the release's own time: the lift-off, not kept */
    if (pen->held && pen->ht != t) r |= commit_held(pen);
    pen->held = false;
    return r | release(pen, now);
}

int pen_ptr_leave(struct pen *pen, int64_t now)
{
    int r = commit_held(pen);
    return r | release(pen, now);
}
