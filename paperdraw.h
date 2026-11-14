/* glassine: drawing the writing paper (see paperdraw.c) */
#ifndef PAPERDRAW_H
#define PAPERDRAW_H

#include "drw.h"
#include "paper.h"

struct kbd;
void paper_draw_all(struct kbd *kb, struct drwsurf *ds);
/* the stroke being written: its points from *drawn on (then *drawn is its length) */
void paper_draw_new_ink(struct kbd *kb, struct drwsurf *ds, int *drawn);
double paper_ink_width(const struct paper *p);

#endif
