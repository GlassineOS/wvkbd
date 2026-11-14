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

# a missing predictor: typing goes on
$B/wvkbd/wvkbd-mobintl --im --auto --hidden --strip -H 300 --predictor /nonexistent/predictor -l full,special > /dev/null 2>/tmp/kbd.err & KP=$!; sleep 0.5
field
ptr tap 460 875; ptr tap 180 825
expect "a missing predictor: typing works" "$(last)" "he|2"
kbd_stop

[ $fail = 0 ] || { echo "--- keyboard"; tail -20 /tmp/kbd.err; echo "--- field"; tail -20 /tmp/ti.out; }
exit $fail
