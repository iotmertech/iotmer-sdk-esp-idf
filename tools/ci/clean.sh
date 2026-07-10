#!/usr/bin/env bash
# Remove ESP-IDF build artifacts from this repo (safe: only known project roots).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

remove_if_exists() {
  if [[ -e "$1" ]]; then
    rm -rf "$1"
    echo "removed: $1"
  fi
}

clean_project_dir() {
  local dir="$1"
  [[ -d "$dir" ]] || return 0
  remove_if_exists "$dir/build"
  remove_if_exists "$dir/sdkconfig"
  remove_if_exists "$dir/sdkconfig.old"
  remove_if_exists "$dir/managed_components"
}

echo "Cleaning IOTMER SDK repo artifacts under $ROOT"

# Accidental root-level IDF outputs (there is no root firmware project)
remove_if_exists "$ROOT/build"
remove_if_exists "$ROOT/sdkconfig"
remove_if_exists "$ROOT/sdkconfig.old"
remove_if_exists "$ROOT/managed_components"
remove_if_exists "$ROOT/dependencies.lock"
remove_if_exists "$ROOT/ci-scratch"

# Top-level reference examples
for ex in "$ROOT"/examples/0[1-5]_*/; do
  clean_project_dir "$ex"
done

# Bundled registry examples (do not recurse into dist/ or pc_ble_client venv)
for ex in "$ROOT"/components/iotmer/examples/0[1-5]_*/; do
  clean_project_dir "$ex"
done

echo "Done. Build only inside an example directory or an external create-app project."
