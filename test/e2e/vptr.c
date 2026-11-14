/* vptr: a virtual pointer for the end-to-end test (run.sh). It keeps the seat's pointer
 * capability while it runs and reads commands from a FIFO, one per line, in layout px of
 * the output (W x H from argv):
 *   tap X Y | hold X Y MS | down X Y | up
 *   stroke X,Y X,Y ...   pressed from the first point to the last (a pen writing; sway
 *                        hands a pen to a client without tablet support as this pointer)
 * vptr FIFO W H */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *mgr;
static struct zwlr_virtual_pointer_v1 *vp;
static struct wl_display *d;
static int W, H;

static void g(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
        mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void gr(void *data, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener rl = { g, gr };

static uint32_t ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void move(int x, int y)
{
    zwlr_virtual_pointer_v1_motion_absolute(vp, ms(), x, y, W, H);
    zwlr_virtual_pointer_v1_frame(vp);
    wl_display_roundtrip(d);
}
static void btn(int down)
{
    zwlr_virtual_pointer_v1_button(vp, ms(), BTN_LEFT, down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(vp);
    wl_display_roundtrip(d);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: vptr FIFO W H\n"); return 1; }
    W = atoi(argv[2]);
    H = atoi(argv[3]);
    d = wl_display_connect(NULL);
    if (!d) return 1;
    wl_registry_add_listener(wl_display_get_registry(d), &rl, NULL);
    wl_display_roundtrip(d);
    if (!mgr) { fprintf(stderr, "vptr: no virtual pointer manager\n"); return 1; }
    vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, NULL);
    wl_display_roundtrip(d);
    fprintf(stderr, "vptr: ready\n");
    for (;;) {
        FILE *f = fopen(argv[1], "r");
        if (!f) return 1;
        static char l[65536];
        int x, y, t;
        while (fgets(l, sizeof l, f)) {
            if (sscanf(l, "tap %d %d", &x, &y) == 2) { move(x, y); btn(1); usleep(60000); btn(0); }
            else if (sscanf(l, "hold %d %d %d", &x, &y, &t) == 3) { move(x, y); btn(1); usleep(t * 1000); btn(0); }
            else if (sscanf(l, "down %d %d", &x, &y) == 2) { move(x, y); btn(1); }
            else if (!strncmp(l, "up", 2)) btn(0);
            else if (!strncmp(l, "stroke ", 7)) {
                char *p = l + 7;
                int first = 1;
                double px, py;
                for (int k = 0; sscanf(p, " %lf,%lf%n", &px, &py, &k) == 2; ) {
                    p += k;
                    move((int)(px + 0.5), (int)(py + 0.5));
                    if (first) { btn(1); first = 0; }
                    usleep(4000);
                }
                if (!first) btn(0);
            }
        }
        fclose(f);
    }
}
