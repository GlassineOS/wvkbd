#!/usr/bin/env python3
# A stand-in for glassine-handwriting (the line contract of GlassineOS/handwriting
# docs/recognizer.md, version 1) that misbehaves on purpose. FAKE_MODE: ok, slow (each
# read answered after FAKE_DELAY s), crash (exits 3 at the first read), idle (exits 0
# after FAKE_IDLE s without a request, as the real one does after 300 s), stale (a read
# is also answered with an older id first), garbage (lines no client can use), hang
# (reads, never answers). FAKE_LOG: every line read is appended there.
#
# Readings: the session's strokes as "w<count>" with the alternatives "a<count>" and
# "b<count>". Gestures, by the newest stroke's shape (points "x,y,t"): a flat line wider
# than 1.2 is a strike (covering the strokes whose middle lies under it), a stroke that
# turns back on itself four times a scratch (covering the strokes its box overlaps), three
# points with the middle one highest a caret (before the character after the strokes left
# of it). FAKE_TEXTS ("1=apple,2=banana"): the reading for that many strokes instead of
# w<count>.
#
# Units: the shapes are judged in em, so ink in device units (begin with device or
# device:PER_MM) is scaled by the session's area: its LINE is 5/3 em, as on the keyboard's
# paper (no area: taken as em). FAKE_AREA_ERROR=bad-area answers every area with that error,
# as a recognizer older than BASE does.
import os, sys, time
mode = os.environ.get("FAKE_MODE", "ok")
log = os.environ.get("FAKE_LOG")
delay = float(os.environ.get("FAKE_DELAY", "1"))
idle = float(os.environ.get("FAKE_IDLE", "1"))
out = sys.stdout
sessions = {}               # name -> {num: points}
counter = {}                # name -> next stroke number
unit = {}                   # name -> client units per em (1 for em)
devunits = {}               # name -> the session is in device units
prev_read = None
texts = dict(kv.split("=", 1) for kv in os.environ.get("FAKE_TEXTS", "").split(",") if "=" in kv)


def say(*f):
    try:
        out.write("\t".join(f) + "\n"); out.flush()
    except BrokenPipeError:
        os._exit(0)


def pts(text):
    return [tuple(float(v) for v in p.split(",")) for p in text.split(" ") if p]


def box(p):
    xs = [q[0] for q in p]; ys = [q[1] for q in p]
    return min(xs), min(ys), max(xs), max(ys)


def gesture(name):
    s = sessions[name]
    if not s:
        return None
    n = max(s)
    u = unit.get(name, 1)
    s = {k: [(q[0] / u, q[1] / u) + tuple(q[2:]) for q in v] for k, v in s.items()}
    p = s[n]
    x0, y0, x1, y1 = box(p)
    others = {k: v for k, v in s.items() if k != n}
    if len(p) >= 2 and x1 - x0 > 1.2 and y1 - y0 < 0.1:
        mid = (y0 + y1) / 2
        cov = [k for k, v in sorted(others.items()) if x0 <= (box(v)[0] + box(v)[2]) / 2 <= x1]
        return "strike", n, (x0, y0, x1, y1), cov
    rev, d = 0, 0
    for a, b in zip(p, p[1:]):
        nd = 1 if b[0] > a[0] else -1 if b[0] < a[0] else 0
        if nd and d and nd != d:
            rev += 1
        d = nd or d
    if rev >= 4:
        cov = [k for k, v in sorted(others.items())
               if not (box(v)[2] < x0 or box(v)[0] > x1 or box(v)[3] < y0 or box(v)[1] > y1)]
        return "scratch", n, (x0, y0, x1, y1), cov
    if len(p) == 3 and p[1][1] < p[0][1] and p[1][1] < p[2][1] and p[0][0] < p[1][0] < p[2][0]:
        cov = [k for k, v in sorted(others.items()) if (box(v)[0] + box(v)[2]) / 2 < p[1][0]]
        return "caret", n, (x0, y0, x1, y1), cov
    return None


import select
buf = b""


def next_line():
    # unbuffered, so the idle wait sees lines already sent
    global buf
    while b"\n" not in buf:
        if mode == "idle":
            r, _, _ = select.select([0], [], [], idle)
            if not r:
                sys.exit(0)
        chunk = os.read(0, 1 << 16)
        if not chunk:
            return None
        buf += chunk
    raw, buf = buf.split(b"\n", 1)
    return raw.decode("utf-8", "replace")


while True:
    line = next_line()
    if line is None:
        break
    if log:
        with open(log, "a") as f:
            f.write(line + "\n")
    f = line.split("\t")
    verb, rid = f[0], (f[1] if len(f) > 1 else "-")
    if verb == "hello":
        say("hello", rid, "1"); continue
    if mode == "hang":
        continue
    if mode == "garbage" and verb == "read":
        out.write("read\t" + rid + "\ttext\t2\tonly\n")
        out.write("x" * 300000 + "\n")
        out.write("read\t" + rid + "\ttext\t1\tbad\\qescape\t-1\t0\n")
        out.write("\x00\x01 nonsense\n"); out.flush(); continue
    if verb == "begin":
        sessions[f[2]] = {}; counter[f[2]] = 0
        unit[f[2]] = 1; devunits[f[2]] = f[5].startswith("device")
        say("begin", rid, "ok")
    elif verb == "area":
        if os.environ.get("FAKE_AREA_ERROR"):
            say("error", rid, os.environ["FAKE_AREA_ERROR"]); continue
        v = f[3].split(",")
        if f[2] not in sessions:
            say("error", rid, "unknown-session"); continue
        if len(v) not in (6, 7):
            say("error", rid, "bad-area"); continue
        if devunits.get(f[2]) and float(v[4]) > 0:
            unit[f[2]] = float(v[4]) * 0.6
        say("area", rid, "ok")
    elif verb == "stroke":
        s = f[2]
        if s not in sessions:
            say("error", rid, "unknown-session"); continue
        n = counter[s]; counter[s] += 1
        sessions[s][n] = pts(f[3])
        say("stroke", rid, "ok", str(n))
    elif verb == "read":
        s = f[2]
        if mode == "crash":
            sys.exit(3)
        if mode == "slow":
            time.sleep(delay)
        if s not in sessions:
            say("error", rid, "unknown-session"); continue
        if mode == "stale" and prev_read:
            say("read", prev_read, "text", "1", "stale", "-1", "0")
        prev_read = rid
        g = gesture(s)
        if g:
            kind, n, b, cov = g
            del sessions[s][n]
            k = len(sessions[s]) - len([c for c in cov])
            chars = "@%d" % len(cov) if kind == "caret" else ("0-%d" % (len(cov) - 1) if cov else "-")
            u = unit.get(s, 1)
            say("read", rid, "gesture", kind, str(n), "%.2f,%.2f,%.2f,%.2f" % tuple(v * u for v in b),
                ",".join(map(str, cov)) or "-", chars)
        elif not sessions[s]:
            say("read", rid, "empty")
        else:
            c = len(sessions[s])
            align = ";".join(str(k) for k in sorted(sessions[s]))
            say("read", rid, "text", "3", texts.get(str(c), "w%d" % c), "-0.1", align, "a%d" % c, "-1.0", align, "b%d" % c, "-2.0", align)
    elif verb == "remove":
        s = f[2]
        gone = 0
        for k in f[3].split(","):
            if s in sessions and int(k) in sessions[s]:
                del sessions[s][int(k)]; gone += 1
        say("remove", rid, "ok", str(gone))
    elif verb == "clear":
        if f[2] in sessions:
            sessions[f[2]] = {}
        say("clear", rid, "ok")
    elif verb == "end":
        sessions.pop(f[2], None)
        say("end", rid, "ok")
    elif verb == "confirm":
        s = f[2]
        if s not in sessions:
            say("error", rid, "unknown-session")
        elif not sessions[s]:
            say("confirm", rid, "skip", "empty")
        else:
            say("confirm", rid, "ok", "0123456789abcdef")
    else:
        say("error", rid, "unknown-verb")
