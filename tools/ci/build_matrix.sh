#!/usr/bin/env bash
# Local reproduction of .github/workflows/ci.yml (requires ESP-IDF export + Docker for builds).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

echo "=== iotmer doctor (examples) ==="
for ex in examples/0[1-5]_*/; do
  echo "--- $ex"
  python3 tools/iotmer.py doctor --project "$ex"
done

echo "=== iotmer doctor (scaffold) ==="
SCRATCH="${TMPDIR:-/tmp}/iotmer-ci-scratch"
rm -rf "$SCRATCH"
python3 tools/iotmer.py create-app local-smoke \
  --deps local --sdk-root "$ROOT" -o "$SCRATCH" --chip esp32c3 --profile field
python3 tools/iotmer.py doctor --project "$SCRATCH/local-smoke"

if [[ -z "${IDF_PATH:-}" ]]; then
  echo ""
  echo "IDF_PATH not set — skipping idf.py builds."
  echo "Source ESP-IDF export.sh, then re-run: $0 --build"
  exit 0
fi

if [[ "${1:-}" != "--build" ]]; then
  echo ""
  echo "Doctor OK. To build the matrix locally: $0 --build"
  exit 0
fi

build_one() {
  local example="$1"
  local target="$2"
  echo "=== idf.py build $example ($target) ==="
  (
    cd "$ROOT/$example"
    idf.py set-target "$target"
    idf.py build
  )
}

build_one examples/01_provisioning esp32c3
build_one examples/02_telemetry esp32
build_one examples/02_telemetry esp32c3
build_one examples/02_telemetry esp32s3
build_one examples/03_lwt_presence esp32c3
build_one examples/04_config esp32c3
build_one examples/05_ble_json esp32c3

echo "=== idf.py build scaffold ==="
(
  cd "$SCRATCH/local-smoke"
  idf.py set-target esp32c3
  idf.py build
)

echo "All builds finished."
