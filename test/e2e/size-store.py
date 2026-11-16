#!/usr/bin/env python3
# (inside the container, see run.sh) a store of a person's letters for the size checks:
# samples confirmed through the recognizer's contract (docs/recognizer.md of
# GlassineOS/handwriting) as another app writes them, at another scale than the keyboard's
# paper: device units at PER_MM with a writing area. The words are the recognizer's own
# synthetic test ink (INK: its tests/data/ink.jsonl), scaled so small letters are 3 mm
# high; lone letters are circles, o 3 mm and O 8 mm. Also em samples of another size,
# which must not count: the recognizer compares the size per unit.
# Usage: size-store.py RECOGNIZER PER_MM INK
import json, math, subprocess, sys

rec, per_mm, ink = sys.argv[1], float(sys.argv[2]), sys.argv[3]
p = subprocess.Popen([rec, "-i", "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
n = 0
SMALL = set("acemnorsuvwxz")


def ask(*f):
    global n
    n += 1
    p.stdin.write("\t".join((f[0], "q%d" % n) + f[1:]) + "\n")
    p.stdin.flush()
    return p.stdout.readline().rstrip("\n").split("\t")[2:]


def pts(stroke):
    return " ".join("%.3f,%.3f,%.1f" % (x, y, t) for x, y, t in stroke)


def keep(text, strokes, units, area):
    """the strokes ([[(x, y, t ms)]]) confirmed as text, corrected"""
    assert ask("begin", "s", "en", "normal", units, "") == ["ok"]
    if area:
        assert ask("area", "s", area) == ["ok"]
    for s in strokes:
        assert ask("stroke", "s", pts(s))[0] == "ok"
    r = ask("confirm", "s", "corrected", text)
    assert r[0] == "ok", r
    ask("end", "s")


def word(sample, xh, dx):
    """a fixture word scaled so its small letters are xh high, its baseline at y 100"""
    st = sample["strokes"]
    hs = []
    for c, span in zip(sample["text"], sample["spans"]):
        if c in SMALL and span:
            ys = [q[1] for k in span for q in st[k]]
            hs.append(max(ys) - min(ys))
    if not hs:
        return None
    hs.sort()
    f = xh / hs[len(hs) // 2]
    base = max(q[1] for c, span in zip(sample["text"], sample["spans"]) if c in SMALL for k in span for q in st[k])
    return [[(dx + q[0] * f, 100 + (q[1] - base) * f, q[2] * 1000) for q in s] for s in st]


def circle(d, dx):
    k = 20
    return [[(dx + d / 2 * math.sin(2 * math.pi * i / k), 100 - d / 2 - d / 2 * math.cos(2 * math.pi * i / k), 8 * i)
             for i in range(k + 1)]]


ask("hello", "1", "org.example.samples")
words = [json.loads(l) for l in open(ink) if l.strip()]
words = [w for w in words if len(w["text"]) >= 3 and any(c in SMALL for c in w["text"])]
area = "0,0,1600,600,150,0,100"
dev = "device:%g" % per_mm
kept = 0
for i, w in enumerate(words):
    s = word(w, 3 * per_mm, 10 + i)
    if s:
        keep(w["text"], s, dev, area)
        kept += 1
for i in range(3):
    keep("o", circle(3 * per_mm, 10 + i), dev, area)
    keep("O", circle(8 * per_mm, 10 + i), dev, area)
# em: much smaller numbers; read as millimetres they would pull every median down
for i, w in enumerate(words):
    s = word(w, 0.05, 1 + i / 100)
    if s:
        keep(w["text"], s, "em", None)
for i in range(3):
    keep("o", circle(0.05, 1 + i / 100), "em", None)
    keep("O", circle(0.12, 1 + i / 100), "em", None)
p.stdin.close()
p.wait(timeout=60)
print("size-store: kept %d words" % kept)
