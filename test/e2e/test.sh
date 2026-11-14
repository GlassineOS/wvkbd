#!/bin/sh
# End-to-end host test (spec 006 acceptance, host level): stock sway headless in an Alpine
# container, the keyboard built from this checkout, a text-input-v3 test field
# (ti-client), a virtual pointer (vptr) and the stand-in predictor. Needs Docker.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
IMG=$(timeout 900 docker build -q "$D")
# HW_SRC (a GlassineOS/handwriting checkout) and HW_MODELS (a directory with
# en/recognizer.ghm): the paper with the real recognizer too
set --
[ -n "${HW_SRC:-}" ] && set -- "$@" -v "$HW_SRC/recognizer":/hw:ro
[ -n "${HW_MODELS:-}" ] && set -- "$@" -v "$HW_MODELS":/models:ro
timeout 900 docker run --rm -e DEBUG="${DEBUG:-}" --memory 1g -v "$D/../..":/src:ro -v "$D":/t:ro "$@" "$IMG" sh /t/run.sh
