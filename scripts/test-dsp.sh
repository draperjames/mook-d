#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
TEST_BIN=${TMPDIR:-/tmp}/mookd-dsp-safety

${CC:-cc} \
  -O1 -g \
  -fsanitize=address,undefined,float-cast-overflow \
  -fno-omit-frame-pointer \
  -I "$ROOT/src/dsp" \
  -o "$TEST_BIN" \
  "$ROOT/tests/dsp_safety.c" \
  -lm

ASAN_OPTIONS=detect_leaks=0 "$TEST_BIN"
