#!/usr/bin/env python3
"""Standalone ESP VP upload receiver.

Run this beside an existing Bambuddy Docker/native instance when that instance
does not include the ESP VP ingest route. The ESP posts its chunked upload here;
this service writes a temporary 3MF, then forwards it to the existing Bambuddy
archive API: POST /api/v1/archives/upload.

This script uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import http.client
import json
import logging
import os
import shutil
import tempfile
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlencode, urlparse


LOG = logging.getLogger("buddy_recv")
CHUNK_SIZE = 1024 * 1024


class UploadError(Exception):
    def __init__(self, status: int, detail: str) -> None:
        super().__init__(detail)
        self.status = status
        self.detail = detail


class Settings:
    def __init__(
        self,
        bambuddy_url: str,
        api_key: str,
        temp_dir: Path,
        max_upload_bytes: int,
        printer_id: int | None,
    ) -> None:
        self.bambuddy_url = bambuddy_url.rstrip("/")
        self.api_key = api_key
        self.temp_dir = temp_dir
        self.max_upload_bytes = max_upload_bytes
        self.printer_id = printer_id

    @property
    def archive_upload_url(self) -> str:
        return f"{self.bambuddy_url}/api/v1/archives/upload"


def safe_3mf_filename(filename: str | None) -> str:
    if not filename:
        raise UploadError(400, "X-Bambuddy-Filename header is required")
    safe = Path(filename.replace("\\", "/")).name.strip()
    if not safe or safe in {".", ".."}:
        raise UploadError(400, "Invalid filename")
    if safe.lower().endswith(".part"):
        safe = safe[:-5]
    if not safe.lower().endswith(".3mf"):
        raise UploadError(400, "File must be a .3mf file")
    if not Path(safe).stem:
        raise UploadError(400, "Invalid filename")
    return safe


class ConnectionWriter:
    def __init__(self, conn: http.client.HTTPConnection) -> None:
        self.conn = conn

    def write(self, data: bytes) -> int:
        self.conn.send(data)
        return len(data)


def forward_file(settings: Settings, path: Path, filename: str) -> dict:
    parsed = urlparse(settings.archive_upload_url)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise UploadError(500, "Invalid Bambuddy URL")

    query = parsed.query
    if settings.printer_id is not None:
        extra_query = urlencode({"printer_id": settings.printer_id})
        query = f"{query}&{extra_query}" if query else extra_query

    path_and_query = parsed.path or "/"
    if query:
        path_and_query += f"?{query}"

    boundary = f"bambuddy-esp-vp-{uuid.uuid4().hex}"
    file_size = path.stat().st_size
    part_head = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
        "Content-Type: application/vnd.ms-package.3dmanufacturing-3dmodel+xml\r\n"
        "\r\n"
    ).encode()
    part_tail = f"\r\n--{boundary}--\r\n".encode()
    content_length = len(part_head) + file_size + len(part_tail)

    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    conn_cls = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
    conn = conn_cls(parsed.hostname, port, timeout=300)
    try:
        conn.putrequest("POST", path_and_query)
        host_header = parsed.hostname if parsed.port is None else f"{parsed.hostname}:{parsed.port}"
        conn.putheader("Host", host_header)
        conn.putheader("User-Agent", "bambuddy-esp-vp-receiver/1")
        conn.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
        conn.putheader("Content-Length", str(content_length))
        if settings.api_key:
            conn.putheader("X-API-Key", settings.api_key)
        conn.endheaders()

        conn.send(part_head)
        with path.open("rb") as fh:
            shutil.copyfileobj(fh, ConnectionWriter(conn), length=CHUNK_SIZE)
        conn.send(part_tail)

        response = conn.getresponse()
        body = response.read()
    finally:
        conn.close()

    if response.status >= 400:
        detail = body.decode(errors="replace")[:1000]
        LOG.error("Bambuddy archive upload failed status=%s body=%s", response.status, detail)
        raise UploadError(502, f"Bambuddy archive upload failed: HTTP {response.status}")

    if body:
        try:
            return json.loads(body)
        except ValueError:
            pass
    return {"status": "forwarded", "bambuddy_status": response.status}


def read_line(handler: BaseHTTPRequestHandler) -> bytes:
    line = handler.rfile.readline(65536)
    if not line:
        raise UploadError(499, "Client disconnected")
    return line


def copy_chunked_body(handler: BaseHTTPRequestHandler, out, settings: Settings) -> int:
    total = 0
    while True:
        size_line = read_line(handler).split(b";", 1)[0].strip()
        try:
            size = int(size_line, 16)
        except ValueError:
            raise UploadError(400, "Invalid chunked upload")
        if size == 0:
            while True:
                trailer = read_line(handler)
                if trailer in {b"\r\n", b"\n"}:
                    break
            return total

        remaining = size
        while remaining > 0:
            data = handler.rfile.read(min(remaining, CHUNK_SIZE))
            if not data:
                raise UploadError(499, "Client disconnected")
            out.write(data)
            total += len(data)
            remaining -= len(data)
            if settings.max_upload_bytes and total > settings.max_upload_bytes:
                raise UploadError(413, "Upload exceeds configured maximum size")

        crlf = handler.rfile.read(2)
        if crlf != b"\r\n":
            raise UploadError(400, "Invalid chunk terminator")


def copy_content_length_body(handler: BaseHTTPRequestHandler, out, settings: Settings, content_length: int) -> int:
    total = 0
    remaining = content_length
    while remaining > 0:
        data = handler.rfile.read(min(remaining, CHUNK_SIZE))
        if not data:
            raise UploadError(499, "Client disconnected")
        out.write(data)
        total += len(data)
        remaining -= len(data)
        if settings.max_upload_bytes and total > settings.max_upload_bytes:
            raise UploadError(413, "Upload exceeds configured maximum size")
    return total


class ReceiverHandler(BaseHTTPRequestHandler):
    server_version = "BambuddyESPVPReceiver/1.0"

    @property
    def settings(self) -> Settings:
        return self.server.settings

    def log_message(self, fmt: str, *args) -> None:
        LOG.info("%s - %s", self.address_string(), fmt % args)

    def send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path != "/health":
            self.send_json(404, {"detail": "Not found"})
            return
        self.send_json(
            200,
            {
                "status": "ok",
                "bambuddy_url": self.settings.bambuddy_url,
                "archive_upload_url": self.settings.archive_upload_url,
            },
        )

    def do_POST(self) -> None:
        if self.path.split("?", 1)[0] != "/api/v1/esp-vp/upload":
            self.send_json(404, {"detail": "Not found"})
            return

        tmp_path: Path | None = None
        filename = "unknown.3mf"
        total = 0
        try:
            filename = safe_3mf_filename(self.headers.get("X-Bambuddy-Filename"))
            vp_name = self.headers.get("X-Bambuddy-VP-Name")
            source_ip = self.headers.get("X-Bambuddy-Source-IP")
            LOG.info(
                "ESP VP upload started filename=%s vp=%s source=%s client=%s",
                filename,
                vp_name,
                source_ip,
                self.client_address[0] if self.client_address else None,
            )

            self.settings.temp_dir.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                mode="wb",
                prefix="esp-vp-",
                suffix=".3mf.part",
                dir=self.settings.temp_dir,
                delete=False,
            ) as out:
                tmp_path = Path(out.name)
                transfer_encoding = self.headers.get("Transfer-Encoding", "").lower()
                if "chunked" in transfer_encoding:
                    total = copy_chunked_body(self, out, self.settings)
                else:
                    content_length = self.headers.get("Content-Length")
                    if content_length is None:
                        raise UploadError(411, "Content-Length or chunked Transfer-Encoding is required")
                    total = copy_content_length_body(self, out, self.settings, int(content_length))

            if total <= 0:
                raise UploadError(400, "Upload body is empty")

            result = forward_file(self.settings, tmp_path, filename)
            archive_id = result.get("id") or result.get("archive_id")
            LOG.info("ESP VP upload forwarded filename=%s bytes=%d archive_id=%s", filename, total, archive_id)
            self.send_json(
                200,
                {
                    "status": "forwarded",
                    "filename": filename,
                    "bytes": total,
                    "archive_id": archive_id,
                    "bambuddy": result,
                },
            )
        except UploadError as exc:
            LOG.warning("ESP VP upload rejected filename=%s status=%s detail=%s", filename, exc.status, exc.detail)
            self.send_json(exc.status, {"detail": exc.detail})
        except Exception as exc:
            LOG.exception("ESP VP upload failed filename=%s bytes=%d: %s", filename, total, exc)
            self.send_json(500, {"detail": "Upload failed"})
        finally:
            if tmp_path is not None:
                try:
                    tmp_path.unlink(missing_ok=True)
                except OSError:
                    LOG.warning("Failed to remove temp upload %s", tmp_path)


class ReceiverServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, server_address: tuple[str, int], settings: Settings) -> None:
        self.settings = settings
        super().__init__(server_address, ReceiverHandler)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Receive ESP VP uploads and forward to Bambuddy")
    parser.add_argument("--host", default=os.getenv("BUDDY_RECV_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.getenv("BUDDY_RECV_PORT", "8001")))
    parser.add_argument(
        "--bambuddy-url",
        default=os.getenv("BAMBUDDY_URL", "http://127.0.0.1:8000"),
        help="existing Bambuddy host URL",
    )
    parser.add_argument("--api-key", default=os.getenv("BAMBUDDY_API_KEY", ""), help="optional Bambuddy API key")
    parser.add_argument(
        "--temp-dir",
        type=Path,
        default=Path(os.getenv("BUDDY_RECV_TEMP_DIR", Path(tempfile.gettempdir()) / "bambuddy-esp-vp")),
    )
    parser.add_argument(
        "--max-upload-bytes",
        type=int,
        default=int(os.getenv("BUDDY_RECV_MAX_UPLOAD_BYTES", "0")),
        help="0 disables size limit",
    )
    parser.add_argument(
        "--printer-id",
        type=int,
        default=int(os.environ["BUDDY_RECV_PRINTER_ID"]) if os.getenv("BUDDY_RECV_PRINTER_ID") else None,
    )
    return parser.parse_args()


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s [%(name)s] %(message)s")
    args = parse_args()
    settings = Settings(
        bambuddy_url=args.bambuddy_url,
        api_key=args.api_key,
        temp_dir=args.temp_dir,
        max_upload_bytes=args.max_upload_bytes,
        printer_id=args.printer_id,
    )
    LOG.info("listening on %s:%s, forwarding to %s", args.host, args.port, settings.archive_upload_url)
    server = ReceiverServer((args.host, args.port), settings)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        LOG.info("stopping")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
