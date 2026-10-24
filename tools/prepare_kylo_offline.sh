#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASSETS="$ROOT/examples/waveshare_amoled_175/assets"
BASE="https://assets.masko.ai/547bbb8e/kylo-the-tide-mender-9fhe8p43"

mkdir -p "$ASSETS"
curl -fL --retry 2 "$BASE/jar-gazing-581594e9-384.mkpk" \
  -o "$ASSETS/kylo-jar-gazing.loop.mkpk"
curl -fL --retry 2 "$BASE/specimen-notes-9122bdf9-384.mkpk" \
  -o "$ASSETS/kylo-specimen-notes.loop.mkpk"
curl -fL --retry 2 "$BASE/excited-wave-c6be4a55-384.mkpk" \
  -o "$ASSETS/kylo-excited-wave.loop.mkpk"
curl -fL --retry 2 "$BASE/jar-gazing---specimen-notes-e33d8658-384.mkpk" \
  -o "$ASSETS/kylo-jar-gazing_to_specimen-notes.once.mkpk"
curl -fL --retry 2 "$BASE/jar-gazing---excited-wave-d3b03951-384.mkpk" \
  -o "$ASSETS/kylo-jar-gazing_to_excited-wave.once.mkpk"

for pack in \
  "$ASSETS/kylo-jar-gazing.loop.mkpk" \
  "$ASSETS/kylo-specimen-notes.loop.mkpk" \
  "$ASSETS/kylo-excited-wave.loop.mkpk" \
  "$ASSETS/kylo-jar-gazing_to_specimen-notes.once.mkpk" \
  "$ASSETS/kylo-jar-gazing_to_excited-wave.once.mkpk"; do
  python3 "$ROOT/tools/masko_pack.py" verify "$pack"
done

python3 "$ROOT/tools/masko_bundle.py" "$ASSETS/mascot_bundle.mkpk" \
  "$ASSETS/kylo-jar-gazing.loop.mkpk" \
  "$ASSETS/kylo-specimen-notes.loop.mkpk" \
  "$ASSETS/kylo-excited-wave.loop.mkpk" \
  "$ASSETS/kylo-jar-gazing_to_specimen-notes.once.mkpk" \
  "$ASSETS/kylo-jar-gazing_to_excited-wave.once.mkpk" \
  --once "$ASSETS/kylo-jar-gazing_to_specimen-notes.once.mkpk" \
  --once "$ASSETS/kylo-jar-gazing_to_excited-wave.once.mkpk"

python3 - "$ASSETS/mascot_bundle.mkpk" <<'PY'
import os
import sys

size = os.path.getsize(sys.argv[1])
limit = int(9.5 * 1024 * 1024)
print(f"bundle: {size} bytes ({size / (1024 * 1024):.2f} MiB); budget: 9.50 MiB")
if size > limit:
    raise SystemExit("bundle exceeds the safe budget for the 10 MiB masko partition")
PY
