# 03_lwt_presence

MQTT retained presence + LWT on `{workspace_slug}/{device_key}/presence`.

## Enable

```c
iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
cfg.presence_lwt_enable = true;

iotmer_client_t client;
iotmer_init(&client, &cfg);
iotmer_connect(&client);
```

## Payload

| Event | JSON |
|-------|------|
| Connect | `{"status":"online","ts":<unix\|0>}` |
| Unexpected disconnect (LWT) | `{"status":"offline","ts":0}` |

## Build

```bash
idf.py set-target esp32
idf.py build flash monitor
```

Configure Wi‑Fi via menuconfig or `sdkconfig.defaults`.
