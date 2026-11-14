/* ti-client: a text field for the end-to-end test (test/e2e/run.sh). One window with one
 * text-input-v3 field that keeps its text, applies what the input method sends (delete
 * surrounding text, commit string) and reports it back as surrounding text, like a
 * toolkit's entry. Every change of the text is printed on stdout as
 *     text <the text, \n escaped>|<cursor byte offset>
 * and every key event as "key CODE 1|0". Edits are counted: "edits N" after each done
 * that changed the text (a pick has to arrive as one edit).
 *
 * ti-client [-p PURPOSE] [-H HINTS] [-n] [-x] [-t TEXT]
 *   -p, -H  the field's content type (text-input-v3 numbers)
 *   -n      the field reports no surrounding text
 *   -x      no text input at all (an app without it: keys only)
 *   -t      the text the field starts with (the cursor at its end) */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm;
static struct zwp_text_input_manager_v3 *tim;
static struct zwp_text_input_v3 *ti;
static struct wl_surface *surf;
static int purpose, hints, no_surrounding, no_ti, configured;
static char text[8192];
static size_t cursor;
static uint32_t commits;
static int edits;
/* the pending input method state (applied on done) */
static char *pend_commit;
static uint32_t pend_del_before, pend_del_after;

static void out_text(void)
{
    printf("text ");
    for (const char *p = text; *p; p++) {
        if (*p == '\n') fputs("\\n", stdout);
        else if (*p == '\\') fputs("\\\\", stdout);
        else putchar(*p);
    }
    printf("|%zu\n", cursor);
    fflush(stdout);
}

static void send_state(void)
{
    if (!no_surrounding) zwp_text_input_v3_set_surrounding_text(ti, text, (int32_t)cursor, (int32_t)cursor);
    zwp_text_input_v3_set_content_type(ti, hints, purpose);
    zwp_text_input_v3_commit(ti);
    commits++;
}

static void ti_enter(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s)
{
    zwp_text_input_v3_enable(t);
    zwp_text_input_v3_set_text_change_cause(t, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER);
    send_state();
    printf("focus 1\n");
    fflush(stdout);
}
static void ti_leave(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s)
{
    zwp_text_input_v3_disable(t);
    zwp_text_input_v3_commit(t);
    commits++;
    printf("focus 0\n");
    fflush(stdout);
}
static void ti_preedit(void *d, struct zwp_text_input_v3 *t, const char *txt, int32_t b, int32_t e)
{
    printf("preedit %s\n", txt ? txt : "");
    fflush(stdout);
}
static void ti_commit_string(void *d, struct zwp_text_input_v3 *t, const char *txt)
{
    free(pend_commit);
    pend_commit = txt ? strdup(txt) : NULL;
}
static void ti_delete(void *d, struct zwp_text_input_v3 *t, uint32_t before, uint32_t after)
{
    pend_del_before = before;
    pend_del_after = after;
}
static void ti_done(void *d, struct zwp_text_input_v3 *t, uint32_t serial)
{
    int changed = 0;
    size_t len = strlen(text);
    if (pend_del_before || pend_del_after) {
        size_t b = pend_del_before > cursor ? cursor : pend_del_before;
        size_t a = pend_del_after > len - cursor ? len - cursor : pend_del_after;
        memmove(text + cursor - b, text + cursor + a, len - cursor - a + 1);
        cursor -= b;
        len -= a + b;
        changed = 1;
    }
    if (pend_commit && *pend_commit) {
        size_t n = strlen(pend_commit);
        if (len + n < sizeof text) {
            memmove(text + cursor + n, text + cursor, len - cursor + 1);
            memcpy(text + cursor, pend_commit, n);
            cursor += n;
            changed = 1;
        }
    }
    free(pend_commit);
    pend_commit = NULL;
    pend_del_before = pend_del_after = 0;
    if (changed) {
        edits++;
        out_text();
        printf("edits %d\n", edits);
        fflush(stdout);
        zwp_text_input_v3_set_text_change_cause(t, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD);
        send_state();
    }
}
static const struct zwp_text_input_v3_listener ti_l = {
    ti_enter, ti_leave, ti_preedit, ti_commit_string, ti_delete, ti_done,
};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) { close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *a) {}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st)
{
    printf("key %u %u\n", key, st);
    fflush(stdout);
    /* a key the field acts on itself: Backspace deletes, Enter is a line break */
    if (st != 1) return;
    size_t len = strlen(text);
    if (key == 14 && cursor > 0) {          /* KEY_BACKSPACE: one character */
        size_t c = cursor - 1;
        while (c > 0 && (text[c] & 0xC0) == 0x80) c--;
        memmove(text + c, text + cursor, len - cursor + 1);
        cursor = c;
    } else if (key == 28 && len + 1 < sizeof text) {  /* KEY_ENTER */
        memmove(text + cursor + 1, text + cursor, len - cursor + 1);
        text[cursor++] = '\n';
    } else return;
    out_text();
    if (!ti) return;
    zwp_text_input_v3_set_text_change_cause(ti, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER);
    send_state();
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) {}
static const struct wl_keyboard_listener kb_l = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };

static void draw(int w, int h)
{
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("ti", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) exit(1);
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memset(p, 0xff, size);
    munmap(p, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(surf, b, 0, 0);
    wl_surface_damage(surf, 0, 0, w, h);
    wl_surface_commit(surf);
}
static void xs_conf(void *d, struct xdg_surface *x, uint32_t s)
{
    xdg_surface_ack_configure(x, s);
    if (!configured) { configured = 1; draw(200, 100); }
}
static const struct xdg_surface_listener xs_l = { xs_conf };
static void tl_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {}
static void tl_close(void *d, struct xdg_toplevel *t) { exit(0); }
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener tl_l = { tl_conf, tl_close, tl_bounds, tl_caps };

static void reg(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name))
        tim = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
}
static void reg_rm(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_l = { reg, reg_rm };

int main(int argc, char **argv)
{
    int o;
    while ((o = getopt(argc, argv, "p:H:nxt:")) != -1) {
        if (o == 'p') purpose = atoi(optarg);
        else if (o == 'H') hints = atoi(optarg);
        else if (o == 'n') no_surrounding = 1;
        else if (o == 'x') no_ti = 1;
        else if (o == 't') snprintf(text, sizeof text, "%s", optarg);
    }
    cursor = strlen(text);
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 1;
    struct wl_registry *r = wl_display_get_registry(dpy);
    wl_registry_add_listener(r, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !seat || !wm || !tim) { fprintf(stderr, "ti-client: globals missing\n"); return 1; }
    xdg_wm_base_add_listener(wm, &wm_l, NULL);
    wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &kb_l, NULL);
    if (!no_ti) {
        ti = zwp_text_input_manager_v3_get_text_input(tim, seat);
        zwp_text_input_v3_add_listener(ti, &ti_l, NULL);
    }
    surf = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_l, NULL);
    xdg_toplevel_set_app_id(tl, "ticlient");
    wl_surface_commit(surf);
    out_text();
    while (wl_display_dispatch(dpy) != -1) ;
    return 0;
}
