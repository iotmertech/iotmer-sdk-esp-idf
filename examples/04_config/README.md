# 04_config — MQTT Config Protocol (reference)

This example demonstrates the **MQTT Config Protocol** implemented by the `iotmer` component.

Flow:

`config/meta` (retained) → `config/get` → `config/resp` (chunked `data_b64`, gzip or identity) → `config/status`

For the complete topic/payload contract (field-by-field), see the docs page:

- `docs/sdk/esp-idf/mqtt-config-protocol.md`

## First build

This example pulls `espressif/zlib` via the component manager (used for gzip inflate). If `dependencies.lock` is not present yet, `cd` into this example (same `main/` exists in both tree locations below), then build:

**Monorepo (recommended):**

```bash
cd examples/04_config
idf.py set-target esp32c3     # or your target SoC
idf.py build                  # generates managed_components/ + dependencies.lock
```

**Same example under the component tree** (`components/iotmer/examples/04_config`, also copied into published `iotmer` packages under `examples/04_config`):

```bash
cd components/iotmer/examples/04_config
idf.py set-target esp32c3
idf.py build
```

`examples/04_config/CMakeLists.txt` sets `EXTRA_COMPONENT_DIRS` so the sibling `components/iotmer` sources are used. The bundled `CMakeLists.txt` under `components/iotmer/examples/` is for the Component Registry layout.

Committing `dependencies.lock` is recommended for CI and reproducible builds.

## What to look for in logs

- Subscribes to `…/config/#`
- Waits for `…/config/meta` (usually retained)
- Publishes `…/config/get` with:
  - `rid` (UUID v4)
  - `want.chunk_bytes=4096`
  - `want.max_total_bytes=1048576`
  - `want.accept_encoding`: `["gzip","identity"]`
- Receives `…/config/resp` as one or more **chunked** messages (`chunk_index`, `data_b64`), validates SHA-256 over the reassembled effective JSON, prints a preview, then publishes `…/config/status`.
