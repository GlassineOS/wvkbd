#!/bin/sh
# (inside the container, see test.sh) /src = this checkout, /t = test/e2e (its build and
# virtual pointer), /k = this directory, /p = GlassineOS/shell's src. ONLY: a list of the
# fields to run (gtk3 gtk4 gtk4view firefox chromium omnibox omnibox-paper). The keyboard's protocol
# log (WAYLAND_DEBUG) shows what each app reports; DEBUG=1 prints it on a failure.
set -u
sh /t/build.sh || exit 1
B=/tmp/b
cc -O2 -o /tmp/glassine-predict /p/glassine-predict.c /p/predict/store.c -lm || exit 1
cc -O2 -o /tmp/predict-build /p/predict/build.c -lm || exit 1
mkdir -p /tmp/models
/tmp/predict-build -m 1 -B 1 -T 1 -P 1 en /tmp/models/en.gpm /k/corpus.txt >/dev/null 2>&1 || exit 1
export GLASSINE_PREDICT_MODELS=/tmp/models XDG_DATA_HOME=/tmp/data XDG_CONFIG_HOME=/tmp/cfg HOME=/tmp/home
mkdir -p $HOME
export XDG_RUNTIME_DIR=/tmp/rt; mkdir -p -m 700 $XDG_RUNTIME_DIR
printf 'output HEADLESS-1 resolution 800x1000\n' > /tmp/c
cp /usr/bin/sway /tmp/sway          # the package has file capabilities: no exec in a container
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 /tmp/sway -c /tmp/c >/tmp/sway.log 2>&1 &
for i in $(seq 1 50); do ls $XDG_RUNTIME_DIR/sway-ipc.* >/dev/null 2>&1 && break; sleep 0.2; done
export SWAYSOCK=$(ls $XDG_RUNTIME_DIR/sway-ipc.*) WAYLAND_DISPLAY=wayland-1
mkfifo /tmp/ptr; $B/vptr /tmp/ptr 800 1000 2>/dev/null & sleep 0.3
fail=0
ptr() { echo "$*" > /tmp/ptr; sleep 0.4; }
ptr tap 10 10                       # the pointer enters (its first event is no press)
# -H 300: the strip at y 650..700, the keys below (as in test/e2e)
WAYLAND_DEBUG=1 $B/wvkbd/wvkbd-mobintl --im --auto --hidden --strip -H 300 --no-popup -o -D -l full,special \
    --predictor /tmp/glassine-predict > /dev/null 2>/tmp/kbd.err & sleep 0.8

hello() { ptr tap 460 875; ptr tap 180 825; ptr tap 700 875; ptr tap 700 875; ptr tap 680 825; sleep 0.8; }
hell() { ptr tap 460 875; ptr tap 180 825; ptr tap 700 875; ptr tap 700 875; sleep 0.8; }
space() { ptr tap 400 975; sleep 0.8; }
bs() { ptr tap 740 925; sleep 0.8; }
# 80 ms a tap: h e l l o, space, h e l l, Backspace
fast() { for k in "460 875" "180 825" "700 875" "700 875" "680 825" "400 975" "460 875" "180 825" "700 875" "700 875" "740 925"; do
    echo "tap $k" > /tmp/ptr; sleep 0.08; done; sleep 1.5; }
pick() { ptr tap 400 675; sleep 0.8; }  # the likeliest word, in the middle
enter() { ptr tap 740 975; sleep 2; }
# what the app reports (the keyboard's log): the last surrounding text
reported() { grep -a 'input_method_v2#[0-9]*\.surrounding_text' /tmp/kbd.err | tail -1 | sed 's/.*surrounding_text//'; }
# the field's text from the app itself: TEXT|CURSOR
field_text() { grep -aho 'text [^"]*|[0-9]*' /tmp/f.out /tmp/f.err 2>/dev/null | tail -1 | cut -c6-; }
# the address bar's query after Enter, from the browser's tab list (oq: as typed)
omni_text() {
    wget -qO- http://127.0.0.1:9222/json/list 2>/dev/null | python3 -c '
import json, sys, urllib.parse
for t in json.load(sys.stdin):
    q = urllib.parse.parse_qs(urllib.parse.urlparse(t["url"]).query, keep_blank_values=True)
    if t["type"] == "page" and "oq" in q: print(q["oq"][0]); break'
}
activations() { grep -ac 'input_method_v2#[0-9]*\.activate()' /tmp/kbd.err; }
dump() {
    [ -n "${DEBUG:-}" ] || return 0
    echo "--- what the app reported (since the field started)"
    tail -n +$n0 /tmp/kbd.err | grep -aE 'input_method_v2#[0-9]+\.(surrounding_text|text_change_cause|delete_surrounding_text|commit_string|commit)\(' | tail -30
}
expect() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "     got:  '$2'"; echo "     want: '$3'"; fail=1; dump; fi; }

FP=
start() {
    a0=$(activations); n0=$(wc -l < /tmp/kbd.err); rm -rf /tmp/data; : > /tmp/f.out; : > /tmp/f.err
    case $1 in
    gtk3|gtk4) GDK_BACKEND=wayland GTK_IM_MODULE=wayland python3 /k/field.py ${1#gtk} > /tmp/f.out 2>/tmp/f.err & FP=$! ;;
    gtk4view) GDK_BACKEND=wayland GTK_IM_MODULE=wayland python3 /k/field.py 4 view > /tmp/f.out 2>/tmp/f.err & FP=$! ;;
    firefox)
        rm -rf /tmp/ff; mkdir -p /tmp/ff
        printf 'user_pref("%s", %s);\n' devtools.console.stdout.content true browser.shell.checkDefaultBrowser false \
            browser.aboutwelcome.enabled false datareporting.policy.dataSubmissionEnabled false \
            toolkit.telemetry.reportingpolicy.firstRun false app.update.disabledForTesting true > /tmp/ff/user.js
        MOZ_ENABLE_WAYLAND=1 GTK_IM_MODULE=wayland firefox --no-remote --profile /tmp/ff --kiosk file:///k/page.html > /tmp/f.out 2>/tmp/f.err & FP=$! ;;
    chromium|omnibox)
        rm -rf /tmp/ch
        if [ $1 = chromium ]; then u="--kiosk file:///k/page.html"; else u="--remote-debugging-port=9222 ${2:-about:blank}"; fi
        chromium --no-sandbox --ozone-platform=wayland --enable-wayland-ime --wayland-text-input-version=3 \
            --user-data-dir=/tmp/ch --no-first-run --disable-gpu --enable-logging=stderr --v=0 $u > /tmp/f.out 2>/tmp/f.err & FP=$! ;;
    esac
    for i in $(seq 1 120); do
        [ "$1" = omnibox ] && [ $i = 40 ] && ptr tap 300 60     # the address bar
        [ "$(activations)" -gt "$a0" ] && break; sleep 0.5
    done
    sleep 1.5
    [ "$(activations)" -gt "$a0" ] || { echo "FAIL $1: no text field focused"; fail=1; }
}
stop() { kill $FP 2>/dev/null; sleep 1; pkill -9 -f firefox; pkill -9 -f chromium; pkill -9 -f field.py; wait $FP 2>/dev/null; sleep 0.5; }

# the paper over the address bar: the stand-in recognizer (its readings by stroke count),
# strokes and taps at paper coordinates (the paper's offset found where the pointer enters it)
paper_at() {   # (again for each word: the paper follows the cursor)
    o=; ptr move 790 5
    for y in 20 60 100 150 200 300 400 500 600; do
        k=$(grep -ac '^Paper: pointer in' /tmp/kbd.err)
        ptr move 400 $y
        [ "$(grep -ac '^Paper: pointer in' /tmp/kbd.err)" -gt "$k" ] || continue
        o=$(grep -a '^Paper: pointer in' /tmp/kbd.err | tail -1 | sed 's/.* in //'); y0=$y; break
    done
    [ -n "$o" ] && echo "     (the paper: screen 400,$y0 = paper $o)"
    [ -n "$o" ] || { echo "FAIL the paper: not found"; fail=1; grep -a '^Paper\|^paper\|^view\|^Resize' /tmp/kbd.err | tail -8; o=0,0; y0=0; }
    dx=$((400 - ${o%,*})); dy=$((y0 - ${o#*,}))
}
pp() { ptr "$1" $(for q in $2; do echo $((${q%,*} + dx)),$((${q#*,} + dy)); done); }
write() {   # stroke counts: each word on a fresh paper (the pen key), its strokes in one
    for c in "$@"; do   # go, committed by the session's hide (the paper follows the cursor:
        kill -USR2 $KP; sleep 1; ptr tap 40 675; sleep 1   # a commit moves it)
        paper_at; i=0
        while [ $i -lt $c ]; do
            echo stroke $((100 + i * 100 + dx)),$((200 + dy)) $((140 + i * 100 + dx)),$((230 + dy)) $((170 + i * 100 + dx)),$((210 + dy)) > /tmp/ptr
            i=$((i + 1))
        done
        sleep 2; kill -USR1 $KP; sleep 1.5
    done
}
pkbd() {
    FAKE_TEXTS="1=apple,2=banana,3=example,4=.com" WAYLAND_DEBUG=1 $B/wvkbd/wvkbd-mobintl --im --auto --hidden --strip \
        -H 300 --no-popup -o -D -l full,special --predictor /tmp/glassine-predict \
        --handwriting "python3 -u /src/test/fake-recognizer.py" > /dev/null 2>/tmp/kbd.err & KP=$!; sleep 0.8
}
omnibox_paper() {   # URL, then stroke counts: the address bar's text as reported. The
    pkbd            # paper is the keyboard's popup at the address bar's cursor, partly off
    start omnibox $1; shift   # the screen: the keyboard goes before the browser (a field
                              # going under an open paper takes the test's sway down)
    write "$@"
    R=$(reported)
}

for f in ${ONLY:-gtk3 gtk4 gtk4view firefox chromium omnibox omnibox-paper}; do
    if [ $f = omnibox-paper ]; then
        pkill -f wvkbd-mobintl; sleep 0.5
        omnibox_paper about:blank 1 2; kill $KP; wait $KP 2>/dev/null; stop
        expect "omnibox, the paper: a search gets its spaces" "$R" '("apple banana ", 13, 13)'
        omnibox_paper about:blank 3 4
        kill -USR2 $KP 2>/dev/null; sleep 1; enter       # (no report after the last edit: Enter, the page's address)
        u=$(wget -qO- http://127.0.0.1:9222/json/list 2>/dev/null | python3 -c 'import json,sys; print([t["url"] for t in json.load(sys.stdin) if t["type"]=="page"][0])')
        kill $KP 2>/dev/null; wait $KP 2>/dev/null; stop
        expect "omnibox, the paper: an address doesn't (example, .com)" "${u#*://}" "example.com/"
        omnibox_paper file:///k/page.html 1 2; kill $KP; wait $KP 2>/dev/null; stop
        expect "omnibox, the paper: a search over the selected address" "$R" '("apple banana ", 13, 13)'
        continue
    fi
    if [ $f = omnibox ]; then
        # the address bar: a URL field (the purpose) that also takes searches; read back
        # as the search it sends on Enter ("oq", the query as typed, spaces kept)
        start omnibox; hello; pick; enter
        expect "omnibox: hello, pick: replaced, a space after (a search)" "$(omni_text)" "Hello "; stop
        start omnibox; hello; space; pick; enter
        expect "omnibox: hello, space, pick: one space between" "$(omni_text)" "hello world "; stop
        start omnibox; hello; pick; bs; pick; enter
        expect "omnibox: pick, Backspace, pick: the word again, no word glued on" "$(omni_text)" "Hello "; stop
        start omnibox; fast; pick; enter
        expect "omnibox: typed fast, with a Backspace, a pick" "$(omni_text)" "hello Hello "; stop
        start omnibox; hell; r=$(reported)
        echo "     (the address bar's report after 'hell': $r)"
        pick; enter
        expect "omnibox: a pick over an inline completion" "$(omni_text)" "Hello "; stop
        continue
    fi
    # Chromium's page field asks for capitals at a sentence start
    w=hello; [ $f = chromium ] && w=Hello
    start $f; hello
    expect "$f: $w is typed" "$(field_text)" "$w|5"
    pick
    expect "$f: $w, pick Hello: replaced, a space after" "$(field_text)" "Hello |6"
    pick
    expect "$f: then a next word: one space between" "$(field_text)" "Hello world |12"
    stop
    start $f; hello; space; pick
    expect "$f: $w, space, pick: one space between" "$(field_text)" "$w world |12"
    stop
    start $f; hello; pick; bs
    expect "$f: pick, Backspace: the space deleted" "$(field_text)" "Hello|5"
    pick
    expect "$f: then a pick completes that word again, nothing glued on" "$(field_text)" "Hello |6"
    stop
    # typing fast (80 ms a tap): hello, space, hell, Backspace; then a pick
    start $f; fast
    expect "$f: typed fast, with a Backspace" "$(field_text)" "$w hel|9"
    pick
    expect "$f: ... then a pick replaces the word" "$(field_text)" "$w Hello |12"
    stop
done
exit $fail
