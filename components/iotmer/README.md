# iotmer (ESP-IDF component)

IOTMER IoT Platform SDK for ESP-IDF: Wi‑Fi provisioning, NVS credentials, MQTT (TLS), telemetry, presence (LWT), optional HTTPS OTA.

## GitHub

- Repo: `https://github.com/iotmertech/iotmer-sdk-esp-idf`
- Issues: `https://github.com/iotmertech/iotmer-sdk-esp-idf/issues`

## Requirements

- ESP-IDF: >= 5.0
- Targets: ESP32 family (see `idf_component.yml`)

## Installation (ESP Component Manager)

Add to your project's `idf_component.yml`:

```yaml
dependencies:
  iotmertech/iotmer: "*"
```

Then:

```bash
idf.py update-dependencies
```

## Usage

Minimal bring-up (NVS + init + connect):

```c
#include "nvs_flash.h"
#include "iotmer_client.h"

void app_main(void)
{
    (void)nvs_flash_init();

    iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
    iotmer_client_t client;

    (void)iotmer_init(&client, &cfg);
    (void)iotmer_connect(&client);
}
```

Telemetry publish (after `iotmer_connect()` succeeds and `client.connected` is true):

```c
(void)iotmer_telemetry_publish(&client, "{\"temp\": 22.5}");
```

Commands subscription (callback signature matches `iotmer_client.h`):

```c
static void on_command(const char *topic, const char *payload, int len, void *ctx)
{
    (void)ctx;
    /* handle payload */
}

(void)iotmer_subscribe_commands(&client, on_command, NULL);
```

## Docs

- Project docs: `https://docs.iotmer.com/`
- ESP-IDF SDK guide: `https://docs.iotmer.com/docs/sdk/esp-idf/`

## Examples

Bundled ESP-IDF examples (also indexed by the ESP Component Registry **Examples** tab):

- `examples/01_provisioning`
- `examples/02_telemetry`
- `examples/03_lwt_presence`
- `examples/04_config`

Canonical copies in the main repository:

- All examples: `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples`
- Provisioning: `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/01_provisioning`
- Telemetry: `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/02_telemetry`
- LWT / presence: `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/03_lwt_presence`
- Config: `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/04_config`

