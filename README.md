# iotmer-sdk-esp-idf

Official ESP-IDF SDK for [IOTMER](https://iotmer.com).

Connects Espressif chips via Wi‑Fi, HTTPS provisioning, NVS credentials, MQTT (TLS), optional HTTPS OTA, and optional BLE JSON (`iotmer_ble`).

Public API: [`components/iotmer/include/iotmer_client.h`](components/iotmer/include/iotmer_client.h)

## Install

```yaml
# idf_component.yml
dependencies:
  iotmertech/iotmer: "*"
```

```bash
idf.py update-dependencies
```

| Package | Registry |
|---------|----------|
| Core SDK | [`iotmertech/iotmer`](https://components.espressif.com/components/iotmertech/iotmer) |
| BLE JSON (optional) | [`iotmertech/iotmer_ble`](https://components.espressif.com/components/iotmertech/iotmer_ble) |

**Requires:** ESP-IDF ≥ 5.x · ESP32 family · TLS via `esp_crt_bundle_attach`

## Minimal example

```c
#include "iotmer_client.h"

iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
iotmer_client_t client;

iotmer_init(&client, &cfg);
iotmer_connect(&client);
iotmer_subscribe_commands(&client, on_command, NULL);
```

## Repository layout

| Path | Contents |
|------|----------|
| [`examples/`](examples/README.md) | Reference firmware (provision, telemetry, config, BLE) |
| [`components/iotmer/`](components/iotmer/README.md) | Core component |
| [`components/iotmer_ble/`](components/iotmer_ble/README.md) | Optional BLE transport |
| [`docs/`](docs/) | Markdown source for [docs.iotmer.com](https://docs.iotmer.com/) |
| [`CHANGELOG.md`](CHANGELOG.md) | Release notes |
| [`AGENTS.md`](AGENTS.md) | Contributor and agent guide |

## Documentation

- **Platform** (console, MQTT, REST): [docs.iotmer.com](https://docs.iotmer.com/)
- **ESP-IDF integration**: [docs.iotmer.com/docs/sdk/esp-idf/intro](https://docs.iotmer.com/docs/sdk/esp-idf/intro)

## License

MIT — [LICENSE](LICENSE)
