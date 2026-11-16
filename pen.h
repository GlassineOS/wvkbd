/* glassine: the pen on the writing paper, from its events to the paper's strokes (osk.c).
 * No Wayland: main.c hands the events over, the host tests replay them.
 *
 * Two ways the pen arrives:
 *  - as a tablet tool (tablet-unstable-v2), events gathered into frames and taken in
 *    their order (a busy compositor puts several reports into one frame). A stroke starts
 *    at the report the down came with (the contact) with its pressure and tilt, takes
 *    every report while the tip is down, and ends at up; the report the up came with is
 *    the lift-off (the digitizer's report as the tip leaves the glass) and is not kept. A
 *    pressure or tilt change without a motion is no point. So the stroke is what a
 *    toolkit with tablet support hands an app (the press at the contact, a motion per
 *    report, the release), close to what the panel engine's live preview draws (its
 *    reports with the tip down).
 *  - as the pointer the compositor emulates for a client without tablet support. Its
 *    press starts the stroke at the latest motion (the contact point, fractional; the
 *    hover before it is not kept); a motion with the release's time is the tip's own
 *    report as it lifts (the compositor sends the lift-off position as a motion right
 *    before the release, with the same time) and is not kept, so it is held back one
 *    event. No pressure on this path.
 * A press on a control taps it instead (osk_paper_tap); an eraser end doesn't write. */
#ifndef PEN_H
#define PEN_H

#include <stdbool.h>
#include <stdint.h>

struct osk;

#define PEN_INK 1          /* new ink to draw (paper_draw_new_ink) */
#define PEN_UP 2           /* a stroke ended */
#define PEN_DOWN 4         /* a stroke began (its drawing starts over) */

#define PEN_FRAME_MAX 32  /* reports kept from one frame */
enum { PEN_EV_MOTION, PEN_EV_DOWN, PEN_EV_UP };
struct pen_ev { int kind; double x, y, p, tx, ty; };

struct pen {
    struct osk *o;
    bool writing;          /* the tip is down on the writing area */
    bool ctl;              /* the press hit a control */
    bool eraser;           /* tablet: the tool is an eraser (it taps, it doesn't write) */
    bool has_pressure, has_tilt;   /* tablet: the tool reports them */
    bool near;             /* tablet: the tool is over the paper */
    /* tablet: the tool's state (the latest values), where it was at the last frame, and the
     * frame being gathered (its events in order; f_*: it has a motion, a down, an up) */
    double x, y, p, tx, ty;
    double lx, ly;
    struct pen_ev f[PEN_FRAME_MAX];
    int nf;
    bool f_motion, f_down, f_up;
    /* pointer: the latest motion, the motion held back while writing */
    double px, py;
    uint32_t pt;
    bool held;
    double hx, hy;
    uint32_t ht;
};

void pen_init(struct pen *pen, struct osk *o);
/* the paper went away or came back: nothing pressed */
void pen_reset(struct pen *pen);

/* tablet tool events (surface px; pressure 0..1; tilt in degrees), applied at the frame.
 * While a tool is over the paper, the pointer doesn't write (no second stroke from it). */
void pen_tool_enter(struct pen *pen);
void pen_tool_motion(struct pen *pen, double x, double y);
void pen_tool_pressure(struct pen *pen, double p);
void pen_tool_tilt(struct pen *pen, double tx, double ty);
void pen_tool_down(struct pen *pen);
void pen_tool_up(struct pen *pen);
int pen_tool_frame(struct pen *pen, uint32_t t, int64_t now);
/* the frame is not the paper's (the tool is over the keys): forgotten */
void pen_tool_drop(struct pen *pen);
/* the tool left the paper (proximity out, the paper hidden): the stroke ends */
int pen_tool_leave(struct pen *pen, int64_t now);

/* the emulated pointer on the paper (ignored while a tool is over it) */
int pen_ptr_motion(struct pen *pen, double x, double y, uint32_t t);
int pen_ptr_button(struct pen *pen, bool down, uint32_t t, int64_t now);
int pen_ptr_leave(struct pen *pen, int64_t now);

#endif
