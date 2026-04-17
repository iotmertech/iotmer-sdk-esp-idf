# 04_config — MQTT Config Protocol v1 (reference)

This example demonstrates the **MQTT Config Protocol v1** implemented by the `iotmer` component.

Flow:

`config/meta` (retained) → `config/get` → `config/resp` (identity or gzip+base64 chunks) → `config/status`

For the complete topic/payload contract (field-by-field), see the docs page:

- `docs/sdk/esp-idf/mqtt-config-protocol.md`

## First build

This example pulls `espressif/zlib` via the component manager (used for gzip inflate). If `dependencies.lock` is not present yet:

```bash
cd examples/04_config
idf.py set-target esp32c3     # or your target SoC
idf.py build                  # generates managed_components/ + dependencies.lock
```

Committing `dependencies.lock` is recommended for CI and reproducible builds.

## What to look for in logs

- Subscribes to `…/config/#`
- Waits for `…/config/meta` (usually retained)
- Publishes `…/config/get` with:
  - `rid` (UUID)
  - `want.chunk_bytes=4096`
  - `want.max_total_bytes=1048576`
- Receives `…/config/resp` (single or chunked), validates SHA-256, prints a preview, then publishes `…/config/status`.
