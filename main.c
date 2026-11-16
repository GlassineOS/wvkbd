#include "proto/virtual-keyboard-unstable-v1-client-protocol.h"
#include "proto/wlr-layer-shell-unstable-v1-client-protocol.h"
#include "proto/xdg-shell-client-protocol.h"
#include "proto/fractional-scale-v1-client-protocol.h"
#include "proto/viewporter-client-protocol.h"
#include "proto/input-method-unstable-v2-protocol.h"
#include "proto/glassine-ink-v1-client-protocol.h"
#include "proto/tablet-unstable-v2-client-protocol.h"
#include <errno.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wchar.h>

#include "keyboard.h"
#include "config.h"
#include "paperdraw.h"
#include "pen.h"

/* lazy die macro */
#define die(...)                                                               \
    fprintf(stderr, __VA_ARGS__);                                              \
    exit(1)

/* array size */
#define countof(x) (sizeof(x) / sizeof(*x))

/* client state */
static const char *namespace = "wvkbd";
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_touch *touch;
static struct wl_region *empty_region;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct xdg_wm_base *wm_base;
static struct xdg_surface *popup_xdg_surface;
static struct xdg_popup *popup_xdg_popup;
static struct xdg_positioner *popup_xdg_positioner;
static struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;
static struct wp_fractional_scale_v1 *wfs_draw_surf;
static struct wp_fractional_scale_manager_v1 *wfs_mgr;
static struct wp_viewport *draw_surf_viewport, *popup_draw_surf_viewport;
static struct wp_viewporter *viewporter;
static struct zwp_input_method_manager_v2 *im_mgr;
/* glassine: the keyboard as the input method (osk.h) */
static struct zwp_input_method_v2 *input_method;
static bool im_mode;                    /* --im: commit text, the strip, the predictor */
static struct osk osk;
static int64_t im_retry_at = -1;        /* another input method had the seat: try again */
static int im_retry_ms = 1000;
static bool strip_on;                   /* --strip */
static int strip_height = -1;           /* --strip-height (-1: a key row's) */
static char *predictor_cmd;             /* --predictor */
static char *notify_path;               /* --notify: focus reports to the session */
static char *hide_cmd, *pen_cmd;        /* --hide-cmd, --pen-cmd */
static bool strip_touch;                /* the touch or press began in the strip */
/* glassine: the writing paper (paper.h): the input method's popup at the field's cursor */
static char *hw_cmd;                    /* --handwriting: the recognizer */
static char *pen_flag;                  /* --pen-flag: the session's mark of a pen tap */
static int paper_height = -1;           /* --paper-height (-1: the keys' height) */
static struct glassine_ink_manager_v1 *ink_mgr;
static struct wl_surface *paper_surf;
static struct zwp_input_popup_surface_v2 *paper_popup;
static struct glassine_ink_surface_v1 *paper_ink;
static struct wp_viewport *paper_viewport;
static struct drwbuf paper_buf_a, paper_buf_b;
static struct drwsurf paper_ds;
static int paper_drawn;                 /* points of the stroke being written drawn */
static struct pen ptr_pen;              /* the pen as the emulated pointer on the paper (pen.c) */
static struct zwp_tablet_manager_v2 *tablet_mgr;
static struct zwp_tablet_seat_v2 *tablet_seat;
/* a tablet tool (the pen's tip, its eraser end): the surface it is over, its paper state */
struct kbd_tool {
    struct zwp_tablet_tool_v2 *t;
    struct wl_surface *surf, *entering;
    bool leaving;
    bool on_keys;                       /* it is the pointer on the keys (or the strip) */
    struct pen pen;
};
#define MAX_TOOLS 4
static struct kbd_tool tools[MAX_TOOLS];

static void
paper_pens_reset(void)
{
    pen_reset(&ptr_pen);
    for (int i = 0; i < MAX_TOOLS; i++) pen_reset(&tools[i].pen);
}
static struct wl_surface *ptr_surface;  /* the surface the pointer is on */
static uint32_t paper_touches;          /* touch ids (< 32) that began on the paper */
static void paper_show(void);
static void paper_hide(void);
static bool popup_xdg_surface_configured;
static bool layer_surface_configured;

static uint32_t available_width, available_height = 0;
static void refresh_available_dimension();

/* drawing */
static struct drw draw_ctx;
static struct drwbuf draw_surf_back_buffer, draw_surf_display_buffer, popup_draw_surf_back_buffer, popup_draw_surf_display_buffer;
static struct drwsurf draw_surf, popup_draw_surf;

/* layer surface parameters */
static uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
static uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

/* application state */
static bool run_display = true;
static int cur_x = -1, cur_y = -1;
static bool cur_press = false;
static struct kbd keyboard;
static uint32_t height, normal_height, landscape_height;
static int rounding = DEFAULT_ROUNDING;
static bool hidden = false;
static bool im_auto = false;
static uint32_t custom_width = 0;
static double width_ratio = 1.0;
static enum { DOCK_FULL = 0, DOCK_LEFT = 1, DOCK_RIGHT = 2 } dock_mode = DOCK_FULL;
static uint8_t bg_alpha = 0;
static bool bg_alpha_defined = false;
static int corner_radius = -1;



/* event handler prototypes */
static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                             uint32_t serial, struct wl_surface *surface,
                             wl_fixed_t surface_x, wl_fixed_t surface_y);
static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                             uint32_t serial, struct wl_surface *surface);
static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
                              uint32_t time, wl_fixed_t surface_x,
                              wl_fixed_t surface_y);
static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                              uint32_t serial, uint32_t time, uint32_t button,
                              uint32_t state);
static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                            uint32_t time, uint32_t axis, wl_fixed_t value);

static void wl_touch_down(void *data, struct wl_touch *wl_touch,
                          uint32_t serial, uint32_t time,
                          struct wl_surface *surface, int32_t id, wl_fixed_t x,
                          wl_fixed_t y);
static void wl_touch_up(void *data, struct wl_touch *wl_touch, uint32_t serial,
                        uint32_t time, int32_t id);
static void wl_touch_motion(void *data, struct wl_touch *wl_touch,
                            uint32_t time, int32_t id, wl_fixed_t x,
                            wl_fixed_t y);
static void wl_touch_frame(void *data, struct wl_touch *wl_touch);
static void wl_touch_cancel(void *data, struct wl_touch *wl_touch);
static void wl_touch_shape(void *data, struct wl_touch *wl_touch, int32_t id,
                           wl_fixed_t major, wl_fixed_t minor);
static void wl_touch_orientation(void *data, struct wl_touch *wl_touch,
                                 int32_t id, wl_fixed_t orientation);

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
                                     enum wl_seat_capability caps);
static void seat_handle_name(void *data, struct wl_seat *wl_seat,
                             const char *name);

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface,
                          uint32_t version);
static void handle_global_remove(void *data, struct wl_registry *registry,
                                 uint32_t name);

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t w, uint32_t h);
static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface);
static void im_activate(void *data, struct zwp_input_method_v2 *zwp_input_method_v2);
static void im_deactivate(void *data, struct zwp_input_method_v2 *zwp_input_method_v2);
static void im_surrounding_text(void *data, struct zwp_input_method_v2 *zwp_input_method_v2,
                                const char *text, uint32_t cursor, uint32_t anchor);
static void im_text_change_cause(void *data, struct zwp_input_method_v2 *zwp_input_method_v2,
                                 uint32_t cause);
static void im_content_type(void *data, struct zwp_input_method_v2 *zwp_input_method_v2,
                            uint32_t hint, uint32_t purpose);
static void im_done(void *data, struct zwp_input_method_v2 *zwp_input_method_v2);
static void im_unavailable(void *data, struct zwp_input_method_v2 *zwp_input_method_v2);
static void redimension_keyboard();
static void show();
static void hide();

/* event handlers */
static const struct zwp_input_method_v2_listener input_method_listener = {
    .activate = im_activate,
    .deactivate = im_deactivate,
    .surrounding_text = im_surrounding_text,
    .text_change_cause = im_text_change_cause,
    .content_type = im_content_type,
    .done = im_done,
    .unavailable = im_unavailable,
};

static const struct wl_pointer_listener pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
    .axis = wl_pointer_axis,
};

static const struct wl_touch_listener touch_listener = {
    .down = wl_touch_down,
    .up = wl_touch_up,
    .motion = wl_touch_motion,
    .frame = wl_touch_frame,
    .cancel = wl_touch_cancel,
    .shape = wl_touch_shape,
    .orientation = wl_touch_orientation,
};

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

/* glassine: the outputs' physical size, for the writing paper's scale (px per mm) */
struct kbd_output {
    struct wl_output *o;
    uint32_t name;
    int32_t mm_w, mm_h;           /* the physical size (0: not known) */
    int32_t px_w, px_h;           /* the current mode */
    int32_t scale;
};
static struct kbd_output outputs[8];
static int noutputs;
static struct wl_output *kbd_on_output;  /* the output the keys or the paper are on */

static struct kbd_output *
output_of(struct wl_output *o)
{
    for (int i = 0; i < noutputs; i++)
        if (outputs[i].o == o) return &outputs[i];
    return NULL;
}

static void
output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t mm_w, int32_t mm_h,
                int32_t subpixel, const char *make, const char *model, int32_t transform)
{
    struct kbd_output *k = output_of(o);
    if (k) { k->mm_w = mm_w; k->mm_h = mm_h; }
}

static void
output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
    struct kbd_output *k = output_of(o);
    if (k && (flags & WL_OUTPUT_MODE_CURRENT)) { k->px_w = w; k->px_h = h; }
}

static void paper_scale_update(void);

static void output_done(void *data, struct wl_output *o) { paper_scale_update(); }

static void
output_scale(void *data, struct wl_output *o, int32_t scale)
{
    struct kbd_output *k = output_of(o);
    if (k) k->scale = scale;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

/* the writing paper's surface px per mm (0: not known). $GLASSINE_PAPER_PER_MM gives it for
 * a screen that doesn't report its size (and for tests). Else the output the keys or the
 * paper are on, or the first that reports its size: its pixels per mm (the same in any
 * turn: sqrt of the pixels over the area, as the samples app reckons it) over the scale. */
static double
paper_per_mm(void)
{
    const char *e = getenv("GLASSINE_PAPER_PER_MM");
    if (e && *e) {
        double v = strtod(e, NULL);
        if (v > 0) return v;
    }
    struct kbd_output *k = kbd_on_output ? output_of(kbd_on_output) : NULL;
    if (!k || k->mm_w <= 0 || k->mm_h <= 0 || k->px_w <= 0 || k->px_h <= 0) {
        k = NULL;
        for (int i = 0; i < noutputs && !k; i++)
            if (outputs[i].mm_w > 0 && outputs[i].mm_h > 0 && outputs[i].px_w > 0 && outputs[i].px_h > 0)
                k = &outputs[i];
    }
    if (!k) return 0;
    double scale = keyboard.preferred_fractional_scale > 0 ? keyboard.preferred_fractional_scale
                   : k->scale > 0 ? k->scale : 1;
    return sqrt((double)k->px_w * k->px_h / ((double)k->mm_w * k->mm_h)) / scale;
}

/* kept current, so a session begins with the scale of the screen it is on */
static void
paper_scale_update(void)
{
    osk_paper_scale(&osk, paper_per_mm());
}

void
wl_surface_enter(void *data, struct wl_surface *wl_surface,
                 struct wl_output *wl_output)
{
    kbd_on_output = wl_output;
    paper_scale_update();
}

void
wl_surface_leave(void *data, struct wl_surface *wl_surface,
                 struct wl_output *wl_output) {
}

void
wl_preferred_buffer_scale(void *data, struct wl_surface *wl_surface,
                          int scale) {
    keyboard.preferred_scale = scale;
}

void
wl_preferred_buffer_transform(void *data, struct wl_surface *wl_surface,
                              uint32_t transform) {
}

static void
paper_surface_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
    kbd_on_output = o;
    paper_scale_update();
}

static void paper_surface_leave(void *data, struct wl_surface *s, struct wl_output *o) {}
static void paper_surface_scale(void *data, struct wl_surface *s, int scale) {}
static void paper_surface_transform(void *data, struct wl_surface *s, uint32_t t) {}

static const struct wl_surface_listener paper_surface_listener = {
    .enter = paper_surface_enter,
    .leave = paper_surface_leave,
    .preferred_buffer_scale = paper_surface_scale,
    .preferred_buffer_transform = paper_surface_transform,
};

static const struct wl_surface_listener surface_listener = {
    .enter = wl_surface_enter,
    .leave = wl_surface_leave,
    .preferred_buffer_scale = wl_preferred_buffer_scale,
    .preferred_buffer_transform = wl_preferred_buffer_transform,
};

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

void
initiate_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                   uint32_t serial, uint32_t w, uint32_t h)
{
    available_width = w;
    available_height = h;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
}

void
initiate_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
}

static const struct zwlr_layer_surface_v1_listener initiate_listener = {
    .configure = initiate_configure,
    .closed = initiate_closed,
};

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* configuration, allows nested code to access above variables */

char *
estrdup(const char *s)
{
    char *p;

    if (!(p = strdup(s))) {
        fprintf(stderr, "strdup:");
        exit(6);
    }

    return p;
}

void
wl_touch_down(void *data, struct wl_touch *wl_touch, uint32_t serial,
              uint32_t time, struct wl_surface *surface, int32_t id,
              wl_fixed_t x, wl_fixed_t y)
{
    if (paper_surf && surface == paper_surf) {         /* glassine: a finger on the paper: its controls */
        if (id >= 0 && id < 32) paper_touches |= 1u << id;
        if (keyboard.debug) fprintf(stderr, "Paper: touch %d,%d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
        osk_paper_tap(&osk, wl_fixed_to_int(x), wl_fixed_to_int(y), monotime());
        return;
    }
    if(!layer_surface_configured || !popup_xdg_surface_configured) {
        return;
    }

    struct key *next_key;
    uint32_t touch_x, touch_y;

    touch_x = wl_fixed_to_int(x);
    touch_y = wl_fixed_to_int(y);

    kbd_unpress_key(&keyboard, time);

    strip_touch = keyboard.osk && osk_strip_down(keyboard.osk, touch_x, touch_y, monotime());
    if (strip_touch) return;
    next_key = kbd_get_key(&keyboard, touch_x, touch_y);
    if (next_key) {
        kbd_press_key(&keyboard, next_key, time);
    } else if (keyboard.compose) {
        keyboard.compose = 0;
        kbd_switch_layout(&keyboard, keyboard.prevlayout,
                          keyboard.last_abc_index);
    }
}

void
wl_touch_up(void *data, struct wl_touch *wl_touch, uint32_t serial,
            uint32_t time, int32_t id)
{
    if (id >= 0 && id < 32 && (paper_touches & (1u << id))) {
        paper_touches &= ~(1u << id);
        if (!paper_touches) osk_paper_release(&osk);
        return;
    }
    if(!layer_surface_configured || !popup_xdg_surface_configured) {
        return;
    }

    if (strip_touch) {
        strip_touch = false;
        osk_strip_up(keyboard.osk, monotime());
        return;
    }
    kbd_release_key(&keyboard, time);
}

void
wl_touch_motion(void *data, struct wl_touch *wl_touch, uint32_t time,
                int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    if (id >= 0 && id < 32 && (paper_touches & (1u << id))) return;
    if(!layer_surface_configured || !popup_xdg_surface_configured) {
        return;
    }

    uint32_t touch_x, touch_y;

    touch_x = wl_fixed_to_int(x);
    touch_y = wl_fixed_to_int(y);

    if (strip_touch) {
        osk_strip_motion(keyboard.osk, touch_x, touch_y);
        return;
    }
    kbd_motion_key(&keyboard, time, touch_x, touch_y);
}

void
wl_touch_frame(void *data, struct wl_touch *wl_touch)
{
}

void
wl_touch_cancel(void *data, struct wl_touch *wl_touch)
{
}

void
wl_touch_shape(void *data, struct wl_touch *wl_touch, int32_t id,
               wl_fixed_t major, wl_fixed_t minor)
{
}

void
wl_touch_orientation(void *data, struct wl_touch *wl_touch, int32_t id,
                     wl_fixed_t orientation)
{
}

void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
                 struct wl_surface *surface, wl_fixed_t surface_x,
                 wl_fixed_t surface_y)
{
    /* glassine: a press right after entering (a pen or pointer that jumps there) */
    cur_x = wl_fixed_to_int(surface_x);
    cur_y = wl_fixed_to_int(surface_y);
    ptr_surface = surface;
    if (paper_surf && surface == paper_surf)            /* where a press starts (hover: not ink) */
        pen_ptr_motion(&ptr_pen, wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y), 0);
    osk_pen_on_keys(&osk, surface && (surface == draw_surf.surf || surface == popup_draw_surf.surf));
    if (keyboard.debug && surface == paper_surf) fprintf(stderr, "Paper: pointer in %d,%d\n", cur_x, cur_y);
}

void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
                 struct wl_surface *surface)
{
    cur_x = cur_y = -1;
    if (ptr_surface && ptr_surface == paper_surf)       /* the pen left the paper */
        pen_ptr_leave(&ptr_pen, monotime());
    if (ptr_surface && ptr_surface == paper_surf) osk_paper_release(&osk);
    if (keyboard.debug && ptr_surface && ptr_surface == paper_surf) fprintf(stderr, "Paper: pointer out\n");
    ptr_surface = NULL;
    osk_pen_on_keys(&osk, false);
}

void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
                  wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    /* glassine: the pen writing on the paper (sway hands a pen to a client without tablet
     * support as the pointer; a finger comes as touch and never writes) */
    if (paper_surf && ptr_surface == paper_surf) {
        cur_x = wl_fixed_to_int(surface_x);
        cur_y = wl_fixed_to_int(surface_y);
        /* the hover isn't kept; while writing, a motion is held back one event (pen.c) */
        if (pen_ptr_motion(&ptr_pen, wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y), time) & PEN_INK)
            paper_draw_new_ink(&keyboard, &paper_ds, &paper_drawn);
        return;
    }
    if(!layer_surface_configured || !popup_xdg_surface_configured) {
        return;
    }

    cur_x = wl_fixed_to_int(surface_x);
    cur_y = wl_fixed_to_int(surface_y);

    if (cur_press && strip_touch) {
        osk_strip_motion(keyboard.osk, cur_x, cur_y);
    } else if (cur_press) {
        kbd_motion_key(&keyboard, time, cur_x, cur_y);
    }
}

void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
                  uint32_t time, uint32_t button, uint32_t state)
{
    if (paper_surf && ptr_surface == paper_surf) {     /* glassine: the paper */
        bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
        int r = pen_ptr_button(&ptr_pen, down, time, monotime());
        if (r & PEN_DOWN) {
            if (keyboard.debug) fprintf(stderr, "Paper: pen down %.2f,%.2f\n", ptr_pen.px, ptr_pen.py);
            paper_drawn = 0;
        }
        if (r & PEN_INK) paper_draw_new_ink(&keyboard, &paper_ds, &paper_drawn);
        if ((r & PEN_UP) && keyboard.debug) fprintf(stderr, "Paper: pen up (%d strokes)\n", osk.pp.ns);
        return;
    }
    if(!layer_surface_configured || !popup_xdg_surface_configured) {
        return;
    }

    struct key *next_key;
    cur_press = state == WL_POINTER_BUTTON_STATE_PRESSED;

    if (!cur_press && strip_touch) {
        strip_touch = false;
        osk_strip_up(keyboard.osk, monotime());
        return;
    }
    if (cur_press) {
        kbd_unpress_key(&keyboard, time);
    } else {
        kbd_release_key(&keyboard, time);
    }

    strip_touch = cur_press && cur_x >= 0 && cur_y >= 0 && keyboard.osk &&
                  osk_strip_down(keyboard.osk, cur_x, cur_y, monotime());
    if (cur_press && cur_x >= 0 && cur_y >= 0 && !strip_touch) {
        next_key = kbd_get_key(&keyboard, cur_x, cur_y);
        if (next_key) {
            kbd_press_key(&keyboard, next_key, time);
        } else if (keyboard.compose) {
            keyboard.compose = 0;
            kbd_switch_layout(&keyboard, keyboard.prevlayout,
                              keyboard.last_abc_index);
        }
    }
}

void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
                uint32_t axis, wl_fixed_t value)
{
    if(!layer_surface_configured || !popup_xdg_surface_configured) {
        return;
    }

    kbd_next_layer(&keyboard, NULL, (value >= 0));
}

/* glassine: the pen as a tablet tool (tablet-unstable-v2, bound with --handwriting). The
 * compositor then sends the pen to all of this client's surfaces as a tool, not as the
 * pointer: on the paper it writes through pen.c (the down frame's own position, pressure
 * and tilt; the lift-off not kept); on the keys and the strip it is the pointer, as
 * before (the same handlers, the frame's position). */
static void
tool_paper_result(struct kbd_tool *k, int r)
{
    if (r & PEN_DOWN) {
        paper_drawn = 0;
        if (keyboard.debug) fprintf(stderr, "Paper: pen down %.2f,%.2f (tablet)\n", k->pen.x, k->pen.y);
    }
    if (r & PEN_INK) paper_draw_new_ink(&keyboard, &paper_ds, &paper_drawn);
    if ((r & PEN_UP) && keyboard.debug) fprintf(stderr, "Paper: pen up (%d strokes)\n", osk.pp.ns);
}

static void tool_type(void *d, struct zwp_tablet_tool_v2 *t, uint32_t type)
{
    struct kbd_tool *k = d;
    k->pen.eraser = type == ZWP_TABLET_TOOL_V2_TYPE_ERASER;
}
static void tool_serial(void *d, struct zwp_tablet_tool_v2 *t, uint32_t hi, uint32_t lo) {}
static void tool_hwid(void *d, struct zwp_tablet_tool_v2 *t, uint32_t hi, uint32_t lo) {}
static void tool_capability(void *d, struct zwp_tablet_tool_v2 *t, uint32_t cap)
{
    struct kbd_tool *k = d;
    if (cap == ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE) k->pen.has_pressure = true;
    if (cap == ZWP_TABLET_TOOL_V2_CAPABILITY_TILT) k->pen.has_tilt = true;
}
static void tool_done(void *d, struct zwp_tablet_tool_v2 *t) {}
static void tool_removed(void *d, struct zwp_tablet_tool_v2 *t)
{
    struct kbd_tool *k = d;
    if (k->surf && k->surf == paper_surf) tool_paper_result(k, pen_tool_leave(&k->pen, monotime()));
    if (k->on_keys) wl_pointer_leave(NULL, NULL, 0, k->surf);
    zwp_tablet_tool_v2_destroy(t);
    memset(k, 0, sizeof *k);
}
static void tool_proximity_in(void *d, struct zwp_tablet_tool_v2 *t, uint32_t serial,
                              struct zwp_tablet_v2 *tablet, struct wl_surface *surface)
{
    struct kbd_tool *k = d;
    k->entering = surface;
    k->leaving = false;
}
static void tool_proximity_out(void *d, struct zwp_tablet_tool_v2 *t)
{
    struct kbd_tool *k = d;
    k->leaving = true;
}
static void tool_down(void *d, struct zwp_tablet_tool_v2 *t, uint32_t serial)
{
    pen_tool_down(&((struct kbd_tool *)d)->pen);
}
static void tool_up(void *d, struct zwp_tablet_tool_v2 *t)
{
    pen_tool_up(&((struct kbd_tool *)d)->pen);
}
static void tool_motion(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y)
{
    pen_tool_motion(&((struct kbd_tool *)d)->pen, wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void tool_pressure(void *d, struct zwp_tablet_tool_v2 *t, uint32_t p)
{
    pen_tool_pressure(&((struct kbd_tool *)d)->pen, p / 65535.0);
}
static void tool_distance(void *d, struct zwp_tablet_tool_v2 *t, uint32_t v) {}
static void tool_tilt(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t tx, wl_fixed_t ty)
{
    pen_tool_tilt(&((struct kbd_tool *)d)->pen, wl_fixed_to_double(tx), wl_fixed_to_double(ty));
}
static void tool_rotation(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t v) {}
static void tool_slider(void *d, struct zwp_tablet_tool_v2 *t, int32_t v) {}
static void tool_wheel(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t v, int32_t c) {}
static void tool_button(void *d, struct zwp_tablet_tool_v2 *t, uint32_t serial, uint32_t button, uint32_t state) {}

static void tool_frame(void *d, struct zwp_tablet_tool_v2 *t, uint32_t time)
{
    struct kbd_tool *k = d;
    struct pen *pn = &k->pen;
    int64_t now = monotime();
    if (k->entering) {
        k->surf = k->entering;
        k->entering = NULL;
        if (paper_surf && k->surf == paper_surf) {
            pen_tool_enter(pn);          /* the pointer doesn't also write (pen.c) */
            osk_pen_on_keys(&osk, false);
            if (keyboard.debug) fprintf(stderr, "Paper: tool in %.2f,%.2f\n", pn->x, pn->y);
        } else {
            k->on_keys = true;
            wl_pointer_enter(NULL, NULL, 0, k->surf, wl_fixed_from_double(pn->x), wl_fixed_from_double(pn->y));
        }
    }
    if (paper_surf && k->surf == paper_surf) {
        tool_paper_result(k, pen_tool_frame(pn, time, now));
        cur_x = (int)pn->x;
        cur_y = (int)pn->y;
    } else if (k->on_keys) {
        /* the keys: the pointer's handlers, the frame's position */
        bool down = pn->f_down, up = pn->f_up, moved = pn->f_motion;
        pen_tool_drop(pn);
        if (moved) wl_pointer_motion(NULL, NULL, time, wl_fixed_from_double(pn->x), wl_fixed_from_double(pn->y));
        if (down) wl_pointer_button(NULL, NULL, 0, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
        if (up) wl_pointer_button(NULL, NULL, 0, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    } else {
        pen_tool_drop(pn);
    }
    if (k->leaving) {
        k->leaving = false;
        if (paper_surf && k->surf == paper_surf) {
            tool_paper_result(k, pen_tool_leave(pn, now));
            osk_paper_release(&osk);
            if (keyboard.debug) fprintf(stderr, "Paper: tool out\n");
        } else if (k->on_keys) {
            wl_pointer_leave(NULL, NULL, 0, k->surf);
        }
        k->surf = NULL;
        k->on_keys = false;
    }
}

static const struct zwp_tablet_tool_v2_listener tool_listener = {
    .type = tool_type, .hardware_serial = tool_serial, .hardware_id_wacom = tool_hwid,
    .capability = tool_capability, .done = tool_done, .removed = tool_removed,
    .proximity_in = tool_proximity_in, .proximity_out = tool_proximity_out,
    .down = tool_down, .up = tool_up, .motion = tool_motion, .pressure = tool_pressure,
    .distance = tool_distance, .tilt = tool_tilt, .rotation = tool_rotation,
    .slider = tool_slider, .wheel = tool_wheel, .button = tool_button, .frame = tool_frame,
};

/* the tablets and pads themselves aren't needed (no listener: their events are dropped) */
static void tseat_tablet(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_v2 *tablet) {}
static void tseat_pad(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_pad_v2 *pad) {}
static void tseat_tool(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_tool_v2 *t)
{
    for (int i = 0; i < MAX_TOOLS; i++) {
        if (tools[i].t) continue;
        memset(&tools[i], 0, sizeof tools[i]);
        tools[i].t = t;
        pen_init(&tools[i].pen, &osk);
        zwp_tablet_tool_v2_add_listener(t, &tool_listener, &tools[i]);
        return;
    }
    zwp_tablet_tool_v2_destroy(t);      /* more tools than kept: not ours to use */
}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
    .tablet_added = tseat_tablet, .tool_added = tseat_tool, .pad_added = tseat_pad,
};

void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
                         enum wl_seat_capability caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER)) {
        if (pointer == NULL) {
            pointer = wl_seat_get_pointer(wl_seat);
            wl_pointer_add_listener(pointer, &pointer_listener, NULL);
        }
    } else {
        if (pointer != NULL) {
            wl_pointer_destroy(pointer);
            pointer = NULL;
        }
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH)) {
        if (touch == NULL) {
            touch = wl_seat_get_touch(wl_seat);
            wl_touch_add_listener(touch, &touch_listener, NULL);
        }
    } else {
        if (touch != NULL) {
            wl_touch_destroy(touch);
            touch = NULL;
        }
    }
}

void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

void
handle_global(void *data, struct wl_registry *registry, uint32_t name,
              const char *interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, 6);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        draw_ctx.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell =
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &xdg_wm_base_listener, NULL);
    } else if (strcmp(interface,
                      wp_fractional_scale_manager_v1_interface.name) == 0) {
        wfs_mgr = wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        viewporter =
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface,
                      zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        vkbd_mgr = wl_registry_bind(
            registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if ((im_auto || im_mode) && strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
        im_mgr = wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1);
    } else if (hw_cmd && strcmp(interface, wl_output_interface.name) == 0 &&
               noutputs < (int)(sizeof outputs / sizeof *outputs) && version >= 2) {
        struct kbd_output *k = &outputs[noutputs++];
        memset(k, 0, sizeof *k);
        k->name = name;
        k->o = wl_registry_bind(registry, name, &wl_output_interface, version < 3 ? version : 3);
        wl_output_add_listener(k->o, &output_listener, NULL);
    } else if (hw_cmd && strcmp(interface, zwp_tablet_manager_v2_interface.name) == 0) {
        tablet_mgr = wl_registry_bind(registry, name, &zwp_tablet_manager_v2_interface, 1);
    } else if (hw_cmd && strcmp(interface, glassine_ink_manager_v1_interface.name) == 0) {
        ink_mgr = wl_registry_bind(registry, name, &glassine_ink_manager_v1_interface, version < 2 ? version : 2);
    }
}

void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    for (int i = 0; i < noutputs; i++) {
        if (outputs[i].name != name) continue;
        if (kbd_on_output == outputs[i].o) kbd_on_output = NULL;
        if (wl_output_get_version(outputs[i].o) >= 3) wl_output_release(outputs[i].o);
        else wl_output_destroy(outputs[i].o);
        outputs[i] = outputs[--noutputs];
        break;
    }
}

static void
xdg_popup_surface_configure(void *data, struct xdg_surface *xdg_surface,
                            uint32_t serial)
{
    xdg_surface_ack_configure(xdg_surface, serial);
    popup_xdg_surface_configured = true;
    drwsurf_attach(&popup_draw_surf);
}

static const struct xdg_surface_listener xdg_popup_surface_listener = {
    .configure = xdg_popup_surface_configure,
};

static void
xdg_popup_configure(void *data, struct xdg_popup *xdg_popup, int32_t x,
                    int32_t y, int32_t width, int32_t height)
{
}

static void
xdg_popup_done(void *data, struct xdg_popup *xdg_popup)
{
}

static const struct xdg_popup_listener xdg_popup_listener = {
    .configure = xdg_popup_configure,
    .popup_done = xdg_popup_done,
};

static void
wp_fractional_scale_preferred_scale(
    void *data, struct wp_fractional_scale_v1 *wp_fractional_scale_v1,
    uint32_t scale)
{
    keyboard.preferred_fractional_scale = (double)scale / 120;
    paper_scale_update();
}

static const struct wp_fractional_scale_v1_listener
    wp_fractional_scale_listener = {
        .preferred_scale = wp_fractional_scale_preferred_scale,
};

/* glassine: the input method's events go to osk.c (state applies on done); --auto
 * shows and hides with the focus (osk_ops.focus) */
void
im_activate(void *data, struct zwp_input_method_v2 *zwp_input_method_v2)
{
    osk_im_activate(&osk);
}

void
im_deactivate(void *data, struct zwp_input_method_v2 *zwp_input_method_v2)
{
    osk_im_deactivate(&osk);
}

void
im_surrounding_text(void *data, struct zwp_input_method_v2 *zwp_input_method_v2,
                    const char *text, uint32_t cursor, uint32_t anchor)
{
    osk_im_surrounding(&osk, text, cursor, anchor);
}

void im_text_change_cause(void *data, struct zwp_input_method_v2 *zwp_input_method_v2,
                          uint32_t cause)
{
    osk_im_cause(&osk, cause);
}

void im_content_type(void *data, struct zwp_input_method_v2 *zwp_input_method_v2,
                     uint32_t hint, uint32_t purpose)
{
    osk_im_content_type(&osk, hint, purpose);
}

void
im_done(void *data, struct zwp_input_method_v2 *zwp_input_method_v2)
{
    osk_im_done(&osk, monotime());
}

static void notify(const char *msg);

/* another input method has the seat: plain keys, and a try again later (the session
 * gives its own up when told, see notify "osk im") */
void
im_unavailable(void *data, struct zwp_input_method_v2 *zwp_input_method_v2)
{
    fprintf(stderr, "input method: the seat has another one, plain keys (again in %d ms)\n", im_retry_ms);
    osk_paper_end(&osk, monotime());    /* its popup goes with it */
    paper_hide();
    zwp_input_method_v2_destroy(zwp_input_method_v2);
    if (zwp_input_method_v2 == input_method) input_method = NULL;
    osk_im_deactivate(&osk);
    osk_im_done(&osk, monotime());
    im_retry_at = monotime() + im_retry_ms;
    im_retry_ms = im_retry_ms * 2 > 30000 ? 30000 : im_retry_ms * 2;
    notify("osk im");
}

static void
im_bind(void)
{
    if (!im_mgr || input_method) return;
    im_retry_at = -1;
    input_method = zwp_input_method_manager_v2_get_input_method(im_mgr, seat);
    zwp_input_method_v2_add_listener(input_method, &input_method_listener, NULL);
    osk_im_new(&osk);           /* a new object: its commits count its own done events */
}

/* ---- glassine: osk_ops (osk.h) ---- */
static void
op_im_send(void *d, uint32_t del, const char *text)
{
    if (!input_method) return;
    if (del) zwp_input_method_v2_delete_surrounding_text(input_method, del, 0);
    if (text && *text) zwp_input_method_v2_commit_string(input_method, text);
    zwp_input_method_v2_commit(input_method, osk_serial(&osk));
}

static void
op_strip_changed(void *d)
{
    if (layer_surface_configured) kbd_draw_strip(&keyboard);
}

static void
op_keys(void *d, enum osk_keys k)
{
    kbd_set_keys(&keyboard, k);
}

static void
op_shift(void *d, bool on)
{
    if (layer_surface_configured) kbd_draw_layout(&keyboard);
}

static pid_t helpers[8];        /* --hide-cmd and --pen-cmd runs, reaped in the loop */

static void
run_cmd(const char *cmd)
{
    if (!cmd || !*cmd) return;
    char *av[] = { "/bin/sh", "-c", (char *)cmd, NULL };
    posix_spawnattr_t at;
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_init(&at);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
    pid_t pid;
    extern char **environ;
    if (posix_spawn(&pid, av[0], NULL, &at, av, environ) == 0) {
        for (unsigned i = 0; i < countof(helpers); i++)
            if (!helpers[i]) { helpers[i] = pid; pid = 0; break; }
        if (pid) waitpid(pid, NULL, WNOHANG);
    }
    posix_spawnattr_destroy(&at);
}

static void
reap_helpers(void)
{
    for (unsigned i = 0; i < countof(helpers); i++)
        if (helpers[i] && waitpid(helpers[i], NULL, WNOHANG) != 0) helpers[i] = 0;
}

static void
op_hide(void *d)
{
    if (hide_cmd) run_cmd(hide_cmd);    /* the session's own hide (it keeps its state) */
    else hide();
}

static void
op_handwriting(void *d)
{
    run_cmd(pen_cmd);
}

/* a one-line datagram to the session (--notify), never waited for */
static void
notify(const char *msg)
{
    if (!notify_path) return;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", notify_path);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa, sizeof sa);
    close(fd);
}

static void
op_focus(void *d, int state, uint32_t purpose)
{
    char m[48];
    if (state == 2) snprintf(m, sizeof m, "osk changed");
    else snprintf(m, sizeof m, "osk focus %d %u", state, purpose);
    notify(m);
    if (im_auto && state == 1) show();
    else if (im_auto && state == 0) hide();
}

/* the paper's edits: the pending text is the preedit; one commit for all of it */
static void
op_im_preedit(void *d, uint32_t del, const char *text, const char *pre)
{
    if (!input_method) return;
    if (del) zwp_input_method_v2_delete_surrounding_text(input_method, del, 0);
    if (text && *text) zwp_input_method_v2_commit_string(input_method, text);
    size_t n = pre ? strlen(pre) : 0;
    zwp_input_method_v2_set_preedit_string(input_method, pre ? pre : "", (int32_t)n, (int32_t)n);
    zwp_input_method_v2_commit(input_method, osk_serial(&osk));
    if (keyboard.debug) fprintf(stderr, "paper: commit \"%s\" del %u preedit \"%s\"\n", text ? text : "", del, pre ? pre : "");
}

static void
op_backspaces(void *d, int n)
{
    uint32_t t = (uint32_t)(monotime() + keyboard.last_press_offset);
    for (int i = 0; i < n && i < 256; i++) {
        zwp_virtual_keyboard_v1_key(keyboard.vkbd, t, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(keyboard.vkbd, t, KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_RELEASED);
    }
}

static void keys_hide(void);

static void
op_view(void *d, enum osk_view v)
{
    if (keyboard.debug) fprintf(stderr, "view %d\n", (int)v);
    switch (v) {
    case OSK_VIEW_KEYS:
        paper_hide();
        show();
        break;
    case OSK_VIEW_PAPER:
        keys_hide();
        paper_show();
        break;
    case OSK_VIEW_PAPER_KEYS:
        paper_show();
        show();
        break;
    case OSK_VIEW_HIDDEN:
        paper_hide();
        op_hide(d);         /* as the hide key: the session's state follows */
        break;
    }
}

static void
op_paper_changed(void *d)
{
    if (paper_surf && paper_ds.attached) paper_draw_all(&keyboard, &paper_ds);
}

static const struct osk_ops osk_ops = {
    .im_send = op_im_send,
    .strip_changed = op_strip_changed,
    .keys = op_keys,
    .shift = op_shift,
    .hide = op_hide,
    .handwriting = op_handwriting,
    .focus = op_focus,
    .im_preedit = op_im_preedit,
    .backspaces = op_backspaces,
    .view = op_view,
    .paper_changed = op_paper_changed,
};

/* ---- glassine: the paper's surface ---- */
static void
paper_rect(void *data, struct zwp_input_popup_surface_v2 *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (keyboard.debug) fprintf(stderr, "Paper: the cursor at %d,%d %dx%d of it\n", x, y, w, h);
}

static const struct zwp_input_popup_surface_v2_listener paper_popup_listener = {
    .text_input_rectangle = paper_rect,
};

static void
paper_show(void)
{
    if (paper_surf || !input_method) return;
    if (!available_width) refresh_available_dimension();
    uint32_t w = keyboard.w ? keyboard.w : available_width;
    uint32_t h = paper_height > 0 ? (uint32_t)paper_height : normal_height;
    if (!w || !h) return;
    double scale = keyboard.scale > 0 ? keyboard.scale : 1;
    paper_surf = wl_compositor_create_surface(compositor);
    paper_popup = zwp_input_method_v2_get_input_popup_surface(input_method, paper_surf);
    zwp_input_popup_surface_v2_add_listener(paper_popup, &paper_popup_listener, NULL);
    paper_ds.ctx = &draw_ctx;
    paper_ds.back_buffer = &paper_buf_a;
    paper_ds.display_buffer = &paper_buf_b;
    paper_ds.surf = paper_surf;
    wl_surface_add_listener(paper_surf, &paper_surface_listener, NULL);
    osk_paper_size(&osk, (int)w, (int)h);
    if (viewporter) {
        paper_viewport = wp_viewporter_get_viewport(viewporter, paper_surf);
        wp_viewport_set_destination(paper_viewport, w, h);
    } else {
        scale = (int)scale;
        wl_surface_set_buffer_scale(paper_surf, (int)scale);
    }
    drwsurf_resize(&paper_ds, w, h, scale);
    if (ink_mgr) {
        /* the pen draws at once on the writing area (not the row and the controls) */
        paper_ink = glassine_ink_manager_v1_get_ink_surface(ink_mgr, paper_surf);
        struct wl_region *r = wl_compositor_create_region(compositor);
        int top = osk.pp.ctl + osk.pp.ctl / 2;
        wl_region_add(r, 0, top, (int32_t)w, (int32_t)h - top);
        glassine_ink_surface_v1_set_areas(paper_ink, r);
        wl_region_destroy(r);
        if (glassine_ink_surface_v1_get_version(paper_ink) >= 2) {
            double iw = paper_ink_width(&osk.pp);
            glassine_ink_surface_v1_set_brush(paper_ink, GLASSINE_INK_SURFACE_V1_TOOL_TIP, GLASSINE_INK_SURFACE_V1_KIND_PEN,
                                              wl_fixed_from_double(iw), wl_fixed_from_double(iw * PAPER_INK_MAX), wl_fixed_from_double(1));
            glassine_ink_surface_v1_set_brush(paper_ink, GLASSINE_INK_SURFACE_V1_TOOL_ERASER, GLASSINE_INK_SURFACE_V1_KIND_NONE,
                                              wl_fixed_from_double(1), wl_fixed_from_double(1), wl_fixed_from_double(1));
        }
    }
    paper_draw_all(&keyboard, &paper_ds);
    drwsurf_attach(&paper_ds);
    paper_pens_reset();
    if (keyboard.debug) fprintf(stderr, "Paper %ux%u\n", w, h);
    if (keyboard.debug) fprintf(stderr, "Paper scale %.4f px/mm\n", osk.per_mm);
}

static void
paper_hide(void)
{
    if (!paper_surf) return;
    if (paper_ds.frame_cb) {
        wl_callback_destroy(paper_ds.frame_cb);
        paper_ds.frame_cb = NULL;
    }
    if (paper_ink) glassine_ink_surface_v1_destroy(paper_ink);
    paper_ink = NULL;
    if (paper_viewport) wp_viewport_destroy(paper_viewport);
    paper_viewport = NULL;
    zwp_input_popup_surface_v2_destroy(paper_popup);
    paper_popup = NULL;
    for (int i = 0; i < MAX_TOOLS; i++)
        if (tools[i].t && tools[i].surf == paper_surf) {
            pen_tool_leave(&tools[i].pen, monotime());
            tools[i].surf = NULL;
        }
    wl_surface_destroy(paper_surf);
    if (ptr_surface == paper_surf) ptr_surface = NULL;
    paper_surf = NULL;
    paper_ds.surf = NULL;
    paper_ds.attached = false;
    paper_pens_reset();
    paper_touches = 0;
    if (keyboard.debug) fprintf(stderr, "Paper hidden\n");
}

void
redimension_keyboard()
{
    keyboard.landscape = available_width > available_height;

    enum layout_id layer;
    if (keyboard.landscape) {
        layer = keyboard.landscape_layers[0];
        height = landscape_height;
    } else {
        layer = keyboard.layers[0];
        height = normal_height;
    }

    if (custom_width > 0) {
        keyboard.w = custom_width;
    } else if (dock_mode != DOCK_FULL) {
        keyboard.w = (uint32_t)((double)available_width * width_ratio);
    } else {
        keyboard.w = available_width;
    }
    /* glassine: the strip, a key row of the first layer high, above the keys */
    keyboard.strip_h = 0;
    if (strip_on) {
        uint8_t rows = kbd_get_rows(&keyboard.layouts[layer]);
        keyboard.strip_h = strip_height >= 0 ? (uint32_t)strip_height : height / (rows ? rows : 1);
    }
    height += keyboard.strip_h;
    keyboard.h = height;
    keyboard.layout = &keyboard.layouts[layer];
    keyboard.layer_index = 0;
    keyboard.prevlayout = keyboard.layout;
    keyboard.last_abc_layout = keyboard.layout;
    keyboard.last_abc_index = 0;
    kbd_reset_layer(&keyboard);     /* glassine: the keys the focused field asks for */
}

void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t w, uint32_t h)
{
    // Swallow events for old/destroyed surface
    if (surface != layer_surface) {
        zwlr_layer_surface_v1_ack_configure(surface, serial);
        return;
    };

    // Not what we expected, or redimension, refresh and restart
    if (keyboard.w != w || keyboard.h != h) {
        zwlr_layer_surface_v1_ack_configure(surface, serial);
        keys_hide();
        show();
        return;
    };

    // Swallow useless events
    if (layer_surface_configured) {
        return;
    };
    layer_surface_configured = true;

    double scale = keyboard.preferred_scale;
    if (keyboard.preferred_fractional_scale) {
        scale = keyboard.preferred_fractional_scale;
    }

    keyboard.scale = scale;
    hidden = false;

    if (wfs_mgr && viewporter) {
        wp_viewport_set_destination(draw_surf_viewport, keyboard.w,
                                    keyboard.h);
    } else {
        wl_surface_set_buffer_scale(draw_surf.surf, keyboard.scale);
    }

    popup_draw_surf.surf = wl_compositor_create_surface(compositor);

    xdg_positioner_set_size(popup_xdg_positioner, w, h * 2);
    xdg_positioner_set_anchor_rect(popup_xdg_positioner, 0, -h, w, h * 2);

    wl_surface_set_input_region(popup_draw_surf.surf, empty_region);
    popup_xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, popup_draw_surf.surf);
    popup_xdg_surface_configured = false;
    xdg_surface_add_listener(popup_xdg_surface, &xdg_popup_surface_listener, NULL);
    popup_xdg_popup = xdg_surface_get_popup(popup_xdg_surface, NULL, popup_xdg_positioner);
    xdg_popup_add_listener(popup_xdg_popup, &xdg_popup_listener, NULL);
    zwlr_layer_surface_v1_get_popup(layer_surface, popup_xdg_popup);

    if (wfs_mgr && viewporter) {
        popup_draw_surf_viewport = wp_viewporter_get_viewport(viewporter, popup_draw_surf.surf);
        wp_viewport_set_destination(popup_draw_surf_viewport, keyboard.w, keyboard.h * 2);
    } else {
        wl_surface_set_buffer_scale(popup_draw_surf.surf, keyboard.scale);
    }

    wl_surface_commit(popup_draw_surf.surf);

    zwlr_layer_surface_v1_ack_configure(surface, serial);

    kbd_resize(&keyboard, layouts, NumLayouts);
    drwsurf_attach(&draw_surf);
}

void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    zwlr_layer_surface_v1_destroy(surface);
    wl_surface_destroy(draw_surf.surf);
    run_display = false;
}

void
usage(char *argv0)
{
    fprintf(stderr,
            "usage: %s [-hov] [-H height] [-L landscape height] [-fn font] [-l "
            "layers]\n",
            argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -D          - Enable debug\n");
    fprintf(stderr, "  -o          - Print pressed keys to standard output\n");
    fprintf(stderr,
            "  -O          - Print intersected keys to standard output\n");
    fprintf(stderr, "  -H [int]    - Height in pixels\n");
    fprintf(stderr, "  -L [int]    - Landscape height in pixels\n");
    fprintf(stderr, "  -R [int]    - Rounding radius in pixels\n");
    fprintf(stderr, "  --fn [font] - Set font (e.g: DejaVu Sans 20)\n");
    fprintf(stderr, "  --hidden    - Start hidden (send SIGUSR2 to show)\n");
    fprintf(stderr, "  --hold [ms]            - Long press: a held key types its alternate after ms (default 400, 0: off)\n");
    fprintf(stderr, "  --no-popup             - Disable the key-press popup\n");
    fprintf(stderr, "  --no-highlight         - Don't highlight a key while pressed\n");
    fprintf(stderr, "  --no-feedback          - Disable all key-press feedback "
                    "(--no-popup and --no-highlight)\n");
    fprintf(
        stderr,
        "  --alpha [int]          - Set alpha value for all colors [0-255]\n");
    fprintf(stderr, "  --auto                 - Automatically toggle visibility based on focus\n");
    fprintf(stderr, "  --bg [rrggbb|aa]       - Set color of background\n");
    fprintf(stderr, "  --fg [rrggbb|aa]       - Set color of keys\n");
    fprintf(stderr, "  --fg-sp [rrggbb|aa]    - Set color of special keys\n");
    fprintf(stderr, "  --press [rrggbb|aa]    - Set color of pressed keys\n");
    fprintf(stderr,
            "  --press-sp [rrggbb|aa] - Set color of pressed special keys\n");
    fprintf(stderr, "  --swipe [rrggbb|aa]    - Set color of swiped keys\n");
    fprintf(stderr,
            "  --swipe-sp [rrggbb|aa] - Set color of swiped special keys\n");
    fprintf(stderr, "  --text [rrggbb|aa]     - Set color of text on keys\n");
    fprintf(stderr,
            "  --text-sp [rrggbb|aa]  - Set color of text on special keys\n");
    fprintf(stderr, "  --stroke, --stroke-sp [rrggbb|aa] - glassine: key outline colors\n");
    fprintf(stderr, "  --stroke-width [int]   - glassine: key outline width in pixels\n");
    fprintf(stderr, "  --gap [int]            - glassine: space around each key in pixels (2)\n");
    fprintf(stderr,
            "  --text-press [rrggbb|aa]    - Set color of text on pressed keys\n");
    fprintf(stderr,
            "  --text-press-sp [rrggbb|aa] - Set color of text on pressed special keys\n");
    fprintf(stderr,
            "  --text-swipe [rrggbb|aa]    - Set color of text on swiped keys\n");
    fprintf(stderr,
            "  --text-swipe-sp [rrggbb|aa] - Set color of text on swiped special keys\n");
    fprintf(stderr,
            "  --list-layers      - Print the list of available layers\n");
    fprintf(stderr,
            "  -l                 - Comma separated list of layers\n");
    fprintf(stderr, "  --landscape-layers - Comma separated list of "
                    "landscape layers\n");
    fprintf(stderr, "  --dock [left|right|full] - Dock keyboard to left or right edge\n");
    fprintf(stderr, "  --ratio [0.3-1.0]        - Screen width ratio for docked keyboard\n");
    fprintf(stderr, "  --width [pixels]         - Custom fixed width in pixels\n");
    fprintf(stderr, "  --corner-radius [pixels] - Corner radius of keyboard window\n");
    fprintf(stderr, "  --bg-alpha [0-255]       - Independent opacity for keyboard baseplate\n");
    fprintf(stderr, "  --double-space-period    - Double tap space to insert period and space\n");
    fprintf(stderr, "  --im                     - glassine: be the input method: text is committed to the focused field\n");
    fprintf(stderr, "  --strip                  - glassine: the suggestion strip above the keys (with --im)\n");
    fprintf(stderr, "  --strip-height [px]      - glassine: its height (default: a key row's)\n");
    fprintf(stderr, "  --predictor [cmd]        - glassine: the word predictor (docs/predictor.md of GlassineOS/shell)\n");
    fprintf(stderr, "  --lang [list]            - glassine: the languages it gets (default: the layout's)\n");
    fprintf(stderr, "  --notify [socket]        - glassine: focus reports to the session (datagrams)\n");
    fprintf(stderr, "  --hide-cmd [cmd]         - glassine: the strip's hide key runs this (default: hide)\n");
    fprintf(stderr, "  --pen-cmd [cmd]          - glassine: the pen key (handwriting) runs this; no pen key without it\n");
    fprintf(stderr, "  --handwriting [cmd]      - glassine: the recognizer (docs/recognizer.md of GlassineOS/handwriting): the pen key opens the writing paper\n");
    fprintf(stderr, "  --pen-flag [file]        - glassine: the session made this file before a show: a pen tap (the paper in an empty field)\n");
    fprintf(stderr, "  --paper-height [px]      - glassine: the writing paper's height (default: the keys')\n");
    fprintf(stderr, "  --non-exclusive    - Allow the keyboard to overlap "
                    "windows. Do not request an exclusive zone from the "
                    "compositor\n");
    fprintf(stderr, "  --wayland-layer [background|bottom|top|overlay]"
                    " - Wayland layer shell z index\n");
}

void
list_layers()
{
    int i;
    for (i = 0; i < NumLayouts - 1; i++) {
        if (layouts[i].name) {
            puts(layouts[i].name);
        }
    }
}

/* the session's hide (SIGUSR1, the hide key): the paper goes too */
void
hide()
{
    if (keyboard.osk) osk_paper_end(keyboard.osk, monotime());
    paper_hide();
    keys_hide();
    hidden = true;
}

/* the keys go (the paper may stay) */
static void
keys_hide(void)
{
    if (!layer_surface) {
        return;
    }

    // Ensure active keys are released, so they're not repeated infinitely.
    uint32_t time = (uint32_t) (monotime() + keyboard.last_press_offset);
    kbd_unpress_key(&keyboard, time);

    if (wfs_draw_surf) {
        wp_fractional_scale_v1_destroy(wfs_draw_surf);
        wfs_draw_surf = NULL;
    }
    if (draw_surf_viewport) {
        wp_viewport_destroy(draw_surf_viewport);
        draw_surf_viewport = NULL;
    }
    if (popup_xdg_popup) {
        xdg_popup_destroy(popup_xdg_popup);
        popup_xdg_popup = NULL;
    }
    if (popup_xdg_surface) {
        xdg_surface_destroy(popup_xdg_surface);
        popup_xdg_surface = NULL;
    }
    if (popup_draw_surf.surf) {
        wl_surface_destroy(popup_draw_surf.surf);
        popup_draw_surf.surf = NULL;
    }

    zwlr_layer_surface_v1_destroy(layer_surface);
    layer_surface = NULL;
    layer_surface_configured = false;

    // Cancel pending frame callback before destroying surface
    if (draw_surf.frame_cb) {
        wl_callback_destroy(draw_surf.frame_cb);
        draw_surf.frame_cb = NULL;
    }
    if (popup_draw_surf.frame_cb) {
        wl_callback_destroy(popup_draw_surf.frame_cb);
        popup_draw_surf.frame_cb = NULL;
    }

    wl_surface_destroy(draw_surf.surf);
    draw_surf.attached = false;

    hidden = true;
    if (keyboard.debug) fprintf(stderr, "hidden\n");
    if (keyboard.osk) osk_visible(keyboard.osk, false, monotime());
}

void
show()
{
    if (layer_surface) {
        return;
    }

    refresh_available_dimension();
    redimension_keyboard();

    draw_surf.surf = wl_compositor_create_surface(compositor);
    wl_surface_add_listener(
        draw_surf.surf, &surface_listener, NULL);
    if (wfs_mgr && viewporter) {
        wfs_draw_surf = wp_fractional_scale_manager_v1_get_fractional_scale(
            wfs_mgr, draw_surf.surf);
        wp_fractional_scale_v1_add_listener(
            wfs_draw_surf, &wp_fractional_scale_listener, NULL);
        draw_surf_viewport =
            wp_viewporter_get_viewport(viewporter, draw_surf.surf);
    }

    struct wl_output *current_output_data = NULL;

    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, draw_surf.surf, current_output_data, layer, namespace);

    anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    uint32_t req_w = 0;
    if (dock_mode == DOCK_LEFT) {
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        req_w = keyboard.w;
    } else if (dock_mode == DOCK_RIGHT) {
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        req_w = keyboard.w;
    } else {
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        req_w = 0;
    }
    zwlr_layer_surface_v1_set_size(layer_surface, req_w, height);
    zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
    if (keyboard.exclusive) {
        zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, height);
    }
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, false);
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener,
                                       NULL);
    wl_surface_commit(draw_surf.surf);
    if (keyboard.osk) {
        osk_languages(keyboard.osk, kbd_lang(&keyboard), monotime());   /* before the first request */
        osk_visible(keyboard.osk, true, monotime());
    }
}

/* glassine: the session's show; after a pen tap (--pen-flag, its mark) the paper when the
 * field is empty and handwriting is offered, else the keys (a finger tap) */
static void
session_show(void)
{
    bool pen = pen_flag && unlink(pen_flag) == 0;
    if (keyboard.osk && pen && osk.paper_up) {     /* the pen again (writing, a re-focus): it stays */
        hidden = false;
        return;
    }
    if (keyboard.osk && pen && osk_show_pen(keyboard.osk, monotime())) {
        hidden = false;
        return;
    }
    if (keyboard.osk && osk.paper_up) {
        osk_paper_end(keyboard.osk, monotime());
        paper_hide();
    }
    show();
}

/* glassine: the session saw the pen come into range (SIGWINCH; ignored by default, so a
 * keyboard without this takes no harm): with the keys up, the paper, unless the pen turns
 * out to be over the keys (osk.c, osk_pen_near) */
static void
pen_near(void)
{
    if (keyboard.osk && layer_surface && !hidden && !osk.paper_up)
        osk_pen_near(keyboard.osk, monotime());
}

void
toggle_visibility()
{
    if (hidden && !paper_surf)
        show();
    else
        hide();
}

void
pipewarn()
{
    fprintf(stderr, "wvkbd: cannot pipe data out.\n");
}

void
set_kbd_colors(uint8_t *bgra, char *hex)
{
    // bg, fg, text, high, swipe
    int length = strlen(hex);
    if (length == 6 || length == 8) {
        char subhex[3] = { 0 };
        memcpy(subhex, hex, 2);
        bgra[2] = (int)strtol(subhex, NULL, 16);
        memcpy(subhex, hex + 2, 2);
        bgra[1] = (int)strtol(subhex, NULL, 16);
        memcpy(subhex, hex + 4, 2);
        bgra[0] = (int)strtol(subhex, NULL, 16);
        if (length == 8) {
            memcpy(subhex, hex + 6, 2);
            bgra[3] = (int)strtol(subhex, NULL, 16);
        }
    }
}

void
refresh_available_dimension()
{
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct zwlr_layer_surface_v1 *layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL, layer, namespace);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_add_listener(layer_surface, &initiate_listener, NULL);
    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    zwlr_layer_surface_v1_destroy(layer_surface);
    wl_surface_destroy(surface);
    wl_display_roundtrip(display);
}

int
main(int argc, char **argv)
{
    /* parse command line arguments */
    char *layer_names_list = NULL, *landscape_layer_names_list = NULL;
    char *fc_font_pattern = NULL;
    height = landscape_height = KBD_PIXEL_LANDSCAPE_HEIGHT;
    normal_height = KBD_PIXEL_HEIGHT;

    char *tmp;
    if ((tmp = getenv("WVKBD_LAYERS")))
        layer_names_list = estrdup(tmp);
    if ((tmp = getenv("WVKBD_LANDSCAPE_LAYERS")))
        landscape_layer_names_list = estrdup(tmp);
    if ((tmp = getenv("WVKBD_HEIGHT")))
        normal_height = atoi(tmp);
    if ((tmp = getenv("WVKBD_LANDSCAPE_HEIGHT")))
        landscape_height = atoi(tmp);

    /* keyboard settings */
    keyboard.layers = (enum layout_id *)&layers;
    keyboard.landscape_layers = (enum layout_id *)&landscape_layers;
    keyboard.schemes = schemes;
    keyboard.landscape = true;
    keyboard.layer_index = 0;
    keyboard.preferred_scale = 1;
    keyboard.preferred_fractional_scale = 0;
    keyboard.exclusive = true;
    keyboard.corner_radius = 0;
    keyboard.show_popup = true;
    keyboard.show_highlight = true;
    keyboard.hold_ms = 400;     /* glassine: long press (--hold) */

    uint8_t alpha = 0;
    bool alpha_defined = false;

    int i;
    for (i = 1; argv[i]; i++) {
        if ((!strcmp(argv[i], "-v")) || (!strcmp(argv[i], "--version"))) {
            printf("wvkbd-%s\n", VERSION);
            exit(0);
        } else if ((!strcmp(argv[i], "-h")) || (!strcmp(argv[i], "--help"))) {
            usage(argv[0]);
            exit(0);
        } else if (!strcmp(argv[i], "-l")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            if (layer_names_list)
                free(layer_names_list);
            layer_names_list = estrdup(argv[++i]);
        } else if ((!strcmp(argv[i], "-landscape-layers")) ||
                   (!strcmp(argv[i], "--landscape-layers"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            if (landscape_layer_names_list)
                free(landscape_layer_names_list);
            landscape_layer_names_list = estrdup(argv[++i]);
        } else if ((!strcmp(argv[i], "-bg")) || (!strcmp(argv[i], "--bg"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[0].bg.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-alpha")) ||
                   (!strcmp(argv[i], "--alpha"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            alpha = atoi(argv[++i]);
            alpha_defined = true;
        } else if (!strcmp(argv[i], "--bg-alpha")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            bg_alpha = atoi(argv[++i]);
            bg_alpha_defined = true;
        } else if ((!strcmp(argv[i], "-fg")) || (!strcmp(argv[i], "--fg"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[0].fg.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-fg-sp")) ||
                   (!strcmp(argv[i], "--fg-sp"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[1].fg.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-press")) ||
                   (!strcmp(argv[i], "--press"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[0].high.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-press-sp")) ||
                   (!strcmp(argv[i], "--press-sp"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[1].high.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-swipe")) ||
                   (!strcmp(argv[i], "--swipe"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[0].swipe.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-swipe-sp")) ||
                   (!strcmp(argv[i], "--swipe-sp"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[1].swipe.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-text")) ||
                   (!strcmp(argv[i], "--text"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[0].text.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-text-sp")) ||
                   (!strcmp(argv[i], "--text-sp"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[1].text.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-text-press")) ||
                   (!strcmp(argv[i], "--text-press"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[0].text_press.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-text-press-sp")) ||
                   (!strcmp(argv[i], "--text-press-sp"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            set_kbd_colors(keyboard.schemes[1].text_press.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-text-swipe")) ||
                   (!strcmp(argv[i], "--text-swipe"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            keyboard.schemes[0].text_swipe.bgra[3] = 255; /* glassine: rrggbb is opaque */
            set_kbd_colors(keyboard.schemes[0].text_swipe.bgra, argv[++i]);
        } else if ((!strcmp(argv[i], "-text-swipe-sp")) ||
                   (!strcmp(argv[i], "--text-swipe-sp"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            keyboard.schemes[1].text_swipe.bgra[3] = 255; /* glassine: rrggbb is opaque */
            set_kbd_colors(keyboard.schemes[1].text_swipe.bgra, argv[++i]);
        } else if (!strcmp(argv[i], "-H")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            normal_height = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-L")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            height = landscape_height = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--stroke") || !strcmp(argv[i], "--stroke-sp")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            uint8_t *c = keyboard.schemes[!strcmp(argv[i], "--stroke-sp")].stroke.bgra;
            c[3] = 255;                 /* rrggbb: opaque (starts at alpha 0 = off) */
            set_kbd_colors(c, argv[++i]);
        } else if (!strcmp(argv[i], "--stroke-width") || !strcmp(argv[i], "--gap")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            int *v = !strcmp(argv[i], "--gap") ? &kbd_key_border : &kbd_stroke_width;
            *v = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-R")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            rounding = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-D")) {
            keyboard.debug = true;
        } else if ((!strcmp(argv[i], "-fn")) || (!strcmp(argv[i], "--fn"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            fc_font_pattern = estrdup(argv[++i]);
        } else if (!strcmp(argv[i], "-o")) {
            keyboard.print = true;
        } else if (!strcmp(argv[i], "-O")) {
            keyboard.print_intersect = true;
        } else if ((!strcmp(argv[i], "-hidden")) ||
                   (!strcmp(argv[i], "--hidden"))) {
            hidden = true;
        } else if ((!strcmp(argv[i], "-hold")) ||
                   (!strcmp(argv[i], "--hold"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            keyboard.hold_ms = atoi(argv[++i]);
            if (keyboard.hold_ms < 0) keyboard.hold_ms = 0;
        } else if ((!strcmp(argv[i], "-no-popup")) ||
                   (!strcmp(argv[i], "--no-popup"))) {
            keyboard.show_popup = false;
        } else if ((!strcmp(argv[i], "-no-highlight")) ||
                   (!strcmp(argv[i], "--no-highlight"))) {
            keyboard.show_highlight = false;
        } else if ((!strcmp(argv[i], "-no-feedback")) ||
                   (!strcmp(argv[i], "--no-feedback"))) {
            keyboard.show_popup = false;
            keyboard.show_highlight = false;
        } else if ((!strcmp(argv[i], "-list-layers")) ||
                   (!strcmp(argv[i], "--list-layers"))) {
            list_layers();
            exit(0);
        } else if ((!strcmp(argv[i], "-non-exclusive")) || (!strcmp(argv[i], "--non-exclusive"))) {
            keyboard.exclusive = false;
        } else if ((!strcmp(argv[i], "-auto")) ||
                   (!strcmp(argv[i], "--auto"))) {
            im_auto = true;
        } else if (!strcmp(argv[i], "--im")) {
            im_mode = true;
        } else if (!strcmp(argv[i], "--strip")) {
            strip_on = true;
        } else if (!strcmp(argv[i], "--strip-height") || !strcmp(argv[i], "--predictor") ||
                   !strcmp(argv[i], "--lang") || !strcmp(argv[i], "--notify") ||
                   !strcmp(argv[i], "--hide-cmd") || !strcmp(argv[i], "--pen-cmd") ||
                   !strcmp(argv[i], "--handwriting") || !strcmp(argv[i], "--pen-flag") ||
                   !strcmp(argv[i], "--paper-height")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            const char *o = argv[i++];
            if (!strcmp(o, "--strip-height")) strip_height = atoi(argv[i]);
            else if (!strcmp(o, "--predictor")) predictor_cmd = estrdup(argv[i]);
            else if (!strcmp(o, "--lang")) keyboard.lang_override = estrdup(argv[i]);
            else if (!strcmp(o, "--notify")) notify_path = estrdup(argv[i]);
            else if (!strcmp(o, "--hide-cmd")) hide_cmd = estrdup(argv[i]);
            else if (!strcmp(o, "--handwriting")) hw_cmd = estrdup(argv[i]);
            else if (!strcmp(o, "--pen-flag")) pen_flag = estrdup(argv[i]);
            else if (!strcmp(o, "--paper-height")) paper_height = atoi(argv[i]);
            else pen_cmd = estrdup(argv[i]);
        } else if ((!strcmp(argv[i], "-wayland-layer")) ||
                   (!strcmp(argv[i], "--wayland-layer"))) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            ++i;

            if (!strcmp(argv[i], "background")) {
                layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
            } else if (!strcmp(argv[i], "bottom")) {
                layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
            } else if (!strcmp(argv[i], "top")) {
                layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
            } else if (!strcmp(argv[i], "overlay")) {
                layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
            } else {
                usage(argv[0]);
                exit(1);
            }
        } else if (!strcmp(argv[i], "--dock")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            i++;
            if (!strcmp(argv[i], "left")) {
                dock_mode = DOCK_LEFT;
                if (width_ratio == 1.0) width_ratio = 0.68;
            } else if (!strcmp(argv[i], "right")) {
                dock_mode = DOCK_RIGHT;
                if (width_ratio == 1.0) width_ratio = 0.68;
            } else {
                dock_mode = DOCK_FULL;
                width_ratio = 1.0;
            }
        } else if (!strcmp(argv[i], "--corner-radius")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            corner_radius = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--width")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            custom_width = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ratio")) {
            if (i >= argc - 1) {
                usage(argv[0]);
                exit(1);
            }
            width_ratio = atof(argv[++i]);
            if (width_ratio < 0.3) width_ratio = 0.3;
            if (width_ratio > 1.0) width_ratio = 1.0;
        } else {
            fprintf(stderr, "Invalid argument: %s\n", argv[i]);
            usage(argv[0]);
            exit(1);
        }
    }

    if (corner_radius >= 0) {
        keyboard.corner_radius = corner_radius;
    } else if (dock_mode != DOCK_FULL) {
        keyboard.corner_radius = 16;
    } else {
        keyboard.corner_radius = 0;
    }

    if (alpha_defined) {
        keyboard.schemes[0].bg.bgra[3] = bg_alpha_defined ? bg_alpha : alpha;
        keyboard.schemes[0].fg.bgra[3] = alpha;
        keyboard.schemes[0].high.bgra[3] = alpha;
        keyboard.schemes[1].bg.bgra[3] = bg_alpha_defined ? bg_alpha : alpha;
        keyboard.schemes[1].fg.bgra[3] = alpha;
        keyboard.schemes[1].high.bgra[3] = alpha;
    } else if (bg_alpha_defined) {
        keyboard.schemes[0].bg.bgra[3] = bg_alpha;
        keyboard.schemes[1].bg.bgra[3] = bg_alpha;
    }

    if (fc_font_pattern) {
        for (i = 0; i < countof(schemes); i++)
            schemes[i].font = fc_font_pattern;
    }

    if (rounding != DEFAULT_ROUNDING) {
        for (i = 0; i < countof(schemes); i++)
            schemes[i].rounding = rounding;
    }

    display = wl_display_connect(NULL);
    if (display == NULL) {
        die("Failed to create display\n");
    }

    draw_surf.ctx = &draw_ctx;
    draw_surf.back_buffer = &draw_surf_back_buffer;
    draw_surf.display_buffer = &draw_surf_display_buffer;
    popup_draw_surf.ctx = &draw_ctx;
    popup_draw_surf.back_buffer = &popup_draw_surf_back_buffer;
    popup_draw_surf.display_buffer = &popup_draw_surf_display_buffer;
    keyboard.surf = &draw_surf;
    keyboard.popup_surf = &popup_draw_surf;

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (compositor == NULL) {
        die("wl_compositor not available\n");
    }
    if (draw_ctx.shm == NULL) {
        die("wl_shm not available\n");
    }
    if (layer_shell == NULL) {
        die("layer_shell not available\n");
    }
    if (wm_base == NULL) {
        die("wm_base not available\n");
    }
    if (vkbd_mgr == NULL) {
        die("virtual_keyboard_manager not available\n");
    }

    /* glassine: the pen as a tablet tool on the paper (else it comes as the pointer) */
    pen_init(&ptr_pen, &osk);
    if (tablet_mgr && seat) {
        tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(tablet_mgr, seat);
        zwp_tablet_seat_v2_add_listener(tablet_seat, &tablet_seat_listener, NULL);
    }

    // A second round-trip to receive wl_outputs events
    wl_display_roundtrip(display);

    empty_region = wl_compositor_create_region(compositor);
    popup_xdg_positioner = xdg_wm_base_create_positioner(wm_base);

    keyboard.vkbd =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkbd_mgr, seat);
    if (keyboard.vkbd == NULL) {
        die("failed to init virtual keyboard_manager\n");
    }
    #ifdef SHIFT_SPACE_IS_TAB
    keyboard.shift_space_is_tab = true;
    #else
    keyboard.shift_space_is_tab = false;
    #endif

    kbd_init(&keyboard, (struct layout *)&layouts, layer_names_list,
             landscape_layer_names_list);

    /* glassine: the input method (--im, or upstream's --auto: show and hide only) */
    static char *pargv[16];
    if (predictor_cmd && strip_on) {    /* words split at spaces, no shell */
        int n = 0;
        for (char *t = strtok(predictor_cmd, " "); t && n < 15; t = strtok(NULL, " ")) pargv[n++] = t;
    }
    osk_init(&osk, &osk_ops, NULL, pargv[0] ? pargv : NULL, strip_on && im_mode, pen_cmd != NULL);
    static char *hargv[16];
    if (hw_cmd && im_mode) {            /* words split at spaces, no shell */
        int n = 0;
        for (char *t = strtok(hw_cmd, " "); t && n < 15; t = strtok(NULL, " ")) hargv[n++] = t;
        osk_handwriting(&osk, hargv[0] ? hargv : NULL);
    }
    paper_scale_update();               /* the outputs are known by now */
    if (pen_flag) unlink(pen_flag);     /* a mark from before this run counts nothing */
    if (im_mode) keyboard.osk = &osk;
    if (!im_mode) osk.strip_on = false;
    im_bind();
    if (im_mgr && im_mode) notify("osk im");

    for (i = 0; i < countof(schemes); i++) {
        schemes[i].font_description =
            pango_font_description_from_string(schemes[i].font);
        /* glassine: the hold alternate's label, 55 % of the key font */
        schemes[i].hint_font_description = pango_font_description_copy(schemes[i].font_description);
        int fs = pango_font_description_get_size(schemes[i].font_description);
        if (fs > 0) {
            if (pango_font_description_get_size_is_absolute(schemes[i].font_description))
                pango_font_description_set_absolute_size(schemes[i].hint_font_description, fs * 0.55);
            else
                pango_font_description_set_size(schemes[i].hint_font_description, (int)(fs * 0.55));
        }
        /* glassine: the strip's words (the likeliest bold) and its notes */
        schemes[i].word_font = pango_font_description_copy(schemes[i].font_description);
        schemes[i].note_font = pango_font_description_copy(schemes[i].font_description);
        if (fs > 0) {
            if (pango_font_description_get_size_is_absolute(schemes[i].font_description)) {
                pango_font_description_set_absolute_size(schemes[i].word_font, fs * 0.85);
                pango_font_description_set_absolute_size(schemes[i].note_font, fs * 0.75);
            } else {
                pango_font_description_set_size(schemes[i].word_font, (int)(fs * 0.85));
                pango_font_description_set_size(schemes[i].note_font, (int)(fs * 0.75));
            }
        }
        pango_font_description_set_weight(schemes[i].word_font, PANGO_WEIGHT_NORMAL);
        schemes[i].best_font = pango_font_description_copy(schemes[i].word_font);
        pango_font_description_set_weight(schemes[i].best_font, PANGO_WEIGHT_SEMIBOLD);
    }

    if (!hidden)
        show();

    struct pollfd fds[6];
    int WAYLAND_FD = 0;
    int SIGNAL_FD = 1;
    int HOLD_FD = 2;    /* glassine: long press */
    int OSK_FD = 3;     /* glassine: the predictor (up to 2) */
    fds[WAYLAND_FD].events = POLLIN;
    fds[SIGNAL_FD].events = POLLIN;
    fds[HOLD_FD].events = POLLIN;
    kbd_hold_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (kbd_hold_timer_fd < 0) {
        fprintf(stderr, "no timerfd (%d): long press off\n", errno);
        keyboard.hold_ms = 0;
    }
    fds[HOLD_FD].fd = kbd_hold_timer_fd;

    fds[WAYLAND_FD].fd = wl_display_get_fd(display);
    if (fds[WAYLAND_FD].fd == -1) {
        die("Failed to get wayland_fd: %d\n", errno);
    }

    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGUSR1);
    sigaddset(&signal_mask, SIGUSR2);
    sigaddset(&signal_mask, SIGRTMIN);
    sigaddset(&signal_mask, SIGWINCH);
    sigaddset(&signal_mask, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &signal_mask, NULL) == -1) {
        die("Failed to disable handled signals: %d\n", errno);
    }

    fds[SIGNAL_FD].fd = signalfd(-1, &signal_mask, 0);
    if (fds[SIGNAL_FD].fd == -1) {
        die("Failed to get signalfd: %d\n", errno);
    }

    while (run_display) {
        wl_display_flush(display);
        /* glassine: the predictor's fds, and the earliest timer of the input method */
        int nosk = osk_poll_fds(&osk, fds + OSK_FD);
        int64_t now = monotime(), dl = osk_next_deadline(&osk);
        if (im_retry_at >= 0 && (dl < 0 || im_retry_at < dl)) dl = im_retry_at;
        int wait = dl < 0 ? -1 : dl <= now ? 0 : (int)(dl - now);
        for (int f = 0; f < OSK_FD; f++) fds[f].revents = 0;
        poll(fds, OSK_FD + nosk, wait);
        now = monotime();
        if (nosk) osk_handle(&osk, fds + OSK_FD, nosk, now);
        osk_tick(&osk, now);
        if (im_retry_at >= 0 && now >= im_retry_at) im_bind();
        reap_helpers();

        if (fds[WAYLAND_FD].revents & POLLIN)
            wl_display_dispatch(display);
        if (fds[WAYLAND_FD].revents & POLLERR) {
            die("Exceptional condition on wayland socket.\n");
        }
        if (fds[WAYLAND_FD].revents & POLLHUP) {
            die("Wayland socket has been disconnected.\n");
        }

        if (fds[HOLD_FD].revents & POLLIN) {
            uint64_t n;
            if (read(fds[HOLD_FD].fd, &n, sizeof n) == sizeof n)
                kbd_hold_fire(&keyboard);
        }

        if (fds[SIGNAL_FD].revents & POLLIN) {
            struct signalfd_siginfo si;

            if (read(fds[SIGNAL_FD].fd, &si, sizeof(si)) != sizeof(si))
                fprintf(stderr, "Signal read error: %d", errno);
            else if (si.ssi_signo == SIGUSR1)
                hide();
            else if (si.ssi_signo == SIGUSR2)
                session_show();
            else if (si.ssi_signo == SIGRTMIN)
                toggle_visibility();
            else if (si.ssi_signo == SIGWINCH)
                pen_near();
            else if (si.ssi_signo == SIGPIPE)
                pipewarn();
        }
    }

    osk_finish(&osk);
    if (fc_font_pattern) {
        free((void *)fc_font_pattern);
        for (i = 0; i < countof(schemes); i++)
            schemes[i].font = NULL;
    }

    return 0;
}
