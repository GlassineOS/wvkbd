#include "proto/virtual-keyboard-unstable-v1-client-protocol.h"
#include <linux/input-event-codes.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ctype.h>
#include <time.h>
#include <sys/timerfd.h>
#include "keyboard.h"
#include <linux/input-event-codes.h>

#include "drw.h"
#include "os-compatibility.h"
#include "textrules.h"

#define MAX_LAYERS 25

/* glassine: the key a Copy key's (and a hold's) character is put on in the temporary
 * keymap: evdev KEY_RO (xkb <AB11>, DOM code IntlRo), a character key no layout here
 * uses. Upstream uses KEY_COMPOSE (127, <COMP>), the Menu key: Chromium opens its
 * context menu on that key code, whatever character it carries. A code without a
 * meaning (KEY_UNKNOWN) does not work either: Chromium drops the event */
#define KBD_UNICODE_KEY 89

/* glassine: long press (see struct kbd). The timer is main.c's timerfd */
int kbd_hold_timer_fd = -1;
static bool hold_skip_press_draw;   /* the held key is drawn pressed already */

static void
hold_arm(int ms)
{
    if (kbd_hold_timer_fd < 0) return;
    struct itimerspec t = { .it_value = { ms / 1000, (long)(ms % 1000) * 1000000L } };
    timerfd_settime(kbd_hold_timer_fd, 0, &t, NULL);
}

static void
hold_disarm(void)
{
    if (kbd_hold_timer_fd < 0) return;
    struct itimerspec t = { 0 };
    timerfd_settime(kbd_hold_timer_fd, 0, &t, NULL);
}

static void kbd_press_key_now(struct kbd *kb, struct key *k, uint32_t time);

/* glassine: the input method's side of a key (osk.h). A private field: nothing typed
 * is printed or logged */
static bool
kbd_private(struct kbd *kb)
{
    return kb->osk && imf_private(&kb->osk->f);
}

/* the key's label is one letter (a capital at a sentence start applies to it) */
static bool
letter_key(struct key *k)
{
    uint32_t c;
    size_t n = strlen(k->label);
    return n && (size_t)tr_dec(k->label, n, &c) == n && tr_letter(c);
}

static bool
kbd_shifted(struct kbd *kb, struct key *k)
{
    return (kb->mods & Shift) ||
           ((kb->mods & CapsLock) && strlen(k->label) == 1 && isalpha(k->label[0])) ||
           (kb->osk && osk_shifted(kb->osk) && letter_key(k));
}

/* text through the input method; false: none is active, send keys */
static bool
kbd_commit(struct kbd *kb, const char *text)
{
    return kb->osk && osk_text(kb->osk, text, monotime());
}

/* the text a key types (NULL: it has none, a key event it is). buf: room for 5 */
static const char *
key_text(struct kbd *kb, struct key *k, char *buf)
{
    if (kb->mods & (Ctrl | Alt | Super | AltGr)) return NULL;
    /* (a Copy key's code_mod is a code point, not modifiers) */
    if (k->type != Copy && (k->code_mod & (Ctrl | Alt | Super))) return NULL;
    bool sh = kbd_shifted(kb, k);
    switch (k->type) {
    case Code:
        switch (k->code) {
        case KEY_SPACE: return " ";
        case KEY_ESC: case KEY_TAB: case KEY_ENTER: case KEY_KPENTER: case KEY_BACKSPACE:
        case KEY_DELETE: case KEY_INSERT: case KEY_UP: case KEY_DOWN: case KEY_LEFT:
        case KEY_RIGHT: case KEY_HOME: case KEY_END: case KEY_PAGEUP: case KEY_PAGEDOWN:
            return NULL;
        }
        if (k->code >= KEY_F1 && k->code <= KEY_F12) return NULL;
        {
            const char *l = sh && k->shift_label ? k->shift_label : k->label;
            uint32_t c;
            size_t n = l ? strlen(l) : 0;
            if (!n || n > 4 || (size_t)tr_dec(l, n, &c) != n || c < 0x20 || c == 0xFFFD) return NULL;
            return l;
        }
    case Copy: {
        uint32_t c = sh ? k->code_mod : k->code;
        if (c < 0x20 || c > 0x10FFFF) return NULL;
        buf[tr_enc(c, buf)] = 0;
        return buf;
    }
    case Text:
        return k->text;
    default:
        return NULL;
    }
}

/* a key that went through the input method: drawn pressed, nothing to release */
static void
kbd_press_im(struct kbd *kb, struct key *k, uint32_t time)
{
    kb->last_swipe = kb->last_press = k;
    kb->last_press_im = true;
    kb->last_press_offset = time - monotime();
    kbd_draw_key(kb, k, Press);
    if ((kb->print || kb->print_intersect) && !kbd_private(kb))
        kbd_print_key_stdout(kb, k);
    if (kb->compose) kb->compose++;
}

/* a Text key with no input method: its characters as Copy keys, one after another */
static void
kbd_type_text(struct kbd *kb, const char *t, uint32_t time)
{
    for (size_t i = 0, n = strlen(t); i < n;) {
        uint32_t c;
        i += tr_dec(t + i, n - i, &c);
        create_and_upload_keymap(kb, kb->layout->keymap_name, c);
        zwp_virtual_keyboard_v1_modifiers(kb->vkbd, 0, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(kb->vkbd, time, KBD_UNICODE_KEY, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(kb->vkbd, time, KBD_UNICODE_KEY, WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    create_and_upload_keymap(kb, kb->layout->keymap_name, 0);
    zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
}

/* glassine: themable keys (outline, gap) */
int kbd_key_border = 2;
int kbd_stroke_width = 0;

static void
draw_key_face(struct drwsurf *ds, struct key *k, uint32_t y, struct clr_scheme *sc,
              Color fill)
{
    int sw = sc->stroke.bgra[3] ? kbd_stroke_width : 0;
    if (sw > 0 && k->w > 2 * (KBD_KEY_BORDER + sw) && k->h > 2 * (KBD_KEY_BORDER + sw)) {
        draw_inset(ds, k->x, y, k->w, k->h, KBD_KEY_BORDER, sc->stroke, sc->rounding);
        int r = sc->rounding > sw ? sc->rounding - sw : 0;
        draw_inset(ds, k->x, y, k->w, k->h, KBD_KEY_BORDER + sw, fill, r);
    } else {
        draw_inset(ds, k->x, y, k->w, k->h, KBD_KEY_BORDER, fill, sc->rounding);
    }
}

/* lazy die macro */
#define die(...)                                                               \
    fprintf(stderr, __VA_ARGS__);                                              \
    exit(1)

#ifndef KEYMAP
#error "make sure to define KEYMAP"
#endif
#include KEYMAP

static struct layout *purpose_layout(struct kbd *kb, struct layout *l);
static void lang_follow(struct kbd *kb, struct layout *l);

void
kbd_switch_layout(struct kbd *kb, struct layout *l, size_t layer_index)
{
    l = purpose_layout(kb, l);  /* glassine: an address field's bottom row */
    lang_follow(kb, l);
    kb->prevlayout = kb->layout;
    if ((kb->layer_index != kb->last_abc_index) && (kb->layout->abc)) {
        kb->last_abc_layout = kb->layout;
        kb->last_abc_index = kb->layer_index;
    }
    kb->layer_index = layer_index;
    kb->layout = l;
    if (kb->debug)
        fprintf(stderr, "Switching to layout %s, layer_index %ld\n",
                kb->layout->name, layer_index);
    if (!l->keymap_name)
        fprintf(stderr, "Layout has no keymap!"); // sanity check
    if ((!kb->prevlayout) ||
        (strcmp(kb->prevlayout->keymap_name, kb->layout->keymap_name) != 0)) {
        fprintf(stderr, "Switching to keymap %s\n", kb->layout->keymap_name);
        create_and_upload_keymap(kb, kb->layout->keymap_name, 0);
    }
    kbd_draw_layout(kb);
}

void
kbd_next_layer(struct kbd *kb, struct key *k, bool invert)
{
    size_t layer_index = kb->layer_index;
    if ((kb->mods & Ctrl) || (kb->mods & Alt) || (kb->mods & AltGr) ||
        ((bool)kb->compose)) {
        // with modifiers ctrl/alt/altgr: switch to the first layer
        layer_index = 0;
        kb->mods = 0;
    } else if ((kb->mods & Shift) || (kb->mods & CapsLock) || (invert)) {
        // with modifiers shift/capslock or invert set: switch to the previous
        // layout in the layer sequence
        if (layer_index > 0) {
            layer_index--;
        } else {
            size_t layercount = 0;
            for (size_t i = 0; layercount == 0; i++) {
                if (kb->landscape) {
                    if (kb->landscape_layers[i] == NumLayouts)
                        layercount = i;
                } else {
                    if (kb->layers[i] == NumLayouts)
                        layercount = i;
                }
            }
            layer_index = layercount - 1;
        }
        if (!invert)
            kb->mods ^= Shift;
    } else {
        // normal behaviour: switch to the next layout in the layer sequence
        layer_index++;
    }
    size_t layercount = 0;
    for (size_t i = 0; layercount == 0; i++) {
        if (kb->landscape) {
            if (kb->landscape_layers[i] == NumLayouts)
                layercount = i;
        } else {
            if (kb->layers[i] == NumLayouts)
                layercount = i;
        }
    }
    if (layer_index >= layercount) {
        if (kb->debug)
            fprintf(stderr, "wrapping layer_index back to start\n");
        layer_index = 0;
    }
    enum layout_id layer;
    if (kb->landscape) {
        layer = kb->landscape_layers[layer_index];
    } else {
        layer = kb->layers[layer_index];
    }
    if (((bool)kb->compose) && (k)) {
        kb->compose = 0;
        kbd_draw_key(kb, k, Unpress);
    }
    kbd_switch_layout(kb, &kb->layouts[layer], layer_index);
}

uint8_t
kbd_get_rows(struct layout *l)
{
    uint8_t rows = 0;
    struct key *k = l->keys;
    while (k->type != Last) {
        if (k->type == EndRow) {
            rows++;
        }
        k++;
    }
    return rows + 1;
}

enum layout_id *
kbd_init_layers(char *layer_names_list)
{
    enum layout_id *layers;
    uint8_t numlayers = 0;
    bool found;
    char *s;
    int i;

    layers = malloc(MAX_LAYERS * sizeof(enum layout_id));
    s = strtok(layer_names_list, ",");
    while (s != NULL) {
        if (numlayers + 1 == MAX_LAYERS) {
            fprintf(stderr, "too many layers specified");
            exit(3);
        }
        found = false;
        for (i = 0; i < NumLayouts - 1; i++) {
            if (layouts[i].name && strcmp(layouts[i].name, s) == 0) {
                fprintf(stderr, "layer #%d = %s\n", numlayers + 1, s);
                layers[numlayers++] = i;
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "No such layer: %s\n", s);
            exit(3);
        }
        s = strtok(NULL, ",");
    }
    layers[numlayers] = NumLayouts; // mark the end of the sequence
    if (numlayers == 0) {
        fprintf(stderr, "No layers defined\n");
        exit(3);
    }

    return layers;
}

void
kbd_init(struct kbd *kb, struct layout *layouts, char *layer_names_list,
         char *landscape_layer_names_list)
{
    int i;

    fprintf(stderr, "Initializing keyboard\n");

    kb->layouts = layouts;

    for (i = 0; i < NumLayouts - 1; i++)
        ;
    fprintf(stderr, "Found %d layouts\n", i);

    kb->layer_index = 0;
    kb->last_abc_index = 0;

    if (layer_names_list)
        kb->layers = kbd_init_layers(layer_names_list);
    if (landscape_layer_names_list)
        kb->landscape_layers = kbd_init_layers(landscape_layer_names_list);

    i = 0;
    enum layout_id lid = kb->layers[0];
    while (lid != NumLayouts) {
        lid = kb->layers[++i];
    }
    fprintf(stderr, "Found %d layers\n", i);

    enum layout_id layer;
    if (kb->landscape) {
        layer = kb->landscape_layers[kb->layer_index];
    } else {
        layer = kb->layers[kb->layer_index];
    }

    kb->layout = &kb->layouts[layer];
    kb->last_abc_layout = &kb->layouts[layer];
    kb->lang_layout = kb->layout;    /* glassine */

    /* upload keymap */
    create_and_upload_keymap(kb, kb->layout->keymap_name, 0);
}

void
kbd_init_layout(struct layout *l, uint32_t y0, uint32_t width, uint32_t height)
{
    uint32_t x = 0, y = y0;
    uint8_t rows = kbd_get_rows(l);

    l->keyheight = height / rows;

    struct key *k = l->keys;
    double rowlength = kbd_get_row_length(k);
    double rowwidth = 0.0;
    while (k->type != Last) {
        if (k->type == EndRow) {
            y += l->keyheight;
            x = 0;
            rowwidth = 0.0;
            rowlength = kbd_get_row_length(k + 1);
        } else if (k->width > 0) {
            k->x = x;
            k->y = y;
            k->w = ((double)width / rowlength) * k->width;
            x += k->w;
            rowwidth += k->width;
            if (x < (rowwidth / rowlength) * (double)width) {
                k->w++;
                x++;
            }
        }
        k->h = l->keyheight;
        k++;
    }
}

double
kbd_get_row_length(struct key *k)
{
    double l = 0.0;
    while ((k->type != Last) && (k->type != EndRow)) {
        l += k->width;
        k++;
    }
    return l;
}

struct key *
kbd_get_key(struct kbd *kb, uint32_t x, uint32_t y)
{
    struct layout *l = kb->layout;
    struct key *k = l->keys;
    if (kb->debug)
        fprintf(stderr, "get key: +%d+%d\n", x, y);
    while (k->type != Last) {
        if ((k->type != EndRow) && (k->type != Pad) && (k->type != Pad) &&
            (x >= k->x) && (y >= k->y) && (x < k->x + k->w) &&
            (y < k->y + k->h)) {
            return k;
        }
        k++;
    }
    return NULL;
}

size_t
kbd_get_layer_index(struct kbd *kb, struct layout *l)
{
    for (size_t i = 0; i < NumLayouts - 1; i++) {
        if (l == &kb->layouts[i]) {
            return i;
        }
    }
    return 0;
}

void
zwp_virtual_keyboard_v1_key_mods(struct zwp_virtual_keyboard_v1 *vkbd, uint32_t time, uint32_t mods, enum wl_keyboard_key_state state)
{
    if ((mods & Shift) == Shift)
        zwp_virtual_keyboard_v1_key(vkbd, time, KEY_LEFTSHIFT, state);
    if ((mods & CapsLock) == CapsLock)
        zwp_virtual_keyboard_v1_key(vkbd, time, KEY_CAPSLOCK, state);
    if ((mods & Ctrl) == Ctrl)
        zwp_virtual_keyboard_v1_key(vkbd, time, KEY_LEFTCTRL, state);
    if ((mods & Alt) == Alt)
        zwp_virtual_keyboard_v1_key(vkbd, time, KEY_LEFTALT, state);
    if ((mods & Super) == Super)
        zwp_virtual_keyboard_v1_key(vkbd, time, KEY_LEFTMETA, state);
    if ((mods & AltGr) == AltGr)
        zwp_virtual_keyboard_v1_key(vkbd, time, KEY_RIGHTALT, state);
}

void
kbd_unpress_key(struct kbd *kb, uint32_t time)
{
    bool unlatch_shift, unlatch_ctrl, unlatch_alt, unlatch_super, unlatch_altgr, unlatch_tab;
    unlatch_shift = unlatch_ctrl = unlatch_alt = unlatch_super = unlatch_altgr = unlatch_tab = false;
    if (kb->hold_key) {
        /* glassine: a held key ends: the alternate was typed (only redraw it),
         * or it was a tap (type its own character now, then release it below) */
        struct key *k = kb->hold_key;
        kb->hold_key = NULL;
        hold_disarm();
        if (kb->hold_fired) {
            kb->hold_fired = false;
            kbd_draw_key(kb, k, Unpress);
            return;
        }
        hold_skip_press_draw = true;
        kbd_press_key_now(kb, k, time);
        hold_skip_press_draw = false;
    }
    if (kb->last_press) {
        unlatch_shift = (kb->mods & Shift) == Shift;
        unlatch_ctrl = (kb->mods & Ctrl) == Ctrl;
        unlatch_alt = (kb->mods & Alt) == Alt;
        unlatch_super = (kb->mods & Super) == Super;
        unlatch_altgr = (kb->mods & AltGr) == AltGr;

        if (kb->last_press_im) {
            /* glassine: it went through the input method: no key to release */
            kb->last_press_im = false;
            if (kb->osk && kb->last_press->type == Code && kb->last_press->code == KEY_BACKSPACE)
                osk_backspace_up(kb->osk);
        } else if (kb->last_press->type == Copy) {
            if (kb->debug) fprintf(stderr, "release copy key (unlatch_shift=%d, mods=%d)\n", unlatch_shift, kb->mods);
            zwp_virtual_keyboard_v1_key(kb->vkbd, time, KBD_UNICODE_KEY, // the unicode key
                                        WL_KEYBOARD_KEY_STATE_RELEASED);
        } else {
            if (kb->debug) fprintf(stderr, "release key %d", kb->last_press->code);
            if ((kb->shift_space_is_tab) && (kb->last_press->code == KEY_SPACE) && (unlatch_shift)) {
                // shift + space is tab
                unlatch_shift = false;
                unlatch_tab = true;
                kb->mods ^= Shift;
                zwp_virtual_keyboard_v1_key(kb->vkbd, time, KEY_TAB,
                                            WL_KEYBOARD_KEY_STATE_RELEASED);
            } else {
                zwp_virtual_keyboard_v1_key(kb->vkbd, time,
                                            unlatch_shift && kb->last_press->shift_code ?
                                            kb->last_press->shift_code : kb->last_press->code,
                                            WL_KEYBOARD_KEY_STATE_RELEASED);
            }
        }

        if (unlatch_shift) {
            if (kb->debug) fprintf(stderr, "unlatch shift");
            kb->mods ^= Shift;
            zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, Shift, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        if (unlatch_ctrl) {
            kb->mods ^= Ctrl;
            zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, Ctrl, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        if (unlatch_alt) {
            kb->mods ^= Alt;
            zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, Alt, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        if (unlatch_super) {
            kb->mods ^= Super;
            zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, Super, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        if (unlatch_altgr) {
            kb->mods ^= AltGr;
            zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, AltGr, WL_KEYBOARD_KEY_STATE_RELEASED);
        }

        if (unlatch_shift||unlatch_ctrl||unlatch_alt||unlatch_super||unlatch_altgr||unlatch_tab) {
            zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        }

        if (kb->compose >= 2) {
            kb->compose = 0;
            kbd_switch_layout(kb, kb->last_abc_layout, kb->last_abc_index);
        } else if (unlatch_shift||unlatch_ctrl||unlatch_alt||unlatch_super||unlatch_altgr||unlatch_tab) {
            kbd_draw_layout(kb);
        } else {
            kbd_draw_key(kb, kb->last_press, Unpress);
        }

        kb->last_press = NULL;
    }
}

void
kbd_release_key(struct kbd *kb, uint32_t time)
{
    kbd_unpress_key(kb, time);
    if (kb->print_intersect && kb->last_swipe) {
        printf("\n");
        // Important so autocompleted words get typed in time
        fflush(stdout);
        kbd_draw_layout(kb);
        kb->last_swipe = NULL;
    }

    kbd_clear_last_popup(kb);
}

void
kbd_motion_key(struct kbd *kb, uint32_t time, uint32_t x, uint32_t y)
{
    struct key *intersect_key;
    intersect_key = kbd_get_key(kb, x, y);

    // Ignore keyboard motion until a new key is reached.
    if (kb->last_press == intersect_key) {
        return;
    }

    // Output intersecting keys
    // (for external 'swiping'-based accelerators).
    if (kb->print_intersect) {
        if (kb->last_press) {
            kbd_unpress_key(kb, time);
            // Redraw last press as a swipe.
            kbd_draw_key(kb, kb->last_swipe, Swipe);
        }

        if (intersect_key && (!kb->last_swipe ||
                              intersect_key->label != kb->last_swipe->label)) {
            kbd_print_key_stdout(kb, intersect_key);
            kb->last_swipe = intersect_key;
            kbd_draw_key(kb, kb->last_swipe, Swipe);
        }
    } else {
        kbd_unpress_key(kb, time);
    }

    kbd_clear_last_popup(kb);
}

/* glassine: a key with a hold alternate waits (see struct kbd) */
void
kbd_press_key(struct kbd *kb, struct key *k, uint32_t time)
{
    if (k->hold && kb->hold_ms > 0 && !kb->compose && (k->type == Code || k->type == Copy)) {
        kb->hold_key = k;
        kb->hold_fired = false;
        kb->hold_offset = time - monotime();
        kbd_draw_key(kb, k, Press);
        hold_arm(kb->hold_ms);
        return;
    }
    kbd_press_key_now(kb, k, time);
}

/* glassine: the timer ran out with the key still held: type its alternate
 * (as a Copy key, through a temporary keymap). No redraw: the key stays drawn
 * pressed until it is released, as for a tap */
void
kbd_hold_fire(struct kbd *kb)
{
    if (!kb->hold_key || kb->hold_fired) return;
    struct key *k = kb->hold_key;
    uint32_t time = (uint32_t)(monotime() + kb->hold_offset);
    kb->hold_fired = true;
    if (kb->debug && !kbd_private(kb)) fprintf(stderr, "hold: %s -> U+%04X\n", k->label, k->hold);
    char t[5] = { 0 };
    tr_enc(k->hold, t);
    if (!kbd_commit(kb, t)) {
        create_and_upload_keymap(kb, kb->layout->keymap_name, k->hold);
        zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(kb->vkbd, time, KBD_UNICODE_KEY, WL_KEYBOARD_KEY_STATE_PRESSED);   // the unicode key
        zwp_virtual_keyboard_v1_key(kb->vkbd, time, KBD_UNICODE_KEY, WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    if ((kb->print || kb->print_intersect) && k->hold_label && !kbd_private(kb)) {
        printf("%s", k->hold_label);
        fflush(stdout);
    }
    if (kb->mods & Shift) {     /* a latched shift ends with the character, as after a tap */
        kb->mods ^= Shift;
        zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, Shift, WL_KEYBOARD_KEY_STATE_RELEASED);
        zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        kbd_draw_layout(kb);
    }
}

static void
kbd_press_key_now(struct kbd *kb, struct key *k, uint32_t time)
{
    if ((kb->compose == 1) && (k->type != Compose) && (k->type != Mod)) {
        if ((k->type == NextLayer) || (k->type == BackLayer) ||
            ((k->type == Code) && (k->code == KEY_SPACE))) {
            kb->compose = 0;
            if (kb->debug)
                fprintf(stderr, "showing layout index\n");
            kbd_switch_layout(kb, &kb->layouts[Index], 0);
            return;
        } else if (k->layout) {
            kb->compose++;
            if (kb->debug)
                fprintf(stderr, "showing compose %d\n", kb->compose);
            kbd_switch_layout(kb, k->layout,
                              kbd_get_layer_index(kb, k->layout));
            return;
        } else {
            return;
        }
    }

    char tb[5];
    const char *t;
    switch (k->type) {
    case Code:
        /* glassine: with a field active, text goes through the input method (not
         * shift+space, which is Tab) */
        if (!(kb->shift_space_is_tab && k->code == KEY_SPACE && (kb->mods & Shift)) &&
            (t = key_text(kb, k, tb)) && kbd_commit(kb, t)) {
            kbd_press_im(kb, k, time);
            break;
        }
        if (k->code == KEY_BACKSPACE && kb->osk && !(kb->mods & (Ctrl | Alt | Super | AltGr)) &&
            osk_backspace(kb->osk, monotime())) {
            kbd_press_im(kb, k, time);
            break;
        }
        if (kb->osk && osk_key_capture(kb->osk, k->code, monotime())) {
            kbd_press_im(kb, k, time);  /* glassine: correcting a reading: Enter uses it */
            break;
        }
        if (kb->osk && k->code != KEY_BACKSPACE) osk_key(kb->osk, k->code, monotime());
        if (k->code_mod) {
            zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, k->code_mod, WL_KEYBOARD_KEY_STATE_PRESSED);
            if (k->reset_mod) {
                zwp_virtual_keyboard_v1_modifiers(kb->vkbd, k->code_mod, 0, 0,
                                                  0);
            } else {
                zwp_virtual_keyboard_v1_modifiers(
                    kb->vkbd, kb->mods ^ k->code_mod, 0, 0, 0);
            }
        } else {
            zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        }
        kb->last_swipe = kb->last_press = k;
        kb->last_press_offset = time - monotime();
        kbd_draw_key(kb, k, Press);
        if ((kb->shift_space_is_tab) && (k->code == KEY_SPACE) && (kb->mods & Shift)) {
            // shift space is tab
            zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, Shift, WL_KEYBOARD_KEY_STATE_RELEASED);
            zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods ^ Shift, 0, 0, 0);
            zwp_virtual_keyboard_v1_key(kb->vkbd, time, KEY_TAB,
                                        WL_KEYBOARD_KEY_STATE_PRESSED);
        } else {
            zwp_virtual_keyboard_v1_key(kb->vkbd, time, (kb->mods & Shift) && k->shift_code ?
                                        k->shift_code : k->code,
                                        WL_KEYBOARD_KEY_STATE_PRESSED);
        }
        if ((kb->print || kb->print_intersect) && !kbd_private(kb))
            kbd_print_key_stdout(kb, k);
        if (kb->compose) {
            if (kb->debug)
                fprintf(stderr, "pressing composed key\n");
            kb->compose++;
        }
        break;
    case Mod:
        kb->mods ^= k->code;
        if (kb->mods & k->code) {
            if (kb->debug)
                fprintf(stderr, "mod pressed\n");
            zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, k->code, WL_KEYBOARD_KEY_STATE_PRESSED);
        } else {
            if (kb->debug)
                fprintf(stderr, "mod released\n");
            zwp_virtual_keyboard_v1_key_mods(kb->vkbd, time, k->code, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
        if ((k->code == Shift) || (k->code == CapsLock)) {
            kbd_draw_layout(kb);
        } else {
            if (kb->mods & k->code) {
                kbd_draw_key(kb, k, Press);
            } else {
                kbd_draw_key(kb, k, Unpress);
            }
        }
        zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        break;
    case Layout:
        // switch to the layout determined by the key
        kbd_switch_layout(kb, k->layout, kbd_get_layer_index(kb, k->layout));
        // reset previous layout to default/first so we don't get any weird
        // cycles
        kb->last_abc_index = 0;
        if (kb->landscape) {
            kb->last_abc_layout = &kb->layouts[kb->landscape_layers[0]];
        } else {
            kb->last_abc_layout = &kb->layouts[kb->layers[0]];
        }
        break;
    case Compose:
        // switch to the associated layout determined by the *next* keypress
        if (kb->compose == 0) {
            kb->compose = 1;
        } else {
            kb->compose = 0;
        }
        if ((bool)kb->compose) {
            kbd_draw_key(kb, k, Press);
        } else {
            kbd_draw_key(kb, k, Unpress);
        }
        break;
    case NextLayer: //(also handles previous layer when shift modifier is on, or
                    //"first layer" with other modifiers)
        kbd_next_layer(kb, k, false);
        break;
    case BackLayer: // triggered when "Abc" keys are pressed
        // switch to the last active alphabetical layout
        if (kb->last_abc_layout) {
            kb->compose = 0;
            kbd_switch_layout(kb, kb->last_abc_layout, kb->last_abc_index);
            // reset previous layout to default/first so we don't get any weird
            // cycles
            kb->last_abc_index = 0;
            if (kb->landscape) {
                kb->last_abc_layout = &kb->layouts[kb->landscape_layers[0]];
            } else {
                kb->last_abc_layout = &kb->layouts[kb->layers[0]];
            }
        }
        break;
    case Text:
        if (kbd_commit(kb, k->text)) {
            kbd_press_im(kb, k, time);
        } else if (k->text) {
            kbd_type_text(kb, k->text, time);
            kbd_press_im(kb, k, time);      /* typed already: nothing to release */
        }
        break;
    case Copy:
        if ((t = key_text(kb, k, tb)) && kbd_commit(kb, t)) {
            kbd_press_im(kb, k, time);
            break;
        }
        // copy code as unicode chr by setting a temporary keymap
        kb->last_swipe = kb->last_press = k;
        kb->last_press_offset = time - monotime();
        kbd_draw_key(kb, k, Press);
        if (kb->mods & Shift) {
            if (kb->debug)
                    fprintf(stderr, "Pressing copy key (with shift)\n");
            create_and_upload_keymap(kb, kb->layout->keymap_name, k->code_mod);
        } else {
            if (kb->debug)
                    fprintf(stderr, "Pressing copy key\n");
            create_and_upload_keymap(kb, kb->layout->keymap_name, k->code);
        }
        zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(kb->vkbd, time, KBD_UNICODE_KEY, // the unicode key
                                    WL_KEYBOARD_KEY_STATE_PRESSED);
        if ((kb->print || kb->print_intersect) && !kbd_private(kb))
            kbd_print_key_stdout(kb, k);
        break;
    default:
        break;
    }
}

void
kbd_print_key_stdout(struct kbd *kb, struct key *k)
{
    /* Printed keys may slightly differ from the actual output
     * we generally print what is on the key LABEL and only support the normal
     * and shift layers. Other modifiers produce no output (Ctrl,Alt).
     * If a label has more than one character,
     * only the first character is printed,
     * because downpipe programs otherwise have no way of differentiating a swipe
     * from a single keypress.
     * */

    bool handled = true;
    if (k->type == Code) {
        switch (k->code) {
        case KEY_SPACE:
            printf(" ");
            break;
        case KEY_ENTER:
            printf("\n");
            break;
        case KEY_BACKSPACE:
            printf("\b");
            break;
        case KEY_TAB:
            printf("\t");
            break;
        default:
            handled = false;
            break;
        }
    } else if (k->type != Copy) {
        return;
    }

    if (!handled) {
        if ((kb->mods & Shift) || 
            ((kb->mods & CapsLock) & (strlen(k->label) == 1 && isalpha(k->label[0]))))
            kbd_print_first_utf8_char_stdout(k->shift_label);
        else if (!(kb->mods & Ctrl) && !(kb->mods & Alt) && !(kb->mods & Super))
            kbd_print_first_utf8_char_stdout(k->label);
    }
    fflush(stdout);
}

void kbd_print_first_utf8_char_stdout(const char *str) {
    unsigned char c = (unsigned char)str[0];
    int len;

    if ((c & 0x80) == 0) {
        len = 1;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
    } else {
        len = 1; // Invalid UTF-8
    }

    for (int i = 0; i < len && str[i] != '\0'; i++) {
        putchar(str[i]);
    }
}

void
kbd_clear_last_popup(struct kbd *kb)
{
    if (kb->last_popup_w && kb->last_popup_h) {
        drw_do_clear(kb->popup_surf, kb->last_popup_x, kb->last_popup_y,
                     kb->last_popup_w, kb->last_popup_h);
        kb->last_popup_w = kb->last_popup_h = 0;
    }
}

void
kbd_draw_key(struct kbd *kb, struct key *k, enum key_draw_type type)
{
    const char *label = kbd_shifted(kb, k) ? k->shift_label : k->label;
    if (kb->debug)
        fprintf(stderr, "Draw key +%d+%d %dx%d -> %s\n", k->x, k->y, k->w, k->h,
                label);
    struct clr_scheme *scheme = &kb->schemes[k->scheme];
    if (hold_skip_press_draw && type == Press && k == kb->last_press) return;
    if (!kb->surf->back_buffer || !kb->surf->back_buffer->cairo) return;   /* glassine: not shown yet */

    switch (type) {
    case None:
    case Unpress:
        draw_key_face(kb->surf, k, k->y, scheme, scheme->fg);
        drw_draw_text(kb->surf, scheme->text, k->x, k->y, k->w, k->h,
                  KBD_KEY_BORDER, label, scheme->font_description);
        break;
    case Press:
        draw_key_face(kb->surf, k, k->y, scheme,
                      kb->show_highlight ? scheme->high : scheme->fg);
        drw_draw_text(kb->surf,
                  kb->show_highlight ? scheme->text_press : scheme->text,
                  k->x, k->y, k->w, k->h, KBD_KEY_BORDER, label,
                  scheme->font_description);
        break;
    case Swipe:
        draw_over_inset(kb->surf, k->x, k->y, k->w, k->h, KBD_KEY_BORDER,
                        scheme->swipe, scheme->rounding);
        drw_draw_text(kb->surf,
                  scheme->text_swipe.bgra[3] ? scheme->text_swipe : scheme->text, k->x, k->y, k->w, k->h,
                  KBD_KEY_BORDER, label, scheme->font_description);
        break;
    default:
        drw_draw_text(kb->surf, scheme->text, k->x, k->y, k->w, k->h,
                  KBD_KEY_BORDER, label, scheme->font_description);
    }

    /* glassine: the hold alternate, small in the top right corner */
    if (k->hold_label && kb->hold_ms > 0 && scheme->hint_font_description &&
        (type == None || type == Unpress || type == Press) && k->w > 4 * KBD_KEY_BORDER) {
        Color c = (type == Press && kb->show_highlight) ? scheme->text_press : scheme->text;
        uint32_t hw = k->w * 45 / 100, hh = k->h * 40 / 100;
        drw_draw_text(kb->surf, c, k->x + k->w - hw - KBD_KEY_BORDER, k->y + KBD_KEY_BORDER,
                      hw, hh, 0, k->hold_label, scheme->hint_font_description);
    }


    if (kb->show_popup && (type == Press || type == Unpress)) {
        kbd_clear_last_popup(kb);

        kb->last_popup_x = k->x;
        kb->last_popup_y = kb->h + k->y - k->h;
        kb->last_popup_w = k->w;
        kb->last_popup_h = k->h;

        drw_fill_rectangle(kb->popup_surf, scheme->bg, k->x,
                           kb->last_popup_y, k->w, k->h, scheme->rounding);
        draw_inset(kb->popup_surf, k->x, kb->last_popup_y, k->w, k->h,
                   KBD_KEY_BORDER,
                   kb->show_highlight ? scheme->high : scheme->fg,
                   scheme->rounding);
        drw_draw_text(kb->popup_surf,
                      kb->show_highlight ? scheme->text_press : scheme->text,
                      k->x, kb->last_popup_y, k->w, k->h, KBD_KEY_BORDER, label,
                      scheme->font_description);
    }
}

void
kbd_draw_layout(struct kbd *kb)
{
    struct drwsurf *d = kb->surf;
    struct key *next_key = kb->layout->keys;
    if (!d->back_buffer || !d->back_buffer->cairo) return;   /* glassine: not shown yet */
    if (kb->debug)
        fprintf(stderr, "Draw layout\n");

    /* glassine: the keys' area only; the strip above them is drawn on its own */
    uint32_t y0 = kb->strip_h < kb->h ? kb->strip_h : 0;
    if (kb->corner_radius > 0) {
        drw_do_clear(d, 0, y0, kb->w, kb->h - y0);
        drw_fill_rectangle(d, kb->schemes[0].bg, 0, y0, kb->w, kb->h - y0, kb->corner_radius);
    } else {
        drw_fill_rectangle(d, kb->schemes[0].bg, 0, y0, kb->w, kb->h - y0, 0);
    }

    while (next_key->type != Last) {
        if ((next_key->type == Pad) || (next_key->type == EndRow)) {
            next_key++;
            continue;
        }
        if ((next_key->type == Mod && kb->mods & next_key->code) ||
            (next_key->type == Compose && kb->compose)) {
            kbd_draw_key(kb, next_key, Press);
        } else {
            kbd_draw_key(kb, next_key, None);
        }
        next_key++;
    }
}

static void variants_resize(struct kbd *kb);

void
kbd_resize(struct kbd *kb, struct layout *layouts, uint8_t layoutcount)
{
    fprintf(stderr, "Resize %dx%d %f, %d layouts\n", kb->w, kb->h, kb->scale,
            layoutcount);

    drwsurf_resize(kb->surf, kb->w, kb->h, kb->scale);
    drwsurf_resize(kb->popup_surf, kb->w, kb->h * 2, kb->scale);
    for (int i = 0; i < layoutcount; i++) {
        if (kb->debug) {
            if (layouts[i].name)
                fprintf(stderr, "Initialising layout %s, keymap %s\n",
                        layouts[i].name, layouts[i].keymap_name);
            else
                fprintf(stderr, "Initialising unnamed layout %d, keymap %s\n",
                        i, layouts[i].keymap_name);
        }
        kbd_init_layout(&layouts[i], kb->strip_h, kb->w, kb->h - kb->strip_h);
    }
    variants_resize(kb);
    if (kb->osk) osk_strip_size(kb->osk, kb->w, kb->strip_h);
    kbd_draw_layout(kb);
    kbd_draw_strip(kb);
}

void
draw_inset(struct drwsurf *ds, uint32_t x, uint32_t y, uint32_t width,
           uint32_t height, uint32_t border, Color color, int rounding)
{
    drw_fill_rectangle(ds, color, x + border, y + border, width - (border * 2),
                       height - (border * 2), rounding);
}
void
draw_over_inset(struct drwsurf *ds, uint32_t x, uint32_t y, uint32_t width,
                uint32_t height, uint32_t border, Color color, int rounding)
{
    drw_over_rectangle(ds, color, x + border, y + border, width - (border * 2),
                       height - (border * 2), rounding);
}

void
create_and_upload_keymap(struct kbd *kb, const char *name, uint32_t comp_unichr)
{
    int keymap_index = -1;
    for (int i = 0; i < NUMKEYMAPS; i++) {
        if (!strcmp(keymap_names[i], name)) {
            keymap_index = i;
        }
    }
    if (keymap_index == -1) {
        fprintf(stderr, "No such keymap defined: %s\n", name);
        exit(9);
    }
    const char *keymap_template = keymaps[keymap_index];
    size_t keymap_size = strlen(keymap_template) + 64;
    char *keymap_str = malloc(keymap_size);
    sprintf(keymap_str, keymap_template, comp_unichr, comp_unichr);
    keymap_size = strlen(keymap_str);
    int keymap_fd = os_create_anonymous_file(keymap_size);
    if (keymap_fd < 0) {
        die("could not create keymap fd\n");
    }
    void *ptr = mmap(NULL, keymap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     keymap_fd, 0);
    if (ptr == (void *)-1) {
        die("could not map keymap data\n");
    }
    if (kb->vkbd == NULL) {
        die("kb.vkbd = NULL\n");
    }
    strcpy(ptr, keymap_str);
    zwp_virtual_keyboard_v1_keymap(kb->vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                   keymap_fd, keymap_size);
    free((void *)keymap_str);
}

// Get a monotonic millisecond timer.
int64_t
monotime()
{
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);

    return (spec.tv_sec * 1000 + spec.tv_nsec / 1000000);
}

/* ---- glassine: the input method's side of the layouts ------------------------------ */

/* An email or web address field keeps the letters and changes their bottom row
 * (GlassineOS/ui shell.keyboard.purpose): a copy of each letter layout with that row,
 * made once. */
#define NVARIANT 2                      /* OSK_EMAIL, OSK_URL */
static struct layout *variant[NVARIANT][NumLayouts];

static struct key row_email[] = {
  {"⌨͕", "⌨͔", 1.5, NextLayer, .scheme = 1},
  {"@", "@", 1.0, Copy, 0x0040, 0, 0x0040},
  {"", "Tab", 3.0, Code, KEY_SPACE},
  {".", ".", 1.0, Code, KEY_DOT},
  {"-", "_", 1.0, Code, KEY_MINUS},
  {"Enter", "Enter", 1.5, Code, KEY_ENTER, .scheme = 1},
  {"", "", 0.0, Last},
};
static struct key row_url[] = {
  {"⌨͕", "⌨͔", 1.5, NextLayer, .scheme = 1},
  {"/", "/", 1.0, Code, KEY_SLASH},
  {"", "Tab", 2.5, Code, KEY_SPACE},
  {".", ".", 1.0, Code, KEY_DOT},
  {".com", ".com", 1.5, Text, .scheme = 1, .text = ".com"},
  {"Enter", "Enter", 1.5, Code, KEY_ENTER, .scheme = 1},
  {"", "", 0.0, Last},
};

/* the layout with its last row replaced by row (NULL: it has no row with a space bar) */
static struct layout *
variant_make(struct layout *base, struct key *row)
{
    struct key *k = base->keys, *last_row = base->keys;
    size_t n = 0;
    bool space = false;
    for (; k[n].type != Last; n++) {
        if (k[n].type == EndRow) { last_row = &k[n + 1]; space = false; }
        else if (k[n].type == Code && k[n].code == KEY_SPACE) space = true;
    }
    if (!space) return NULL;
    size_t keep = (size_t)(last_row - base->keys), rn = 0;
    while (row[rn].type != Last) rn++;
    struct key *nk = calloc(keep + rn + 1, sizeof *nk);
    struct layout *l = calloc(1, sizeof *l);
    if (!nk || !l) { free(nk); free(l); return NULL; }
    memcpy(nk, base->keys, keep * sizeof *nk);
    memcpy(nk + keep, row, (rn + 1) * sizeof *nk);
    memcpy(l, base, sizeof *l);
    l->keys = nk;
    return l;
}

static int
layout_index(struct kbd *kb, struct layout *l)
{
    for (int i = 0; i < NumLayouts; i++) {
        if (l == &kb->layouts[i]) return i;
        for (int v = 0; v < NVARIANT; v++) if (variant[v][i] && l == variant[v][i]) return i;
    }
    return -1;
}

static struct layout *
purpose_layout(struct kbd *kb, struct layout *l)
{
    int i = layout_index(kb, l);
    if (i < 0) return l;
    struct layout *base = &kb->layouts[i];
    int v = kb->keys_kind == OSK_EMAIL ? 0 : kb->keys_kind == OSK_URL ? 1 : -1;
    if (v < 0 || !base->abc) return base;
    if (!variant[v][i]) {
        variant[v][i] = variant_make(base, v ? row_url : row_email);
        if (variant[v][i] && kb->w) kbd_init_layout(variant[v][i], kb->strip_h, kb->w, kb->h - kb->strip_h);
    }
    return variant[v][i] ? variant[v][i] : base;
}

static void
variants_resize(struct kbd *kb)
{
    for (int v = 0; v < NVARIANT; v++)
        for (int i = 0; i < NumLayouts; i++)
            if (variant[v][i]) kbd_init_layout(variant[v][i], kb->strip_h, kb->w, kb->h - kb->strip_h);
}

static struct layout *
layout_named(struct kbd *kb, const char *name)
{
    for (int i = 0; i < NumLayouts; i++)
        if (kb->layouts[i].name && !strcmp(kb->layouts[i].name, name)) return &kb->layouts[i];
    return NULL;
}

/* the field's purpose picks the keys (osk_ops.keys): a keypad, or the letters with an
 * address row; back to the letters (the layer before the keypad) after it */
void
kbd_set_keys(struct kbd *kb, enum osk_keys kind)
{
    enum osk_keys old = kb->keys_kind;
    bool was_pad = old == OSK_NUMBER || old == OSK_PHONE || old == OSK_PIN;
    struct layout *pad = kind == OSK_NUMBER ? layout_named(kb, "padnumber") :
                         kind == OSK_PHONE ? layout_named(kb, "padphone") :
                         kind == OSK_PIN ? layout_named(kb, "padpin") : NULL;
    if (kind == old && (!pad || kb->layout == pad)) return;
    if (kind != OSK_LETTERS && kind != OSK_EMAIL && kind != OSK_URL && !pad) return;  /* a layout without keypads */
    kb->keys_kind = kind;
    if (pad) {
        if (!was_pad) {
            kb->pad_return = kb->layout;
            kb->pad_return_index = kb->layer_index;
        }
        if (kb->pad_return) {           /* its Abc key (BackLayer) goes back there */
            kb->last_abc_layout = kb->pad_return;
            kb->last_abc_index = kb->pad_return_index;
        }
        kbd_switch_layout(kb, pad, kb->pad_return_index);
        kb->last_abc_layout = kb->pad_return ? kb->pad_return : kb->last_abc_layout;
        kb->last_abc_index = kb->pad_return_index;
        return;
    }
    if (was_pad && kb->pad_return) {
        struct layout *r = kb->pad_return;
        kb->pad_return = NULL;
        kbd_switch_layout(kb, r, kb->pad_return_index);
        return;
    }
    if (kb->layout->abc && purpose_layout(kb, kb->layout) != kb->layout)
        kbd_switch_layout(kb, kb->layout, kb->layer_index);
}

/* the layer to show: the first one, or the keypad the focused field asks for (no
 * drawing: the keyboard is being shown) */
void
kbd_reset_layer(struct kbd *kb)
{
    struct layout *pad = kb->keys_kind == OSK_NUMBER ? layout_named(kb, "padnumber") :
                         kb->keys_kind == OSK_PHONE ? layout_named(kb, "padphone") :
                         kb->keys_kind == OSK_PIN ? layout_named(kb, "padpin") : NULL;
    kb->lang_layout = kb->layout;
    kb->pad_return = NULL;
    if (pad) {
        kb->pad_return = kb->last_abc_layout = kb->layout;
        kb->pad_return_index = kb->last_abc_index = kb->layer_index;
        kb->layout = kb->prevlayout = pad;
    } else {
        kb->layout = kb->prevlayout = purpose_layout(kb, kb->layout);
    }
}

/* the predictor's language of a letter layout (the layouts' names are upstream's) */
static const char *
layout_lang(struct layout *l)
{
    static const char *const map[][2] = {
        { "full", "en" }, { "fullwide", "en" }, { "simple", "en" }, { "landscape", "en" },
        { "german", "de" }, { "cyrillic", "ru" }, { "arabic", "ar" }, { "persian", "fa" },
        { "greek", "el" }, { "georgian", "ka" }, { "hebrew", "he" },
    };
    for (unsigned i = 0; l && l->name && i < sizeof map / sizeof *map; i++)
        if (!strcmp(l->name, map[i][0])) return map[i][1];
    return NULL;
}

/* a letter layout with a language becomes the one the predictor hears of */
static void
lang_follow(struct kbd *kb, struct layout *l)
{
    int i = layout_index(kb, l);
    struct layout *base = i >= 0 ? &kb->layouts[i] : l;
    if (!base->abc || !layout_lang(base) || base == kb->lang_layout) return;
    kb->lang_layout = base;
    if (kb->osk) osk_languages(kb->osk, kbd_lang(kb), monotime());
}

const char *
kbd_lang(struct kbd *kb)
{
    if (kb->lang_override) return kb->lang_override;
    const char *l = layout_lang(kb->lang_layout);
    return l ? l : "";
}

/* the suggestion strip: its own rectangle only (an update is no full-panel refresh) */
static void
strip_face(struct kbd *kb, struct strip_item *it, struct clr_scheme *sc, Color fill)
{
    struct key k = { .x = it->x, .y = it->y, .w = it->w, .h = it->h };
    draw_key_face(kb->surf, &k, k.y, sc, fill);
}

void
kbd_draw_strip(struct kbd *kb)
{
    if (!kb->osk || !kb->strip_h || !kb->surf || !kb->surf->damage) return;
    if (!kb->surf->back_buffer || !kb->surf->back_buffer->cairo) return;
    struct strip *s = &kb->osk->s;
    struct clr_scheme *sc = &kb->schemes[0], *sp = &kb->schemes[1];
    if (kb->debug) fprintf(stderr, "Draw strip +0+0 %dx%d\n", kb->w, kb->strip_h);
    drw_fill_rectangle(kb->surf, sc->bg, 0, 0, kb->w, kb->strip_h, 0);
    struct strip_item it[STRIP_ITEMS];
    int n = strip_items(s, it);
    for (int i = 0; i < n; i++) {
        struct strip_item *t = &it[i];
        switch (t->kind) {
        case SI_PEN:
            strip_face(kb, t, sp, sp->fg);
            drw_draw_icon(kb->surf, sp->text, t->x, t->y, t->w, t->h, DRW_ICON_PEN);
            break;
        case SI_HIDE:
            strip_face(kb, t, sp, sp->fg);
            drw_draw_icon(kb->surf, sp->text, t->x, t->y, t->w, t->h, DRW_ICON_HIDE);
            break;
        case SI_WORD:
            strip_face(kb, t, sc, sc->fg);
            drw_draw_text_ex(kb->surf, sc->text, t->x, t->y, t->w, t->h, KBD_KEY_BORDER + 4,
                             s->words.word[t->word], t->best ? sc->best_font : sc->word_font, true);
            break;
        case SI_ASK_TEXT: {
            char q[STRIP_TEXT_MAX];
            strip_ask_text(s->ask, q, sizeof q);
            drw_draw_text_ex(kb->surf, sc->text, t->x, t->y, t->w, t->h, KBD_KEY_BORDER, q, sc->note_font, true);
            break;
        }
        case SI_KEEP:
            strip_face(kb, t, sp, sp->fg);
            drw_draw_text_ex(kb->surf, sp->text, t->x, t->y, t->w, t->h, KBD_KEY_BORDER, "Keep", sc->note_font, true);
            break;
        case SI_REMOVE:         /* the destructive choice: inverted, as a key that is down */
            strip_face(kb, t, sc, sc->high);
            drw_draw_text_ex(kb->surf, sc->text_press, t->x, t->y, t->w, t->h, KBD_KEY_BORDER, "Remove", sc->note_font, true);
            break;
        case SI_NOTE:
            drw_draw_text_ex(kb->surf, sp->text, t->x, t->y, t->w, t->h, KBD_KEY_BORDER, s->note, sc->note_font, true);
            break;
        case SI_LOCK_NOTE: {
            int tw = drw_text_width(kb->surf, STRIP_PRIVATE_TEXT, sc->note_font);
            int ic = t->h / 2, gap = ic / 3;
            if (tw > t->w - ic - gap) tw = t->w - ic - gap;
            int x = t->x + (t->w - tw - ic - gap) / 2;
            drw_draw_icon(kb->surf, sp->text, x, t->y, ic, t->h, DRW_ICON_LOCK);
            drw_draw_text_ex(kb->surf, sp->text, x + ic + gap, t->y, tw, t->h, 0, STRIP_PRIVATE_TEXT, sc->note_font, true);
            break;
        }
        }
    }
}

/* ---- glassine: the writing paper (paper.h, the shell.write boards of GlassineOS/ui).
 * The look comes from the keyboard's theme: the paper is the key face colour, partly
 * see-through; its edge, the chips and the type are the keys' outline, faces and fonts.
 * Ink is drawn as it is written, only the new segment's rectangle each time. */
#define PAPER_ALPHA 0xc2        /* the sheet: about three quarters opaque */

static void rgba(cairo_t *c, Color k, double alpha)
{
    cairo_set_source_rgba(c, k.bgra[2] / 255.0, k.bgra[1] / 255.0, k.bgra[0] / 255.0, k.bgra[3] / 255.0 * alpha);
}

static void rounded(cairo_t *c, double x, double y, double w, double h, double r)
{
    if (r <= 0) { cairo_rectangle(c, x, y, w, h); return; }
    cairo_new_sub_path(c);
    cairo_arc(c, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(c, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(c, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(c, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(c);
}

double paper_ink_width(const struct paper *p)
{
    double w = p->h / 110.0;
    return w < 2 ? 2 : w;
}

/* a stroke's ink from point from on (the source set): the points as stored, the width by
 * each step's pressure as the compositor's preview draws it (paper_ink_at), else base */
static void stroke_ink(cairo_t *c, const struct paper_stroke *s, int from, double base)
{
    if (!s->n) return;
    if (from < 0) from = 0;
    if (s->n == 1) {                                                  /* a dot */
        cairo_move_to(c, s->pt[0].x, s->pt[0].y);
        cairo_line_to(c, s->pt[0].x + 0.1, s->pt[0].y);
        cairo_set_line_width(c, paper_ink_at(base, s->pressure ? s->pt[0].p : -1));
        cairo_stroke(c);
        return;
    }
    if (!s->pressure) {
        cairo_move_to(c, s->pt[from].x, s->pt[from].y);
        for (int i = from + 1; i < s->n; i++) cairo_line_to(c, s->pt[i].x, s->pt[i].y);
        cairo_set_line_width(c, base);
        cairo_stroke(c);
        return;
    }
    for (int i = from + 1; i < s->n; i++) {
        cairo_move_to(c, s->pt[i - 1].x, s->pt[i - 1].y);
        cairo_line_to(c, s->pt[i].x, s->pt[i].y);
        cairo_set_line_width(c, paper_ink_at(base, (s->pt[i - 1].p + s->pt[i].p) / 2));
        cairo_stroke(c);
    }
}

void paper_draw_all(struct kbd *kb, struct drwsurf *ds)
{
    struct osk *o = kb->osk;
    if (!o || !ds->back_buffer || !ds->back_buffer->cairo) return;
    struct paper *p = &o->pp;
    struct clr_scheme *sc = &kb->schemes[0], *sp = &kb->schemes[1];
    drwsurf_flip(ds);
    drwsurf_damage(ds, 0, 0, p->w, p->h);
    cairo_t *c = ds->back_buffer->cairo;
    cairo_save(c);
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(c, 0, 0, 0, 0);
    cairo_paint(c);
    cairo_set_operator(c, CAIRO_OPERATOR_OVER);
    double r = sc->rounding > 0 ? sc->rounding * 2 : 0, ew = kbd_stroke_width > 0 ? kbd_stroke_width : 1;
    /* the sheet and its thin edge */
    Color paper = sc->fg;
    paper.bgra[3] = PAPER_ALPHA;
    rounded(c, ew / 2, ew / 2, p->w - ew, p->h - ew, r);
    rgba(c, paper, 1);
    cairo_fill_preserve(c);
    rgba(c, sc->stroke.bgra[3] ? sc->stroke : sc->text, 0.6);
    cairo_set_line_width(c, ew);
    cairo_stroke(c);
    /* one guide line to write on */
    double gy = paper_guide_y(p) + 0.5, dash = p->ctl / 8.0;
    cairo_set_dash(c, &dash, 1, 0);
    cairo_move_to(c, p->ctl * 0.75, gy);
    cairo_line_to(c, p->w - p->ctl * 0.75, gy);
    rgba(c, sc->text, 0.35);
    cairo_set_line_width(c, 1.5);
    cairo_stroke(c);
    cairo_set_dash(c, NULL, 0, 0);
    /* the ink; the ink that was read dims */
    cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(c, CAIRO_LINE_JOIN_ROUND);
    int dim = p->pending ? (p->pending_n < p->ns ? p->pending_n : p->ns) : 0;
    rgba(c, sc->text, 1);
    if (dim) {
        cairo_push_group(c);            /* the steps overlap at their round ends: dimmed as one */
        for (int i = 0; i < dim; i++) stroke_ink(c, &p->s[i], 0, paper_ink_width(p));
        cairo_pop_group_to_source(c);
        cairo_paint_with_alpha(c, 0.35);
        rgba(c, sc->text, 1);
    }
    for (int i = dim; i < p->ns; i++) stroke_ink(c, &p->s[i], 0, paper_ink_width(p));
    cairo_restore(c);
    /* the row and the controls */
    struct paper_item it[PAPER_ITEMS];
    int n = paper_items(p, it);
    for (int i = 0; i < n; i++) {
        struct paper_item *t = &it[i];
        switch (t->kind) {
        case PI_CHIP: {
            bool best = t->cand == 0;
            cairo_save(c);
            rounded(c, t->x + 0.5, t->y + 0.5, t->w - 1, t->h - 1, sc->rounding > 0 ? sc->rounding + 2 : 0);
            rgba(c, best ? sp->fg : sc->fg, 1);
            cairo_fill_preserve(c);
            rgba(c, sc->stroke.bgra[3] ? sc->stroke : sc->text, 1);
            cairo_set_line_width(c, best ? 2 : 1);
            cairo_stroke(c);
            cairo_restore(c);
            if (t->cand >= 0 && t->cand < p->ncand)
                drw_draw_text_ex(ds, sc->text, t->x, t->y, t->w, t->h, 6, p->cand[t->cand],
                                 best ? sc->best_font : sc->word_font, true);
            break;
        }
        case PI_FIX:
            drw_draw_icon(ds, sc->text, t->x, t->y, t->w, t->h, DRW_ICON_PEN);
            break;
        case PI_DROP:
            drw_draw_icon(ds, sc->text, t->x, t->y, t->w, t->h, DRW_ICON_DROP);
            break;
        case PI_BACK:
            drw_draw_icon(ds, sc->text, t->x, t->y, t->w, t->h, DRW_ICON_BACK);
            break;
        case PI_KEYS:
            drw_draw_icon(ds, sc->text, t->x, t->y, t->w, t->h, DRW_ICON_KEYS);
            break;
        case PI_CLOSE:
            drw_draw_icon(ds, sc->text, t->x, t->y, t->w, t->h, DRW_ICON_CLOSE);
            break;
        case PI_EDIT: {
            cairo_save(c);
            rounded(c, t->x + 0.5, t->y + 0.5, t->w - 1, t->h - 1, sc->rounding > 0 ? sc->rounding + 2 : 0);
            rgba(c, sc->fg, 1);
            cairo_fill_preserve(c);
            rgba(c, sc->text, 1);
            cairo_set_line_width(c, 1.5);
            cairo_stroke(c);
            cairo_restore(c);
            char shown[PAPER_TEXT_MAX + 4];
            snprintf(shown, sizeof shown, "%s|", p->fix);       /* the cursor at its end */
            int tw = drw_text_width(ds, shown, sc->word_font), room = t->w - 16;
            drw_draw_text_ex(ds, sc->text, t->x + 8, t->y, (tw < room ? tw : room) + 2, t->h, 0, shown,
                             sc->word_font, true);
            break;
        }
        case PI_USE:
            cairo_save(c);
            rounded(c, t->x + 0.5, t->y + 0.5, t->w - 1, t->h - 1, sc->rounding > 0 ? sc->rounding + 2 : 0);
            rgba(c, sc->high, 1);
            cairo_fill(c);
            cairo_restore(c);
            drw_draw_text_ex(ds, sc->text_press, t->x, t->y, t->w, t->h, 4, "Use", sc->best_font, true);
            break;
        case PI_NOTE:
            drw_draw_text_ex(ds, sc->text, t->x, t->y, t->w, t->h, 0, p->note, sc->note_font, true);
            break;
        }
    }
}

void paper_draw_new_ink(struct kbd *kb, struct drwsurf *ds, int *drawn)
{
    struct osk *o = kb->osk;
    if (!o || !ds->back_buffer || !ds->back_buffer->cairo || !o->pp.ns) return;
    struct paper *p = &o->pp;
    struct paper_stroke *s = &p->s[p->ns - 1];
    int from = *drawn > 0 ? *drawn - 1 : 0;
    if (s->n <= from || (s->n > 1 && s->n - 1 <= from)) return;
    double lw = paper_ink_width(p) * PAPER_INK_MAX, x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = from; i < s->n; i++) {
        x0 = fmin(x0, s->pt[i].x); y0 = fmin(y0, s->pt[i].y);
        x1 = fmax(x1, s->pt[i].x); y1 = fmax(y1, s->pt[i].y);
    }
    drwsurf_flip(ds);
    int m = (int)lw + 2;
    int bx = (int)x0 - m, by = (int)y0 - m;
    drwsurf_damage(ds, bx < 0 ? 0 : bx, by < 0 ? 0 : by, (int)(x1 - x0) + 2 * m + 1, (int)(y1 - y0) + 2 * m + 1);
    cairo_t *c = ds->back_buffer->cairo;
    cairo_save(c);
    cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(c, CAIRO_LINE_JOIN_ROUND);
    rgba(c, kb->schemes[0].text, 1);
    stroke_ink(c, s, from, paper_ink_width(p));
    cairo_restore(c);
    *drawn = s->n;
}
