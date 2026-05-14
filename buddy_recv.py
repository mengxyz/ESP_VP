#!/usr/bin/env python3
"""Local ESP VP device manager and Bambuddy upload receiver."""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import logging
import os
import secrets
import shutil
import socket
import sqlite3
import tempfile
import time
import uuid
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import httpx
import uvicorn
from fastapi import Depends, FastAPI, File, Form, Header, HTTPException, Request, Response, UploadFile
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
except Exception:  # pragma: no cover - optional until cert generation is used
    x509 = None
    rsa = None
    hashes = None
    serialization = None
    ExtendedKeyUsageOID = None
    NameOID = None


LOG = logging.getLogger("buddy_recv")
APP_VERSION = "2.0.0"
CHUNK_SIZE = 1024 * 1024
SESSION_TTL_SECONDS = 14 * 24 * 60 * 60
FORWARD_MODES = {"immediate", "library", "print_queue", "proxy_status", "auto", "archive", "esp-vp"}
DEFAULT_DATA_DIR = Path(os.getenv("BUDDY_RECV_DATA_DIR", Path(tempfile.gettempdir()) / "bambuddy-esp-vp"))
MODELS: dict[str, dict[str, str]] = {
    "BL-P001": {"display": "X1C", "product_name": "X1 Carbon", "serial_prefix": "00M00A"},
    "BL-P002": {"display": "X1", "product_name": "X1", "serial_prefix": "00M00A"},
    "C13": {"display": "X1E", "product_name": "X1E", "serial_prefix": "03W00A"},
    "N6": {"display": "X2D", "product_name": "X2D", "serial_prefix": "20P90A"},
    "C11": {"display": "P1P", "product_name": "P1P", "serial_prefix": "01S00A"},
    "C12": {"display": "P1S", "product_name": "P1S", "serial_prefix": "01P00A"},
    "N7": {"display": "P2S", "product_name": "P2S", "serial_prefix": "22E00A"},
    "N2S": {"display": "A1", "product_name": "A1", "serial_prefix": "03900A"},
    "N1": {"display": "A1 Mini", "product_name": "A1 mini", "serial_prefix": "03000A"},
    "O1D": {"display": "H2D", "product_name": "H2D", "serial_prefix": "09400A"},
    "O1C": {"display": "H2C", "product_name": "H2C", "serial_prefix": "09400A"},
    "O1C2": {"display": "H2C Dual", "product_name": "H2C", "serial_prefix": "09400A"},
    "O1S": {"display": "H2S", "product_name": "H2S", "serial_prefix": "09400A"},
}


class UploadError(Exception):
    def __init__(self, status: int, detail: str) -> None:
        super().__init__(detail)
        self.status = status
        self.detail = detail


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def json_dumps(value: Any) -> str:
    return json.dumps(value, separators=(",", ":"), sort_keys=True)


def safe_3mf_filename(filename: str | None) -> str:
    if not filename:
        raise UploadError(400, "X-Bambuddy-Filename header is required")
    safe = Path(filename.replace("\\", "/")).name.strip()
    if not safe or safe in {".", ".."}:
        raise UploadError(400, "Invalid filename")
    if safe.lower().endswith(".part"):
        safe = safe[:-5]
    if not safe.lower().endswith(".3mf") or not Path(safe).stem:
        raise UploadError(400, "File must be a .3mf file")
    return safe


def generated_serial(model_code: str, device_id: str) -> str:
    model = MODELS.get(model_code) or MODELS["C12"]
    suffix_num = int(hashlib.sha1(device_id.encode()).hexdigest()[:12], 16) % 1_000_000_000
    return f"{model['serial_prefix']}{suffix_num:09d}"


def password_hash(password: str, salt: str | None = None) -> str:
    salt = salt or secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt.encode(), 180_000)
    return f"pbkdf2_sha256${salt}${base64.b64encode(digest).decode()}"


def verify_password(password: str, stored: str) -> bool:
    try:
        _algo, salt, digest = stored.split("$", 2)
    except ValueError:
        return False
    return hmac.compare_digest(password_hash(password, salt).split("$", 2)[2], digest)


class Store:
    def __init__(self, db_path: Path) -> None:
        self.db_path = db_path
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(self.db_path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA foreign_keys=ON")
        self.migrate()

    def migrate(self) -> None:
        cur = self.conn.cursor()
        cur.executescript(
            """
            CREATE TABLE IF NOT EXISTS settings (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS admin_users (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                username TEXT NOT NULL UNIQUE,
                password_hash TEXT NOT NULL,
                created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS sessions (
                token TEXT PRIMARY KEY,
                user_id INTEGER NOT NULL,
                expires_at INTEGER NOT NULL,
                created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS devices (
                device_id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                ip TEXT,
                management_url TEXT,
                firmware_version TEXT,
                configured INTEGER NOT NULL DEFAULT 0,
                paired INTEGER NOT NULL DEFAULT 0,
                pair_ready INTEGER NOT NULL DEFAULT 0,
                pair_remaining_seconds INTEGER NOT NULL DEFAULT 0,
                claimed INTEGER NOT NULL DEFAULT 0,
                receiver_managed INTEGER NOT NULL DEFAULT 0,
                token_hash TEXT,
                device_token TEXT,
                last_seen TEXT,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS device_configs (
                device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
                config_json TEXT NOT NULL,
                cert_path TEXT,
                key_path TEXT,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS uploads (
                id TEXT PRIMARY KEY,
                filename TEXT NOT NULL,
                bytes INTEGER NOT NULL,
                source_ip TEXT,
                vp_name TEXT,
                device_id TEXT,
                forward_mode TEXT NOT NULL,
                status TEXT NOT NULL,
                result_json TEXT,
                error TEXT,
                created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS proxy_status (
                device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
                status_json TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS device_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
                stage TEXT NOT NULL,
                status TEXT NOT NULL,
                message TEXT NOT NULL,
                detail_json TEXT,
                created_at TEXT NOT NULL
            );
            """
        )
        self.conn.commit()
        columns = {row["name"] for row in self.conn.execute("PRAGMA table_info(devices)").fetchall()}
        if "device_token" not in columns:
            self.conn.execute("ALTER TABLE devices ADD COLUMN device_token TEXT")
            self.conn.commit()
        for column, ddl in {
            "paired": "ALTER TABLE devices ADD COLUMN paired INTEGER NOT NULL DEFAULT 0",
            "pair_ready": "ALTER TABLE devices ADD COLUMN pair_ready INTEGER NOT NULL DEFAULT 0",
            "pair_remaining_seconds": "ALTER TABLE devices ADD COLUMN pair_remaining_seconds INTEGER NOT NULL DEFAULT 0",
        }.items():
            if column not in columns:
                self.conn.execute(ddl)
                self.conn.commit()

    def add_device_event(
        self,
        device_id: str,
        stage: str,
        status: str,
        message: str,
        detail: dict[str, Any] | None = None,
    ) -> None:
        self.conn.execute(
            """
            INSERT INTO device_events(device_id,stage,status,message,detail_json,created_at)
            VALUES(?,?,?,?,?,?)
            """,
            (device_id, stage, status, message, json_dumps(detail) if detail else None, now_iso()),
        )
        self.conn.commit()

    def device_events(self, device_id: str, limit: int = 50) -> list[dict[str, Any]]:
        self.device(device_id)
        rows = self.conn.execute(
            "SELECT * FROM device_events WHERE device_id = ? ORDER BY id DESC LIMIT ?",
            (device_id, limit),
        ).fetchall()
        events: list[dict[str, Any]] = []
        for row in rows:
            event = dict(row)
            if event.get("detail_json"):
                try:
                    event["detail"] = json.loads(event["detail_json"])
                except ValueError:
                    event["detail"] = event["detail_json"]
            else:
                event["detail"] = None
            del event["detail_json"]
            events.append(event)
        events.reverse()
        return events

    def get_setting(self, key: str, default: Any = None) -> Any:
        row = self.conn.execute("SELECT value FROM settings WHERE key = ?", (key,)).fetchone()
        if row is None:
            return default
        try:
            return json.loads(row["value"])
        except ValueError:
            return row["value"]

    def set_setting(self, key: str, value: Any) -> None:
        self.conn.execute(
            "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            (key, json_dumps(value)),
        )
        self.conn.commit()

    def settings_dict(self) -> dict[str, Any]:
        rows = self.conn.execute("SELECT key,value FROM settings").fetchall()
        result: dict[str, Any] = {}
        for row in rows:
            try:
                result[row["key"]] = json.loads(row["value"])
            except ValueError:
                result[row["key"]] = row["value"]
        return result

    def first_run(self) -> bool:
        row = self.conn.execute("SELECT 1 FROM admin_users LIMIT 1").fetchone()
        return row is None

    def create_admin(self, username: str, password: str) -> None:
        if not username or not password:
            raise HTTPException(400, "Username and password are required")
        self.conn.execute(
            "INSERT INTO admin_users(id,username,password_hash,created_at) VALUES(1,?,?,?)",
            (username, password_hash(password), now_iso()),
        )
        self.conn.commit()

    def authenticate(self, username: str, password: str) -> bool:
        row = self.conn.execute("SELECT password_hash FROM admin_users WHERE username = ?", (username,)).fetchone()
        return bool(row and verify_password(password, row["password_hash"]))

    def create_session(self) -> str:
        token = secrets.token_urlsafe(32)
        self.conn.execute(
            "INSERT INTO sessions(token,user_id,expires_at,created_at) VALUES(?,1,?,?)",
            (token, int(time.time()) + SESSION_TTL_SECONDS, now_iso()),
        )
        self.conn.commit()
        return token

    def valid_session(self, token: str | None) -> bool:
        if not token:
            return False
        row = self.conn.execute("SELECT expires_at FROM sessions WHERE token = ?", (token,)).fetchone()
        if not row:
            return False
        if int(row["expires_at"]) < int(time.time()):
            self.conn.execute("DELETE FROM sessions WHERE token = ?", (token,))
            self.conn.commit()
            return False
        return True

    def delete_session(self, token: str | None) -> None:
        if token:
            self.conn.execute("DELETE FROM sessions WHERE token = ?", (token,))
            self.conn.commit()

    def upsert_device(self, payload: dict[str, Any], claimed: bool | None = None) -> sqlite3.Row:
        device_id = str(payload.get("device_id") or "").strip()
        if not device_id:
            raise HTTPException(400, "device_id is required")
        existing = self.conn.execute("SELECT * FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        name = str(payload.get("name") or (existing["name"] if existing else f"vp_{device_id[-6:]}"))
        values = {
            "device_id": device_id,
            "name": name,
            "ip": payload.get("ip") or (existing["ip"] if existing else None),
            "management_url": payload.get("management_url") or (existing["management_url"] if existing else None),
            "firmware_version": payload.get("firmware_version") or (existing["firmware_version"] if existing else None),
            "configured": int(bool(payload.get("configured", existing["configured"] if existing else False))),
            "paired": int(bool(payload.get("paired", existing["paired"] if existing and "paired" in existing.keys() else False))),
            "pair_ready": int(bool(payload.get("pair_ready", existing["pair_ready"] if existing and "pair_ready" in existing.keys() else False))),
            "pair_remaining_seconds": int(payload.get("pair_remaining_seconds", existing["pair_remaining_seconds"] if existing and "pair_remaining_seconds" in existing.keys() else 0) or 0),
            "claimed": int(bool(claimed if claimed is not None else (existing["claimed"] if existing else False))),
            "receiver_managed": int(bool(payload.get("receiver_managed", True))),
            "last_seen": now_iso(),
            "created_at": existing["created_at"] if existing else now_iso(),
            "updated_at": now_iso(),
        }
        self.conn.execute(
            """
            INSERT INTO devices(device_id,name,ip,management_url,firmware_version,configured,paired,pair_ready,pair_remaining_seconds,claimed,receiver_managed,last_seen,created_at,updated_at)
            VALUES(:device_id,:name,:ip,:management_url,:firmware_version,:configured,:paired,:pair_ready,:pair_remaining_seconds,:claimed,:receiver_managed,:last_seen,:created_at,:updated_at)
            ON CONFLICT(device_id) DO UPDATE SET
              name=excluded.name, ip=excluded.ip, management_url=excluded.management_url,
              firmware_version=excluded.firmware_version, configured=excluded.configured,
              paired=excluded.paired, pair_ready=excluded.pair_ready,
              pair_remaining_seconds=excluded.pair_remaining_seconds,
              claimed=excluded.claimed, receiver_managed=excluded.receiver_managed,
              last_seen=excluded.last_seen, updated_at=excluded.updated_at
            """,
            values,
        )
        self.conn.commit()
        self.add_device_event(device_id, "discovery", "success", "Device discovered or updated", {"ip": values["ip"], "management_url": values["management_url"]})
        return self.device(device_id)

    def delete_device(self, device_id: str) -> None:
        self.device(device_id)
        self.conn.execute("DELETE FROM devices WHERE device_id = ?", (device_id,))
        self.conn.commit()

    def device(self, device_id: str) -> sqlite3.Row:
        row = self.conn.execute("SELECT * FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        if not row:
            raise HTTPException(404, "Device not found")
        return row

    def devices(self) -> list[dict[str, Any]]:
        return [dict(row) for row in self.conn.execute("SELECT * FROM devices ORDER BY last_seen DESC NULLS LAST").fetchall()]

    def set_device_token(self, device_id: str, token: str) -> None:
        self.conn.execute(
            "UPDATE devices SET token_hash = ?, device_token = ?, claimed = 1, paired = 1, pair_ready = 0, pair_remaining_seconds = 0, updated_at = ? WHERE device_id = ?",
            (password_hash(token), token, now_iso(), device_id),
        )
        self.conn.commit()
        self.add_device_event(device_id, "pair", "success", "Device paired")

    def verify_device_token(self, token: str | None, device_id: str | None = None) -> bool:
        if not token:
            return False
        enrollment_key = self.get_setting("enrollment_key", "")
        if enrollment_key and hmac.compare_digest(token, enrollment_key):
            return True
        if not device_id:
            return False
        row = self.conn.execute("SELECT token_hash FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        return bool(row and row["token_hash"] and verify_password(token, row["token_hash"]))

    def save_device_config(self, device_id: str, config: dict[str, Any], cert_path: Path | None = None, key_path: Path | None = None) -> None:
        self.device(device_id)
        old = self.device_config(device_id) or {}
        merged = {**old, **config}
        self.conn.execute(
            """
            INSERT INTO device_configs(device_id,config_json,cert_path,key_path,updated_at)
            VALUES(?,?,?,?,?)
            ON CONFLICT(device_id) DO UPDATE SET
              config_json=excluded.config_json, cert_path=COALESCE(excluded.cert_path, device_configs.cert_path),
              key_path=COALESCE(excluded.key_path, device_configs.key_path), updated_at=excluded.updated_at
            """,
            (device_id, json_dumps(merged), str(cert_path) if cert_path else None, str(key_path) if key_path else None, now_iso()),
        )
        self.conn.commit()
        self.add_device_event(device_id, "config_save", "success", "Device configuration saved")

    def mark_device_configured(self, device_id: str, configured: bool = True) -> None:
        self.conn.execute(
            "UPDATE devices SET configured = ?, updated_at = ? WHERE device_id = ?",
            (1 if configured else 0, now_iso(), device_id),
        )
        self.conn.commit()

    def device_config(self, device_id: str) -> dict[str, Any] | None:
        row = self.conn.execute("SELECT * FROM device_configs WHERE device_id = ?", (device_id,)).fetchone()
        if not row:
            return None
        config = json.loads(row["config_json"])
        config["cert_path"] = row["cert_path"]
        config["key_path"] = row["key_path"]
        return config

    def add_upload(self, row: dict[str, Any]) -> None:
        self.conn.execute(
            """
            INSERT INTO uploads(id,filename,bytes,source_ip,vp_name,device_id,forward_mode,status,result_json,error,created_at)
            VALUES(:id,:filename,:bytes,:source_ip,:vp_name,:device_id,:forward_mode,:status,:result_json,:error,:created_at)
            """,
            row,
        )
        self.conn.commit()

    def set_proxy_status(self, device_id: str, payload: dict[str, Any]) -> None:
        self.conn.execute(
            "INSERT INTO proxy_status(device_id,status_json,updated_at) VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET status_json=excluded.status_json, updated_at=excluded.updated_at",
            (device_id, json_dumps(payload), now_iso()),
        )
        self.conn.commit()

    def get_proxy_status(self, device_id: str) -> dict[str, Any] | None:
        row = self.conn.execute("SELECT status_json, updated_at FROM proxy_status WHERE device_id = ?", (device_id,)).fetchone()
        if not row:
            return None
        payload = json.loads(row["status_json"])
        payload["updated_at"] = row["updated_at"]
        return payload

    def uploads(self) -> list[dict[str, Any]]:
        return [dict(row) for row in self.conn.execute("SELECT * FROM uploads ORDER BY created_at DESC LIMIT 100").fetchall()]


class Manager:
    def __init__(self, args: argparse.Namespace) -> None:
        self.data_dir = args.data_dir
        self.temp_dir = args.temp_dir
        self.cert_dir = self.data_dir / "certs"
        self.store = Store(self.data_dir / "buddy_recv.sqlite3")
        self.max_upload_bytes = args.max_upload_bytes
        self.login_token = ""
        self.login_time = 0.0
        self.temp_dir.mkdir(parents=True, exist_ok=True)
        self.cert_dir.mkdir(parents=True, exist_ok=True)
        self.seed_settings(args)

    def seed_settings(self, args: argparse.Namespace) -> None:
        defaults = {
            "bambuddy_url": args.bambuddy_url.rstrip("/"),
            "receiver_url": args.receiver_url.rstrip("/") if args.receiver_url else "",
            "api_key": args.api_key,
            "bearer_token": args.bearer_token,
            "username": args.username,
            "password": args.password,
            "forward_mode": args.forward_mode,
            "printer_id": args.printer_id,
            "library_folder_id": args.library_folder_id,
            "queue_options": {},
            "enrollment_key": args.enrollment_key or secrets.token_urlsafe(24),
        }
        for key, value in defaults.items():
            if self.store.get_setting(key) is None:
                self.store.set_setting(key, value)
        if args.receiver_url and not self.store.get_setting("receiver_url", ""):
            self.store.set_setting("receiver_url", args.receiver_url.rstrip("/"))

    def bambuddy_url(self, path: str) -> str:
        return f"{self.store.get_setting('bambuddy_url', 'http://127.0.0.1:8000').rstrip('/')}{path}"

    async def test_bambuddy_host(self, base_url: str | None = None) -> dict[str, Any]:
        url = (base_url or self.store.get_setting("bambuddy_url", "http://127.0.0.1:8000")).strip().rstrip("/")
        if not url.startswith(("http://", "https://")):
            raise HTTPException(400, "Bambuddy URL must start with http:// or https://")
        health_url = f"{url}/health"
        try:
            async with httpx.AsyncClient(timeout=httpx.Timeout(8.0, connect=3.0), follow_redirects=False) as client:
                response = await client.get(health_url)
        except httpx.ConnectError as exc:
            return {
                "status": "unreachable",
                "bambuddy_url": url,
                "detail": "Bambuddy host is not accepting connections",
                "hint": "Check the host, port, Docker network, and whether Bambuddy is running.",
                "error_type": exc.__class__.__name__,
            }
        except httpx.TimeoutException as exc:
            return {
                "status": "timeout",
                "bambuddy_url": url,
                "detail": "Bambuddy host timed out",
                "hint": "Check network reachability and whether the Bambuddy server is overloaded.",
                "error_type": exc.__class__.__name__,
            }
        except httpx.RequestError as exc:
            return {
                "status": "error",
                "bambuddy_url": url,
                "detail": "Bambuddy host test failed",
                "hint": "Check the URL format, DNS, and network path from this receiver.",
                "error_type": exc.__class__.__name__,
            }
        result: dict[str, Any] = {
            "status": "ok" if response.status_code < 400 else "http_error",
            "bambuddy_url": url,
            "health_url": health_url,
            "status_code": response.status_code,
        }
        try:
            body: Any = response.json()
        except ValueError:
            body = response.text[:500] if response.text else None
        if body is not None:
            result["response"] = body
        if response.status_code >= 400:
            result["detail"] = f"Bambuddy host responded with HTTP {response.status_code}"
            result["hint"] = "The host is reachable, but /health did not return a successful response."
        return result

    async def list_bambuddy_printers(self) -> list[dict[str, Any]]:
        headers = await self.auth_headers()
        async with httpx.AsyncClient(timeout=httpx.Timeout(12.0, connect=4.0)) as client:
            response = await client.get(self.bambuddy_url("/api/v1/printers/"), headers=headers)
        if response.status_code >= 400:
            raise HTTPException(502, f"Bambuddy printer list failed: HTTP {response.status_code}")
        body = response.json() if response.content else []
        if not isinstance(body, list):
            raise HTTPException(502, "Bambuddy printer list response was not an array")
        return [item for item in body if isinstance(item, dict)]

    async def fetch_bambuddy_printer_status(self, printer_id: int) -> dict[str, Any]:
        headers = await self.auth_headers()
        async with httpx.AsyncClient(timeout=httpx.Timeout(12.0, connect=4.0)) as client:
            response = await client.get(self.bambuddy_url(f"/api/v1/printers/{printer_id}/status"), headers=headers)
        if response.status_code >= 400:
            raise UploadError(502, f"Bambuddy printer status failed: HTTP {response.status_code}")
        body = response.json() if response.content else {}
        if not isinstance(body, dict):
            raise UploadError(502, "Bambuddy printer status response was not an object")
        return body

    def status_to_bambu_report(self, status: dict[str, Any]) -> dict[str, Any]:
        state = str(status.get("state") or ("IDLE" if status.get("connected") else "OFFLINE")).upper()
        temperatures = status.get("temperatures") if isinstance(status.get("temperatures"), dict) else {}
        nozzle = temperatures.get("nozzle") or temperatures.get("nozzle_temper") or 0
        nozzle_target = temperatures.get("nozzle_target") or temperatures.get("nozzle_target_temper") or 0
        bed = temperatures.get("bed") or temperatures.get("bed_temper") or 0
        bed_target = temperatures.get("bed_target") or temperatures.get("bed_target_temper") or 0
        progress = status.get("progress")
        if progress is None:
            progress = 0
        try:
            progress = int(float(progress))
        except (TypeError, ValueError):
            progress = 0
        remaining = status.get("remaining_time") or 0
        try:
            remaining = int(remaining)
        except (TypeError, ValueError):
            remaining = 0
        return {
            "print": {
                "sequence_id": str(int(time.time())),
                "command": "push_status",
                "msg": 0,
                "gcode_state": state,
                "gcode_file": status.get("gcode_file") or "",
                "subtask_name": status.get("subtask_name") or status.get("current_print") or "",
                "mc_print_stage": str(status.get("stg_cur_name") or ""),
                "mc_percent": progress,
                "mc_remaining_time": remaining,
                "layer_num": status.get("layer_num") or 0,
                "total_layer_num": status.get("total_layers") or 0,
                "nozzle_temper": nozzle,
                "nozzle_target_temper": nozzle_target,
                "bed_temper": bed,
                "bed_target_temper": bed_target,
                "wifi_signal": f"{status.get('wifi_signal') or -44}dBm",
                "home_flag": 256,
                "sdcard": bool(status.get("sdcard", True)),
                "storage": {"free": 1000000000, "total": 32000000000},
                "online": {"ahb": False, "rfid": False, "version": 7},
                "ams_status": int(status.get("ams_status_main") or 0),
                "ams_status_sub": int(status.get("ams_status_sub") or 0),
                "ams": {"ams": status.get("ams") or []},
                "vt_tray": status.get("vt_tray") or [],
                "tray_now": status.get("tray_now", 255),
                "door_open": bool(status.get("door_open", False)),
                "chamber_light": bool(status.get("chamber_light", False)),
                "nozzle_diameter": "0.4",
                "nozzle_type": "hardened_steel",
            }
        }

    async def auth_headers(self, force_login: bool = False) -> dict[str, str]:
        bearer = self.store.get_setting("bearer_token", "")
        if bearer:
            return {"Authorization": f"Bearer {bearer}"}
        token = await self.login_to_bambuddy(force=force_login)
        if token:
            return {"Authorization": f"Bearer {token}"}
        api_key = self.store.get_setting("api_key", "")
        if not api_key:
            return {}
        return {"X-API-Key": api_key, "Authorization": f"Bearer {api_key}"}

    async def login_to_bambuddy(self, force: bool = False) -> str:
        username = self.store.get_setting("username", "")
        password = self.store.get_setting("password", "")
        if not username or not password:
            return ""
        if self.login_token and not force and time.time() - self.login_time < 3300:
            return self.login_token
        async with httpx.AsyncClient(timeout=60) as client:
            response = await client.post(self.bambuddy_url("/api/v1/auth/login"), json={"username": username, "password": password})
        if response.status_code >= 400:
            raise UploadError(502, f"Bambuddy login failed: HTTP {response.status_code}")
        payload = response.json()
        if payload.get("requires_2fa"):
            raise UploadError(502, "Bambuddy login requires 2FA; use bearer token or a service user")
        token = payload.get("access_token")
        if not isinstance(token, str) or not token:
            raise UploadError(502, "Bambuddy login did not return an access token")
        self.login_token = token
        self.login_time = time.time()
        return token

    async def forward_multipart(self, path: Path, filename: str, url: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        headers = await self.auth_headers()
        async with httpx.AsyncClient(timeout=300) as client:
            with path.open("rb") as fh:
                response = await client.post(
                    url,
                    params={k: v for k, v in (params or {}).items() if v is not None},
                    headers=headers,
                    files={"file": (filename, fh, "application/vnd.ms-package.3dmanufacturing-3dmodel+xml")},
                )
            if response.status_code == 401 and self.store.get_setting("username", "") and not self.store.get_setting("bearer_token", ""):
                headers = await self.auth_headers(force_login=True)
                with path.open("rb") as retry_fh:
                    response = await client.post(
                        url,
                        params={k: v for k, v in (params or {}).items() if v is not None},
                        headers=headers,
                        files={"file": (filename, retry_fh, "application/vnd.ms-package.3dmanufacturing-3dmodel+xml")},
                    )
        if response.status_code >= 400:
            raise UploadError(502, f"Bambuddy upload failed: HTTP {response.status_code}")
        try:
            payload = response.json()
            return payload if isinstance(payload, dict) else {"response": payload}
        except ValueError:
            return {"status": "forwarded", "bambuddy_status": response.status_code}

    async def forward_upload(self, path: Path, filename: str, vp_name: str | None, source_ip: str | None) -> dict[str, Any]:
        mode = self.store.get_setting("forward_mode", "library")
        if mode in {"immediate", "archive", "proxy_status"}:
            return await self.forward_multipart(
                path,
                filename,
                self.bambuddy_url("/api/v1/archives/upload"),
                {"printer_id": self.store.get_setting("printer_id")},
            )
        if mode == "print_queue":
            archive = await self.forward_multipart(path, filename, self.bambuddy_url("/api/v1/archives/upload"))
            archive_id = archive.get("id") or archive.get("archive_id")
            if archive_id:
                headers = await self.auth_headers()
                body = {"archive_id": archive_id, **self.store.get_setting("queue_options", {})}
                async with httpx.AsyncClient(timeout=60) as client:
                    queue_response = await client.post(self.bambuddy_url("/api/v1/queue"), headers=headers, json=body)
                if queue_response.status_code >= 400:
                    raise UploadError(502, f"Bambuddy queue failed: HTTP {queue_response.status_code}")
                archive["queue"] = queue_response.json() if queue_response.content else {"status": "queued"}
            return archive
        if mode in {"esp-vp", "auto"}:
            return await self.forward_raw(path, filename, vp_name, source_ip)
        return await self.forward_multipart(
            path,
            filename,
            self.bambuddy_url("/api/v1/library/files"),
            {"folder_id": self.store.get_setting("library_folder_id")},
        )

    async def forward_raw(self, path: Path, filename: str, vp_name: str | None, source_ip: str | None) -> dict[str, Any]:
        headers = await self.auth_headers()
        headers.update({"X-Bambuddy-Filename": filename})
        if vp_name:
            headers["X-Bambuddy-VP-Name"] = vp_name
        if source_ip:
            headers["X-Bambuddy-Source-IP"] = source_ip
        async with httpx.AsyncClient(timeout=300) as client:
            with path.open("rb") as fh:
                response = await client.post(self.bambuddy_url("/api/v1/esp-vp/upload"), headers=headers, content=fh)
        if response.status_code >= 400:
            raise UploadError(502, f"Bambuddy ESP ingest failed: HTTP {response.status_code}")
        return response.json() if response.content else {"status": "forwarded"}

    async def save_upload_body(self, request: Request, filename: str) -> tuple[Path, int]:
        tmp = tempfile.NamedTemporaryFile("wb", prefix="esp-vp-", suffix=".3mf.part", dir=self.temp_dir, delete=False)
        tmp_path = Path(tmp.name)
        total = 0
        try:
            async for chunk in request.stream():
                if not chunk:
                    continue
                tmp.write(chunk)
                total += len(chunk)
                if self.max_upload_bytes and total > self.max_upload_bytes:
                    raise UploadError(413, "Upload exceeds configured maximum size")
        finally:
            tmp.close()
        if total <= 0:
            tmp_path.unlink(missing_ok=True)
            raise UploadError(400, "Upload body is empty")
        return tmp_path, total

    async def discover(self, timeout: float = 1.5) -> dict[str, Any]:
        receiver_url = str(self.store.get_setting("receiver_url", "") or "").strip().rstrip("/")
        receiver_header = f"X-Esp-Vp-Receiver-Url: {receiver_url}\r\n" if receiver_url else ""
        message = (
            "M-SEARCH * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:2021\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "MX: 1\r\n"
            "ST: urn:bambulab-com:device:3dprinter:1\r\n"
            f"{receiver_header}\r\n"
        ).encode()
        found: dict[str, dict[str, Any]] = {}
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(timeout)
        try:
            sock.sendto(message, ("255.255.255.255", 2021))
            deadline = time.time() + timeout
            while time.time() < deadline:
                try:
                    data, addr = sock.recvfrom(4096)
                except socket.timeout:
                    break
                parsed = parse_ssdp(data.decode(errors="replace"), addr[0])
                if parsed:
                    found[parsed["device_id"]] = parsed
        finally:
            sock.close()
        for item in found.values():
            self.store.upsert_device(item)
        return {
            "devices": list(found.values()),
            "receiver_url_sent": receiver_url or None,
            "receiver_url_header_sent": bool(receiver_url),
        }

    def generate_device_cert(self, device_id: str, config: dict[str, Any]) -> tuple[Path, Path]:
        if x509 is None or rsa is None or serialization is None:
            raise HTTPException(500, "cryptography is required for certificate generation")
        ca_cert_path = self.cert_dir / "bbl_ca.crt"
        ca_key_path = self.cert_dir / "bbl_ca.key"
        if not ca_cert_path.exists() or not ca_key_path.exists():
            raise HTTPException(400, "Import a Bambuddy VP CA certificate and key first")
        ca_cert = x509.load_pem_x509_certificate(ca_cert_path.read_bytes())
        ca_key = serialization.load_pem_private_key(ca_key_path.read_bytes(), password=None)
        serial = str(config.get("serial") or device_id)
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, serial)])
        cert = (
            x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(ca_cert.subject)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(datetime.now(timezone.utc))
            .not_valid_after(datetime.now(timezone.utc).replace(year=datetime.now(timezone.utc).year + 10))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.KeyUsage(True, True, False, False, False, False, False, False, False), critical=True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH, ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName(serial)]), critical=False)
            .sign(ca_key, hashes.SHA256())
        )
        target = self.cert_dir / device_id
        target.mkdir(parents=True, exist_ok=True)
        cert_path = target / "printer.crt"
        key_path = target / "printer.key"
        cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM) + ca_cert.public_bytes(serialization.Encoding.PEM))
        key_path.write_bytes(
            key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption())
        )
        key_path.chmod(0o600)
        self.store.add_device_event(device_id, "cert_generation", "success", "Printer certificate generated")
        return cert_path, key_path

    async def probe_device(self, device_id: str) -> dict[str, Any]:
        device = dict(self.store.device(device_id))
        management_url = str(device.get("management_url") or "").rstrip("/")
        if not management_url:
            raise HTTPException(400, "Device has no management URL")
        probe_url = f"{management_url}/api/v1/device/info"
        self.store.add_device_event(device_id, "probe", "running", "Probing ESP management API", {"management_url": management_url})
        try:
            async with httpx.AsyncClient(timeout=httpx.Timeout(8.0, connect=3.0)) as client:
                response = await client.get(probe_url)
        except httpx.ConnectError as exc:
            payload = self.esp_error_payload(device_id, management_url, "probe", "ESP management API unavailable", exc)
            self.store.add_device_event(device_id, "probe", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload) from exc
        except httpx.TimeoutException as exc:
            payload = self.esp_error_payload(device_id, management_url, "probe", "ESP management API timed out", exc)
            self.store.add_device_event(device_id, "probe", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload) from exc
        except httpx.RequestError as exc:
            payload = self.esp_error_payload(device_id, management_url, "probe", "ESP management API request failed", exc)
            self.store.add_device_event(device_id, "probe", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload) from exc
        if response.status_code >= 400:
            payload = self.esp_response_error_payload(device_id, management_url, "probe", response)
            self.store.add_device_event(device_id, "probe", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload)
        body = response.json() if response.content else {"status": "reachable"}
        result = body if isinstance(body, dict) else {"response": body}
        update = {
            "device_id": str(result.get("device_id") or device_id),
            "name": result.get("name") or device.get("name"),
            "ip": result.get("ip") or device.get("ip"),
            "management_url": management_url,
            "firmware_version": result.get("firmware"),
            "configured": bool(result.get("configured", device.get("configured"))),
            "paired": bool(result.get("paired", device.get("paired", False))),
            "pair_ready": bool(result.get("pair_ready", False)),
            "pair_remaining_seconds": int(result.get("pair_remaining_seconds") or 0),
            "receiver_managed": True,
        }
        if update["device_id"] == device_id:
            self.store.upsert_device(update)
        self.store.add_device_event(device_id, "probe", "success", "ESP management API reachable", {"status_code": response.status_code})
        return {"status": "reachable", "device_id": device_id, "management_url": management_url, "esp": result}

    def esp_error_payload(
        self,
        device_id: str,
        management_url: str,
        stage: str,
        detail: str,
        exc: httpx.RequestError,
    ) -> dict[str, Any]:
        return {
            "stage": stage,
            "device_id": device_id,
            "management_url": management_url,
            "detail": detail,
            "hint": "Confirm the ESP is powered on, on this network, and running firmware with the management API on port 8080.",
            "error_type": exc.__class__.__name__,
        }

    def esp_response_error_payload(self, device_id: str, management_url: str, stage: str, response: httpx.Response) -> dict[str, Any]:
        message = f"ESP management API rejected request: HTTP {response.status_code}"
        try:
            body: Any = response.json()
        except ValueError:
            body = response.text[:500] if response.text else None
        return {
            "stage": stage,
            "device_id": device_id,
            "management_url": management_url,
            "detail": message,
            "hint": "The ESP management API responded, but did not accept the request. Check firmware version, device token, and endpoint support.",
            "status_code": response.status_code,
            "response": body,
        }


def parse_ssdp(raw: str, ip: str) -> dict[str, Any] | None:
    headers: dict[str, str] = {}
    for line in raw.splitlines()[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    serial = headers.get("usn", "")
    device_id = headers.get("x-esp-vp-device-id") or serial or f"ip-{ip}"
    if "devmodel.bambu.com" not in headers and "x-esp-vp-device-id" not in headers:
        return None
    location = headers.get("location") or ip
    management_url = headers.get("x-esp-vp-management-url") or f"http://{ip}:8080"
    return {
        "device_id": device_id,
        "name": headers.get("devname.bambu.com") or headers.get("x-esp-vp-name") or f"vp_{device_id[-6:]}",
        "ip": ip,
        "management_url": management_url,
        "firmware_version": headers.get("x-esp-vp-firmware") or headers.get("devversion.bambu.com"),
        "configured": headers.get("x-esp-vp-configured", "false").lower() in {"1", "true", "yes"},
        "paired": headers.get("x-esp-vp-paired", "false").lower() in {"1", "true", "yes"},
        "pair_ready": headers.get("x-esp-vp-pair-ready", "false").lower() in {"1", "true", "yes"},
        "pair_remaining_seconds": int(headers.get("x-esp-vp-pair-remaining-seconds") or 0),
        "receiver_managed": headers.get("x-esp-vp-managed", "false").lower() in {"1", "true", "yes"},
        "location": location,
    }


def get_manager(request: Request) -> Manager:
    return request.app.state.manager


def bearer_token(authorization: str | None) -> str | None:
    if not authorization:
        return None
    prefix = "Bearer "
    if not authorization.startswith(prefix):
        return None
    return authorization[len(prefix) :].strip()


def request_base_url(request: Request) -> str:
    forwarded_proto = request.headers.get("x-forwarded-proto")
    forwarded_host = request.headers.get("x-forwarded-host")
    scheme = forwarded_proto or request.url.scheme
    host = forwarded_host or request.headers.get("host") or request.url.netloc
    return f"{scheme}://{host}".rstrip("/")


async def require_admin(request: Request, manager: Manager = Depends(get_manager)) -> None:
    if manager.store.first_run():
        raise HTTPException(428, "Setup required")
    if not manager.store.valid_session(request.cookies.get("buddy_session")):
        raise HTTPException(401, "Login required")


async def require_device_auth(
    device_id: str | None = None,
    authorization: str | None = Header(default=None),
    manager: Manager = Depends(get_manager),
) -> str | None:
    token = bearer_token(authorization)
    if not manager.store.verify_device_token(token, device_id):
        raise HTTPException(401, "Invalid device token")
    return token


UI_HTML = r"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP VP Manager</title>
  <style>
    :root{color-scheme:light dark;--bg:#f4f6f8;--panel:#fff;--text:#17202a;--muted:#65717d;--line:#d8dee5;--accent:#0b7285;--bad:#b42318}
    @media (prefers-color-scheme: dark){:root{--bg:#101418;--panel:#171d23;--text:#edf2f7;--muted:#9aa6b2;--line:#2a333d;--accent:#4cc9d8;--bad:#ff8a80}}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
    header{height:56px;display:flex;align-items:center;justify-content:space-between;padding:0 20px;border-bottom:1px solid var(--line);background:var(--panel)}
    main{display:grid;grid-template-columns:220px 1fr;min-height:calc(100vh - 56px)}nav{border-right:1px solid var(--line);padding:16px;background:var(--panel)}
    button,input,select,textarea{font:inherit}button{border:1px solid var(--line);background:var(--panel);color:var(--text);border-radius:6px;padding:8px 10px;cursor:pointer}
    button.primary{background:var(--accent);border-color:var(--accent);color:white}.danger{color:var(--bad)}nav button{display:block;width:100%;text-align:left;margin-bottom:6px}
    section{padding:20px;max-width:1180px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px}
    label{display:block;color:var(--muted);font-size:12px;margin-top:10px}input,select,textarea{width:100%;border:1px solid var(--line);border-radius:6px;background:var(--panel);color:var(--text);padding:8px;margin-top:4px}
    table{width:100%;border-collapse:collapse;background:var(--panel);border:1px solid var(--line)}td,th{border-bottom:1px solid var(--line);padding:8px;text-align:left;vertical-align:top}th{color:var(--muted);font-weight:600}
    pre{white-space:pre-wrap;background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:12px;max-height:360px;overflow:auto}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.muted{color:var(--muted)}.hidden{display:none}
    @media (max-width:760px){main{display:block}nav{display:flex;overflow:auto;border-right:0;border-bottom:1px solid var(--line)}nav button{width:auto;white-space:nowrap}}
  </style>
</head>
<body>
<header><strong>ESP VP Manager</strong><div class="row"><span id="status" class="muted"></span><button onclick="logout()">Log out</button></div></header>
<main><nav id="nav"></nav><section id="view"></section></main>
<script>
let state={}, tab='dashboard', selectedDevice=null;
const navItems=['dashboard','devices','settings','certificates','uploads','logs'];
const $=s=>document.querySelector(s);
function esc(v){return String(v??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function api(path, opts={}){const r=await fetch(path,{headers:{'Content-Type':'application/json',...(opts.headers||{})},...opts});if(r.status===428||r.status===401){renderLogin(r.status===428);throw new Error('auth')}if(!r.ok)throw new Error(await r.text());return r.headers.get('content-type')?.includes('json')?r.json():r.text()}
function readRoute(){const parts=(location.hash.replace(/^#/,'')||'dashboard').split('/');tab=navItems.includes(parts[0])?parts[0]:'dashboard';selectedDevice=tab==='devices'&&parts[1]?decodeURIComponent(parts[1]):null}
function go(route){location.hash=route}
async function refresh(){readRoute();state=await api('/api/state');$('#status').textContent=state.first_run?'setup required':`${state.devices.length} device(s)`;render()}
function renderNav(){document.getElementById('nav').innerHTML=navItems.map(i=>`<button class="${tab===i?'primary':''}" onclick="go('${i}')">${i[0].toUpperCase()+i.slice(1)}</button>`).join('')}
function renderLogin(setup){document.body.innerHTML=`<main style="display:block;max-width:420px;margin:12vh auto;min-height:0"><div class="card"><h2>${setup?'First-run setup':'Login'}</h2><label>Username<input id="u" autocomplete="username"></label><label>Password<input id="p" type="password" autocomplete="${setup?'new-password':'current-password'}"></label><button class="primary" onclick="${setup?'setup()':'login()'}">${setup?'Create admin':'Log in'}</button></div></main>`}
async function setup(){await api('/api/setup',{method:'POST',body:JSON.stringify({username:$('#u').value,password:$('#p').value})});location.reload()}
async function login(){await api('/api/login',{method:'POST',body:JSON.stringify({username:$('#u').value,password:$('#p').value})});location.reload()}
async function logout(){await api('/api/logout',{method:'POST'});location.reload()}
function render(){readRoute();renderNav();if(tab==='dashboard')renderDashboard();if(tab==='devices')renderDevices();if(tab==='settings')renderSettings();if(tab==='certificates')renderCerts();if(tab==='uploads')renderUploads();if(tab==='logs')renderLogs()}
function renderDashboard(){view.innerHTML=`<h2>Dashboard</h2><div class="grid"><div class="card"><b>Receiver</b><p class="muted">Version ${esc(state.version)}</p><p>Forward mode: ${esc(state.settings.forward_mode)}</p><p>Bambuddy: ${esc(state.settings.bambuddy_url)}</p></div><div class="card"><b>Enrollment key</b><p class="muted">Use this in manager-mode firmware builds.</p><pre>${esc(state.settings.enrollment_key)}</pre></div></div>`}
async function discover(){await api('/api/discover',{method:'POST'});await refresh()}
async function claim(id){const r=await api(`/api/devices/${encodeURIComponent(id)}/claim`,{method:'POST'});alert(`Device token:\n${r.device_token}`);await refresh()}
function renderDevices(){view.innerHTML=`<div class="row"><h2>Devices</h2><button class="primary" onclick="discover()">Discover</button></div><table><tr><th>Name</th><th>Device ID</th><th>IP</th><th>State</th><th></th></tr>${state.devices.map(d=>`<tr><td>${esc(d.name)}</td><td>${esc(d.device_id)}</td><td>${esc(d.ip)}</td><td>${d.claimed?'claimed':'unclaimed'} ${d.configured?'configured':''}</td><td><button onclick="go('devices/${encodeURIComponent(d.device_id)}')">Open</button>${d.claimed?'':` <button onclick="claim('${esc(d.device_id)}')">Claim</button>`}</td></tr>`).join('')}</table><div id="deviceDetail"></div>`;if(selectedDevice)renderDevice()}
function modelOptions(selected){return Object.entries(state.models||{}).map(([code,m])=>`<option value="${esc(code)}" ${code==selected?'selected':''}>${esc(m.display)} (${esc(code)})</option>`).join('')}
function autoSerialValue(modelCode,deviceId){const model=state.models[modelCode]||state.models.C12||{serial_prefix:'01P00A'};let h=0;for(const ch of deviceId){h=((h*31)+ch.charCodeAt(0))%1000000000}return `${model.serial_prefix}${String(h).padStart(9,'0')}`}
function fillSerial(force=false){const el=$('#sn');if(force||!el.value||el.dataset.auto==='1'){el.value=autoSerialValue($('#mc').value,selectedDevice||'vp');el.dataset.auto='1'}}
function devicePayload(){const model=state.models[$('#mc').value]||{};return{name:$('#dn').value,model_code:$('#mc').value,product_name:model.product_name||$('#mc').value,serial:$('#sn').value,access_code:$('#ac').value,mode:$('#mode').value,generate_cert:$('#genCert').checked}}
async function renderDevice(){const d=await api(`/api/devices/${encodeURIComponent(selectedDevice)}`);const c=d.config||{};const selected=c.model_code||'C12';const certCtl=state.ca_imported?`<label><input id="genCert" type="checkbox" checked style="width:auto;margin-right:6px">Generate and include printer cert/key</label>`:`<label><input id="genCert" type="checkbox" disabled style="width:auto;margin-right:6px">Import CA first to generate printer cert/key</label>`;document.getElementById('deviceDetail').innerHTML=`<h3>${esc(d.device.name)}</h3><div class="grid"><div class="card"><label>Name<input id="dn" value="${esc(d.device.name)}"></label><label>Model<select id="mc" onchange="fillSerial(false)">${modelOptions(selected)}</select></label><label>Serial<div class="row"><input id="sn" value="${esc(c.serial||'')}" oninput="this.dataset.auto='0'" style="flex:1;min-width:180px"><button onclick="fillSerial(true)">Auto</button></div></label><label>Access code<input id="ac" value="${esc(c.access_code||'12345678')}" maxlength="8"></label><label>Mode<select id="mode"><option>immediate</option><option>print_queue</option><option>library</option><option>proxy_status</option></select></label>${certCtl}<div class="row"><button class="primary" onclick="saveAndPushDevice()">Save + push</button><button onclick="saveDevice()">Save only</button><button onclick="pushDevice()">Push saved</button></div><p class="muted">Push sends this config and cert/key to ${esc(d.device.management_url||'the ESP management API')}.</p></div><pre>${esc(JSON.stringify(d,null,2))}</pre></div>`;$('#mode').value=c.mode||state.settings.forward_mode||'immediate';if(!c.serial)fillSerial(true)}
async function saveDevice(){await api(`/api/devices/${encodeURIComponent(selectedDevice)}/config`,{method:'POST',body:JSON.stringify(devicePayload())});await refresh();await renderDevice()}
async function pushDevice(){await api(`/api/devices/${encodeURIComponent(selectedDevice)}/push-config`,{method:'POST'});alert('Config pushed to ESP')}
async function saveAndPushDevice(){await api(`/api/devices/${encodeURIComponent(selectedDevice)}/config`,{method:'POST',body:JSON.stringify(devicePayload())});await api(`/api/devices/${encodeURIComponent(selectedDevice)}/push-config`,{method:'POST'});alert('Config and cert pushed to ESP');await refresh();await renderDevice()}
function renderSettings(){const s=state.settings;view.innerHTML=`<h2>Bambuddy connection</h2><div class="grid"><div class="card"><label>URL<input id="bu" value="${esc(s.bambuddy_url)}"></label><label>Forward mode<select id="fm"><option>immediate</option><option>print_queue</option><option>library</option><option>proxy_status</option></select></label><label>API key<input id="ak" value="${esc(s.api_key||'')}"></label><label>Bearer token<input id="bt" value="${esc(s.bearer_token||'')}"></label><label>Username<input id="un" value="${esc(s.username||'')}"></label><label>Password<input id="pw" type="password" value="${esc(s.password||'')}"></label><button class="primary" onclick="saveSettings()">Save</button></div></div>`;$('#fm').value=s.forward_mode||'library'}
async function saveSettings(){await api('/api/settings',{method:'POST',body:JSON.stringify({bambuddy_url:$('#bu').value,forward_mode:$('#fm').value,api_key:$('#ak').value,bearer_token:$('#bt').value,username:$('#un').value,password:$('#pw').value})});await refresh()}
function renderCerts(){view.innerHTML=`<h2>Certificates</h2><div class="card"><label>CA certificate PEM<textarea id="cc" rows="8"></textarea></label><label>CA private key PEM<textarea id="ck" rows="8"></textarea></label><button class="primary" onclick="importCert()">Import CA</button></div>`}
async function importCert(){await api('/api/certificates/ca',{method:'POST',body:JSON.stringify({cert_pem:$('#cc').value,key_pem:$('#ck').value})});alert('Imported')}
function renderUploads(){view.innerHTML=`<h2>Upload history</h2><table><tr><th>Time</th><th>File</th><th>Bytes</th><th>Mode</th><th>Status</th></tr>${state.uploads.map(u=>`<tr><td>${esc(u.created_at)}</td><td>${esc(u.filename)}</td><td>${u.bytes}</td><td>${esc(u.forward_mode)}</td><td>${esc(u.status||u.error)}</td></tr>`).join('')}</table>`}
function renderLogs(){view.innerHTML=`<h2>Status</h2><pre>${esc(JSON.stringify(state,null,2))}</pre>`}
window.addEventListener('hashchange',()=>render());
if(!location.hash)location.hash='dashboard';
refresh().catch(()=>{});
</script>
</body></html>"""


@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    app.state.manager.store.conn.close()


def create_app(args: argparse.Namespace) -> FastAPI:
    app = FastAPI(title="Bambuddy ESP VP Manager", version=APP_VERSION, lifespan=lifespan)
    app.state.manager = Manager(args)
    ui_dist = Path(__file__).resolve().parent / "ui" / "dist"
    if ui_dist.exists():
        assets_dir = ui_dist / "assets"
        if assets_dir.exists():
            app.mount("/assets", StaticFiles(directory=assets_dir), name="ui-assets")

    @app.get("/", response_class=HTMLResponse)
    async def ui():
        index = ui_dist / "index.html"
        if index.exists():
            return FileResponse(index)
        return UI_HTML

    @app.get("/health")
    async def health(manager: Manager = Depends(get_manager)) -> dict[str, Any]:
        return {
            "status": "ok",
            "version": APP_VERSION,
            "first_run": manager.store.first_run(),
            "bambuddy_url": manager.store.get_setting("bambuddy_url"),
            "forward_mode": manager.store.get_setting("forward_mode"),
        }

    @app.post("/api/setup")
    async def setup(payload: dict[str, str], response: Response, manager: Manager = Depends(get_manager)) -> dict[str, str]:
        if not manager.store.first_run():
            raise HTTPException(409, "Setup already completed")
        manager.store.create_admin(payload.get("username", ""), payload.get("password", ""))
        token = manager.store.create_session()
        response.set_cookie("buddy_session", token, httponly=True, samesite="lax", max_age=SESSION_TTL_SECONDS)
        return {"status": "ok"}

    @app.post("/api/login")
    async def login(payload: dict[str, str], response: Response, manager: Manager = Depends(get_manager)) -> dict[str, str]:
        if not manager.store.authenticate(payload.get("username", ""), payload.get("password", "")):
            raise HTTPException(401, "Invalid credentials")
        token = manager.store.create_session()
        response.set_cookie("buddy_session", token, httponly=True, samesite="lax", max_age=SESSION_TTL_SECONDS)
        return {"status": "ok"}

    @app.post("/api/logout")
    async def logout(request: Request, response: Response, manager: Manager = Depends(get_manager)) -> dict[str, str]:
        manager.store.delete_session(request.cookies.get("buddy_session"))
        response.delete_cookie("buddy_session")
        return {"status": "ok"}

    @app.get("/api/state", dependencies=[Depends(require_admin)])
    async def state(manager: Manager = Depends(get_manager)) -> dict[str, Any]:
        settings = manager.store.settings_dict()
        visible = {k: v for k, v in settings.items() if k not in {"password", "bearer_token"}}
        visible["password"] = "***" if settings.get("password") else ""
        visible["bearer_token"] = "***" if settings.get("bearer_token") else ""
        return {
            "version": APP_VERSION,
            "first_run": False,
            "settings": visible,
            "models": MODELS,
            "ca_imported": (manager.cert_dir / "bbl_ca.crt").exists() and (manager.cert_dir / "bbl_ca.key").exists(),
            "devices": manager.store.devices(),
            "uploads": manager.store.uploads(),
        }

    @app.post("/api/settings", dependencies=[Depends(require_admin)])
    async def save_settings(payload: dict[str, Any], manager: Manager = Depends(get_manager)) -> dict[str, str]:
        for key in ("bambuddy_url", "receiver_url", "api_key", "bearer_token", "username", "password", "forward_mode", "printer_id", "library_folder_id", "queue_options"):
            if key in payload and payload[key] != "***":
                if key == "forward_mode" and payload[key] not in FORWARD_MODES:
                    raise HTTPException(400, "Invalid forward mode")
                value = payload[key].rstrip("/") if key in {"bambuddy_url", "receiver_url"} and isinstance(payload[key], str) else payload[key]
                manager.store.set_setting(key, value)
        return {"status": "ok"}

    @app.post("/api/settings/test-bambuddy", dependencies=[Depends(require_admin)])
    async def test_bambuddy_settings(payload: dict[str, Any], manager: Manager = Depends(get_manager)) -> dict[str, Any]:
        return await manager.test_bambuddy_host(str(payload.get("bambuddy_url") or ""))

    @app.get("/api/bambuddy/printers", dependencies=[Depends(require_admin)])
    async def bambuddy_printers(manager: Manager = Depends(get_manager)) -> list[dict[str, Any]]:
        return await manager.list_bambuddy_printers()

    @app.post("/api/discover", dependencies=[Depends(require_admin)])
    async def discover(manager: Manager = Depends(get_manager)) -> dict[str, Any]:
        return await asyncio.to_thread(lambda: asyncio.run(manager.discover()))

    @app.get("/api/devices", dependencies=[Depends(require_admin)])
    async def devices(manager: Manager = Depends(get_manager)) -> list[dict[str, Any]]:
        return manager.store.devices()

    @app.get("/api/devices/{device_id}", dependencies=[Depends(require_admin)])
    async def device(device_id: str, manager: Manager = Depends(get_manager)) -> dict[str, Any]:
        return {
            "device": dict(manager.store.device(device_id)),
            "config": manager.store.device_config(device_id),
            "events": manager.store.device_events(device_id),
        }

    @app.get("/api/devices/{device_id}/events", dependencies=[Depends(require_admin)])
    async def device_events(device_id: str, manager: Manager = Depends(get_manager)) -> list[dict[str, Any]]:
        return manager.store.device_events(device_id)

    @app.delete("/api/devices/{device_id}", dependencies=[Depends(require_admin)])
    async def delete_device(device_id: str, manager: Manager = Depends(get_manager)) -> dict[str, str]:
        manager.store.delete_device(device_id)
        return {"status": "deleted"}

    @app.post("/api/devices/{device_id}/pair", dependencies=[Depends(require_admin)])
    async def pair_device(device_id: str, request: Request, manager: Manager = Depends(get_manager)) -> dict[str, Any]:
        device = dict(manager.store.device(device_id))
        if not device.get("management_url"):
            raise HTTPException(400, "Device has no management URL")
        management_url = str(device["management_url"]).rstrip("/")
        probe = await manager.probe_device(device_id)
        esp = probe.get("esp") if isinstance(probe.get("esp"), dict) else {}
        esp_device_id = str(esp.get("device_id") or device_id)
        if esp_device_id != device_id:
            raise HTTPException(409, f"ESP reported device_id {esp_device_id}, expected {device_id}")
        if not bool(esp.get("pair_ready")):
            manager.store.add_device_event(
                device_id,
                "pair",
                "failure",
                "ESP is not in pair mode",
                {"management_url": management_url, "hint": "Hold BOOT for 5 seconds until the RGB LED blinks cyan, then pair again."},
            )
            raise HTTPException(409, "Hold BOOT for 5 seconds until the RGB LED blinks cyan, then pair again.")

        configured_receiver_url = str(manager.store.get_setting("receiver_url", "") or "").strip()
        receiver_url = (configured_receiver_url or request_base_url(request)).rstrip("/")
        token = secrets.token_urlsafe(32)
        payload = {
            "receiver_url": receiver_url,
            "upload_url": f"{receiver_url}/api/v1/esp-vp/upload",
            "receiver_token": token,
        }
        manager.store.add_device_event(device_id, "pair", "running", "Pairing ESP with VP Manager", {"management_url": management_url})
        try:
            async with httpx.AsyncClient(timeout=httpx.Timeout(12.0, connect=4.0)) as client:
                response = await client.post(f"{management_url}/api/v1/device/pair", json=payload)
        except httpx.ConnectError as exc:
            error = manager.esp_error_payload(device_id, management_url, "pair", "ESP management API unavailable", exc)
            manager.store.add_device_event(device_id, "pair", "failure", error["detail"], error)
            raise HTTPException(status_code=502, detail=error) from exc
        except httpx.TimeoutException as exc:
            error = manager.esp_error_payload(device_id, management_url, "pair", "ESP management API timed out", exc)
            manager.store.add_device_event(device_id, "pair", "failure", error["detail"], error)
            raise HTTPException(status_code=502, detail=error) from exc
        except httpx.RequestError as exc:
            error = manager.esp_error_payload(device_id, management_url, "pair", "ESP management API request failed", exc)
            manager.store.add_device_event(device_id, "pair", "failure", error["detail"], error)
            raise HTTPException(status_code=502, detail=error) from exc
        if response.status_code >= 400:
            error = manager.esp_response_error_payload(device_id, management_url, "pair", response)
            manager.store.add_device_event(device_id, "pair", "failure", error["detail"], error)
            raise HTTPException(status_code=502, detail=error)

        manager.store.set_device_token(device_id, token)
        manager.store.upsert_device(
            {
                "device_id": device_id,
                "name": device.get("name"),
                "ip": device.get("ip"),
                "management_url": management_url,
                "firmware_version": device.get("firmware_version"),
                "configured": device.get("configured"),
                "paired": True,
                "pair_ready": False,
                "pair_remaining_seconds": 0,
                "receiver_managed": True,
            },
            claimed=True,
        )
        manager.store.save_device_config(device_id, payload)
        body = response.json() if response.content else {"status": "paired"}
        return body if isinstance(body, dict) else {"status": "paired", "response": body}

    @app.post("/api/devices/{device_id}/claim", dependencies=[Depends(require_admin)])
    async def claim(device_id: str, manager: Manager = Depends(get_manager)) -> dict[str, str]:
        manager.store.device(device_id)
        raise HTTPException(410, "Use physical pair mode instead of reclaim")

    @app.post("/api/devices/{device_id}/config", dependencies=[Depends(require_admin)])
    async def config_device(
        device_id: str,
        payload: dict[str, Any],
        request: Request,
        manager: Manager = Depends(get_manager),
    ) -> dict[str, str]:
        device = dict(manager.store.device(device_id))
        if payload.get("name"):
            manager.store.upsert_device({"device_id": device_id, "name": payload["name"]})
        model_code = str(payload.get("model_code") or "C12")
        if model_code not in MODELS:
            raise HTTPException(400, "Invalid model code")
        payload["model_code"] = model_code
        payload["product_name"] = payload.get("product_name") or MODELS[model_code]["product_name"]
        if not str(payload.get("serial") or "").strip():
            payload["serial"] = generated_serial(model_code, device_id)
        configured_receiver_url = str(manager.store.get_setting("receiver_url", "") or "").strip()
        base_url = str(payload.get("receiver_url") or configured_receiver_url or request_base_url(request)).rstrip("/")
        payload["receiver_url"] = base_url
        payload["upload_url"] = f"{base_url}/api/v1/esp-vp/upload"
        if device.get("device_token"):
            payload["receiver_token"] = device["device_token"]
        cert_path = key_path = None
        if payload.pop("generate_cert", False):
            cert_path, key_path = manager.generate_device_cert(device_id, payload)
            payload["tls_cert_pem"] = cert_path.read_text()
            payload["tls_key_pem"] = key_path.read_text()
        manager.store.save_device_config(device_id, payload, cert_path, key_path)
        return {"status": "ok"}

    @app.post("/api/devices/{device_id}/probe", dependencies=[Depends(require_admin)])
    async def probe_device(device_id: str, manager: Manager = Depends(get_manager)) -> dict[str, Any]:
        return await manager.probe_device(device_id)

    @app.post("/api/devices/{device_id}/push-config", dependencies=[Depends(require_admin)])
    async def push_config(device_id: str, manager: Manager = Depends(get_manager)) -> dict[str, Any]:
        device = dict(manager.store.device(device_id))
        config = manager.store.device_config(device_id)
        if not device.get("management_url"):
            raise HTTPException(400, "Device has no management URL")
        if not config:
            raise HTTPException(400, "Device config has not been saved")
        device_token = device.get("device_token")
        if not device_token:
            raise HTTPException(400, "Pair the device before pushing config")
        management_url = str(device["management_url"]).rstrip("/")
        await manager.probe_device(device_id)
        manager.store.add_device_event(device_id, "push_start", "running", "Pushing configuration to ESP", {"management_url": management_url})
        try:
            async with httpx.AsyncClient(timeout=httpx.Timeout(20.0, connect=5.0)) as client:
                response = await client.post(
                    f"{management_url}/api/v1/device/config",
                    headers={"Authorization": f"Bearer {device_token}"},
                    json=config,
                )
        except httpx.ConnectError as exc:
            payload = manager.esp_error_payload(device_id, management_url, "push", "ESP management API unavailable", exc)
            manager.store.add_device_event(device_id, "push_failure", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload) from exc
        except httpx.TimeoutException as exc:
            payload = manager.esp_error_payload(device_id, management_url, "push", "ESP management API timed out", exc)
            manager.store.add_device_event(device_id, "push_failure", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload) from exc
        except httpx.RequestError as exc:
            payload = manager.esp_error_payload(device_id, management_url, "push", "ESP management API request failed", exc)
            manager.store.add_device_event(device_id, "push_failure", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload) from exc
        if response.status_code >= 400:
            payload = manager.esp_response_error_payload(device_id, management_url, "push", response)
            manager.store.add_device_event(device_id, "push_failure", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload)
        payload = response.json() if response.content else {"status": "pushed"}
        manager.store.mark_device_configured(device_id, True)
        manager.store.add_device_event(device_id, "push_success", "success", "Configuration pushed to ESP", {"status_code": response.status_code})
        return payload if isinstance(payload, dict) else {"status": "pushed", "response": payload}

    @app.post("/api/devices/{device_id}/firmware", dependencies=[Depends(require_admin)])
    async def update_firmware(
        device_id: str,
        firmware: UploadFile = File(...),
        manager: Manager = Depends(get_manager),
    ) -> dict[str, Any]:
        device = dict(manager.store.device(device_id))
        if not device.get("management_url"):
            raise HTTPException(400, "Device has no management URL")
        device_token = device.get("device_token")
        if not device_token:
            raise HTTPException(400, "Pair the device before updating firmware")
        filename = safe_filename(firmware.filename or "firmware.bin")
        if not filename.endswith(".bin"):
            raise HTTPException(400, "Firmware must be a .bin file")
        data = await firmware.read()
        if not data:
            raise HTTPException(400, "Firmware file is empty")
        if len(data) > 4 * 1024 * 1024:
            raise HTTPException(413, "Firmware file is too large")

        management_url = str(device["management_url"]).rstrip("/")
        manager.store.add_device_event(
            device_id,
            "firmware_update",
            "running",
            "Uploading firmware to ESP OTA slot",
            {"management_url": management_url, "filename": filename, "bytes": len(data)},
        )
        try:
            async with httpx.AsyncClient(timeout=httpx.Timeout(90.0, connect=5.0)) as client:
                response = await client.post(
                    f"{management_url}/api/v1/device/ota",
                    headers={
                        "Authorization": f"Bearer {device_token}",
                        "Content-Type": "application/octet-stream",
                        "X-Firmware-Filename": filename,
                    },
                    content=data,
                )
        except httpx.ConnectError as exc:
            payload = manager.esp_error_payload(device_id, management_url, "firmware_update", "ESP management API unavailable", exc)
            manager.store.add_device_event(device_id, "firmware_update", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload) from exc
        except httpx.TimeoutException as exc:
            payload = manager.esp_error_payload(device_id, management_url, "firmware_update", "ESP firmware update timed out", exc)
            manager.store.add_device_event(device_id, "firmware_update", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload) from exc
        except httpx.RequestError as exc:
            payload = manager.esp_error_payload(device_id, management_url, "firmware_update", "ESP firmware update request failed", exc)
            manager.store.add_device_event(device_id, "firmware_update", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload) from exc
        if response.status_code >= 400:
            payload = manager.esp_response_error_payload(device_id, management_url, "firmware_update", response)
            manager.store.add_device_event(device_id, "firmware_update", "failure", payload["detail"], payload)
            raise HTTPException(status_code=502, detail=payload)
        payload = response.json() if response.content else {"status": "ota_applied", "rebooting": True}
        manager.store.add_device_event(
            device_id,
            "firmware_update",
            "success",
            "Firmware uploaded; ESP is rebooting",
            {"status_code": response.status_code, "filename": filename, "bytes": len(data)},
        )
        return payload if isinstance(payload, dict) else {"status": "ota_applied", "response": payload}

    @app.post("/api/certificates/ca", dependencies=[Depends(require_admin)])
    async def import_ca(payload: dict[str, str], manager: Manager = Depends(get_manager)) -> dict[str, str]:
        cert = payload.get("cert_pem", "")
        key = payload.get("key_pem", "")
        if "BEGIN CERTIFICATE" not in cert or "BEGIN" not in key:
            raise HTTPException(400, "PEM certificate and private key are required")
        (manager.cert_dir / "bbl_ca.crt").write_text(cert)
        key_path = manager.cert_dir / "bbl_ca.key"
        key_path.write_text(key)
        key_path.chmod(0o600)
        return {"status": "ok"}

    @app.post("/api/v1/devices/enroll")
    async def enroll(
        payload: dict[str, Any],
        authorization: str | None = Header(default=None),
        manager: Manager = Depends(get_manager),
    ) -> dict[str, Any]:
        require_token = bearer_token(authorization)
        if not manager.store.verify_device_token(require_token, str(payload.get("device_id") or "")):
            raise HTTPException(401, "Invalid enrollment token")
        device = manager.store.upsert_device(payload)
        token = None
        if not device["claimed"]:
            token = secrets.token_urlsafe(32)
            manager.store.set_device_token(device["device_id"], token)
        return {"status": "enrolled", "device_token": token, "config": manager.store.device_config(device["device_id"])}

    @app.post("/api/v1/devices/{device_id}/heartbeat")
    async def heartbeat(
        device_id: str,
        payload: dict[str, Any],
        _token: str | None = Depends(require_device_auth),
        manager: Manager = Depends(get_manager),
    ) -> dict[str, Any]:
        payload["device_id"] = device_id
        manager.store.upsert_device(payload)
        return {"status": "ok", "config": manager.store.device_config(device_id)}

    @app.post("/api/v1/devices/{device_id}/status-snapshot")
    async def status_snapshot(
        device_id: str,
        payload: dict[str, Any],
        _token: str | None = Depends(require_device_auth),
        manager: Manager = Depends(get_manager),
    ) -> dict[str, str]:
        manager.store.device(device_id)
        manager.store.conn.execute(
            "INSERT INTO proxy_status(device_id,status_json,updated_at) VALUES(?,?,?) ON CONFLICT(device_id) DO UPDATE SET status_json=excluded.status_json, updated_at=excluded.updated_at",
            (device_id, json_dumps(payload), now_iso()),
        )
        manager.store.conn.commit()
        return {"status": "ok"}

    @app.get("/api/v1/devices/{device_id}/proxy-status")
    async def proxy_status(
        device_id: str,
        _token: str | None = Depends(require_device_auth),
        manager: Manager = Depends(get_manager),
    ) -> dict[str, Any]:
        config = manager.store.device_config(device_id) or {}
        if str(config.get("mode") or "") != "proxy_status":
            raise HTTPException(409, "Device is not configured for proxy_status mode")
        raw_printer_id = config.get("paired_printer_id") or config.get("target_printer_id") or config.get("printer_id")
        if raw_printer_id in (None, ""):
            raise HTTPException(400, "Device has no paired Bambuddy printer")
        try:
            printer_id = int(raw_printer_id)
        except (TypeError, ValueError) as exc:
            raise HTTPException(400, "Invalid paired Bambuddy printer id") from exc
        try:
            bambuddy_status = await manager.fetch_bambuddy_printer_status(printer_id)
            report = manager.status_to_bambu_report(bambuddy_status)
            payload = {
                "status": "ok",
                "stale": False,
                "device_id": device_id,
                "paired_printer_id": printer_id,
                "bambuddy_status": bambuddy_status,
                "report": report,
                "updated_at": now_iso(),
            }
            manager.store.set_proxy_status(device_id, payload)
            return payload
        except UploadError as exc:
            cached = manager.store.get_proxy_status(device_id)
            if cached:
                cached["status"] = "stale"
                cached["stale"] = True
                cached["error"] = exc.detail
                return cached
            raise HTTPException(exc.status, exc.detail) from exc

    @app.post("/api/v1/esp-vp/upload")
    async def esp_upload(
        request: Request,
        x_bambuddy_filename: str | None = Header(default=None),
        x_bambuddy_vp_name: str | None = Header(default=None),
        x_bambuddy_source_ip: str | None = Header(default=None),
        x_esp_vp_device_id: str | None = Header(default=None),
        manager: Manager = Depends(get_manager),
    ) -> JSONResponse:
        tmp_path: Path | None = None
        filename = "unknown.3mf"
        total = 0
        upload_id = uuid.uuid4().hex
        try:
            filename = safe_3mf_filename(x_bambuddy_filename)
            tmp_path, total = await manager.save_upload_body(request, filename)
            result = await manager.forward_upload(tmp_path, filename, x_bambuddy_vp_name, x_bambuddy_source_ip)
            manager.store.add_upload(
                {
                    "id": upload_id,
                    "filename": filename,
                    "bytes": total,
                    "source_ip": x_bambuddy_source_ip,
                    "vp_name": x_bambuddy_vp_name,
                    "device_id": x_esp_vp_device_id,
                    "forward_mode": manager.store.get_setting("forward_mode", "library"),
                    "status": "forwarded",
                    "result_json": json_dumps(result),
                    "error": None,
                    "created_at": now_iso(),
                }
            )
            return JSONResponse({"status": "forwarded", "filename": filename, "bytes": total, "upload_id": result.get("id") or result.get("archive_id"), "bambuddy": result})
        except UploadError as exc:
            manager.store.add_upload(
                {
                    "id": upload_id,
                    "filename": filename,
                    "bytes": total,
                    "source_ip": x_bambuddy_source_ip,
                    "vp_name": x_bambuddy_vp_name,
                    "device_id": x_esp_vp_device_id,
                    "forward_mode": manager.store.get_setting("forward_mode", "library"),
                    "status": "failed",
                    "result_json": None,
                    "error": exc.detail,
                    "created_at": now_iso(),
                }
            )
            return JSONResponse({"detail": exc.detail}, status_code=exc.status)
        finally:
            if tmp_path is not None:
                tmp_path.unlink(missing_ok=True)

    @app.get("/{path:path}", response_class=HTMLResponse)
    async def ui_fallback(path: str):
        if path.startswith("api/"):
            raise HTTPException(404, "Not found")
        index = ui_dist / "index.html"
        if index.exists():
            return FileResponse(index)
        return UI_HTML

    return app


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the Bambuddy ESP VP manager")
    parser.add_argument("--host", default=os.getenv("BUDDY_RECV_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.getenv("BUDDY_RECV_PORT", "8001")))
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR)
    parser.add_argument("--temp-dir", type=Path, default=Path(os.getenv("BUDDY_RECV_TEMP_DIR", DEFAULT_DATA_DIR / "tmp")))
    parser.add_argument("--bambuddy-url", default=os.getenv("BAMBUDDY_URL", "http://127.0.0.1:8000"))
    parser.add_argument("--receiver-url", default=os.getenv("BUDDY_RECV_PUBLIC_URL", ""))
    parser.add_argument("--api-key", default=os.getenv("BAMBUDDY_API_KEY", ""))
    parser.add_argument("--bearer-token", default=os.getenv("BAMBUDDY_BEARER_TOKEN", ""))
    parser.add_argument("--username", default=os.getenv("BAMBUDDY_USERNAME", ""))
    parser.add_argument("--password", default=os.getenv("BAMBUDDY_PASSWORD", ""))
    parser.add_argument("--enrollment-key", default=os.getenv("BUDDY_RECV_ENROLLMENT_KEY", ""))
    parser.add_argument("--max-upload-bytes", type=int, default=int(os.getenv("BUDDY_RECV_MAX_UPLOAD_BYTES", "0")))
    parser.add_argument("--printer-id", type=int, default=int(os.environ["BUDDY_RECV_PRINTER_ID"]) if os.getenv("BUDDY_RECV_PRINTER_ID") else None)
    parser.add_argument("--library-folder-id", type=int, default=int(os.environ["BUDDY_RECV_LIBRARY_FOLDER_ID"]) if os.getenv("BUDDY_RECV_LIBRARY_FOLDER_ID") else None)
    parser.add_argument("--forward-mode", choices=sorted(FORWARD_MODES), default=os.getenv("BUDDY_RECV_FORWARD_MODE", "library"))
    return parser.parse_args()


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s [%(name)s] %(message)s")
    args = parse_args()
    args.data_dir.mkdir(parents=True, exist_ok=True)
    app = create_app(args)
    LOG.info("ESP VP manager listening on %s:%s data=%s", args.host, args.port, args.data_dir)
    uvicorn.run(app, host=args.host, port=args.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
