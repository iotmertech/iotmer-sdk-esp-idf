#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""IOTMER ESP-IDF developer utilities (scaffold + sdkconfig doctor)."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
SDK_ROOT = SCRIPT_DIR.parent
TEMPLATE_ROOT = SCRIPT_DIR / "templates" / "app"

SUPPORTED_CHIPS = (
    "esp32",
    "esp32s2",
    "esp32s3",
    "esp32c3",
    "esp32c6",
    "esp32h2",
    "esp32p4",
)

PROFILE_DEFAULTS = {
    "field": "sdkconfig.defaults.field",
    "factory": "sdkconfig.defaults.factory",
}


@dataclass
class CheckResult:
    level: str  # ok | warn | error
    code: str
    message: str
    hint: str = ""


def _sanitize_project_name(name: str) -> str:
    cleaned = re.sub(r"[^a-zA-Z0-9_-]+", "_", name.strip())
    cleaned = re.sub(r"_+", "_", cleaned).strip("_").lower()
    if not cleaned:
        raise ValueError("project name is empty after sanitization")
    if cleaned[0].isdigit():
        cleaned = f"app_{cleaned}"
    return cleaned


def _render(text: str, mapping: Dict[str, str]) -> str:
    for key, value in mapping.items():
        text = text.replace(f"{{{{{key}}}}}", value)
    if "{{" in text:
        leftover = re.findall(r"\{\{[A-Z0-9_]+\}\}", text)
        if leftover:
            raise ValueError(f"unresolved template placeholders: {', '.join(sorted(set(leftover)))}")
    return text


def _copy_template_tree(
    dest: Path,
    mapping: Dict[str, str],
    profile: str,
) -> None:
    if not TEMPLATE_ROOT.is_dir():
        raise FileNotFoundError(f"template directory not found: {TEMPLATE_ROOT}")

    for src in sorted(TEMPLATE_ROOT.rglob("*")):
        rel = src.relative_to(TEMPLATE_ROOT)
        if rel.parts and rel.parts[0].startswith("sdkconfig.defaults."):
            continue

        out = dest / rel
        if src.is_dir():
            out.mkdir(parents=True, exist_ok=True)
            continue

        out.parent.mkdir(parents=True, exist_ok=True)
        if src.suffix in {".csv", ".lock"}:
            shutil.copy2(src, out)
            continue

        content = src.read_text(encoding="utf-8")
        out.write_text(_render(content, mapping), encoding="utf-8")

    profile_file = TEMPLATE_ROOT / PROFILE_DEFAULTS[profile]
    if not profile_file.is_file():
        raise FileNotFoundError(f"profile defaults missing: {profile_file}")
    (dest / "sdkconfig.defaults").write_text(profile_file.read_text(encoding="utf-8"), encoding="utf-8")

    other = "factory" if profile == "field" else "field"
    shutil.copy2(TEMPLATE_ROOT / PROFILE_DEFAULTS[other], dest / PROFILE_DEFAULTS[other])


def cmd_create_app(args: argparse.Namespace) -> int:
    name = _sanitize_project_name(args.name)
    dest = Path(args.output).expanduser().resolve() / name
    profile = args.profile
    chip = args.chip

    if dest.exists():
        print(f"error: destination already exists: {dest}", file=sys.stderr)
        return 1

    if args.deps == "local":
        sdk_root = Path(args.sdk_root).expanduser().resolve()
        components = sdk_root / "components"
        if not (components / "iotmer").is_dir():
            print(f"error: local SDK components not found under {components}", file=sys.stderr)
            return 1
        rel = os.path.relpath(components, dest)
        extra_cmake = f'set(EXTRA_COMPONENT_DIRS "${{CMAKE_CURRENT_LIST_DIR}}/{rel}")'
        component_yml = (
            "# iotmer is resolved via EXTRA_COMPONENT_DIRS in CMakeLists.txt (local SDK checkout).\n"
            "dependencies:\n"
            "  idf: \">=5.0.0\"\n"
        )
    else:
        extra_cmake = ""
        component_yml = (
            "dependencies:\n"
            "  idf: \">=5.0.0\"\n"
            "  iotmertech/iotmer: \"*\"\n"
        )

    mapping = {
        "PROJECT_NAME": name,
        "PROJECT_TITLE": args.name.strip(),
        "CHIP": chip,
        "PROFILE": profile,
        "PROFILE_UPPER": profile.upper(),
        "EXTRA_COMPONENT_DIRS": extra_cmake,
        "IDF_COMPONENT_YML": component_yml,
        "SDK_ROOT_HINT": str(SDK_ROOT),
    }

    try:
        dest.mkdir(parents=True)
        _copy_template_tree(dest, mapping, profile)
    except (OSError, ValueError) as exc:
        shutil.rmtree(dest, ignore_errors=True)
        print(f"error: scaffold failed: {exc}", file=sys.stderr)
        return 1

    print(f"Created IOTMER app: {dest}")
    print(f"  profile : {profile}")
    print(f"  chip    : {chip} (run idf.py set-target {chip} inside the project)")
    print(f"  deps    : {args.deps}")
    print()
    print("Next steps:")
    print(f"  cd {dest}")
    if not os.environ.get("IDF_PATH"):
        print("  source $IDF_PATH/export.sh   # or your ESP-IDF export script")
    print(f"  idf.py set-target {chip}")
    print("  idf.py menuconfig            # Component config → IOTMER + HW")
    print("  idf.py build flash monitor")
    print(f"  python {SCRIPT_DIR / 'iotmer.py'} doctor --project {dest}")
    return 0


def _parse_kconfig_assignments(paths: Iterable[Path]) -> Dict[str, str]:
    values: Dict[str, str] = {}
    assign_re = re.compile(
        r"^CONFIG_([A-Z0-9_]+)=(?:y|(\"(?:[^\"\\]|\\.)*\")|(-?\d+))$"
    )
    for path in paths:
        if not path.is_file():
            continue
        for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = assign_re.match(line)
            if not m:
                continue
            key = m.group(1)
            if m.group(2) is not None:
                values[key] = bytes(m.group(2), "utf-8").decode("unicode_escape").strip('"')
            elif m.group(3) is not None:
                values[key] = m.group(3)
            else:
                values[key] = "y"
    return values


def _is_enabled(values: Dict[str, str], key: str) -> bool:
    return values.get(key) == "y"


def _int_value(values: Dict[str, str], key: str, default: int = 0) -> int:
    raw = values.get(key)
    if raw is None or raw == "y":
        return default
    try:
        return int(raw, 0)
    except ValueError:
        return default


def _detect_profile(values: Dict[str, str]) -> str:
    if values.get("IOTMER_PROVISION_AUTH_CODE", "").strip():
        return "factory"
    if _is_enabled(values, "IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA"):
        return "factory"
    return "field"


def _project_has_iotmer(project: Path) -> Tuple[bool, str]:
    yml = project / "idf_component.yml"
    if yml.is_file() and "iotmertech/iotmer" in yml.read_text(encoding="utf-8"):
        return True, "idf_component.yml → iotmertech/iotmer"
    cmake = project / "CMakeLists.txt"
    if cmake.is_file():
        text = cmake.read_text(encoding="utf-8")
        if "EXTRA_COMPONENT_DIRS" in text:
            if "iotmer" in text or "/components" in text or "components" in text:
                return True, "CMakeLists.txt → EXTRA_COMPONENT_DIRS (local components)"
            return True, "CMakeLists.txt → EXTRA_COMPONENT_DIRS"
    return False, ""


def _collect_doctor_checks(project: Path) -> List[CheckResult]:
    results: List[CheckResult] = []

    if not os.environ.get("IDF_PATH"):
        results.append(CheckResult(
            "warn", "idf_path",
            "IDF_PATH is not set in this shell.",
            "Source ESP-IDF export.sh before build; doctor can still check sdkconfig files.",
        ))
    else:
        results.append(CheckResult("ok", "idf_path", f"IDF_PATH={os.environ['IDF_PATH']}"))

    if not (project / "CMakeLists.txt").is_file():
        results.append(CheckResult(
            "error", "cmake",
            "CMakeLists.txt not found — not an ESP-IDF project root?",
            f"Run create-app or cd into your firmware directory.",
        ))
        return results

    defaults = project / "sdkconfig.defaults"
    sdkconfig = project / "sdkconfig"

    if not defaults.is_file():
        results.append(CheckResult(
            "warn", "sdkconfig_defaults",
            "sdkconfig.defaults is missing.",
            "Copy from tools/templates/app or examples/02_telemetry/sdkconfig.defaults.",
        ))
    else:
        results.append(CheckResult("ok", "sdkconfig_defaults", f"Found {defaults.name}"))

    values: Dict[str, str] = {}
    if defaults.is_file():
        values.update(_parse_kconfig_assignments([defaults]))
    if sdkconfig.is_file():
        values.update(_parse_kconfig_assignments([sdkconfig]))
    if sdkconfig.is_file():
        results.append(CheckResult("ok", "sdkconfig", "sdkconfig present (merged values include overrides)."))
    profile = _detect_profile(values)
    results.append(CheckResult(
        "ok", "profile_guess",
        f"Detected firmware profile: {profile}",
        "factory → provision auth code set or OTA-even-if-same-SHA enabled; field → empty auth code.",
    ))

    has_iotmer, how = _project_has_iotmer(project)
    if has_iotmer:
        results.append(CheckResult("ok", "iotmer_dep", f"iotmer dependency: {how}"))
    else:
        results.append(CheckResult(
            "error", "iotmer_dep",
            "iotmer dependency not detected.",
            "Add iotmertech/iotmer to idf_component.yml or EXTRA_COMPONENT_DIRS for a local SDK checkout.",
        ))

    if _is_enabled(values, "MBEDTLS_CERTIFICATE_BUNDLE"):
        results.append(CheckResult("ok", "tls_bundle", "MBEDTLS_CERTIFICATE_BUNDLE is enabled."))
    else:
        results.append(CheckResult(
            "error", "tls_bundle",
            "MBEDTLS_CERTIFICATE_BUNDLE is not enabled.",
            "Required for HTTPS provision and MQTT TLS (see examples/02_telemetry/sdkconfig.defaults).",
        ))

    main_stack = _int_value(values, "ESP_MAIN_TASK_STACK_SIZE", 3584)
    if main_stack >= 8192:
        results.append(CheckResult("ok", "main_stack", f"ESP_MAIN_TASK_STACK_SIZE={main_stack}"))
    else:
        results.append(CheckResult(
            "error", "main_stack",
            f"ESP_MAIN_TASK_STACK_SIZE={main_stack} (recommended ≥ 8192).",
            "TLS handshakes run on the main task during iotmer_init(); increase stack in sdkconfig.defaults.",
        ))

    if _is_enabled(values, "IOTMER_AUTO_OTA") or profile == "factory":
        ota_custom = _is_enabled(values, "PARTITION_TABLE_CUSTOM")
        ota_two = _is_enabled(values, "PARTITION_TABLE_TWO_OTA")
        if ota_custom or ota_two:
            part = values.get("PARTITION_TABLE_CUSTOM_FILENAME", "built-in two_ota")
            results.append(CheckResult(
                "ok", "ota_partition",
                f"OTA partition layout configured ({'custom' if ota_custom else 'two_ota'}).",
                part if ota_custom else "",
            ))
        else:
            results.append(CheckResult(
                "warn", "ota_partition",
                "OTA is enabled but no dual-slot partition table detected.",
                "Use CONFIG_PARTITION_TABLE_TWO_OTA or a custom csv with ota_0/ota_1 (see partitions_ota_4mb.csv).",
            ))

        flash_ok = any(
            _is_enabled(values, key)
            for key in (
                "ESPTOOLPY_FLASHSIZE_4MB",
                "ESPTOOLPY_FLASHSIZE_8MB",
                "ESPTOOLPY_FLASHSIZE_16MB",
                "ESPTOOLPY_FLASHSIZE_32MB",
                "ESPTOOLPY_FLASHSIZE_64MB",
                "ESPTOOLPY_FLASHSIZE_128MB",
            )
        )
        if flash_ok:
            results.append(CheckResult("ok", "flash_size", "Flash size symbol set in sdkconfig."))
        else:
            results.append(CheckResult(
                "warn", "flash_size",
                "Flash size not explicitly set to ≥ 4MB.",
                "Dual OTA slots typically need CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y or larger.",
            ))

    auth = values.get("IOTMER_PROVISION_AUTH_CODE", "")
    workspace = values.get("IOTMER_WORKSPACE_ID", "")
    if profile == "factory":
        if auth.strip():
            results.append(CheckResult("ok", "factory_auth", "Factory profile: provision auth code is set."))
        else:
            results.append(CheckResult(
                "warn", "factory_auth",
                "Factory profile expected but IOTMER_PROVISION_AUTH_CODE is empty.",
                "Set via menuconfig for the first HTTPS provision on the production line.",
            ))
        if not workspace.strip():
            results.append(CheckResult(
                "warn", "factory_workspace",
                "IOTMER_WORKSPACE_ID is empty (required when auth code is set).",
                "Set workspace UUID in menuconfig for factory images.",
            ))
    else:
        if auth.strip():
            results.append(CheckResult(
                "warn", "field_auth",
                "Field profile: IOTMER_PROVISION_AUTH_CODE should be empty.",
                "Auth code belongs in factory firmware only; field devices use NVS session.",
            ))
        else:
            results.append(CheckResult("ok", "field_auth", "Field profile: provision auth code is empty (NVS session)."))

    wifi_ssid = values.get("IOTMER_WIFI_SSID", "")
    if not wifi_ssid.strip():
        results.append(CheckResult(
            "warn", "wifi_ssid",
            "IOTMER_WIFI_SSID is empty.",
            "Set in menuconfig, NVS, or BLE JSON (05_ble_json) before deployment.",
        ))
    else:
        results.append(CheckResult("ok", "wifi_ssid", "Wi-Fi SSID configured."))

    hw_dir = project / "components" / "hw"
    cloud_dir = project / "components" / "cloud"
    if hw_dir.is_dir() and cloud_dir.is_dir():
        results.append(CheckResult("ok", "layout", "hw/ + cloud/ components present (custom hardware scaffold)."))
    else:
        results.append(CheckResult(
            "warn", "layout",
            "Project does not use hw/ + cloud/ split.",
            "Optional — see docs/sdk/esp-idf/custom-hardware.md; examples/ work without it.",
        ))

    return results


def _print_results(results: List[CheckResult]) -> int:
    icons = {"ok": "✓", "warn": "!", "error": "✗"}
    errors = 0
    warns = 0
    for item in results:
        if item.level == "error":
            errors += 1
        elif item.level == "warn":
            warns += 1
        print(f"{icons.get(item.level, '?')} [{item.code}] {item.message}")
        if item.hint:
            print(f"    → {item.hint}")
    print()
    print(f"Summary: {errors} error(s), {warns} warning(s), {len(results) - errors - warns} ok")
    return 1 if errors else 0


def cmd_doctor(args: argparse.Namespace) -> int:
    project = Path(args.project).expanduser().resolve()
    if not project.is_dir():
        print(f"error: not a directory: {project}", file=sys.stderr)
        return 1
    print(f"Checking IOTMER project: {project}\n")
    return _print_results(_collect_doctor_checks(project))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="iotmer",
        description="IOTMER ESP-IDF developer tools (scaffold custom hardware projects, validate sdkconfig).",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    create = sub.add_parser(
        "create-app",
        help="Scaffold a new ESP-IDF project for custom hardware (hw/ + cloud/ layout).",
    )
    create.add_argument("name", help="Project name (e.g. my-gateway)")
    create.add_argument(
        "-o", "--output",
        default=".",
        help="Output parent directory (default: current directory)",
    )
    create.add_argument(
        "--chip",
        default="esp32c3",
        choices=SUPPORTED_CHIPS,
        help="ESP-IDF target chip (default: esp32c3)",
    )
    create.add_argument(
        "--profile",
        default="field",
        choices=sorted(PROFILE_DEFAULTS),
        help="Firmware profile: field (NVS session) or factory (HTTPS provision)",
    )
    create.add_argument(
        "--deps",
        default="registry",
        choices=("registry", "local"),
        help="iotmer source: ESP Component Registry or local SDK checkout",
    )
    create.add_argument(
        "--sdk-root",
        default=str(SDK_ROOT),
        help="SDK repo root for --deps local (default: parent of tools/)",
    )
    create.set_defaults(func=cmd_create_app)

    doctor = sub.add_parser(
        "doctor",
        help="Validate sdkconfig defaults and common IOTMER integration mistakes.",
    )
    doctor.add_argument(
        "--project",
        default=".",
        help="ESP-IDF project directory (default: current directory)",
    )
    doctor.set_defaults(func=cmd_doctor)

    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
