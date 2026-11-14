/* glassine: the handwriting recognizer client (see hw.h) */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hw.h"

extern char **environ;

static pid_t reap_pid[8];         /* stopped recognizers not reaped yet */

static void reap(void)
{
    for (unsigned i = 0; i < sizeof reap_pid / sizeof *reap_pid; i++)
        if (reap_pid[i] > 0 && waitpid(reap_pid[i], NULL, WNOHANG) != 0) reap_pid[i] = 0;
}

static void reap_later(pid_t pid)
{
    for (unsigned i = 0; i < sizeof reap_pid / sizeof *reap_pid; i++)
        if (reap_pid[i] <= 0) { reap_pid[i] = pid; return; }
    waitpid(pid, NULL, WNOHANG);
}

void hw_init(struct hw *h, char **argv, void (*on_line)(void *, char **, int, int64_t),
             void (*on_stop)(void *, bool), void *data)
{
    memset(h, 0, sizeof *h);
    h->argv = argv;
    h->fd = -1;
    h->backoff_ms = HW_BACKOFF_MS;
    h->on_line = on_line;
    h->on_stop = on_stop;
    h->data = data;
}

bool hw_running(const struct hw *h) { return h->fd >= 0; }
bool hw_available(const struct hw *h) { return h->argv && h->argv[0]; }

static void forget(struct hw *h)
{
    if (h->fd >= 0) close(h->fd);
    h->fd = -1;
    h->outlen = h->inlen = 0;
    h->skipping = false;
    h->waiting = 0;
}

/* it went away: idle (status 0 with nothing asked) or crashed (anything else) */
static void gone(struct hw *h, int64_t now, bool kill_it)
{
    bool crashed = h->waiting > 0 || kill_it;
    if (h->pid > 0) {
        int st;
        if (kill_it) kill(h->pid, SIGKILL);
        pid_t r = kill_it ? 0 : waitpid(h->pid, &st, WNOHANG);
        if (r == h->pid) crashed = crashed || !WIFEXITED(st) || WEXITSTATUS(st) != 0;
        else reap_later(h->pid);
    }
    h->pid = 0;
    forget(h);
    if (crashed) {
        h->crashes++;
        if (now - h->started_ms >= HW_HEALTHY_MS) h->backoff_ms = HW_BACKOFF_MS;
        h->restart_ms = now + h->backoff_ms;
        h->backoff_ms = h->backoff_ms * 2 > HW_BACKOFF_MAX_MS ? HW_BACKOFF_MAX_MS : h->backoff_ms * 2;
    } else {
        h->idle_stops++;          /* its idle exit: again at once when needed */
        h->restart_ms = now;
    }
    reap();
    if (h->on_stop) h->on_stop(h->data, crashed);
}

static bool queue(struct hw *h, const char *s, size_t n)
{
    if (h->outlen + n > HW_OUT_MAX) return false;
    if (h->outlen + n > h->outcap) {
        size_t cap = h->outcap ? h->outcap : 16384;
        while (cap < h->outlen + n) cap *= 2;
        if (cap > HW_OUT_MAX) cap = HW_OUT_MAX;
        char *o = realloc(h->out, cap);
        if (!o) return false;
        h->out = o;
        h->outcap = cap;
    }
    memcpy(h->out + h->outlen, s, n);
    h->outlen += n;
    return true;
}

static void flush(struct hw *h, int64_t now)
{
    while (h->fd >= 0 && h->outlen) {
        ssize_t w = send(h->fd, h->out, h->outlen, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            gone(h, now, true);
            return;
        }
        memmove(h->out, h->out + w, h->outlen - (size_t)w);
        h->outlen -= (size_t)w;
    }
}

static void sent(struct hw *h, int64_t now)
{
    if (h->waiting >= 0 && h->waiting < HW_WAIT_MAX) h->sent_ms[h->waiting] = now;
    h->waiting++;
}

static bool start(struct hw *h, int64_t now)
{
    if (h->fd >= 0) return true;
    if (!hw_available(h) || now < h->restart_ms) return false;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return false;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
    posix_spawnattr_init(&at);
    /* the keyboard blocks the signals it reads from a signalfd: the child gets none of that */
    sigset_t none, def;
    sigemptyset(&none);
    sigemptyset(&def);
    int sigs[] = { SIGPIPE, SIGUSR1, SIGUSR2, SIGRTMIN, SIGTERM, SIGINT, SIGHUP, SIGCHLD };
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaddset(&def, sigs[i]);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid;
    int e = posix_spawnp(&pid, h->argv[0], &fa, &at, h->argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    h->started_ms = now;
    if (e) {                      /* missing: as a crash (backoff), nothing to stop */
        close(sv[0]);
        h->crashes++;
        h->restart_ms = now + h->backoff_ms;
        h->backoff_ms = h->backoff_ms * 2 > HW_BACKOFF_MAX_MS ? HW_BACKOFF_MAX_MS : h->backoff_ms * 2;
        return false;
    }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    h->pid = pid;
    h->fd = sv[0];
    h->gen++;
    h->restarts++;
    h->waiting = 0;
    static const char hello[] = "hello\th0\t1\torg.glassine.keyboard\n";
    queue(h, hello, sizeof hello - 1);
    sent(h, now);
    return true;
}

void hw_stop(struct hw *h)
{
    if (h->fd >= 0) {
        flush(h, h->started_ms);
        if (h->fd >= 0) {
            shutdown(h->fd, SHUT_WR);           /* it exits at the end of its input */
            if (h->pid > 0) reap_later(h->pid);
        }
    }
    h->pid = 0;
    forget(h);
}

/* a text field: \ tab newline and carriage return escaped; NUL bytes dropped */
static size_t esc_len(const char *s)
{
    size_t k = 0;
    for (; *s; s++) k += (*s == '\\' || *s == '\t' || *s == '\n' || *s == '\r') ? 2 : 1;
    return k;
}

static char *esc(char *o, const char *s)
{
    for (; *s; s++) {
        char e = *s == '\\' ? '\\' : *s == '\t' ? 't' : *s == '\n' ? 'n' : *s == '\r' ? 'r' : 0;
        if (e) { *o++ = '\\'; *o++ = e; }
        else *o++ = *s;
    }
    return o;
}

static void new_id(struct hw *h, char *id)
{
    snprintf(id, 16, "h%lu", ++h->next_id % 1000000000UL);
}

static bool send_buf(struct hw *h, int64_t now, char *b, size_t n)
{
    bool ok = queue(h, b, n);
    free(b);
    if (!ok) return false;
    sent(h, now);
    flush(h, now);
    return h->fd >= 0;
}

bool hw_request(struct hw *h, int64_t now, char *id, const char *verb, ...)
{
    char tmp[16];
    if (!id) id = tmp;
    id[0] = 0;
    if (!start(h, now)) return false;
    new_id(h, id);
    va_list ap;
    size_t n = strlen(verb) + 1 + strlen(id) + 1;
    va_start(ap, verb);
    for (const char *f; (f = va_arg(ap, const char *));) n += 1 + esc_len(f);
    va_end(ap);
    char *b = malloc(n + 1), *o = b;
    if (!b) return false;
    o += sprintf(o, "%s\t%s", verb, id);
    va_start(ap, verb);
    for (const char *f; (f = va_arg(ap, const char *));) { *o++ = '\t'; o = esc(o, f); }
    va_end(ap);
    *o++ = '\n';
    return send_buf(h, now, b, (size_t)(o - b));
}

bool hw_request_raw(struct hw *h, int64_t now, char *id, const char *verb, const char *f1,
                    const char *raw, size_t rawlen)
{
    char tmp[16];
    if (!id) id = tmp;
    id[0] = 0;
    if (!start(h, now)) return false;
    new_id(h, id);
    size_t n = strlen(verb) + strlen(id) + esc_len(f1) + rawlen + 4;
    char *b = malloc(n + 1), *o = b;
    if (!b) return false;
    o += sprintf(o, "%s\t%s\t", verb, id);
    o = esc(o, f1);
    *o++ = '\t';
    memcpy(o, raw, rawlen);
    o += rawlen;
    *o++ = '\n';
    return send_buf(h, now, b, (size_t)(o - b));
}

bool hw_unesc(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s != '\\') { *o++ = *s; continue; }
        s++;
        if (*s == '\\') *o++ = '\\';
        else if (*s == 't') *o++ = '\t';
        else if (*s == 'n') *o++ = '\n';
        else if (*s == 'r') *o++ = '\r';
        else return false;
    }
    *o = 0;
    return true;
}

void hw_line(struct hw *h, char *line, int64_t now)
{
    if (h->waiting > 0) {         /* one reply per request, in order */
        h->waiting--;
        int k = h->waiting < HW_WAIT_MAX - 1 ? h->waiting : HW_WAIT_MAX - 1;
        memmove(h->sent_ms, h->sent_ms + 1, (size_t)k * sizeof *h->sent_ms);
    }
    char *f[HW_FIELDS];
    int n = 0;
    for (char *p = line;; ) {
        if (n == HW_FIELDS) return;               /* more than any reply the keyboard reads */
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    if (n < 2 || !strcmp(f[0], "hello")) return;
    if (h->on_line) h->on_line(h->data, f, n, now);
}

int hw_poll_fds(struct hw *h, struct pollfd *fds)
{
    if (h->fd < 0) return 0;
    fds[0] = (struct pollfd){ .fd = h->fd, .events = POLLIN | (h->outlen ? POLLOUT : 0) };
    return 1;
}

void hw_handle(struct hw *h, const struct pollfd *fds, int n, int64_t now)
{
    if (n < 1 || h->fd < 0 || fds[0].fd != h->fd) return;
    if (fds[0].revents & POLLOUT) flush(h, now);
    if (h->fd < 0 || !(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) return;
    if (!h->in && !(h->in = malloc(HW_IN_MAX))) return;
    unsigned gen = h->gen;
    for (;;) {
        ssize_t r = read(h->fd, h->in + h->inlen, HW_IN_MAX - h->inlen);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (r <= 0) { gone(h, now, false); return; }
        size_t len = h->inlen + (size_t)r, at = 0;
        for (size_t i = h->inlen; i < len; i++) {
            if (h->in[i] != '\n') continue;
            h->in[i] = 0;
            if (h->skipping) {
                h->skipping = false;
                if (h->waiting > 0) h->waiting--;   /* it answered something, unread */
            } else if (!memchr(h->in + at, 0, i - at)) {
                hw_line(h, h->in + at, now);
            } else if (h->waiting > 0) {
                h->waiting--;
            }
            at = i + 1;
            if (h->fd < 0 || h->gen != gen) return;  /* stopped from a callback */
        }
        memmove(h->in, h->in + at, len - at);
        h->inlen = len - at;
        if (h->inlen == HW_IN_MAX) {                 /* a line longer than any reply */
            h->skipping = true;
            h->inlen = 0;
        }
    }
}

int64_t hw_next_deadline(const struct hw *h)
{
    if (h->fd < 0 || h->waiting <= 0) return -1;
    return h->sent_ms[0] + HW_HUNG_MS;
}

void hw_tick(struct hw *h, int64_t now)
{
    reap();
    if (h->fd >= 0 && h->waiting > 0 && now - h->sent_ms[0] >= HW_HUNG_MS) gone(h, now, true);
}
