# 04_config

MQTT Config Protocol demo.

## Flow

```
config/meta → config/get → config/resp (chunked) → config/status
```

Full contract: [mqtt-config-protocol.md](../../docs/sdk/esp-idf/mqtt-config-protocol.md)

## Build

Pulls `espressif/zlib` for gzip inflate.

```bash
idf.py set-target esp32c3
idf.py build flash monitor
```

Commit `dependencies.lock` for reproducible CI builds.

## Expected logs

1. Subscribe `…/config/#`
2. Receive retained `…/config/meta`
3. Publish `…/config/get` with `rid`, `want.chunk_bytes=4096`, `accept_encoding: ["gzip","identity"]`
4. Reassemble `…/config/resp` chunks, verify SHA256, publish `…/config/status`

## Callbacks

Default (`IOTMER_CONFIG_DEFER_CALLBACKS=y`): `on_cfg_event` runs on the SDK worker task. Safe to call `iotmer_config_request()` and `iotmer_config_publish_status()` from the callback.
