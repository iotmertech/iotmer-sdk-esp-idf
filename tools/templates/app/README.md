# {{PROJECT_TITLE}}

Custom-hardware IOTMER firmware (profile: **{{PROFILE}}**, target: **{{CHIP}}**).

Scaffolded with `iotmer create-app`. You own all hardware code under `components/hw/`; cloud
connectivity lives in `components/cloud/`.

## Layout

| Path | Your responsibility |
|------|---------------------|
| `components/hw/` | Pins, sensors, RS485, LEDs, `hw_init()` |
| `components/cloud/` | IOTMER init, MQTT, telemetry (edit sparingly) |
| `main/` | Boot order: NVS → hw → cloud |
| `sdkconfig.defaults` | Active profile (field or factory) |

## Profiles

| Profile | When | `IOTMER_PROVISION_AUTH_CODE` |
|---------|------|------------------------------|
| **field** | Devices already provisioned; deployed to site | Empty |
| **factory** | First flash on production line | Set in menuconfig |

Switch profile:

```bash
cp sdkconfig.defaults.field sdkconfig.defaults    # field
cp sdkconfig.defaults.factory sdkconfig.defaults  # factory
idf.py fullclean build
```

See [Factory vs field](../../../docs/sdk/esp-idf/factory-field-profiles.md) in the SDK repo.

## Build

```bash
idf.py set-target {{CHIP}}
idf.py menuconfig    # Component config → IOTMER, HW
idf.py build flash monitor
```

Validate configuration:

```bash
python /path/to/iotmer-sdk-esp-idf/tools/iotmer.py doctor --project .
```

## Configure IOTMER

`idf.py menuconfig` → **Component config → IOTMER**

| Setting | Field | Factory |
|---------|-------|---------|
| `IOTMER_PROVISION_AUTH_CODE` | Empty | API key |
| `IOTMER_WORKSPACE_ID` | Ignored | Workspace UUID |
| `IOTMER_WIFI_SSID` / `PASSWORD` | Required (or NVS / BLE) | Required |

Docs: [docs.iotmer.com](https://docs.iotmer.com/)
