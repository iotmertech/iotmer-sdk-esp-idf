# IOTMER developer tools

Python utilities for teams integrating the SDK on **custom hardware** (no IOTMER reference board).

Requires **Python 3.8+**. No extra packages.

## `create-app`

Scaffolds an ESP-IDF project with `components/hw/` (your PCB) and `components/cloud/` (IOTMER).

```bash
python tools/iotmer.py create-app my-gateway --chip esp32s3 --profile field
cd my-gateway
idf.py set-target esp32s3
idf.py build
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `-o`, `--output` | `.` | Parent directory for the new project |
| `--chip` | `esp32c3` | `esp32`, `esp32s3`, `esp32c3`, `esp32c6`, … |
| `--profile` | `field` | `field` (NVS session) or `factory` (HTTPS provision) |
| `--deps` | `registry` | `registry` → `iotmertech/iotmer` in `idf_component.yml` |
| `--sdk-root` | repo root | Used with `--deps local` for `EXTRA_COMPONENT_DIRS` |

### Local SDK checkout

```bash
python tools/iotmer.py create-app lab-test --deps local --sdk-root .
```

## `doctor`

Checks `sdkconfig.defaults` / `sdkconfig` for common integration mistakes (TLS bundle, main stack, OTA partitions, factory/field auth code).

```bash
python tools/iotmer.py doctor --project examples/02_telemetry
python tools/iotmer.py doctor --project ./my-gateway
```

Exit code `1` when errors are found; warnings do not fail the command.

## CI (GitHub Actions)

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs on pushes and PRs to `main`:

| Job | What |
|-----|------|
| **doctor** | `iotmer doctor` on all `examples/0*` + `create-app` smoke |
| **build-examples** | Matrix: examples × `esp32` / `esp32c3` / `esp32s3` (ESP-IDF v5.4) |
| **build-scaffold** | `create-app` + `idf.py build` on `esp32c3` |

Reproduce locally (doctor only without ESP-IDF):

```bash
bash tools/ci/build_matrix.sh
bash tools/ci/build_matrix.sh --build   # after sourcing export.sh
```


- [Custom hardware integration](../docs/sdk/esp-idf/custom-hardware.md)
- [Factory vs field profiles](../docs/sdk/esp-idf/factory-field-profiles.md)
