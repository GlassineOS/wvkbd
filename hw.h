/* glassine: the handwriting recognizer client. The recognizer is a child process speaking
 * the line contract of GlassineOS/handwriting docs/recognizer.md (version 1): requests on
 * its stdin, replies on its stdout, one line each, fields separated by tabs, every request
 * answered by exactly one reply in order.
 *
 * This is the transport only: it starts the program on the first request (with a hello),
 * never waits (non-blocking socket, polled by the keyboard's loop), hands every reply line
 * to on_line, and tells on_stop when the program went away, so the sessions it held are
 * gone and the caller sends its strokes again. A stroke line can be long (the contract
 * allows 4 MiB), so the queue grows up to HW_OUT_MAX.
 *
 * The program exits by itself after a while without requests (status 0): that is a stop,
 * not a crash, and it is started again at the next request without a backoff. A crash (an
 * exit with requests unanswered or a status other than 0), a hang (no reply for
 * HW_HUNG_MS) or garbage it can't recover from: started again after a backoff
 * (HW_BACKOFF_MS, doubling up to HW_BACKOFF_MAX_MS). Missing: no replies, nothing else. */
#ifndef HW_H
#define HW_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define HW_OUT_MAX (4 << 20)      /* bytes waiting to be written; more: the request is refused */
#define HW_IN_MAX (256 << 10)     /* a longer reply line is skipped */
#ifndef HW_HUNG_MS
#define HW_HUNG_MS 5000           /* a reply that long overdue: stopped and started again */
#endif
#define HW_BACKOFF_MS 500
#define HW_BACKOFF_MAX_MS 30000
#define HW_HEALTHY_MS 30000       /* ran that long: the next restart is quick again */
#define HW_WAIT_MAX 64            /* requests in flight remembered (for the hang check) */
#define HW_FIELDS 40              /* fields of a reply passed on (3 candidates need 13) */

struct hw {
    char **argv;                  /* the recognizer's command (argv[0] looked up in PATH) */
    pid_t pid;
    int fd;                       /* our end of its stdin and stdout; -1: not running */
    unsigned gen;                 /* counts the starts: a reply belongs to the current one */
    char *out;
    size_t outlen, outcap;
    char *in;
    size_t inlen;
    bool skipping;                /* inside a reply line that was too long */
    unsigned long next_id;
    int64_t sent_ms[HW_WAIT_MAX]; /* the send times of the requests not answered yet */
    int waiting;                  /* requests not answered yet (may exceed HW_WAIT_MAX) */
    int64_t started_ms, restart_ms;
    int backoff_ms;
    int restarts, crashes, idle_stops;      /* (tests) */
    void (*on_line)(void *data, char **f, int n, int64_t now);
    void (*on_stop)(void *data, bool crashed);
    void *data;
};

void hw_init(struct hw *h, char **argv, void (*on_line)(void *, char **, int, int64_t),
             void (*on_stop)(void *, bool), void *data);
void hw_stop(struct hw *h);       /* close its stdin (it exits), forget it */
bool hw_running(const struct hw *h);
bool hw_available(const struct hw *h);   /* a command was given */

/* a request: the verb, a fresh id written to id (16 bytes), then the fields (NULL ends
 * them; text fields escaped here). False: nothing sent (not started, backing off, queue
 * full). Starting it queues the hello first. */
bool hw_request(struct hw *h, int64_t now, char *id, const char *verb, ...);
/* a request whose last field is given raw (a stroke's points; nothing to escape) */
bool hw_request_raw(struct hw *h, int64_t now, char *id, const char *verb, const char *f1,
                    const char *raw, size_t rawlen);

/* the event loop: the fd to poll (at most 1), its result, timers */
int hw_poll_fds(struct hw *h, struct pollfd *fds);
void hw_handle(struct hw *h, const struct pollfd *fds, int n, int64_t now);
int64_t hw_next_deadline(const struct hw *h);   /* -1: none */
void hw_tick(struct hw *h, int64_t now);

/* text fields: unescape in place (false: a bad escape) */
bool hw_unesc(char *s);
/* (tests) one reply line, as read */
void hw_line(struct hw *h, char *line, int64_t now);

#endif
