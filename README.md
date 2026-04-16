# iotmer-sdk-esp-idf

Official ESP-IDF component to connect Espressif chips to **[IOTMER](https://iotmer.com)** — Wi‑Fi, HTTPS device provisioning, NVS credentials, MQTT (TLS), optional HTTPS OTA. Public API: [`components/iotmer/include/iotmer_client.h`](components/iotmer/include/iotmer_client.h).

- **Documentation** (console, workspaces, devices, MQTT, REST API): [https://docs.iotmer.com/](https://docs.iotmer.com/)
- **ESP-IDF SDK guide** (introduction): [https://docs.iotmer.com/docs/sdk/esp-idf/intro](https://docs.iotmer.com/docs/sdk/esp-idf/intro)

This repository adds ESP-IDF–specific build notes, Kconfig, and `examples/` alongside that documentation.

```c
#include "iotmer_client.h"

iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
iotmer_client_t client;

iotmer_init(&client, &cfg);
iotmer_connect(&client);
iotmer_subscribe_commands(&client, on_command, NULL);
/* … iotmer_telemetry_publish(&client, "{...}"); */
```

## Requirements

| Item        | Notes                                      |
|-------------|--------------------------------------------|
| ESP-IDF     | ≥ 5.x (6.x supported; managed `cjson` / `mqtt` in `components/iotmer/idf_component.yml`) |
| Targets     | ESP32 family                               |
| TLS         | Uses `esp_crt_bundle_attach`; HTTPS endpoints must present a complete certificate chain (platform / edge configuration). |

## Installation

**Component Manager** — in `idf_component.yml`:

```yaml
dependencies:
  iotmertech/iotmer-sdk-esp-idf: "*"
```

```bash
idf.py update-dependencies
```

**Submodule:**

```bash
git submodule add https://github.com/iotmertech/iotmer-sdk-esp-idf components/iotmer-sdk-esp-idf
```

In the project `CMakeLists.txt`, before `project()`:

```cmake
set(EXTRA_COMPONENT_DIRS components/iotmer-sdk-esp-idf/components)
```

## Examples & local docs

- **[examples/README.md](examples/README.md)** — build, flash, `menuconfig`, troubleshooting; reference apps under `examples/` (grows over time; includes `01_provisioning`, `02_telemetry`, …).
- **`docs/`** — English Markdown scaffold for copying into a [Docusaurus](https://docusaurus.io/) site (see [docs/README.md](docs/README.md)).

## License

MIT — [LICENSE](LICENSE).
