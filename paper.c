/* glassine: the writing paper's model and geometry (see paper.h) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "paper.h"

void paper_init(struct paper *p)
{
    memset(p, 0, sizeof *p);
}

void paper_clear(struct paper *p)
{
    for (int i = 0; i < p->ns; i++) {
        if (p->s[i].pt) memset(p->s[i].pt, 0, (size_t)p->s[i].cap * sizeof *p->s[i].pt);
        free(p->s[i].pt);
    }
    p->ns = 0;
    p->drawing = false;
}

void paper_free(struct paper *p)
{
    paper_clear(p);
    free(p->s);
    p->s = NULL;
    p->cap = 0;
    memset(p->cand, 0, sizeof p->cand);
    memset(p->fix, 0, sizeof p->fix);
}

void paper_size(struct paper *p, int w, int h)
{
    p->w = w;
    p->h = h;
    int c = h / 6;
    p->ctl = c < 36 ? 36 : c > 64 ? 64 : c;
}

struct paper_stroke *paper_stroke_new(struct paper *p)
{
    if (p->ns >= PAPER_STROKES_MAX) return NULL;
    if (p->ns == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        struct paper_stroke *s = realloc(p->s, (size_t)cap * sizeof *s);
        if (!s) return NULL;
        p->s = s;
        p->cap = cap;
    }
    struct paper_stroke *s = &p->s[p->ns++];
    memset(s, 0, sizeof *s);
    s->num = -1;
    return s;
}

bool paper_point(struct paper_stroke *s, float x, float y, float pressure, int32_t t)
{
    return paper_point_tilt(s, x, y, pressure, 0, 0, t);
}

bool paper_point_tilt(struct paper_stroke *s, float x, float y, float pressure, float tx, float ty, int32_t t)
{
    if (s->n >= PAPER_POINTS_MAX) return false;
    if (s->n && s->pt[s->n - 1].x == x && s->pt[s->n - 1].y == y) return true;  /* no move */
    if (s->n && t < s->pt[s->n - 1].t) t = s->pt[s->n - 1].t;                   /* increasing */
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        struct paper_pt *q = realloc(s->pt, (size_t)cap * sizeof *q);
        if (!q) return false;
        s->pt = q;
        s->cap = cap;
    }
    s->pt[s->n++] = (struct paper_pt){ x, y, pressure < 0 ? 0 : pressure > 1 ? 1 : pressure, tx, ty, t };
    return true;
}

void paper_remove(struct paper *p, int i)
{
    if (i < 0 || i >= p->ns) return;
    free(p->s[i].pt);
    memmove(&p->s[i], &p->s[i + 1], (size_t)(p->ns - i - 1) * sizeof *p->s);
    p->ns--;
}

int paper_find_num(const struct paper *p, int num)
{
    for (int i = 0; i < p->ns; i++) if (p->s[i].num == num) return i;
    return -1;
}

int paper_find_id(const struct paper *p, const char *id)
{
    for (int i = 0; i < p->ns; i++) if (p->s[i].id[0] && !strcmp(p->s[i].id, id)) return i;
    return -1;
}

int paper_guide_y(const struct paper *p)
{
    return p->h * 2 / 3;
}

double paper_em(const struct paper *p)
{
    return p->h > 0 ? p->h * 0.3 : 1;
}

double paper_unit(const struct paper *p, double per_mm)
{
    return per_mm > 0 ? 1 : paper_em(p);
}

double paper_line(const struct paper *p)
{
    return p->h > 0 ? p->h * 0.5 : 1;
}

void paper_area_text(const struct paper *p, double per_mm, int rot, char *out, size_t n)
{
    double u = paper_unit(p, per_mm);
    snprintf(out, n, "0,0,%.6g,%.6g,%.6g,%d,%.6g", p->w / u, p->h / u, paper_line(p) / u, rot,
             paper_guide_y(p) / u);
}

int paper_chip_order(int n, int slot)
{
    /* slots left to right: alternative 1, the best, alternative 2 */
    if (n <= 1) return slot == 0 ? 0 : -1;
    if (n == 2) return slot == 0 ? 0 : slot == 1 ? 1 : -1;
    return slot == 0 ? 1 : slot == 1 ? 0 : slot == 2 ? 2 : -1;
}

int paper_items(const struct paper *p, struct paper_item *out)
{
    int n = 0, c = p->ctl, m = c / 4, gap = c / 5;
    int x1 = p->w - m - 3 * c - m;                  /* the row ends before the controls */
    if (p->fixing) {
        int uw = 2 * c, ew = x1 - m - uw - gap;
        if (ew < c) ew = c;
        out[n++] = (struct paper_item){ PI_EDIT, m, m, ew, c, 0 };
        out[n++] = (struct paper_item){ PI_USE, m + ew + gap, m, uw, c, 0 };
    } else if (p->pending && p->ncand > 0) {
        int k = p->ncand > PAPER_CANDS ? PAPER_CANDS : p->ncand;
        int room = x1 - m - 2 * c - gap * (k + 1);
        int cw = room / PAPER_CANDS;
        int total = k * cw + (k + 1) * gap + 2 * c;
        int x = m + (x1 - m - total) / 2;
        for (int slot = 0; slot < k; slot++) {
            out[n++] = (struct paper_item){ PI_CHIP, x, m, cw, c, paper_chip_order(k, slot) };
            x += cw + gap;
        }
        out[n++] = (struct paper_item){ PI_FIX, x, m, c, c, 0 };
        out[n++] = (struct paper_item){ PI_DROP, x + c + gap, m, c, c, 0 };
    }
    out[n++] = (struct paper_item){ PI_BACK, p->w - m - 3 * c, m, c, c, 0 };
    out[n++] = (struct paper_item){ PI_KEYS, p->w - m - 2 * c, m, c, c, 0 };
    out[n++] = (struct paper_item){ PI_CLOSE, p->w - m - c, m, c, c, 0 };
    if (p->note[0]) {
        int nh = c * 2 / 3;
        out[n++] = (struct paper_item){ PI_NOTE, m * 2, p->h - m - nh, p->w - m * 4, nh, 0 };
    }
    return n;
}

int paper_hit(const struct paper *p, int x, int y, struct paper_item *out)
{
    struct paper_item it[PAPER_ITEMS];
    int n = paper_items(p, it);
    for (int i = 0; i < n; i++) {
        if (it[i].kind == PI_NOTE) continue;    /* writing goes on over the note */
        if (x >= it[i].x && x < it[i].x + it[i].w && y >= it[i].y && y < it[i].y + it[i].h) {
            if (out) *out = it[i];
            return i;
        }
    }
    return -1;
}

char *paper_points_text(const struct paper *p, const struct paper_stroke *s, bool pressure, double per_mm,
                        size_t *len)
{
    double em = paper_unit(p, per_mm);
    bool tilt = pressure && s->tilt;
    size_t cap = (size_t)s->n * 72 + 16, k = 0;
    char *b = malloc(cap);
    if (!b) return NULL;
    for (int i = 0; i < s->n; i++) {
        const struct paper_pt *q = &s->pt[i];
        int w = tilt ? snprintf(b + k, cap - k, "%s%.4f,%.4f,%d,%.6g,%.6g,%.6g", i ? " " : "", q->x / em, q->y / em,
                                q->t, q->p, q->tx, q->ty)
                : pressure ? snprintf(b + k, cap - k, "%s%.4f,%.4f,%d,%.6g", i ? " " : "", q->x / em, q->y / em, q->t, q->p)
                           : snprintf(b + k, cap - k, "%s%.4f,%.4f,%d", i ? " " : "", q->x / em, q->y / em, q->t);
        if (w < 0 || (size_t)w >= cap - k) { free(b); return NULL; }
        k += (size_t)w;
    }
    b[k] = 0;
    *len = k;
    return b;
}

double paper_ink_at(double base, double pressure)
{
    if (pressure < 0) return base;
    if (pressure > 1) pressure = 1;
    return base * (1 + (PAPER_INK_MAX - 1) * pressure);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

int paper_rate(const struct paper *p)
{
    int n = 0;
    for (int i = 0; i < p->ns; i++) n += p->s[i].n;
    if (n < 9) return 0;
    int *d = malloc((size_t)n * sizeof *d), k = 0;
    if (!d) return 0;
    for (int i = 0; i < p->ns; i++)
        for (int j = 1; j < p->s[i].n; j++)
            if (p->s[i].pt[j].t > p->s[i].pt[j - 1].t) d[k++] = p->s[i].pt[j].t - p->s[i].pt[j - 1].t;
    int r = 0;
    if (k >= 8) {
        qsort(d, (size_t)k, sizeof *d, cmp_int);
        double med = k % 2 ? d[k / 2] : (d[k / 2 - 1] + d[k / 2]) / 2.0;
        r = (int)lround(1000 / med);
    }
    free(d);
    return r;
}
