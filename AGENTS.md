# Guide for AI agents and automation

This file helps **AI coding agents** (Cursor, Copilot, and similar) and humans who clone the repo to establish context quickly and make safe, consistent changes.

## What this repository does

- **Official ESP-IDF component** connecting Espressif chips to the **[IOTMER](https://www.iotmer.com)** cloud.
- **High-level flow**: Wi‑Fi, HTTPS device provisioning, credentials in NVS, MQTT (TLS), optional HTTPS OTA, optional **BLE Wi‑Fi credential provisioning** (NimBLE, `iotmer_ble_wifi_prov`).
- **Public application API**: `components/iotmer/include/iotmer_client.h` (headers applications should include).
- **Optional BLE provisioning API**: `components/iotmer_ble_wifi_prov/include/iotmer_ble_wifi_prov.h` (separate component; not included unless the app adds it).
- **MQTT Config Protocol v1** (device side): `components/iotmer/include/iotmer_config.h` and `iotmer_config.c`.

For authoritative behaviour details, use **[docs.iotmer.com](https://docs.iotmer.com/)** together with the `docs/sdk/esp-idf/` scaffold in this repo.

## Read first (priority order)

| Order | Path | Notes |
|-------|------|-------|
| 1 | [`README.md`](README.md) | Install, Component Manager, `EXTRA_COMPONENT_DIRS` |
| 2 | [`components/iotmer/README.md`](components/iotmer/README.md) | Component overview, example index |
| 3 | [`examples/README.md`](examples/README.md) | `idf.py`, `menuconfig`, directory → role table, troubleshooting |
| 4 | [`components/iotmer/Kconfig.projbuild`](components/iotmer/Kconfig.projbuild) | Core `CONFIG_IOTMER_*` symbols |
| 5 | [`components/iotmer_ble_wifi_prov/Kconfig.projbuild`](components/iotmer_ble_wifi_prov/Kconfig.projbuild) | BLE provisioning `CONFIG_IOTMER_BLE_*` (when component is in the project) |
| 6 | [`docs/sdk/esp-idf/ble-wifi-provisioning.md`](docs/sdk/esp-idf/ble-wifi-provisioning.md) | BLE GATT v1 + Claim `…0404…`; bind-claim + `device_http_token`: [`provisioning.md`](docs/sdk/esp-idf/provisioning.md) |

When changing code, cross-check the relevant example’s `sdkconfig.defaults`, the “SDK layout” section in `examples/README.md`, and signatures in `iotmer_client.h`.

## Directory map (short)

- `components/iotmer/` — shipping component: `*.c`, `include/`, `CMakeLists.txt`, `idf_component.yml`, Kconfig.
- `components/iotmer_ble_wifi_prov/` — optional BLE Wi‑Fi provisioning (NimBLE); `README.md`, `Kconfig.projbuild`, public `iotmer_ble_wifi_prov.h`.
- `examples/*` — reference apps; treat each as its own ESP-IDF project root.
- `docs/` — English Markdown scaffold; keep aligned with the published site **[docs.iotmer.com](https://docs.iotmer.com/)** when behaviour is documented there.

## Change principles (for agents)

1. **Public API surface**  
   When adding functions or types for applications, define the contract in `iotmer_client.h` / `iotmer_config.h` first. Keep internal helpers behind `iotmer_internal.h`; applications must not depend on that header (as in the examples).

2. **Kconfig and backward compatibility**  
   New `CONFIG_IOTMER_*` entries should default so existing examples keep working. Keep `Kconfig.projbuild` and example `sdkconfig.defaults` in sync.

3. **Examples**  
   If behaviour changes, update at least one `examples/` app where the feature is exercised (provisioning / MQTT / config protocol differ across `01` … `04`).

4. **Documentation**  
   If user-visible behaviour or configuration changes, update the relevant page under `docs/sdk/esp-idf/` and, when needed, `components/iotmer/README.md` or the root `README.md`.

5. **Secrets and safety**  
   Never commit real auth codes, passwords, or production certificates. Secrets in `sdkconfig` are excluded via `.gitignore`; examples should use `sdkconfig.defaults` or placeholders only.

6. **TLS and certificates**  
   Keep `esp_crt_bundle_attach` usage and certificate-chain expectations consistent with the root `README.md` and example `sdkconfig.defaults`. For TLS failures, consult the troubleshooting table in `examples/README.md` first.

7. **Scope**  
   Avoid drive-by refactors outside the requested task; match existing style (naming, error codes, logging).

## Build and verification

- When practical, verify in the affected example directory with `idf.py set-target <chip>` and `idf.py build` (ESP-IDF environment must be sourced via `export` / `export.ps1`).
- Per-example `dependencies.lock` should stay consistent with `managed_components` if you bump managed dependency versions—update the lock file accordingly.

## License

Project is **MIT** — see `LICENSE`.
