# 01_provisioning

Factory bring-up: HTTPS provision, NVS save, optional OTA. No MQTT client.

## Code

```c
nvs_flash_init();

iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
iotmer_client_t client;

iotmer_init(&client, &cfg);
/* logs device_id, credentials summary */
```

See `main/main.c`.

## Configure

`idf.py menuconfig` → **IOTMER**

| Setting | Value |
|---------|-------|
| `IOTMER_PROVISION_AUTH_CODE` | Factory API key |
| `IOTMER_WORKSPACE_ID` | Workspace UUID |
| Wi‑Fi SSID / password | Required |

## Build

```bash
idf.py set-target esp32
idf.py build flash monitor
```

Example defaults enable `IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA` for forced re-flash workflows.
