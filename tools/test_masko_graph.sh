#!/usr/bin/env bash
#
# Host tests for masko_graph: the canvas state machine and the JSON loader.
#
# Uses the system compiler, so this runs in seconds with no ESP-IDF toolchain
# and no hardware. The JSON loader needs cJSON; rather than vendor a copy we
# compile the one that ships with ESP-IDF, so the test exercises the same parser
# the firmware will use.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
CJSON_DIR="$IDF_PATH/components/json/cJSON"
[ -f "$CJSON_DIR/cJSON.c" ] || {
    echo "error: cJSON not found at $CJSON_DIR" >&2
    echo "       set IDF_PATH to your ESP-IDF checkout" >&2
    exit 2
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CFLAGS=(-std=c11 -Wall -Wextra -Werror
        -I "$ROOT/components/masko_graph/include"
        -I "$CJSON_DIR")

echo "== graph state machine =="
cc "${CFLAGS[@]}" \
   "$ROOT/test/test_masko_graph.c" \
   "$ROOT/components/masko_graph/masko_graph.c" \
   -o "$TMP/test_graph"
"$TMP/test_graph"

echo
echo "== MaskoAnimationConfig loader =="
cc "${CFLAGS[@]}" \
   "$ROOT/test/test_masko_graph_json.c" \
   "$ROOT/components/masko_graph/masko_graph.c" \
   "$ROOT/components/masko_graph/masko_graph_json.c" \
   "$CJSON_DIR/cJSON.c" \
   -lm \
   -o "$TMP/test_graph_json"
"$TMP/test_graph_json"
