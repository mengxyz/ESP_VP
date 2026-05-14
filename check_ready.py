#!/usr/bin/env python3
"""Quick readiness check for an ESP virtual printer on the LAN."""

from __future__ import annotations

import argparse
import ssl
import socket
import sys
from dataclasses import dataclass


TCP_PORTS = {
    3000: ("bind plain", False),
    3002: ("bind tls", True),
    8883: ("mqtt tls", True),
    990: ("ftps implicit tls", True),
}


@dataclass
class CheckResult:
    name: str
    ok: bool
    detail: str


def check_tcp(host: str, port: int, timeout: float) -> CheckResult:
    name, use_tls = TCP_PORTS[port]
    label = f"tcp/{port} {name}"
    try:
        with socket.create_connection((host, port), timeout=timeout) as raw:
            raw.settimeout(timeout)
            if use_tls:
                context = ssl._create_unverified_context()
                with context.wrap_socket(raw, server_hostname=host) as tls:
                    tls.settimeout(timeout)
                    if port == 990:
                        banner = tls.recv(128).decode("utf-8", errors="replace").strip()
                        return CheckResult(label, banner.startswith("220"), banner or "no banner")
                    return CheckResult(label, True, "tls handshake ok")
            elif port == 3000:
                raw.sendall(b"\0")
                try:
                    data = raw.recv(16)
                except socket.timeout:
                    data = b""
                detail = "detect response" if data.startswith(b"\xa5\xa5") else "open"
                return CheckResult(label, True, detail)
            return CheckResult(label, True, "open")
    except ssl.SSLError as exc:
        return CheckResult(label, False, f"tls failed: {exc}")
    except OSError as exc:
        return CheckResult(label, False, str(exc))


def check_udp_send(host: str, port: int, timeout: float) -> CheckResult:
    label = f"udp/{port} ssdp discovery"
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(timeout)
            sock.sendto(b"M-SEARCH * HTTP/1.1\r\nST: urn:bambulab-com:device:3dprinter:1\r\n\r\n", (host, port))
        return CheckResult(label, True, "probe sent")
    except OSError as exc:
        return CheckResult(label, False, str(exc))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Check whether an ESP VP is ready")
    parser.add_argument("host", help="ESP IP address, e.g. 192.168.1.60")
    parser.add_argument("--timeout", type=float, default=2.0, help="per-port timeout in seconds")
    parser.add_argument("--skip-udp", action="store_true", help="skip UDP/2021 discovery probe")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    results = [check_tcp(args.host, port, args.timeout) for port in TCP_PORTS]
    if not args.skip_udp:
        results.append(check_udp_send(args.host, 2021, args.timeout))

    width = max(len(result.name) for result in results)
    for result in results:
        status = "OK" if result.ok else "FAIL"
        print(f"{status:4} {result.name:<{width}} {result.detail}")

    failed = [result for result in results if not result.ok]
    if failed:
        print()
        print(f"ESP VP at {args.host} is not fully reachable.")
        return 1

    print()
    print(f"ESP VP at {args.host} looks ready.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
