/* the host tests' checks: each prints ok/FAIL; main returns the failures */
#ifndef T_H
#define T_H
#include <stdio.h>
#include <string.h>
static int t_fail, t_run;
#define CHECK(name, cond) do { t_run++; if (cond) printf("ok   %s\n", name); \
    else { t_fail++; printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); } } while (0)
#define CHECK_STR(name, got, want) do { const char *g_ = (got), *w_ = (want); t_run++; \
    if (g_ && w_ && !strcmp(g_, w_)) printf("ok   %s\n", name); \
    else { t_fail++; printf("FAIL %s: got \"%s\", want \"%s\" (%s:%d)\n", name, g_ ? g_ : "(null)", w_ ? w_ : "(null)", __FILE__, __LINE__); } } while (0)
#define CHECK_INT(name, got, want) do { long g_ = (long)(got), w_ = (long)(want); t_run++; \
    if (g_ == w_) printf("ok   %s\n", name); \
    else { t_fail++; printf("FAIL %s: got %ld, want %ld (%s:%d)\n", name, g_, w_, __FILE__, __LINE__); } } while (0)
#define T_DONE() (printf("%d of %d checks failed\n", t_fail, t_run), t_fail != 0)
#endif
