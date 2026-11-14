#!/usr/bin/env python3
# A stand-in for glassine-predict (the line contract of GlassineOS/shell docs/predictor.md)
# that misbehaves on purpose. FAKE_MODE: ok (words from the typed word), slow (each
# predict answered after FAKE_DELAY s), crash (exits at the first predict), garbage
# (lines no client can use), stale (answers with an older id), hang (reads, never
# answers), v1 (speaks version 1). FAKE_LOG: every line read is appended there.
import os, sys, time
mode = os.environ.get("FAKE_MODE", "ok")
log = os.environ.get("FAKE_LOG")
delay = float(os.environ.get("FAKE_DELAY", "1"))
out = sys.stdout
prev_id = None
def say(*f):
    try:
        out.write("\t".join(f) + "\n"); out.flush()
    except BrokenPipeError:                # the client went away: so do we
        os._exit(0)
for raw in sys.stdin:
    line = raw.rstrip("\n")
    if log:
        with open(log, "a") as f: f.write(line + "\n")
    f = line.split("\t")
    verb, rid = f[0], (f[1] if len(f) > 1 else "-")
    if verb == "hello":
        say("hello", rid, "1" if mode == "v1" else "2"); continue
    if mode == "hang": continue
    if verb == "predict":
        word = f[5] if len(f) > 5 else ""
        base = word or "next"
        if mode == "crash": sys.exit(3)
        if mode == "garbage":
            out.write("predict\t" + rid + "\tonly-a-word\n")          # v2 needs a language
            out.write("x" * 70000 + "\n")                            # longer than any reply
            out.write("predict\t" + rid + "\tbad\\qescape\txx\n")      # a bad escape
            out.write("\x00\x01\x02 nonsense\n"); out.flush(); continue
        if mode == "slow": time.sleep(delay)
        if mode == "stale":
            if prev_id: say("predict", prev_id, "old", "xx")
            say("predict", "k999999", "wrong", "xx")
            prev_id = rid; continue
        if mode == "v1": say("predict", rid, base + "a", base + "b", base + "c")
        else: say("predict", rid, base + "a", "xx", base + "b", "xx", base + "c", "xx")
    elif verb == "learn": say("learn", rid, "ok", "xx")
    elif verb == "remove": say("remove", rid, "ok")
    else: say("error", rid, "unknown-verb")
