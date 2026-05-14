#!/usr/bin/env python3
"""Build helper for the ESP virtual printer firmware.

Generates include/app_config.generated.h from build settings, creates a shared
TLS certificate/key for the selected build, then runs native ESP-IDF once per
selected printer model. The checked-in C headers remain generic defaults.
"""

from __future__ import annotations

import argparse
import getpass
import json
import shutil
import string
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
GENERATED_HEADER = ROOT / "include" / "app_config.generated.h"
GENERATED_DIR = ROOT / ".generated"
CERT_DIR = GENERATED_DIR / "certs"
BUILD_ROOT = ROOT / "build"
OUT_DIR = ROOT / "out"
DEFAULT_BAMBUDDY_URL = "http://192.168.1.127:8000"
DEFAULT_SERIAL_SUFFIX = "391800002"
DEFAULT_CERT_IP = "192.168.1.60"
DEFAULT_IDF_TARGET = "esp32s3"

MODELS: dict[str, tuple[str, str, str]] = {
    "BL-P001": ("X1C", "X1 Carbon", "00M00A"),
    "BL-P002": ("X1", "X1", "00M00A"),
    "C13": ("X1E", "X1E", "03W00A"),
    "N6": ("X2D", "X2D", "20P90A"),
    "C11": ("P1P", "P1P", "01S00A"),
    "C12": ("P1S", "P1S", "01P00A"),
    "N7": ("P2S", "P2S", "22E00A"),
    "N2S": ("A1", "A1", "03900A"),
    "N1": ("A1 Mini", "A1 mini", "03000A"),
    "O1D": ("H2D", "H2D", "09400A"),
    "O1C": ("H2C", "H2C", "09400A"),
    "O1C2": ("H2C Dual", "H2C", "09400A"),
    "O1S": ("H2S", "H2S", "09400A"),
}


def c_string(value: object) -> str:
    return json.dumps(str(value))


def slug(value: str) -> str:
    allowed = string.ascii_lowercase + string.digits + "-"
    clean = []
    last_dash = False
    for char in value.lower():
        if char.isalnum():
            clean.append(char)
            last_dash = False
        elif not last_dash:
            clean.append("-")
            last_dash = True
    result = "".join(clean).strip("-")
    return "".join(ch for ch in result if ch in allowed) or "printer"


def normalize_model(value: str) -> str:
    needle = value.strip().lower()
    for code, (display, _product, _prefix) in MODELS.items():
        if needle in {code.lower(), display.lower()}:
            return code
    raise argparse.ArgumentTypeError(f"unknown model {value!r}; run --list-models")


def list_models() -> None:
    for code, (display, product, prefix) in MODELS.items():
        print(f"{code:8} {display:10} product={product!r} serial_prefix={prefix}")


def split_csv(values: list[str] | None) -> list[str]:
    result: list[str] = []
    for value in values or []:
        for item in value.split(","):
            item = item.strip()
            if item:
                result.append(item)
    return result


def optional_path(value: str | None) -> Path | None:
    if not value:
        return None
    return Path(value).expanduser().resolve()


def normalize_models(values: list[str] | None) -> list[str]:
    models: list[str] = []
    for value in split_csv(values):
        code = normalize_model(value)
        if code not in models:
            models.append(code)
    return models


def select_models(default: list[str]) -> list[str]:
    if not sys.stdin.isatty():
        return default

    print("Select virtual printer model(s):")
    items = list(MODELS.items())
    for idx, (code, (display, _product, _prefix)) in enumerate(items, start=1):
        marker = " (default)" if code in default else ""
        print(f"  {idx:2}. {display:10} {code}{marker}")

    raw = input("Models, comma separated numbers/codes/names [default]: ").strip()
    if not raw:
        return default

    selected: list[str] = []
    for token in [part.strip() for part in raw.split(",") if part.strip()]:
        if token.lower() == "all":
            return [code for code, _model in items]
        if token.isdigit():
            index = int(token)
            if 1 <= index <= len(items):
                code = items[index - 1][0]
            else:
                raise SystemExit(f"model index out of range: {token}")
        else:
            code = normalize_model(token)
        if code not in selected:
            selected.append(code)
    return selected or default


def prompt_text(label: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    value = input(f"{label}{suffix}: ").strip()
    return value or default


def prompt_secret(label: str, default: str = "") -> str:
    suffix = " [set]" if default else ""
    value = getpass.getpass(f"{label}{suffix}: ")
    return value or default


def prompt_access_code(default: str) -> str:
    while True:
        value = prompt_text("Access code", default)
        try:
            return validate_access_code(value)
        except argparse.ArgumentTypeError as exc:
            print(f"  {exc}")


def prompt_serial_suffix(default: str | None = None) -> str:
    while True:
        value = prompt_text("Serial suffix, 9 chars", default or DEFAULT_SERIAL_SUFFIX)
        if not value:
            return default or DEFAULT_SERIAL_SUFFIX
        if len(value) == 9:
            return value.upper()
        print("  serial suffix must be exactly 9 characters")


def prompt_interactive(args: argparse.Namespace) -> None:
    if not sys.stdin.isatty():
        return

    print()
    print("Bambuddy ESP Virtual Printer Builder")
    print("------------------------------------")
    print("Press Enter to accept defaults.")
    print()

    if not args.model and not args.models:
        args.models = select_models(["BL-P001"])
    requested_models = split_csv(args.models) + split_csv(args.model)
    if any(value.lower() == "all" for value in requested_models):
        selected_models = list(MODELS.keys())
    else:
        selected_models = normalize_models(requested_models)
    display = MODELS[selected_models[0]][0] if selected_models else MODELS["BL-P001"][0]
    if args.name is None:
        args.name = prompt_text("Printer name prefix", f"Bambuddy {display} VP")
    if args.wifi_ssid is None:
        args.wifi_ssid = prompt_text("Wi-Fi SSID")
    if args.wifi_password is None:
        args.wifi_password = prompt_secret("Wi-Fi password")
    if args.bambuddy_url is None:
        args.bambuddy_url = prompt_text("Bambuddy URL", DEFAULT_BAMBUDDY_URL)
    if args.api_key is None:
        args.api_key = prompt_secret("Bambuddy API key (optional)")
    if args.access_code is None:
        args.access_code = prompt_access_code("12345678")
    if args.serial is None and args.serial_suffix is None:
        args.serial_suffix = prompt_serial_suffix(DEFAULT_SERIAL_SUFFIX)


def validate_access_code(value: str) -> str:
    if len(value) != 8:
        raise argparse.ArgumentTypeError("access code must be exactly 8 characters")
    return value


def resolve_serial(model: str, serial: str | None, serial_suffix: str | None) -> tuple[str, str]:
    prefix = MODELS[model][2]
    if serial:
        serial = serial.strip().upper()
        if len(serial) < 7:
            raise SystemExit("--serial must be a full Bambu-style serial")
        return serial, serial[len(prefix) :] if serial.startswith(prefix) else serial[-9:]

    suffix = (serial_suffix or DEFAULT_SERIAL_SUFFIX).strip().upper()
    if len(suffix) != 9:
        raise SystemExit("--serial-suffix must be exactly 9 characters")
    return prefix + suffix, suffix


def run_openssl(
    cert_path: Path,
    key_path: Path,
    common_name: str,
    force: bool,
    dns_names: list[str] | None = None,
    ip_addresses: list[str] | None = None,
    ca_cert_source: Path | None = None,
    ca_key_source: Path | None = None,
) -> None:
    if cert_path.exists() and key_path.exists() and not force:
        return

    cert_path.parent.mkdir(parents=True, exist_ok=True)
    ca_dir = CERT_DIR / "ca"
    ca_dir.mkdir(parents=True, exist_ok=True)
    ca_key = ca_dir / "virtual_printer_ca.key"
    ca_cert = ca_dir / "virtual_printer_ca.crt"
    leaf_cert = cert_path.with_suffix(".leaf.crt")
    csr_path = cert_path.with_suffix(".csr")

    try:
        if ca_cert_source or ca_key_source:
            if not ca_cert_source or not ca_key_source:
                raise SystemExit("--ca-cert and --ca-key must be used together")
            if not ca_cert_source.exists():
                raise SystemExit(f"--ca-cert does not exist: {ca_cert_source}")
            if not ca_key_source.exists():
                raise SystemExit(f"--ca-key does not exist: {ca_key_source}")
            if ca_cert_source.resolve() != ca_cert.resolve():
                shutil.copy2(ca_cert_source, ca_cert)
            if ca_key_source.resolve() != ca_key.resolve():
                shutil.copy2(ca_key_source, ca_key)
            try:
                ca_key.chmod(0o600)
            except OSError:
                pass

        if not ca_cert_source and not ca_key_source and (not ca_key.exists() or not ca_cert.exists() or force):
            subprocess.run(
                [
                    "openssl",
                    "req",
                    "-x509",
                    "-newkey",
                    "rsa:2048",
                    "-nodes",
                    "-sha256",
                    "-days",
                    "7300",
                    "-subj",
                    "/CN=Virtual Printer CA",
                    "-keyout",
                    str(ca_key),
                    "-out",
                    str(ca_cert),
                    "-addext",
                    "basicConstraints=critical,CA:TRUE,pathlen:0",
                    "-addext",
                    "keyUsage=critical,keyCertSign,cRLSign",
                ],
                cwd=ROOT,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

        san_entries: list[str] = []
        for name in dns_names or []:
            if name and name not in san_entries:
                san_entries.append(f"DNS:{name}")
        for ip in ip_addresses or []:
            if ip:
                san_entries.append(f"IP:{ip}")
        if not san_entries:
            san_entries = [f"DNS:{common_name}"]

        ext_text = "\n".join(
            [
                "basicConstraints=critical,CA:FALSE",
                "keyUsage=critical,digitalSignature,keyEncipherment",
                "extendedKeyUsage=serverAuth,clientAuth",
                f"subjectAltName={','.join(san_entries)}",
                "",
            ]
        )
        with tempfile.NamedTemporaryFile("w", delete=False, dir=cert_path.parent, suffix=".ext") as ext_file:
            ext_file.write(ext_text)
            ext_path = Path(ext_file.name)

        subprocess.run(
            [
                "openssl",
                "req",
                "-new",
                "-newkey",
                "rsa:2048",
                "-nodes",
                "-sha256",
                "-subj",
                f"/CN={common_name}",
                "-keyout",
                str(key_path),
                "-out",
                str(csr_path),
            ],
            cwd=ROOT,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        subprocess.run(
            [
                "openssl",
                "x509",
                "-req",
                "-in",
                str(csr_path),
                "-CA",
                str(ca_cert),
                "-CAkey",
                str(ca_key),
                "-CAcreateserial",
                "-out",
                str(leaf_cert),
                "-days",
                "3650",
                "-sha256",
                "-extfile",
                str(ext_path),
            ],
            cwd=ROOT,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        cert_path.write_text(leaf_cert.read_text() + ca_cert.read_text())
        try:
            key_path.chmod(0o600)
            ca_key.chmod(0o600)
        except OSError:
            pass
    except subprocess.CalledProcessError:
        print("openssl failed while generating the VP certificate/key", file=sys.stderr)
        raise
    finally:
        for path_name in ("ext_path",):
            path = locals().get(path_name)
            if isinstance(path, Path):
                try:
                    path.unlink()
                except OSError:
                    pass
        for path in (csr_path, leaf_cert):
            try:
                path.unlink()
            except OSError:
                pass


def model_out_dir(model: str) -> Path:
    return OUT_DIR / slug(MODELS[model][0])


def find_existing_output_cert(models: list[str]) -> tuple[Path, Path] | None:
    for model in models:
        target_dir = model_out_dir(model)
        cert_path = target_dir / "printer.crt"
        key_path = target_dir / "printer.key"
        if cert_path.exists() and key_path.exists():
            return cert_path, key_path
    return None


def model_name(args: argparse.Namespace, model: str, multiple: bool) -> str:
    display = MODELS[model][0]
    if not args.name:
        return f"Bambuddy {display} VP"
    if multiple:
        return f"{args.name} {display}"
    return args.name


def write_generated_header(
    args: argparse.Namespace,
    model: str,
    serial: str,
    suffix: str,
    cert: str,
    key: str,
    multiple: bool,
) -> None:
    _display, product, _prefix = MODELS[model]
    name = model_name(args, model, multiple)
    lines = [
        "#pragma once",
        "",
        "/* Generated by esp-vp/build.py. Do not commit this file. */",
        f"#define APP_WIFI_SSID {c_string(args.wifi_ssid or '')}",
        f"#define APP_WIFI_PASSWORD {c_string(args.wifi_password or '')}",
        f"#define APP_BAMBUDDY_BASE_URL {c_string(args.bambuddy_url)}",
        f"#define APP_BAMBUDDY_API_KEY {c_string(args.api_key or '')}",
        f"#define APP_VP_NAME {c_string(name)}",
        f"#define APP_VP_MODEL_CODE {c_string(model)}",
        f"#define APP_VP_PRODUCT_NAME {c_string(product)}",
        f"#define APP_VP_ACCESS_CODE {c_string(args.access_code)}",
        f"#define APP_VP_SERIAL_SUFFIX {c_string(suffix)}",
        f"#define APP_VP_SERIAL {c_string(serial)}",
        f"#define APP_TLS_CERT_PEM {c_string(cert)}",
        f"#define APP_TLS_KEY_PEM {c_string(key)}",
        "",
    ]
    GENERATED_HEADER.write_text("\n".join(lines))


FLASH_ARTIFACTS = {
    ("bootloader", "bootloader.bin"): "bootloader.bin",
    ("partition_table", "partition-table.bin"): "partitions.bin",
    ("", "esp_vp.bin"): "firmware.bin",
}


def model_build_dir(model: str) -> Path:
    return BUILD_ROOT / slug(MODELS[model][0])


def copy_flash_artifacts_to_out(build_dir: Path, model: str, cert_path: Path, key_path: Path) -> Path:
    target_dir = model_out_dir(model)
    target_dir.mkdir(parents=True, exist_ok=True)

    for source_parts, target_name in FLASH_ARTIFACTS.items():
        source = build_dir.joinpath(*[part for part in source_parts if part])
        if not source.exists():
            raise FileNotFoundError(f"ESP-IDF did not produce {source}")
        shutil.copy2(source, target_dir / target_name)

    cert_target = target_dir / "printer.crt"
    key_target = target_dir / "printer.key"
    if cert_path.resolve() != cert_target.resolve():
        shutil.copy2(cert_path, cert_target)
    if key_path.resolve() != key_target.resolve():
        shutil.copy2(key_path, key_target)
    ca_candidates = [
        CERT_DIR / "ca" / "virtual_printer_ca.crt",
        cert_path.parent / "virtual_printer_ca.crt",
        cert_path.parent / "bbl_ca.crt",
    ]
    ca_cert = next((path for path in ca_candidates if path.exists()), None)
    if ca_cert:
        shutil.copy2(ca_cert, target_dir / "virtual_printer_ca.crt")
        shutil.copy2(ca_cert, target_dir / "bbl_ca.crt")
    return target_dir


def run_idf_build(args: argparse.Namespace, model: str) -> int:
    idf_py = shutil.which("idf.py")
    if idf_py is None:
        print(
            "idf.py was not found. Install/export ESP-IDF 6.0 first, then run this build helper again.",
            file=sys.stderr,
        )
        print("Example: source ~/esp/esp-idf/export.sh", file=sys.stderr)
        return 127

    build_dir = model_build_dir(model)
    build_dir.mkdir(parents=True, exist_ok=True)
    sdkconfig = build_dir / "sdkconfig"
    cmd = [
        idf_py,
        "-B",
        str(build_dir),
        "-D",
        f"SDKCONFIG={sdkconfig}",
        "-D",
        "SDKCONFIG_DEFAULTS=sdkconfig.defaults",
        "-D",
        f"IDF_TARGET={args.idf_target}",
        "build",
    ]
    result = subprocess.run(cmd, cwd=ROOT)
    return result.returncode


def flash_command_for(target_dir: Path) -> str:
    rel_dir = target_dir.relative_to(ROOT)
    return (
        "python3 -m esptool --chip esp32s3 --port <PORT> --baud 460800 "
        "write_flash -z "
        f"0x0 {rel_dir}/bootloader.bin "
        f"0x8000 {rel_dir}/partitions.bin "
        f"0x10000 {rel_dir}/firmware.bin"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Configure and build ESP VP firmware")
    parser.add_argument("--list-models", action="store_true", help="print selectable printer models and exit")
    parser.add_argument(
        "--model",
        action="append",
        help="printer model code/name; may be repeated or comma-separated, e.g. C12,P1S",
    )
    parser.add_argument(
        "--models",
        action="append",
        help="printer model list; alias for --model, accepts comma-separated values or all",
    )
    parser.add_argument("--name", help="virtual printer name or prefix shown in slicers")
    parser.add_argument("--serial", help="full serial to embed; otherwise generated from selected model")
    parser.add_argument("--serial-suffix", help="9-character suffix to use with the selected model prefix")
    parser.add_argument("--access-code", type=validate_access_code, help="8-char access code")
    parser.add_argument("--wifi-ssid", help="Wi-Fi SSID")
    parser.add_argument("--wifi-password", help="Wi-Fi password")
    parser.add_argument("--bambuddy-url", help="Bambuddy base URL")
    parser.add_argument("--api-key", help="Bambuddy API key for X-API-Key")
    parser.add_argument("--cert-ip", default=DEFAULT_CERT_IP, help="ESP IP address to include in TLS certificate SAN")
    parser.add_argument("--ca-cert", type=optional_path, help="existing Bambuddy VP bbl_ca.crt used to sign the ESP cert")
    parser.add_argument("--ca-key", type=optional_path, help="existing Bambuddy VP bbl_ca.key used to sign the ESP cert")
    parser.add_argument("--idf-target", default=DEFAULT_IDF_TARGET, help="ESP-IDF target, default esp32s3")
    parser.add_argument("--regen-cert", action="store_true", help="regenerate cert/key even if they exist")
    parser.add_argument("--no-build", action="store_true", help="only generate config and cert/key")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.list_models:
        list_models()
        return 0

    prompt_interactive(args)
    requested_models = split_csv(args.models) + split_csv(args.model)
    if any(value.lower() == "all" for value in requested_models):
        models = list(MODELS.keys())
    else:
        models = normalize_models(requested_models) or select_models(["BL-P001"])

    multiple = len(models) > 1
    if args.name is None and not multiple:
        args.name = f"Bambuddy {MODELS[models[0]][0]} VP"
    args.wifi_ssid = args.wifi_ssid or ""
    args.wifi_password = args.wifi_password or ""
    args.bambuddy_url = args.bambuddy_url or DEFAULT_BAMBUDDY_URL
    args.api_key = args.api_key or ""
    args.access_code = args.access_code or "12345678"

    if args.serial and multiple:
        raise SystemExit("--serial can only be used with a single selected model; use --serial-suffix for multi-model builds")
    if bool(args.ca_cert) != bool(args.ca_key):
        raise SystemExit("--ca-cert and --ca-key must be used together")

    first_serial, suffix = resolve_serial(models[0], args.serial, args.serial_suffix)
    import_ca = args.ca_cert is not None and args.ca_key is not None
    existing_cert = None if args.regen_cert or import_ca else find_existing_output_cert(models)
    if existing_cert:
        cert_path, key_path = existing_cert
        print(f"Reusing TLS cert/key from {cert_path.parent.relative_to(ROOT)}")
    else:
        cert_id = f"vp-{suffix}"
        cert_path = CERT_DIR / cert_id / "printer.crt"
        key_path = CERT_DIR / cert_id / "printer.key"
        cert_serials = [resolve_serial(model, args.serial, suffix)[0] for model in models]
        dns_names = ["localhost", "bambuddy", *cert_serials]
        ip_addresses = ["127.0.0.1"]
        if args.cert_ip:
            ip_addresses.append(args.cert_ip)
        run_openssl(
            cert_path,
            key_path,
            first_serial,
            args.regen_cert or import_ca,
            dns_names=dns_names,
            ip_addresses=ip_addresses,
            ca_cert_source=args.ca_cert,
            ca_key_source=args.ca_key,
        )
    cert = cert_path.read_text()
    key = key_path.read_text()

    print(f"Models: {', '.join(MODELS[model][0] for model in models)}")
    print(f"Serial suffix: {suffix}")
    print(f"Access code: {args.access_code}")
    print(f"TLS cert: {cert_path.relative_to(ROOT)}")
    if import_ca:
        print(f"TLS CA: {args.ca_cert}")

    outputs: list[Path] = []
    for model in models:
        serial, _suffix = resolve_serial(model, args.serial, suffix)
        write_generated_header(args, model, serial, suffix, cert, key, multiple)
        print()
        print(f"Generated {GENERATED_HEADER.relative_to(ROOT)}")
        print(f"Building {MODELS[model][0]} ({model})")
        print(f"Serial: {serial}")

        if args.no_build:
            continue

        sys.stdout.flush()
        result = run_idf_build(args, model)
        if result != 0:
            return result

        out_dir = copy_flash_artifacts_to_out(model_build_dir(model), model, cert_path, key_path)
        outputs.append(out_dir)
        print(f"Copied flash artifacts and TLS files: {out_dir.relative_to(ROOT)}")

    if outputs:
        print()
        print("Build outputs:")
        for out_dir in outputs:
            print(f"  {out_dir.relative_to(ROOT)}/")
            print(f"    TLS cert: {out_dir.relative_to(ROOT)}/printer.crt")
            print(f"    TLS key:  {out_dir.relative_to(ROOT)}/printer.key")
            ca_out = out_dir / "bbl_ca.crt"
            if ca_out.exists():
                print(f"    TLS CA:   {ca_out.relative_to(ROOT)}")
            print(f"    {flash_command_for(out_dir)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
