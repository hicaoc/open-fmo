#!/usr/bin/env python3
"""Publish Open FMO firmware to OTA server via MCP protocol.

Usage:
  python publish_ota.py                    # publish current build (stable)
  python publish_ota.py --channel beta     # publish to beta channel
  python publish_ota.py --version 0.2.0    # override version
  python publish_ota.py --notes "fix xxx"  # add release notes
  python publish_ota.py --verify-only      # check if already published

Environment (optional, overrides defaults):
  OTA_SERVER_URL   Default: https://ota.nrlptt.com/nrlota/api
  OTA_ADMIN_TOKEN  Default: admin
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

# ─── Configuration ────────────────────────────────────────────────────────────
BOARD_ID = "open_fmo"
CHIP_FAMILY = "esp32s3"
DEFAULT_SERVER = "https://ota.nrlptt.com/nrlota/api"
DEFAULT_TOKEN = "admin"
MCP_PROTOCOL_VERSION = "2025-11-25"

# Resolve project paths relative to this script
SCRIPT_DIR = Path(__file__).resolve().parent
FIRMWARE_DIR = SCRIPT_DIR.parent / "firmware"
BUILD_DIR = FIRMWARE_DIR / "build"
VERSION_H = FIRMWARE_DIR / "main" / "version.h"


# ─── MCP Client ───────────────────────────────────────────────────────────────
class MCPError(RuntimeError):
    pass


class MCPClient:
    def __init__(self, server: str, token: str) -> None:
        server = server.rstrip("/")
        self.url = server if server.endswith("/mcp") else server + "/mcp"
        parsed = urllib.parse.urlsplit(self.url)
        self.origin = urllib.parse.urlunsplit(
            (parsed.scheme, parsed.netloc, "", "", "")
        )
        self.token = token
        self._id = 0

    def _request(self, method: str, params: dict) -> dict:
        self._id += 1
        payload = json.dumps(
            {"jsonrpc": "2.0", "id": self._id, "method": method, "params": params},
            separators=(",", ":"),
        ).encode()
        req = urllib.request.Request(
            self.url,
            data=payload,
            headers={
                "Authorization": "Bearer " + self.token,
                "Accept": "application/json, text/event-stream",
                "Content-Type": "application/json",
                "MCP-Protocol-Version": MCP_PROTOCOL_VERSION,
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                body = resp.read()
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise MCPError(f"HTTP {exc.code}: {detail[:300]}") from exc
        except urllib.error.URLError as exc:
            raise MCPError(f"Connection failed: {exc.reason}") from exc
        result = json.loads(body)
        if "error" in result:
            raise MCPError(f"MCP error: {json.dumps(result['error'], ensure_ascii=False)}")
        return result.get("result", {})

    def call_tool(self, name: str, arguments: dict) -> dict:
        result = self._request("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            msgs = [
                item.get("text", "")
                for item in result.get("content", [])
                if item.get("type") == "text"
            ]
            raise MCPError(f"{name}: {'; '.join(msgs) or 'unknown error'}")
        structured = result.get("structuredContent")
        if isinstance(structured, dict):
            return structured
        for item in result.get("content", []):
            if item.get("type") == "text":
                try:
                    decoded = json.loads(item["text"])
                    if isinstance(decoded, dict):
                        return decoded
                except (json.JSONDecodeError, ValueError):
                    continue
        raise MCPError(f"{name}: no structured result")

    def upload_url(self, path: str) -> str:
        parsed = urllib.parse.urlsplit(path)
        if parsed.scheme in ("http", "https") and parsed.netloc:
            return path
        if path.startswith("/"):
            return self.origin + path
        return urllib.parse.urljoin(self.url + "/", path)


# ─── Build Package ────────────────────────────────────────────────────────────
def get_firmware_version() -> str:
    """Read version from version.h."""
    env_ver = os.environ.get("OTA_VERSION", "")
    if env_ver:
        return env_ver.removeprefix("v")
    text = VERSION_H.read_text(encoding="utf-8")
    match = re.search(r'FMO_FIRMWARE_VERSION\s+"([^"]+)"', text)
    if not match:
        raise ValueError(f"Cannot parse version from {VERSION_H}; set OTA_VERSION")
    return match.group(1)


def package_from_build(version: str, channel: str, notes: str) -> tuple[dict, list]:
    """Build multipart package metadata and file list from ESP-IDF build output."""
    flasher_args_path = BUILD_DIR / "flasher_args.json"
    if not flasher_args_path.is_file():
        raise ValueError(
            f"{flasher_args_path} not found. Build firmware first:\n"
            f"  cd {FIRMWARE_DIR} && idf.py build"
        )
    flasher_args = json.loads(flasher_args_path.read_text(encoding="utf-8"))
    app_offset = int(flasher_args["app"]["offset"], 16)

    parts = []
    files = []
    flash_files = sorted(
        flasher_args["flash_files"].items(), key=lambda x: int(x[0], 16)
    )
    for offset_hex, relative_path in flash_files:
        name = Path(relative_path).name
        source = BUILD_DIR / relative_path
        if not source.is_file():
            raise ValueError(f"Missing flash image: {source}")
        parts.append({"offset": int(offset_hex, 16), "name": name})
        files.append((name, source))

    meta = {
        "board": BOARD_ID,
        "version": version,
        "channel": channel,
        "notes": notes,
        "chip_family": CHIP_FAMILY,
        "app_offset": app_offset,
        "parts": parts,
    }
    return meta, files


def build_multipart(meta: dict, files: list) -> tuple[bytes, str]:
    """Build multipart/form-data body."""
    boundary = uuid.uuid4().hex
    sep = b"--" + boundary.encode()
    body = bytearray()
    body += sep + b'\r\nContent-Disposition: form-data; name="meta"\r\n\r\n'
    body += json.dumps(meta).encode() + b"\r\n"
    for name, path in files:
        body += sep + b"\r\n"
        body += (
            f'Content-Disposition: form-data; name="{name}"; filename="{name}"\r\n'
        ).encode()
        body += b"Content-Type: application/octet-stream\r\n\r\n"
        body += path.read_bytes() + b"\r\n"
    body += sep + b"--\r\n"
    return bytes(body), boundary


def app_sha256(meta: dict, files: list) -> tuple[int, str]:
    """Compute size and SHA-256 of the application image."""
    app_name = next(
        (p["name"] for p in meta["parts"] if p["offset"] == meta["app_offset"]),
        None,
    )
    app_path = next((path for name, path in files if name == app_name), None)
    if app_path is None:
        raise ValueError("Application image not found in package")
    digest = hashlib.sha256()
    with app_path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return app_path.stat().st_size, digest.hexdigest()


# ─── Publish Flow ─────────────────────────────────────────────────────────────
def publish(
    client: MCPClient,
    version: str,
    channel: str,
    notes: str,
    ttl_minutes: int,
    verify_only: bool,
) -> bool:
    meta, files = package_from_build(version, channel, notes)
    size, sha = app_sha256(meta, files)
    print(f"Package: {BOARD_ID} v{version} ({channel})")
    print(f"  App: {size:,} bytes, sha256={sha[:16]}...")
    print(f"  Parts: {', '.join(name for name, _ in files)}")

    # Check if already published
    existing = None
    try:
        listing = client.call_tool(
            "firmware.list",
            {"board": BOARD_ID, "channel": channel, "include_archived": False},
        )
        releases = listing.get("releases") or []
        existing = next(
            (
                r
                for r in releases
                if r.get("version") == version and r.get("channel") == channel
            ),
            None,
        )
    except MCPError:
        pass

    if existing:
        matches = (
            int(existing.get("size", -1)) == size
            and str(existing.get("sha256", "")).lower() == sha
        )
        if matches:
            print(f"  Already published and verified. Nothing to do.")
            return True
        print(f"  WARNING: existing release has different hash, re-publishing...")

    if verify_only:
        print(f"  NOT published (verify-only mode).")
        return False

    # Create upload session
    print(f"  Creating upload session...")
    upload = client.call_tool(
        "firmware.create_upload",
        {
            "board": BOARD_ID,
            "version": version,
            "channel": channel,
            "notes": notes,
            "ttl_minutes": ttl_minutes,
        },
    )
    upload_id = str(upload["upload_id"])
    upload_path = str(upload["upload_path"])
    upload_token = str(upload["upload_token"])

    # Upload multipart package
    print(f"  Uploading {len(files)} parts...")
    body, boundary = build_multipart(meta, files)
    req = urllib.request.Request(
        client.upload_url(upload_path),
        data=body,
        headers={
            "Authorization": "Bearer " + upload_token,
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=300) as resp:
            resp.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise MCPError(f"Upload HTTP {exc.code}: {detail[:300]}") from exc

    # Verify upload status
    status = client.call_tool("firmware.get_status", {"upload_id": upload_id})
    upload_status = status.get("upload", {}).get("status", "")
    if upload_status != "uploaded":
        raise MCPError(f"Upload did not complete: status={upload_status}")

    # Publish
    print(f"  Publishing...")
    published = client.call_tool(
        "firmware.publish", {"upload_id": upload_id, "confirm": True}
    )
    if published.get("status") != "published":
        raise MCPError(f"Publish failed: {published}")
    pub_size = int(published.get("size", -1))
    pub_sha = str(published.get("sha256", "")).lower()
    if pub_size != size or pub_sha != sha:
        raise MCPError(
            f"Verification mismatch: server={pub_size}/{pub_sha[:16]} "
            f"local={size}/{sha[:16]}"
        )
    print(f"  Published successfully! ({pub_size:,} bytes)")
    return True


# ─── Main ─────────────────────────────────────────────────────────────────────
def main() -> int:
    parser = argparse.ArgumentParser(
        description="Publish Open FMO firmware to OTA server."
    )
    parser.add_argument("--server", default=os.environ.get("OTA_SERVER_URL", DEFAULT_SERVER))
    parser.add_argument("--token", default=os.environ.get("OTA_ADMIN_TOKEN", DEFAULT_TOKEN))
    parser.add_argument("--version", help="Override firmware version")
    parser.add_argument("--channel", default="stable", choices=["stable", "beta"])
    parser.add_argument("--notes", default=os.environ.get("OTA_RELEASE_NOTES", ""))
    parser.add_argument("--ttl-minutes", type=int, default=30)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()

    version = (args.version or get_firmware_version()).removeprefix("v")

    try:
        client = MCPClient(args.server, args.token)
        ok = publish(client, version, args.channel, args.notes, args.ttl_minutes, args.verify_only)
    except (MCPError, ValueError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
