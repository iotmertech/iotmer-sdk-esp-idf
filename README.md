# iotmer-sdk-esp-idf

Official ESP-IDF SDK for [IOTMER](https://iotmer.com).

Connects Espressif chips via Wi‑Fi, HTTPS provisioning, NVS credentials, MQTT (TLS), optional HTTPS OTA, and optional BLE JSON (`iotmer_ble`).

Public API: [`components/iotmer/include/iotmer_client.h`](components/iotmer/include/iotmer_client.h)

## Install

```yaml
# idf_component.yml
dependencies:
  iotmertech/iotmer: "0.3.5"
```

```bash
idf.py update-dependencies
```

| Package | Registry |
|---------|----------|
| Core SDK | [`iotmertech/iotmer`](https://components.espressif.com/components/iotmertech/iotmer) |
| BLE JSON (optional) | [`iotmertech/iotmer_ble`](https://components.espressif.com/components/iotmertech/iotmer_ble) |

**Requires:** ESP-IDF ≥ 6.0 and &lt; 6.2 · ESP32 family · TLS via `esp_crt_bundle_attach`

`0.3.5` always solves `espressif/cjson` `1.7.19~2`, `espressif/mqtt` `1.0.0`, and `espressif/zlib` `1.3.2`. ESP-IDF is the copy you install inside that range; it is not downloaded with the component. Optional BLE: `iotmertech/iotmer_ble: "0.3.4"`.

## Minimal example

```c
#include "iotmer_client.h"

iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
iotmer_client_t client;

iotmer_init(&client, &cfg);
iotmer_connect(&client);
iotmer_subscribe_commands(&client, on_command, NULL);
```

## Custom hardware (your PCB)

No IOTMER reference board is required. Scaffold a project with separated `hw/` and `cloud/` layers:

```bash
python tools/iotmer.py create-app my-gateway --chip esp32s3 --profile field
python tools/iotmer.py doctor --project my-gateway
```

See [`tools/README.md`](tools/README.md) and [`docs/sdk/esp-idf/custom-hardware.md`](docs/sdk/esp-idf/custom-hardware.md).

## Repository layout

| Path | Contents |
|------|----------|
| [`tools/`](tools/README.md) | `create-app` scaffold, `doctor` sdkconfig checks, `ci/clean.sh` |
| [`examples/`](examples/README.md) | Reference firmware — **build only inside an example directory** |
| [`components/iotmer/`](components/iotmer/README.md) | Core component |
| [`components/iotmer_ble/`](components/iotmer_ble/README.md) | Optional BLE transport |
| [`docs/`](docs/) | Markdown source for [docs.iotmer.com](https://docs.iotmer.com/) |
| [`CHANGELOG.md`](CHANGELOG.md) | Release notes |
| [`AGENTS.md`](AGENTS.md) | Contributor and agent guide |

CI: [`.github/workflows/ci.yml`](.github/workflows/ci.yml) — `iotmer doctor`, example build matrix, scaffold smoke build.

## Documentation

- **Platform** (console, MQTT, REST): [docs.iotmer.com](https://docs.iotmer.com/)
- **ESP-IDF integration**: [docs.iotmer.com/docs/sdk/esp-idf/](https://docs.iotmer.com/docs/sdk/esp-idf/)

## License

MIT — [LICENSE](LICENSE)
