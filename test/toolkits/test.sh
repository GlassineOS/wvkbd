#!/bin/sh
# Picks in real toolkits' text fields (host test, needs Docker): stock sway headless in an
# Alpine container, the keyboard built from this checkout with the real glassine-predict
# (built from a GlassineOS/shell checkout: GLASSINE_SHELL=path; a small model from
# corpus.txt) against a GTK 3 entry, a GTK 4 entry and text view, a field in a Firefox
# and in a Chromium page, and Chromium's address bar. Each field's text is read back from
# the app. Slow (the browsers): not part of check-e2e.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
S=${GLASSINE_SHELL:-}
if [ -z "$S" ] || [ ! -f "$S/src/glassine-predict.c" ]; then
    echo "GLASSINE_SHELL must name a GlassineOS/shell checkout (for glassine-predict)" >&2
    exit 2
fi
S=$(cd "$S" && pwd)
IMG=$(timeout 1200 docker build -q "$D")
timeout 1800 docker run --rm -e DEBUG="${DEBUG:-}" -e ONLY="${ONLY:-}" --memory 3g --shm-size 1g \
    -v "$D/../..":/src:ro -v "$D/../e2e":/t:ro -v "$D":/k:ro -v "$S/src":/p:ro "$IMG" sh /k/run.sh
