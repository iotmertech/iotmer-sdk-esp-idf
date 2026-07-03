# 02_telemetry

Field firmware: MQTT connect, subscribe to commands, publish telemetry.

## Code

```c
iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
iotmer_client_t client;

iotmer_init(&client, &cfg);
iotmer_connect(&client);

iotmer_subscribe_commands(&client, on_command, NULL);
iotmer_telemetry_publish(&client, "{\"temp\":22.5,\"hum\":60}");
```

See `main/main.c` for the full connect/wait loop.

## Configure

After first provision via `01_provisioning`, leave `IOTMER_PROVISION_AUTH_CODE` empty. HTTPS is skipped when NVS holds a complete session.

## Build

```bash
idf.py set-target esp32
idf.py build flash monitor
```
