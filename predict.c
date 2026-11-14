/* glassine: the word predictor client (see predict.h) */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "predict.h"

extern char **environ;

static pid_t reap_pid[8];         /* stopped predictors not reaped yet */

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

void pc_init(struct pc *c, char **argv, void (*on_words)(void *, const struct pc_words *), void *data)
{
    memset(c, 0, sizeof *c);
    c->argv = argv;
    c->to = c->from = -1;
    c->backoff_ms = PC_BACKOFF_MS;
    c->on_words = on_words;
    c->data = data;
}

static void show_empty(struct pc *c)
{
    struct pc_words w = { 0 };
    if (c->on_words) c->on_words(c->data, &w);
}

/* it exited, closed its end, or hangs: forget it, start it again later */
static void dead(struct pc *c, int64_t now)
{
    if (c->to >= 0) close(c->to);
    c->to = c->from = -1;
    if (c->pid > 0) {
        kill(c->pid, SIGKILL);
        reap_later(c->pid);
    }
    c->pid = 0;
    c->outlen = c->inlen = 0;
    c->skipping = false;
    c->version = 0;
    if (now - c->started_ms >= PC_HEALTHY_MS) c->backoff_ms = PC_BACKOFF_MS;
    c->restart_ms = now + c->backoff_ms;
    c->backoff_ms = c->backoff_ms * 2 > PC_BACKOFF_MAX_MS ? PC_BACKOFF_MAX_MS : c->backoff_ms * 2;
    if (c->want[0]) {
        c->want[0] = 0;
        show_empty(c);
    }
    reap();
}

static bool queue(struct pc *c, const char *line, size_t n)
{
    if (c->outlen + n > PC_OUT_MAX) return false;
    memcpy(c->out + c->outlen, line, n);
    c->outlen += n;
    return true;
}

static void flush(struct pc *c, int64_t now)
{
    while (c->to >= 0 && c->outlen) {
        ssize_t w = send(c->to, c->out, c->outlen, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            dead(c, now);
            return;
        }
        memmove(c->out, c->out + w, c->outlen - (size_t)w);
        c->outlen -= (size_t)w;
    }
}

static bool start(struct pc *c, int64_t now)
{
    if (c->to >= 0) return true;
    if (!c->argv || !c->argv[0] || now < c->restart_ms) return false;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) { dead(c, now); return false; }
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
    int e = posix_spawnp(&pid, c->argv[0], &fa, &at, c->argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    c->started_ms = now;
    if (e) {
        close(sv[0]);
        dead(c, now);
        return false;
    }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    c->pid = pid;
    c->to = c->from = sv[0];
    c->restarts++;
    c->version = 0;
    queue(c, "hello\tk0\t2\n", 11);
    return true;
}

void pc_stop(struct pc *c)
{
    if (c->to >= 0) {
        flush(c, c->started_ms);
        shutdown(c->to, SHUT_WR);           /* it saves what it learned and exits */
        close(c->to);
        if (c->pid > 0) reap_later(c->pid);
    }
    c->to = c->from = -1;
    c->pid = 0;
    c->want[0] = 0;
}

/* a text field of a request: \ tab newline and carriage return escaped */
static size_t esc(char *o, size_t room, const char *s, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char e = s[i] == '\\' ? '\\' : s[i] == '\t' ? 't' : s[i] == '\n' ? 'n' : s[i] == '\r' ? 'r' : 0;
        if (k + 2 >= room) return (size_t)-1;
        if (e) { o[k++] = '\\'; o[k++] = e; }
        else if (s[i]) o[k++] = s[i];
    }
    return k;
}

struct line { char b[16384]; size_t n; bool bad; };

static void put(struct line *l, const char *s)
{
    size_t n = strlen(s);
    if (l->n + n + 2 >= sizeof l->b) { l->bad = true; return; }
    memcpy(l->b + l->n, s, n);
    l->n += n;
}
static void field(struct line *l, const char *s, size_t n)
{
    if (l->bad) return;
    l->b[l->n++] = '\t';
    size_t k = esc(l->b + l->n, sizeof l->b - l->n - 2, s, n);
    if (k == (size_t)-1) { l->bad = true; return; }
    l->n += k;
}

static bool send_line(struct pc *c, struct line *l, int64_t now)
{
    if (l->bad) return false;
    l->b[l->n++] = '\n';
    if (!queue(c, l->b, l->n)) return false;
    flush(c, now);
    return c->to >= 0;
}

static void id(struct pc *c, char *out)
{
    snprintf(out, 16, "k%lu", ++c->next_id % 1000000000UL);
}

/* only the last bytes of the context count (the predictor reads 512): 1 KB is sent */
#define CONTEXT_MAX 1024

static void context(const char **s, size_t *n)
{
    if (*n <= CONTEXT_MAX) return;
    size_t cut = *n - CONTEXT_MAX;
    while (cut < *n && ((unsigned char)(*s)[cut] & 0xC0) == 0x80) cut++;
    *s += cut;
    *n -= cut;
}

bool pc_predict(struct pc *c, const char *langs, const char *fld, const char *before,
                size_t bl, const char *word, size_t wl, int64_t now)
{
    if (!start(c, now)) return false;
    struct line l = { .n = 0 };
    char i[16];
    id(c, i);
    context(&before, &bl);
    put(&l, "predict");
    field(&l, i, strlen(i));
    field(&l, langs, strlen(langs));
    field(&l, fld, strlen(fld));
    field(&l, before, bl);
    field(&l, word, wl);
    if (!send_line(c, &l, now)) return false;
    snprintf(c->want, sizeof c->want, "%s", i);
    c->want_ms = now;
    c->shown_empty = false;
    return true;
}

bool pc_learn(struct pc *c, const char *langs, const char *fld, const char *before,
              size_t bl, const char *word, size_t wl, const char *picked, int64_t now)
{
    if (!start(c, now)) return false;
    struct line l = { .n = 0 };
    char i[16];
    id(c, i);
    context(&before, &bl);
    put(&l, "learn");
    field(&l, i, strlen(i));
    field(&l, langs, strlen(langs));
    field(&l, fld, strlen(fld));
    field(&l, before, bl);
    field(&l, word, wl);
    field(&l, picked ? picked : "", picked ? strlen(picked) : 0);
    return send_line(c, &l, now);
}

bool pc_remove(struct pc *c, const char *langs, const char *word, int64_t now)
{
    if (!start(c, now)) return false;
    struct line l = { .n = 0 };
    char i[16];
    id(c, i);
    put(&l, "remove");
    field(&l, i, strlen(i));
    field(&l, langs, strlen(langs));
    field(&l, word, strlen(word));
    return send_line(c, &l, now);
}

void pc_forget(struct pc *c)
{
    c->want[0] = 0;
}

/* a text field of a reply, unescaped in place; false: a bad escape */
static bool unesc(char *s)
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

void pc_line(struct pc *c, char *line, int64_t now)
{
    char *f[2 + 2 * PC_WORDS + 2];
    int n = 0;
    for (char *p = line;; ) {
        if (n == (int)(sizeof f / sizeof *f)) return;   /* more fields than any reply has */
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0;
        p = t + 1;
    }
    if (n < 2) return;
    if (!strcmp(f[0], "hello")) {
        if (n == 3 && !strcmp(f[1], "k0")) c->version = atoi(f[2]) >= 2 ? 2 : 1;
        return;
    }
    if (!c->want[0] || strcmp(f[1], c->want)) return;     /* an older request's, or none */
    struct pc_words w = { 0 };
    char none[1] = "";
    if (!strcmp(f[0], "predict")) {
        int per = c->version >= 2 ? 2 : 1, k = n - 2;
        if (k % per || k / per > PC_WORDS) return;      /* not a reply of this version */
        for (int i = 0; i < k / per; i++) {
            char *word = f[2 + i * per], *lang = per == 2 ? f[3 + i * per] : none;
            if (!unesc(word) || !unesc(lang) || !*word) return;
            if (strlen(word) >= PC_WORD_MAX || strlen(lang) >= PC_LANG_MAX) return;
            snprintf(w.word[w.n], PC_WORD_MAX, "%s", word);
            snprintf(w.lang[w.n], PC_LANG_MAX, "%s", lang);
            w.n++;
        }
    } else if (strcmp(f[0], "drop") && strcmp(f[0], "error")) {
        return;                                         /* not an answer to a predict */
    }
    c->want[0] = 0;
    if (c->on_words) c->on_words(c->data, &w);
    (void)now;
}

int pc_poll_fds(struct pc *c, struct pollfd *fds)
{
    if (c->from < 0) return 0;
    fds[0] = (struct pollfd){ .fd = c->from, .events = POLLIN | (c->outlen ? POLLOUT : 0) };
    return 1;
}

void pc_handle(struct pc *c, const struct pollfd *fds, int n, int64_t now)
{
    if (n < 1 || c->from < 0 || fds[0].fd != c->from) return;
    if (fds[0].revents & POLLOUT) flush(c, now);
    if (c->from < 0 || !(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) return;
    for (;;) {
        ssize_t r = read(c->from, c->in + c->inlen, sizeof c->in - c->inlen);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (r <= 0) { dead(c, now); return; }
        size_t len = c->inlen + (size_t)r, at = 0;
        for (size_t i = c->inlen; i < len; i++) {
            if (c->in[i] != '\n') continue;
            c->in[i] = 0;
            if (c->skipping) c->skipping = false;
            else if (!memchr(c->in + at, 0, i - at)) pc_line(c, c->in + at, now);
            at = i + 1;
            if (c->from < 0) return;
        }
        memmove(c->in, c->in + at, len - at);
        c->inlen = len - at;
        if (c->inlen == sizeof c->in) {                 /* a line longer than any reply */
            c->skipping = true;
            c->inlen = 0;
        }
    }
}

int64_t pc_next_deadline(const struct pc *c)
{
    int64_t d = -1;
    if (c->want[0]) d = c->want_ms + (c->shown_empty ? PC_HUNG_MS : PC_SLOW_MS);
    return d;
}

void pc_tick(struct pc *c, int64_t now)
{
    reap();
    if (!c->want[0]) return;
    if (now - c->want_ms >= PC_HUNG_MS) { dead(c, now); return; }
    if (now - c->want_ms >= PC_SLOW_MS && !c->shown_empty) {
        c->shown_empty = true;
        show_empty(c);
    }
}
