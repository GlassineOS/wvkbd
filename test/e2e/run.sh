#!/bin/sh
# (inside the container, see test.sh) the keyboard as the input method in headless sway:
# /src = this checkout (read-only), /t = test/e2e. A text-input-v3 field (ti-client) gets
# the typed text as committed text, a picked word as one edit, Backspace through the
# input method; the strip draws on its own; private fields send nothing to the predictor
# and print nothing; the purpose picks the keypad; an app without text input gets keys;
# a missing predictor leaves typing working.
set -u
sh /t/build.sh || exit 1
B=/tmp/b
export XDG_RUNTIME_DIR=/tmp/rt; mkdir -p -m 700 $XDG_RUNTIME_DIR
printf 'output HEADLESS-1 resolution 800x1000\n' > /tmp/c
cp /usr/bin/sway /tmp/sway          # the package has file capabilities: no exec in a container
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 /tmp/sway -c /tmp/c >/tmp/sway.log 2>&1 &
for i in $(seq 1 50); do ls $XDG_RUNTIME_DIR/sway-ipc.* >/dev/null 2>&1 && break; sleep 0.2; done
export SWAYSOCK=$(ls $XDG_RUNTIME_DIR/sway-ipc.*) WAYLAND_DISPLAY=wayland-1
mkfifo /tmp/ptr; $B/vptr /tmp/ptr 800 1000 2>/dev/null & sleep 0.3
export FAKE_LOG=/tmp/predict.log FAKE_MODE=ok
fail=0
dump() { [ -n "${DEBUG:-}" ] || return 0; echo "--- predict.log"; tail -6 /tmp/predict.log; echo "--- keyboard"; tail -25 /tmp/kbd.err; echo "--- field"; tail -6 /tmp/ti.out; }
expect() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "     got:  $2"; echo "     want: $3"; fail=1; dump; fi; }
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; dump; fi; }
ptr() { echo "$*" > /tmp/ptr; sleep 0.25; }
last() { grep '^text ' /tmp/ti.out | tail -1 | cut -c6-; }
edits() { grep '^edits ' /tmp/ti.out | tail -1 | cut -d' ' -f2; }
# -H 300: six key rows of 50 px; the strip (50 px) above them; the keyboard at y 650..1000
kbd() { $B/wvkbd/wvkbd-mobintl --im --auto --hidden --strip -H 300 --no-popup -o -D \
          --predictor "python3 -u /src/test/fake-predict.py" --pen-cmd "touch /tmp/pen" "$@" > /tmp/kbd.out 2>/tmp/kbd.err & KP=$!; sleep 0.5; }
TP=
field_stop() { [ -n "$TP" ] && { kill $TP 2>/dev/null; wait $TP 2>/dev/null; TP=; sleep 0.2; }; }
field() { field_stop; : > /tmp/ti.out; $B/ti-client "$@" >> /tmp/ti.out 2>&1 & TP=$!; sleep 1; }
kbd_stop() { kill $KP 2>/dev/null; wait $KP 2>/dev/null; field_stop; }
ptr tap 10 10                       # the pointer enters (its first event is no press)

kbd -l full,special
field -p 0 -t "I say "
check "the field's focus shows the keyboard" "grep -q '^Resize 800x350' /tmp/kbd.err"
nl0=$(grep -c 'Draw layout' /tmp/kbd.err)
ns0=$(grep -c 'Draw strip' /tmp/kbd.err)
ptr tap 460 875; ptr tap 180 825; ptr tap 700 875      # h e l
expect "typed letters arrive as committed text" "$(last)" "I say hel|9"
check "no key events for them" "! grep -q '^key 3[58] ' /tmp/ti.out"
sleep 0.3
check "the predictor heard the context" "grep -q \"$(printf 'predict\tk[0-9]*\ten\tnormal\tI say \thel')\" /tmp/predict.log"
check "words in the strip: drawn on its own" "[ \$(grep -c 'Draw strip' /tmp/kbd.err) -gt $ns0 ]"
expect "no full redraw for typing or for the strip" "$(grep -c 'Draw layout' /tmp/kbd.err)" "$nl0"
e0=$(edits)
ptr tap 400 675                     # the likeliest, in the middle
expect "a picked word replaces the word being typed, a space after" "$(last)" "I say hela |11"
expect "in one edit" "$(edits)" "$((e0 + 1))"
check "the pick is learned with its language" "grep -q \"$(printf 'learn\tk[0-9]*\ten\tnormal\tI say \thela\txx')\" /tmp/predict.log"
ptr tap 740 925                     # Backspace
expect "Backspace deletes through the input method" "$(last)" "I say hela|10"
check "no Backspace key event" "! grep -q '^key 14 ' /tmp/ti.out"
ptr tap 740 975                     # Enter
check "Enter is a key event" "grep -q '^key 28 1' /tmp/ti.out"
sleep 0.3
expect "the picked word, its space deleted, then Enter: learned once" \
    "$(grep -c "$(printf '^learn\tk[0-9]*\ten\tnormal\tI say \thela\t')" /tmp/predict.log)" "1"
ptr tap 360 825; ptr tap 180 825    # t e
sleep 0.3
t0=$(last)
ptr hold 400 675 700
sleep 0.2
expect "holding a word types nothing" "$(last)" "$t0"
ptr tap 670 675                     # Remove (the last button before the hide key)
sleep 0.3
check "Remove tells the predictor" "grep -q \"$(printf 'remove\tk[0-9]*\ten\ttea')\" /tmp/predict.log"
ptr tap 40 675                      # the pen key: its hook
check "the pen key runs its hook" "[ -e /tmp/pen ]"
ptr tap 760 675                     # the hide key
check "the hide key hides" "tail -3 /tmp/kbd.err | grep -q '^hidden'"

# private: nothing to the predictor, nothing printed
for p in "8 0 password" "9 0 pin" "0 128 sensitive_data" "0 64 hidden_text"; do
    set -- $p
    : > /tmp/predict.log; : > /tmp/kbd.out
    field -p $1 -H $2
    kill -USR2 $KP; sleep 0.4
    if [ $1 = 9 ]; then ptr tap 100 743; ptr tap 300 743; else ptr tap 460 875; ptr tap 180 825; ptr tap 400 975; fi
    sleep 0.3
    check "private $3: the text arrives" "[ -n \"\$(last | cut -d'|' -f1)\" ]"
    expect "private $3: nothing written to the predictor" "$(cat /tmp/predict.log)" ""
    expect "private $3: nothing printed" "$(cat /tmp/kbd.out)" ""
    # PIN: the PIN pad (three columns: 1 at x 0..266)
    [ $1 = 9 ] && expect "pin: the PIN pad's digits" "$(last)" "12|2"
done

# the purpose picks the keys: number pad (four columns of 200 px, four rows of 75 px)
field -p 3
kill -USR2 $KP; sleep 0.4
ptr tap 100 737; ptr tap 700 737; ptr tap 300 812
expect "number: the number pad (1 - 5)" "$(last)" "1-5|3"
: > /tmp/predict.log
field -p 4
kill -USR2 $KP; sleep 0.4
ptr tap 700 737; ptr tap 300 962
expect "phone: the phone pad (+ 0)" "$(last)" "+0|2"
field -p 6 -t "ana"
kill -USR2 $KP; sleep 0.4
ptr tap 200 975                     # the address row: ⌨ @ space . - Enter (1.5 1 3 1 1 1.5 of 9)
expect "email: @ in the bottom row" "$(last)" "ana@|4"
field -p 5 -t "glassine"
kill -USR2 $KP; sleep 0.4
ptr tap 650 975                     # .com (the 5th: 1.5 1 2.5 1 1.5 1.5 of 9)
expect "url: .com in the bottom row" "$(last)" "glassine.com|12"
kbd_stop

# no surrounding text: the own typing is the context
kbd -l full,special
field -n
ptr tap 120 875; ptr tap 680 825; ptr tap 400 975; ptr tap 360 825; ptr tap 460 875   # s o _ t h
sleep 0.3
check "no surrounding text: words from what was typed" "grep -q \"$(printf '\tso \tth')\" /tmp/predict.log"
kbd_stop

# a character outside the keymap: the German layer's ü, and a held alternate
kbd -l german,special
field
ptr tap 760 825; ptr hold 40 825 600; sleep 0.5
expect "ü and the held ! arrive as text" "$(last)" "ü!|3"
kbd_stop

# the English layer's alternates: held q and . type ! and :
kbd -l full,special
field
ptr tap 40 825; ptr hold 40 825 600; sleep 0.3; ptr hold 640 975 600; sleep 0.8
expect "a tapped q, the held ! and : arrive as text" "$(last)" "q!:|3"
kbd_stop

# an app without text input: plain keys, nothing for the predictor
: > /tmp/predict.log
kbd -l full,special
field -x                            # (after the keyboard: a seat keyboard must exist first)
kill -USR2 $KP; sleep 0.4
ptr tap 460 875
check "no text input: a key event" "grep -q '^key 35 1' /tmp/ti.out"
expect "no text input: nothing to the predictor" "$(cat /tmp/predict.log)" ""
kbd_stop

# the settings' keys for the keyboard: suggestions off sends nothing, handwriting off: no pen key
mkdir -p /tmp/cfg/glassine; printf 'suggestions = off\nhandwriting = off\n' > /tmp/cfg/glassine/predict.conf
: > /tmp/predict.log; rm -f /tmp/pen
XDG_CONFIG_HOME=/tmp/cfg kbd -l full,special
field
ptr tap 460 875; ptr tap 180 825; ptr tap 400 975
sleep 0.3
expect "suggestions off: typing works" "$(last)" "he |3"
expect "suggestions off: nothing to the predictor" "$(cat /tmp/predict.log)" ""
ptr tap 40 675
check "handwriting off: no pen key" "[ ! -e /tmp/pen ]"
kbd_stop

# the session's way (glassine-wvkbd of GlassineOS/shell): no --auto, focus reports on a datagram
# socket (a stand-in for glassine-swayd), the hide key runs a command, and another input method
# holds the seat first (on a real boot glassine-swayd's): "osk im", then retried until it is free
python3 -c '
import os, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(sys.argv[1])
with open(sys.argv[2], "a") as f:
    while True: f.write(s.recv(256).decode() + "\n"); f.flush()
' /tmp/rt/swayd.sock /tmp/notify.log & NP=$!
$B/wvkbd/wvkbd-mobintl --im --hidden -l full,special > /dev/null 2>&1 & HP=$!; sleep 0.5
rm -f /tmp/hid; : > /tmp/predict.log
$B/wvkbd/wvkbd-mobintl --hidden -H 300 -L 240 --no-popup -l full,special --landscape-layers landscape,landscapespecial \
    --im --strip --notify /tmp/rt/swayd.sock --hide-cmd "touch /tmp/hid" \
    --predictor "python3 -u /src/test/fake-predict.py" > /dev/null 2>/tmp/kbd.err & KP=$!; sleep 1
check "the seat's input method taken: the session is told (osk im)" "grep -qx 'osk im' /tmp/notify.log"
kill $HP; wait $HP 2>/dev/null; sleep 3.5     # the retries: 1 s, 2 s
field
check "then it is the input method: the focus reported" "grep -qx 'osk focus 1 0' /tmp/notify.log"
check "... and nothing shown by itself (no --auto)" "! grep -q '^Resize' /tmp/kbd.err"
kill -USR2 $KP; sleep 0.5
ptr tap 460 875; ptr tap 180 825
expect "shown by the session: typing commits text" "$(last)" "he|2"
check "... words asked for" "grep -q \"$(printf 'predict\tk[0-9]*\ten\tnormal\t\the')\" /tmp/predict.log"
ptr tap 760 675; sleep 0.3
check "the hide key runs the session's command" "[ -e /tmp/hid ]"
field_stop; sleep 0.3
check "the field goes: reported" "tail -2 /tmp/notify.log | grep -qx 'osk focus 0 0'"
kbd_stop; kill $NP 2>/dev/null

# a missing predictor: typing goes on
$B/wvkbd/wvkbd-mobintl --im --auto --hidden --strip -H 300 --predictor /nonexistent/predictor -l full,special > /dev/null 2>/tmp/kbd.err & KP=$!; sleep 0.5
field
ptr tap 460 875; ptr tap 180 825
expect "a missing predictor: typing works" "$(last)" "he|2"
kbd_stop

# handwriting (FR-010..FR-013a): the paper is the input method's popup at the field's cursor
# (-c 0,0,2,20 in the field's window, under its title bar: the paper's surface, 800x300 with
# 1 em = 90 px, starts at screen 2,45, so surface y = screen y - 45). A tablet
# can't be made in a headless compositor: the strokes are drawn with the virtual pointer,
# which is how sway hands a pen to a client that doesn't bind the tablet protocol. The
# stand-in recognizer reads the ink as w<strokes> (alternatives a<n>, b<n>).
# Controls (surface px; the top row y 12..62 = screen y 57..107): the chips at x 12..170 (an alternative),
# 180..338 (the read text), 348..506, the correction 506..556, discard 566..616; Backspace
# 638..688, keys 688..738, put away 738..788.
HW=${HW:-python3 -u /src/test/fake-recognizer.py}
pkbd() { $B/wvkbd/wvkbd-mobintl --im --hidden --strip -H 300 --no-popup -o -D -l full,special \
          --predictor "python3 -u /src/test/fake-predict.py" --handwriting "$HW" \
          --pen-flag /tmp/penflag > /tmp/kbd.out 2>/tmp/kbd.err & KP=$!; sleep 0.5; }
pen_tap() { touch /tmp/penflag; kill -USR2 $KP; sleep 0.6; }
preedit() { grep '^preedit' /tmp/ti.out | tail -1 | cut -c9-; }
lastview() { grep '^view' /tmp/kbd.err | tail -1 | cut -c6-; }       # 0 keys 1 paper 2 both 3 hidden
keys_up() { [ "$(grep -E '^(hidden|Resize)' /tmp/kbd.err | tail -1 | cut -c1-6)" = Resize ]; }
: > /tmp/hw.log
FAKE_LOG=/tmp/hw.log pkbd
field -c 0,0,2,20
pen_tap
check "a pen tap into an empty field: the paper, not the keys" "grep -q '^Paper 800x300' /tmp/kbd.err && ! grep -q '^Resize' /tmp/kbd.err"
check "... under the field's cursor" "grep -q '^Paper: the cursor at 0,-20 2x20' /tmp/kbd.err"
ptr stroke 100,200 150,230 200,210 250,240
ptr stroke 300,200 350,230 380,215
check "the strokes reach the recognizer as they are written" "[ \$(grep -c '^stroke' /tmp/hw.log) = 2 ] && grep -q '^read' /tmp/hw.log"
check "... in em (x,y,t)" "grep -Eq '^stroke.h[0-9]+.s[0-9]+.1\\.0889,1\\.7222,[0-9]+ ' /tmp/hw.log"
check "... the screen reports no size (headless): no scale, em" "grep -q '^Paper scale 0.0000 px/mm' /tmp/kbd.err && grep -q '^begin.*	em	' /tmp/hw.log"
check "... the area in em: the paper, its line (half of it), its guide line" "grep -Eq '^area.h[0-9]+.s[0-9]+.0,0,8.88889,3.33333,1.66667,0,2.22222$' /tmp/hw.log"
expect "before the pause: nothing in the field" "$(last)" "|0"
sleep 1.2
expect "after the pause: the reading is pending text" "$(preedit)" "w2"
pen_tap                             # the session's show for the pen again (a re-focus): the paper stays
check "a pen show while the paper is up: it stays" "! grep -q '^Paper hidden' /tmp/kbd.err && [ \"\$(preedit)\" = w2 ]"
expect "... nothing committed yet" "$(last)" "|0"
ptr tap 100 80                      # the left chip: an alternative
sleep 0.4
expect "an alternative tapped: committed, a space after" "$(last)" "a2 |3"
check "... the lesson (picked)" "grep -q \"$(printf '^confirm\th[0-9]*\ts[0-9]*\tpicked\ta2')\" /tmp/hw.log"
ptr stroke 100,200 150,230 200,210 250,240
sleep 1.2
ptr stroke 400,200 450,230 500,210
sleep 0.4
expect "writing on: the reading commits as it was" "$(last)" "a2 w1 |6"
expect "... no lesson for it" "$(grep -c '^confirm' /tmp/hw.log)" "1"
sleep 1.2
ptr stroke 60,200 300,201 560,200   # a strike-through over the pending ink
sleep 0.6
expect "a strike-through over pending text: read again without it" "$(preedit)" ""
expect "... nothing committed" "$(last)" "a2 w1 |6"
ptr stroke 60,200 300,201 560,200   # and on the bare paper: the last word
sleep 0.6
expect "a strike-through on the bare paper: the last word goes" "$(last)" "a2 |3"
ptr stroke 100,200 150,230 200,210 250,240
sleep 1.2
ptr tap 713 80                      # keys
sleep 0.5
expect "the keys control: the pending text commits" "$(last)" "a2 w1 |6"
check "... the keys come" "grep -q '^Resize 800x350' /tmp/kbd.err && grep -q '^Paper hidden' /tmp/kbd.err"
ptr tap 40 675                      # the pen key in the strip
sleep 0.5
check "the pen key: the paper again (any field)" "[ \$(grep -c '^Paper 800x300' /tmp/kbd.err) = 2 ]"
ptr stroke 100,200 150,230 200,210 250,240
sleep 1.2
expect "a reading for discard" "$(preedit)" "w1"
ptr tap 591 80                      # discard
sleep 0.4
expect "discard: the reading goes" "$(preedit)" ""
expect "... nothing committed" "$(last)" "a2 w1 |6"
ptr tap 663 80                      # Backspace
sleep 0.4
expect "the paper's Backspace: the character before the cursor" "$(last)" "a2 w1|5"
ptr tap 713 80                      # keys
sleep 0.5
ptr move 400 800                    # the pen hovers over the keys, then the session says it is near
kill -WINCH $KP; sleep 0.5
check "the pen near over the keys: the keys stay" "[ \$(grep -c '^Paper 800x300' /tmp/kbd.err) = 2 ] && keys_up"
ptr move 400 300                    # over the app
kill -WINCH $KP; sleep 0.5
check "the pen near over the app: the paper" "[ \$(grep -c '^Paper 800x300' /tmp/kbd.err) = 3 ]"
ptr tap 763 80                      # put away
sleep 0.5
check "put away: all of it goes" "[ \"\$(lastview)\" = 3 ] && tail -3 /tmp/kbd.err | grep -q '^Paper hidden' && ! keys_up"
field -c 0,0,2,20 -t "hi"
pen_tap
check "a pen tap into a field with text: the keys" "[ \$(grep -c '^Paper 800x300' /tmp/kbd.err) = 3 ] && keys_up"
for p in "8 0 password" "9 0 pin" "0 128 sensitive_data" "0 64 hidden_text"; do
    set -- $p
    : > /tmp/hw.log
    field -c 0,0,2,20 -p $1 -H $2
    pen_tap
    ptr stroke 100,200 150,230 200,210
    sleep 0.3
    check "private $3: no paper" "[ \$(grep -c '^Paper 800x300' /tmp/kbd.err) = 3 ]"
    expect "private $3: nothing to the recognizer" "$(cat /tmp/hw.log)" ""
done
kbd_stop

# a URL field takes searches too: a space after plain words, none once it looks like an
# address; the paper's space goes again before a reading that starts an address part
export FAKE_TEXTS="1=apple,2=banana,3=example,4=.com"
url_words() {   # purpose, then the stroke counts to write, each committed as read
    field -c 0,0,2,20 -p $1; shift
    pen_tap
    for c in "$@"; do
        i=0; while [ $i -lt $c ]; do ptr stroke $((100 + i * 100)),200 $((140 + i * 100)),230 $((170 + i * 100)),210; i=$((i + 1)); done
        sleep 1.2
        ptr tap 259 80                  # the read text: committed as it was read
        sleep 0.4
    done
    ptr tap 763 80                      # put away: the container's sway crashes when a field goes away under an open paper
    sleep 0.4
}
pkbd
url_words 5 1 2
expect "url field, a search: a space after each word" "$(last)" "apple banana |13"
url_words 5 3 4
expect "url field, an address: the space goes again" "$(last)" "example.com|11"
url_words 6 1 2
expect "email field: no space" "$(last)" "applebanana|11"
kbd_stop
unset FAKE_TEXTS

# the screen's scale (GLASSINE_PAPER_PER_MM, as a screen that reports its size gives it):
# the points in the paper's px with their size, the area with its guide line
: > /tmp/hw.log
GLASSINE_PAPER_PER_MM=4.4749 FAKE_LOG=/tmp/hw.log pkbd
field -c 0,0,2,20
pen_tap
ptr stroke 100,200 150,230 200,210 250,240
sleep 1.2
check "a scale: the paper's" "grep -q '^Paper scale 4.4749 px/mm' /tmp/kbd.err"
check "... begun in px with their size (device:PER_MM)" "grep -q '^begin.*	device:4.4749	' /tmp/hw.log"
check "... the area: the paper, its line, its guide line (BASE)" "grep -Eq '^area.h[0-9]+.s[0-9]+.0,0,800,300,150,0,200$' /tmp/hw.log"
check "... the points in px" "grep -Eq '^stroke.h[0-9]+.s[0-9]+.98\\.0000,155\\.0000,[0-9]+ ' /tmp/hw.log"
expect "... read as before" "$(preedit)" "w1"
ptr stroke 60,200 300,201 560,200   # a strike-through, in px now
sleep 0.6
expect "... a strike-through over the pending text: read again without it" "$(preedit)" ""
ptr tap 763 80                      # put away (a field gone under an open paper crashes the container's sway)
sleep 0.4
kbd_stop

# a missing recognizer: the paper takes ink, typing goes on
HW=/nonexistent/glassine-handwriting pkbd
field -c 0,0,2,20
pen_tap
ptr stroke 100,200 150,230 200,210
sleep 1.2
check "a missing recognizer: the keyboard stays" "kill -0 $KP"
expect "... nothing pending" "$(preedit)" ""
ptr tap 713 80; sleep 0.5
ptr tap 460 875; ptr tap 180 825
expect "... typing works" "$(last)" "he|2"
kbd_stop

# the real recognizer with a trained model (HW_SRC and HW_MODELS given to test.sh):
# its reading lands as pending text, Use of the correction commits with a lesson
if [ -x /tmp/b/glassine-handwriting ] && [ -d /models ]; then
    export GLASSINE_HANDWRITING_MODELS=/models XDG_CONFIG_HOME=/tmp/hwcfg XDG_DATA_HOME=/tmp/hwdata \
        GLASSINE_PREDICT_MODELS=/tmp/nowords GLASSINE_HANDWRITING_FIT_OK=2
    HW=/tmp/b/glassine-handwriting pkbd
    field -c 0,0,2,20
    pen_tap
    # "lo" on the guide line (the paper's y 200 = screen 245)
    ptr stroke 120,145 118,185 116,225 115,245 125,247
    ptr stroke 170,215 155,225 152,240 165,249 180,240 182,225 170,215
    sleep 2.5
    check "real recognizer: a reading is pending" "[ -n \"\$(preedit)\" ]"
    p=$(preedit)
    ptr tap 531 80                  # the correction: the reading in the edit field
    sleep 0.4
    check "real recognizer: correcting shows the keys under the paper" "[ \"\$(lastview)\" = 2 ] && keys_up"
    ptr tap 740 975                 # Enter: Use
    sleep 1
    expect "real recognizer: Use commits the reading" "$(last)" "$p |$(( $(printf '%s' "$p" | wc -m) + 1 ))"
    # the lessons as samples in the store: source, text, and the candidate it was (pick)
    lesson() { python3 -c 'import json,sys
l=[json.loads(x) for x in open(sys.argv[1]).read().replace("\0","\n").splitlines() if x.startswith("{")]
s=l[-1] if l else {}; k=s.get("pick") or {}
print(len(l), s.get("source"), k.get("rank"), k.get("from"), s.get("text"))' /tmp/hwdata/glassine/handwriting/samples.jsonl 2>/dev/null; }
    sleep 0.5
    expect "real recognizer: Use keeps a sample: the reading as it was, candidate 1" "$(lesson)" "1 confirmed 1 ink $p"
    # an alternative picked: a sample with its place; the fit check is for picks the words
    # put there (none here: no word model), so a check nothing passes still keeps it
    ptr stroke 120,145 118,185 116,225 115,245 125,247
    ptr stroke 170,215 155,225 152,240 165,249 180,240 182,225 170,215
    sleep 2.5
    p=$(preedit)
    ptr tap 100 80                  # the left chip: an alternative
    sleep 1
    check "real recognizer: a picked alternative keeps a sample, its place 2 or more" \
        "lesson | grep -Eq '^2 confirmed [2-9] (beam|ink) '"
    check "real recognizer: still running" "kill -0 $KP"
    kbd_stop

    # the size (docs/recognizer.md "Size"): the person's letters kept by another app at
    # another scale (8 units per mm: small letters 3 mm, lone o 3 mm and O 8 mm; and em
    # samples that must not count), read on the paper at 4.4749 px per mm (1 mm = 4.47 px):
    # a lone mark and the case by their size in millimetres, a comma from an apostrophe by
    # the guide line (surface y 200 = screen 245)
    export XDG_DATA_HOME=/tmp/hwsize
    check "size: the person's letters kept (another app, 8 units/mm)" \
        "python3 /t/size-store.py /tmp/b/glassine-handwriting 8 /hw/tests/data/ink.jsonl | grep -q kept"
    GLASSINE_PAPER_PER_MM=4.4749 HW=/tmp/b/glassine-handwriting pkbd
    field -c 0,0,2,20
    pen_tap
    # a circle: centre x, bottom y (surface px), height (px); as screen points
    circ() { python3 -c 'import math,sys
x,b,d=map(float,sys.argv[1:4])
print(" ".join("%d,%d"%(round(x+2+d/2*math.sin(2*math.pi*i/16)),round(b+45-d/2-d/2*math.cos(2*math.pi*i/16))) for i in range(17)))' "$@"; }
    reads() {   # name, wanted readings (any of them, |-separated), then the strokes
        n=$1 w=$2; shift 2
        for st in "$@"; do ptr stroke $st; done
        sleep 2.5
        got=$(preedit)
        case "|$w|" in
        *"|$got|"*) [ -n "$got" ] && echo "ok   size: $n reads as $w" || { echo "FAIL size: $n reads as $w"; echo "     got nothing"; fail=1; dump; } ;;
        *) echo "FAIL size: $n reads as $w"; echo "     got:  $got"; fail=1; dump ;;
        esac
        ptr tap 591 80; sleep 0.5          # discard: the paper bare again
    }
    reads "a lone dot on the line" "." "$(circ 300 200 4)"
    reads "a tick down through the line (2.2 mm)" "," "302,243 301,246 300,250 299,253"
    reads "a tick high above the line" "'|’" "302,205 301,208 301,212 300,215"
    reads "a short line above the line (5 mm): a hyphen, not a strike" "-" "302,235 309,235 316,236 324,235"
    reads "a small circle (2.5 mm)" "o" "$(circ 300 200 11)"
    reads "a large circle (10 mm)" "O|0" "$(circ 300 200 45)"
    check "size: the person's letters measured" "[ -s /tmp/hwsize/glassine/handwriting/size ]"
    check "size: still running" "kill -0 $KP"
    ptr tap 763 80; sleep 0.4
    kbd_stop
else
    echo "skip the real recognizer (test.sh: HW_SRC=handwriting checkout HW_MODELS=dir with en/recognizer.ghm)"
fi

[ $fail = 0 ] || { echo "--- keyboard"; tail -20 /tmp/kbd.err; echo "--- field"; tail -20 /tmp/ti.out; }
exit $fail
