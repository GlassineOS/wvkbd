/* glassine: the word predictor client. The predictor is a child process speaking the
 * line contract of GlassineOS/shell docs/predictor.md (version 2; a version 1 answer
 * is understood too): requests on its stdin, replies on its stdout, one line each.
 *
 * Nothing here ever waits: pipes are non-blocking and polled by the keyboard's event
 * loop (pc_poll_fds, pc_handle), timeouts come back through pc_next_deadline/pc_tick.
 * Only the reply to the newest predict request is shown; a predictor that is missing,
 * exits, answers garbage or doesn't answer within PC_SLOW_MS gives an empty list, and
 * one that exited is started again later (PC_BACKOFF_MS, doubling up to
 * PC_BACKOFF_MAX_MS). It is started on the first request, not before. */
#ifndef PREDICT_H
#define PREDICT_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PC_WORDS 3
#define PC_WORD_MAX 256           /* bytes of a word shown */
#define PC_LANG_MAX 32
#define PC_SLOW_MS 400            /* no answer by then: the list is emptied */
#define PC_HUNG_MS 5000           /* still none: the predictor is stopped and restarted */
#define PC_BACKOFF_MS 500
#define PC_BACKOFF_MAX_MS 30000
#define PC_HEALTHY_MS 30000       /* ran that long: the next restart is quick again */
#define PC_OUT_MAX 65536          /* bytes waiting to be written; more: predict requests dropped */
#define PC_IN_MAX 65536           /* a longer reply line is skipped */

struct pc_words {
    int n;
    char word[PC_WORDS][PC_WORD_MAX];
    char lang[PC_WORDS][PC_LANG_MAX];
};

struct pc {
    char **argv;                  /* the predictor's command (argv[0] looked up in PATH) */
    pid_t pid;
    int to, from;                 /* our ends of its stdin and stdout; -1: closed */
    int version;                  /* the contract version it answered (0: not yet) */
    char out[PC_OUT_MAX];
    size_t outlen;
    char in[PC_IN_MAX];
    size_t inlen;
    bool skipping;                /* inside a reply line that was too long */
    unsigned long next_id;
    char want[16];                /* the newest predict request's id ("": none waiting) */
    int64_t want_ms;              /* when it was sent */
    bool shown_empty;             /* the list was emptied for it (slow) */
    int64_t started_ms, restart_ms;
    int backoff_ms;
    int restarts;                 /* (tests) times it was started */
    void (*on_words)(void *data, const struct pc_words *w);  /* the list to show */
    void *data;
};

void pc_init(struct pc *c, char **argv, void (*on_words)(void *, const struct pc_words *), void *data);
void pc_stop(struct pc *c);       /* close its stdin (it saves and exits), forget it */

/* requests: LANGS a comma-separated list ("" lets the predictor's setting decide), FIELD
 * im_field_name's text. They return false when nothing was sent. */
bool pc_predict(struct pc *c, const char *langs, const char *field, const char *before,
                size_t before_len, const char *word, size_t word_len, int64_t now);
bool pc_learn(struct pc *c, const char *langs, const char *field, const char *before,
              size_t before_len, const char *word, size_t word_len, const char *picked, int64_t now);
bool pc_remove(struct pc *c, const char *langs, const char *word, int64_t now);
/* forget the newest predict request: its answer is not shown */
void pc_forget(struct pc *c);

/* the event loop: fds to poll (at most 2), their results, timers */
int pc_poll_fds(struct pc *c, struct pollfd *fds);
void pc_handle(struct pc *c, const struct pollfd *fds, int n, int64_t now);
int64_t pc_next_deadline(const struct pc *c);   /* -1: none */
void pc_tick(struct pc *c, int64_t now);

/* (tests) one reply line, as read */
void pc_line(struct pc *c, char *line, int64_t now);

#endif
