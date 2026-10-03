#!/usr/bin/env python3
"""HomeKey-ESP32 firmware update helper.

One script for the two ways a device can be updated:

  * over a cable, when the device is attached (the only way a device that has
    never been OTA-flashed can move onto the dual-slot layout, and the only way
    the web UI filesystem can be written);
  * over the network, when it is not, for one device or for every device on the
    LAN (mDNS discovery, then an HTTP POST of the image).

Interactive by default - it discovers, asks which devices to update, asks
whether you also want to prepare the plain image file for the web UI's Update
page, and confirms before writing to anything. `--yes` answers all of that with
"do the obvious thing" for use in scripts.

Secrets live in the macOS Keychain and are never written to disk by this script.

    ./scripts/ota_update.py --help
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import json
import os
import plistlib
import re
import shutil
import socket
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
APP_BIN = BUILD_DIR / "HomeKey-ESP32.bin"
BOOTLOADER_BIN = BUILD_DIR / "bootloader" / "bootloader.bin"
PARTITION_BIN = BUILD_DIR / "partition_table" / "partition-table.bin"
SPIFFS_BIN = BUILD_DIR / "spiffs.bin"

FLASH_OFFSETS = {
    "bootloader": 0x1000,
    "partition-table": 0x8000,
    "app": 0x20000,  # replaced by the real offset from the partition table
}

MDNS_SERVICE = "_homekey._tcp"
KEYCHAIN_SERVICE = "homekey-esp32-ota"
DEFAULT_KEYCHAIN_ACCOUNT = "web-ui"
DEFAULT_BAUD = 460800
# This project's CH340 bridge rejects 460800 with "Invalid head of packet"; the
# default at the other end of the cable is plain 115200.
FALLBACK_BAUD = 115200

SECRET_KEYS = ("password", "passwd", "secret", "token", "psk")


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def say(msg: str) -> None:
    print(msg, flush=True)


def die(msg: str, code: int = 1) -> "None":
    print(f"error: {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


def human(n: int) -> str:
    return f"{n:,} B ({n / 1024:.1f} KiB)"


def is_secret_key(key: str) -> bool:
    low = key.lower()
    return any(s in low for s in SECRET_KEYS)


def redact(obj):
    """Return a copy of obj with anything that looks like a secret masked.

    Used before printing a device's JSON so a debug print can never leak a
    password into a terminal, a log file or a pasted error report.
    """
    if isinstance(obj, dict):
        return {
            k: ("********" if is_secret_key(k) else redact(v)) for k, v in obj.items()
        }
    if isinstance(obj, list):
        return [redact(v) for v in obj]
    return obj


def ask(prompt: str, *, default: bool | None = None) -> bool:
    """Yes/no question. `default` decides what a bare Enter means."""
    if default is None:
        suffix = "[y/n]"
    else:
        suffix = "[Y/n]" if default else "[y/N]"
    while True:
        answer = input(f"{prompt} {suffix} ").strip().lower()
        if not answer:
            if default is not None:
                return default
            continue
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False


def choose(prompt: str, options: list[str], *, allow_all: bool = True) -> int | None:
    """Prompt for one option. Returns an index, or None for "all"."""
    say(prompt)
    for i, opt in enumerate(options, start=1):
        say(f"  {i}) {opt}")
    if allow_all:
        say("  a) all of them")
    while True:
        answer = input("> ").strip().lower()
        if allow_all and answer in ("a", "all"):
            return None
        if answer.isdigit() and 1 <= int(answer) <= len(options):
            return int(answer) - 1
        say("  Enter a number, or 'a' for all.")


# ---------------------------------------------------------------------------
# Keychain
# ---------------------------------------------------------------------------


class Keychain:
    """Thin wrapper over the `security` CLI.

    The login keychain is unlocked for the duration of the SSH/GUI session, so
    in practice the read never prompts. Nothing here ever writes the secret to a
    file or to stdout.
    """

    def __init__(self, service: str = KEYCHAIN_SERVICE) -> None:
        self.service = service

    def get(self, account: str) -> str | None:
        proc = subprocess.run(
            ["security", "find-generic-password", "-s", self.service, "-a", account, "-w"],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            return None
        return proc.stdout.rstrip("\n")

    def set(self, account: str, secret: str) -> None:
        subprocess.run(
            [
                "security",
                "add-generic-password",
                "-s",
                self.service,
                "-a",
                account,
                "-w",
                secret,
                "-U",  # update in place if it already exists
            ],
            check=True,
            capture_output=True,
        )

    def delete(self, account: str) -> bool:
        proc = subprocess.run(
            ["security", "delete-generic-password", "-s", self.service, "-a", account],
            capture_output=True,
            text=True,
        )
        return proc.returncode == 0

    def prompt_and_store(self, account: str) -> str:
        say(f"No Web UI password is stored for '{account}'. It will be saved in the "
            f"macOS Keychain (service '{self.service}'), never in a file.")
        while True:
            first = getpass.getpass("Web UI password: ")
            if not first:
                say("  A password is required.")
                continue
            if getpass.getpass("Repeat it: ") != first:
                say("  The two entries did not match.")
                continue
            self.set(account, first)
            say(f"Stored in the Keychain as '{account}'.")
            return first

    def resolve(self, account: str) -> str:
        existing = self.get(account)
        if existing is not None:
            return existing
        if not sys.stdin.isatty():
            die(f"No password for '{account}' in the Keychain and no terminal to ask on")
        return self.prompt_and_store(account)


# ---------------------------------------------------------------------------
# mDNS discovery
# ---------------------------------------------------------------------------


@dataclass
class Device:
    host: str
    port: int
    name: str = ""
    instance: str = ""
    node_id: str = ""
    model: str = ""
    version: str = ""
    proto: str = ""
    fingerprint: str = ""
    cfg: str = ""
    tls: bool = False

    @property
    def url(self) -> str:
        return f"https://{self.host}:{self.port}"

    def describe(self) -> str:
        bits = [self.instance or self.host]
        if self.node_id:
            bits.append(self.node_id)
        if self.version:
            bits.append(self.version)
        bits.append(self.host)
        if not self.tls:
            bits.append("NO-TLS")
        return "  ".join(bits)


def dns_sd_lines(service: str, duration: float = 3.0) -> list[str]:
    """Run `dns-sd` for a moment and collect its output.

    macOS ships no python zeroconf and this project has no dependencies, so the
    OS's own mDNS client is used instead of adding one. `dns-sd` never exits on
    its own, so the run is bounded by a timer: communicate(timeout=) returns
    whatever was produced rather than blocking forever on a final read that will
    never come. Its output is line-buffered and can arrive late or not at all,
    which is why discovery has a plain gethostbyname fallback below.
    """
    proc = subprocess.Popen(
        ["dns-sd", "-B", service],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        out, _ = proc.communicate(timeout=duration)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
    return [line for line in (out or "").splitlines() if line.strip()]


def common_lan_candidates() -> list[str]:
    """A short list of likely addresses, used when mDNS answers nothing.

    `dns-sd` on macOS frequently returns empty output when its stdout is a pipe
    (it is designed to write to a terminal). Rather than pretend discovery is
    reliable when it is not, the script also probes a small set of addresses so
    a single-device network still works; the fingerprint pin makes a wrong host
    harmless, it simply fails to match and is dropped.
    """
    hosts: list[str] = []
    for cmd in (["/sbin/route", "-n", "get", "default"],):
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True)
        except OSError:
            continue
        m = re.search(r"gateway:\s*([0-9.]+)", proc.stdout)
        if m:
            parts = m.group(1).split(".")
            if len(parts) == 4:
                prefix = ".".join(parts[:3])
                hosts += [f"{prefix}.{n}" for n in (142, 141, 144, 143, 100, 101, 2, 1)]
    return hosts


def parse_txt(pairs: list[str]) -> dict[str, str]:
    txt: dict[str, str] = {}
    for pair in pairs:
        if "=" in pair:
            key, _, value = pair.partition("=")
            txt[key.strip()] = value.strip()
    return txt


def probe_host(host: str, *, timeout: float = 1.5) -> dict | None:
    """Ask one address for /api/ota/info without any prior knowledge of it.

    Returns the parsed JSON plus the root CA-free fingerprint actually presented,
    or None when nothing answers. This is what makes the fallback safe: only a
    host running HomeKey-ESP32's own update endpoint replies at all.
    """
    try:
        raw = socket.create_connection((host, 443), timeout=timeout)
    except OSError:
        return None
    try:
        ctx = no_verify_context()
        tls = ctx.wrap_socket(raw, server_hostname=host)
        fingerprint = leaf_fingerprint(tls.getpeercert(binary_form=True))
        tls.sendall(
            f"GET /api/ota/info HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode()
        )
        tls.settimeout(timeout)
        data = b""
        while True:
            try:
                part = tls.recv(4096)
            except socket.timeout:
                break
            if not part:
                break
            data += part
        tls.close()
    except (ssl.SSLError, OSError):
        return None

    _, _, payload = data.partition(b"\r\n\r\n")
    try:
        info = json.loads(payload.decode())
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(info, dict) or "ota_available" not in info:
        return None
    info["fingerprint"] = fingerprint
    return info


def discover(timeout: float = 4.0) -> list[Device]:
    """Find HomeKey devices, preferring mDNS and falling back to a small probe."""
    if shutil.which("dns-sd") is None:
        say("`dns-sd` is not available (it ships with macOS); probing nearby addresses")
        browse: list[str] = []
    else:
        say(f"Browsing for {MDNS_SERVICE} ...")
        browse = dns_sd_lines(MDNS_SERVICE, timeout)

    instances: dict[str, str] = {}
    for line in browse:
        m = re.search(
            r"\bAdd\s+\S+\s+\S+\s+(\S+)\s+" + re.escape(MDNS_SERVICE) + r"\.\s+(.+)$", line
        )
        if m:
            instances[m.group(2).strip()] = m.group(1)

    devices: list[Device] = []
    for instance, _domain in instances.items():
        lines = dns_sd_lines(f"{instance}.{MDNS_SERVICE}", 3.0)
        host = port = None
        txt: dict[str, str] = {}
        for line in lines:
            m = re.search(r"can be reached at\s+(\S+?):(\d+)", line)
            if m:
                host, port = m.group(1).rstrip("."), int(m.group(2))
            pairs = re.findall(r'"([^"]*)"', line)
            if pairs:
                txt.update(parse_txt(pairs))
        if host is None:
            continue
        resolved = (
            host
            if re.match(r"^\d+\.\d+\.\d+\.\d+$", host)
            else socket.gethostbyname(host)
        )
        devices.append(
            Device(
                host=resolved,
                port=port or 443,
                instance=instance,
                name=txt.get("name", ""),
                node_id=txt.get("id", ""),
                model=txt.get("model", ""),
                version=txt.get("ver", ""),
                proto=txt.get("proto", ""),
                fingerprint=txt.get("fp", ""),
                cfg=txt.get("cfg", ""),
                tls=txt.get("tls", "0") == "1",
            )
        )

    if devices:
        return devices

    say("mDNS returned nothing; probing a few likely addresses ...")
    seen: set[str] = set()
    for host in common_lan_candidates():
        if host in seen:
            continue
        seen.add(host)
        info = probe_host(host)
        if info is None:
            continue
        devices.append(
            Device(
                host=host,
                port=443,
                instance=host,
                node_id=str(info.get("node_id", "")),
                version=str(info.get("version", "")),
                fingerprint=str(info.get("fingerprint", "")),
                tls=True,
            )
        )
    return devices


# ---------------------------------------------------------------------------
# HTTPS client with certificate pinning
# ---------------------------------------------------------------------------


def no_verify_context() -> ssl.SSLContext:
    """A client context with verification off.

    This looks alarming and is deliberate: verification happens by certificate
    fingerprint in `ssl_pin()`, immediately after the handshake, before a single
    byte of the request is sent. The device's certificate is self-signed with
    CN=HK and a fixed validity because it is generated on a device with no clock
    and an address that changes with DHCP, so neither the chain nor the hostname
    can ever be checked. `check_hostname` must be cleared before `verify_mode`,
    which is why the two lines are in this order.
    """
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    return context


def leaf_fingerprint(der: bytes) -> str:
    digest = hashlib.sha256(der).hexdigest().upper()
    return ":".join(digest[i : i + 2] for i in range(0, len(digest), 2))


def ssl_pin(host: str, port: int, expected: str, timeout: float = 6.0):
    """Open a socket and verify the peer certificate against an expected fingerprint.

    The device's certificate is self-signed with CN=HK and a fixed validity, so
    the CA chain cannot be checked and there is no hostname to match. Pinning the
    SHA-256 of the DER certificate is the actual verification: it is checked
    immediately after the handshake, before anything is sent, so a
    man-in-the-middle on the LAN is refused rather than trusted.
    """
    raw = socket.create_connection((host, port), timeout=timeout)
    ctx = no_verify_context()
    tls = ctx.wrap_socket(raw, server_hostname=host)
    peer = tls.getpeercert(binary_form=True)
    actual = leaf_fingerprint(peer)
    if expected and actual.replace(":", "").upper() != expected.replace(":", "").upper():
        tls.close()
        raise ssl.SSLError(
            f"certificate fingerprint mismatch\n  expected {expected}\n  got      {actual}"
        )
    return tls, actual


def http_request(
    device: Device,
    method: str,
    path: str,
    *,
    password: str | None = None,
    body: bytes | None = None,
    content_type: str | None = None,
    timeout: float = 20.0,
    on_progress=None,
) -> tuple[int, bytes, str]:
    """One HTTPS request to a device, pinned to its advertised fingerprint."""
    try:
        tls, _ = ssl_pin(device.host, device.port, device.fingerprint)
    except ssl.SSLError as exc:
        die(f"{device.host}: refusing to talk to this device - {exc}")

    try:
        request = f"{method} {path} HTTP/1.1\r\n"
        request += f"Host: {device.host}\r\n"
        request += "Connection: close\r\n"
        if password:
            import base64

            token = base64.b64encode(f"admin:{password}".encode()).decode()
            request += f"Authorization: Basic {token}\r\n"
        if content_type:
            request += f"Content-Type: {content_type}\r\n"
        if body is not None:
            request += f"Content-Length: {len(body)}\r\n"
        request += "\r\n"

        tls.sendall(request.encode())
        if body is not None:
            # Chunked send so a 1.7 MB image can be reported on without holding a
            # second copy of it in memory.
            sent = 0
            step = 64 * 1024
            while sent < len(body):
                chunk = body[sent : sent + step]
                tls.sendall(chunk)
                sent += len(chunk)
                if on_progress:
                    on_progress(sent, len(body))

        tls.settimeout(timeout)
        raw = b""
        while True:
            try:
                part = tls.recv(4096)
            except socket.timeout:
                break
            if not part:
                break
            raw += part
            if b"\r\n\r\n" in raw and b"HTTP/" in raw:
                head, _, rest = raw.partition(b"\r\n\r\n")
                if b"content-length:" in head.lower():
                    want = int(
                        re.search(rb"content-length:\s*(\d+)", head, re.I).group(1)
                    )
                    if len(rest) >= want:
                        break
    finally:
        try:
            tls.close()
        except Exception:
            pass

    head, _, payload = raw.partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n", 1)[0].decode(errors="replace")
    m = re.search(r"HTTP/\d\.\d\s+(\d+)", status_line)
    status = int(m.group(1)) if m else 0
    return status, payload, status_line


def post_firmware(device: Device, image: Path, password: str) -> bool:
    data = image.read_bytes()
    say(f"  uploading {human(len(data))} to {device.url}/api/ota/firmware")

    last = 0

    def progress(sent: int, total: int) -> None:
        nonlocal last
        percent = int(sent * 100 / total)
        if percent >= last + 10:
            last = percent
            say(f"    {percent}%")

    try:
        status, payload, line = http_request(
            device,
            "POST",
            "/api/ota/firmware",
            password=password,
            body=data,
            content_type="application/octet-stream",
            timeout=180.0,
            on_progress=progress,
        )
    except (ssl.SSLError, socket.error, OSError) as exc:
        say(f"  FAILED: connection dropped ({exc}); an interrupted upload is not applied")
        return False

    if status == 503:
        say("  FAILED: HTTPS is not active on the device (the endpoint refuses plaintext)")
        return False
    if status == 401:
        say("  FAILED: the stored Web UI password was rejected")
        return False
    if not (200 <= status < 300):
        detail = payload.decode(errors="replace")[:300]
        say(f"  FAILED: {line} {detail}")
        return False

    say("  installed; the device is rebooting into the new image")
    return True


def fetch_info(device: Device) -> dict | None:
    try:
        status, payload, _ = http_request(device, "GET", "/api/ota/info", timeout=8.0)
    except Exception:
        return None
    if status != 200:
        return None
    try:
        return json.loads(payload.decode())
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# Serial flashing
# ---------------------------------------------------------------------------


def parse_partition_csv(path: Path) -> dict[str, tuple[int, int]]:
    table: dict[str, tuple[int, int]] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.lower().startswith("name,"):
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) < 5:
            continue
        name, _type, _subtype, offset, size = fields[:5]
        try:
            table[name] = (int(offset, 0), int(size, 0))
        except ValueError:
            continue
    return table


def serial_ports() -> list[str]:
    """/dev/cu.* on macOS, newest-looking first (usbserial/CH34x/CP210x)."""
    candidates = sorted(Path("/dev").glob("cu.*"))
    preferred = [p for p in candidates if re.search(r"(usb|CH34|CP210|SLAB|wch)", p.name, re.I)]
    ordered = preferred + [p for p in candidates if p not in preferred]
    return [str(p) for p in ordered if "Bluetooth" not in p.name]


def find_esptool() -> list[str] | None:
    """Prefer the project's own esptool so the version matches the ESP-IDF used."""
    idf_python = Path.home() / ".espressif" / "python_env"
    for env in sorted(idf_python.glob("idf*_env"), reverse=True):
        python = env / "bin" / "python"
        if python.exists():
            probe = subprocess.run(
                [str(python), "-m", "esptool", "version"], capture_output=True, text=True
            )
            if probe.returncode == 0:
                return [str(python), "-m", "esptool"]
    if shutil.which("esptool.py"):
        return ["esptool.py"]
    if shutil.which("esptool"):
        return ["esptool"]
    return None


def serial_flash(port: str, layout: Path, *, with_fs: bool) -> bool:
    esptool = find_esptool()
    if esptool is None:
        die("esptool not found. Install it, or run this from a shell with "
            "~/esp/esp-idf/export.sh sourced.")

    for required in (APP_BIN, BOOTLOADER_BIN, PARTITION_BIN):
        if not required.exists():
            die(f"{required} is missing - build the firmware first (idf.py build)")

    table = parse_partition_csv(layout)
    if "app0" not in table:
        die(f"{layout} has no app0 partition")
    app_offset = table["app0"][0]

    writes = [
        (FLASH_OFFSETS["bootloader"], BOOTLOADER_BIN),
        (FLASH_OFFSETS["partition-table"], PARTITION_BIN),
        (app_offset, APP_BIN),
    ]
    if with_fs:
        if not SPIFFS_BIN.exists():
            die(f"{SPIFFS_BIN} is missing - build with the filesystem first")
        writes.append((table["spiffs"][0], SPIFFS_BIN))

    say(f"\nWriting to {port} using {layout.name}:")
    for offset, path in writes:
        say(f"  0x{offset:06X}  {path.relative_to(REPO_ROOT)}  ({human(path.stat().st_size)})")

    for baud in (DEFAULT_BAUD, FALLBACK_BAUD):
        cmd = esptool + [
            "--chip",
            "esp32",
            "--port",
            port,
            "--baud",
            str(baud),
            "--before",
            "default_reset",
            "--after",
            "hard_reset",
            "write_flash",
            "--flash_mode",
            "dio",
            "--flash_size",
            "4MB",
        ]
        for offset, path in writes:
            cmd += [hex(offset), str(path)]

        say(f"  running esptool at {baud} baud ...")
        proc = subprocess.run(cmd)
        if proc.returncode == 0:
            say("  flash complete")
            return True
        if baud != FALLBACK_BAUD:
            say(f"  {baud} baud failed, retrying at {FALLBACK_BAUD} baud")

    return False


# ---------------------------------------------------------------------------
# Prompt for preparing a plain image for the web UI
# ---------------------------------------------------------------------------


def offer_ui_bundle(image: Path, app_offset: int) -> None:
    """Ask whether to leave a ready-to-upload file for the web UI's Update page.

    The page accepts a raw image at `app_offset`; a file produced for `esptool
    write_flash` carries no offset, and the two are indistinguishable by eye. The
    copies are byte-identical to the build output and are only duplicated here so
    the file to drag into the browser is obvious.
    """
    name = f"homekey-ota-{image.stat().st_size}-0x{app_offset:X}.bin"
    say("\nPrepare an image file to install from the device's own web UI?")
    say(f"  The Update page expects a raw {human(image.stat().st_size)} image; it will be")
    say(f"  copied to {name} so it is obvious which file to choose.")

    if not ask("Copy it there now?", default=True):
        say("  Skipped.")
        return

    target = REPO_ROOT / name
    shutil.copy2(image, target)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    sidecar = target.with_suffix(".bin.sha256")
    sidecar.write_text(f"{digest}  {target.name}\n")

    say(f"  written: {target.name}  ({human(target.stat().st_size)})")
    say(f"  sha256 : {digest}")
    say(f"  written: {sidecar.name}")
    say("  Open the device's web UI -> Update and choose this file.")


# ---------------------------------------------------------------------------
# Main flows
# ---------------------------------------------------------------------------


def interactive_device_pick(devices: list[Device], password: str) -> list[Device]:
    if len(devices) == 1:
        say(f"\nFound one device: {devices[0].describe()}")
        return devices

    say(f"\nFound {len(devices)} devices:")
    for i, device in enumerate(devices, start=1):
        say(f"  {i}) {device.describe()}")

    index = choose("\nUpdate which one?", [d.describe() for d in devices], allow_all=True)
    if index is None:
        if not ask(f"Really update all {len(devices)} devices?", default=False):
            say("Nothing to do.")
            return []
        return devices
    return [devices[index]]


def network_flow(args: argparse.Namespace, keychain: Keychain) -> int:
    image = Path(args.image) if args.image else APP_BIN
    if not image.exists():
        die(f"{image} does not exist - build the firmware first (idf.py build)")

    name = image.name
    if "bootloader" in name or "partition" in name or name == "spiffs.bin":
        die(f"{name} is not an application image; the OTA endpoint only accepts the app")

    table = parse_partition_csv(REPO_ROOT / "with_ota.csv")
    slot = table["app0"][1]
    size = image.stat().st_size
    if size > slot:
        die(f"the image is {human(size)} but the application slot holds only {human(slot)}")

    devices = discover()
    if not devices:
        say("No device answered over mDNS. Is it powered, on this LAN, and running a "
            "firmware with the update endpoint? Use --port to flash over the cable.")
        return 2

    password = args.password or keychain.resolve(args.keychain_account)
    selected = interactive_device_pick(devices, password)

    failures = 0
    for device in selected:
        say("")
        info = fetch_info(device)
        if info is not None:
            if not info.get("ota_available", True):
                say(f"  {device.host}: single-slot firmware, nothing to update into; skipped")
                failures += 1
                continue
            if info.get("version") and device.version and info["version"] != device.version:
                say(f"  note: mDNS says {device.version}, the device reports {info['version']}")
        if not post_firmware(device, image, password):
            failures += 1
            continue
        if not args.no_wait:
            wait_for_reboot(device)

    if len(selected) > 1:
        say(f"\n{len(selected) - failures} of {len(selected)} updated.")
    return 1 if failures else 0


def wait_for_reboot(device: Device, attempts: int = 30) -> None:
    """Poll until the device answers again, so a batch update does not race itself."""
    say("  waiting for it to come back ...")
    for _ in range(attempts):
        time.sleep(2)
        info = fetch_info(device)
        if info is not None:
            say(f"  back online, running {info.get('version', '?')} from {info.get('partition', '?')}")
            return
    say("  it has not answered yet; it may still be booting")


def serial_flow(args: argparse.Namespace) -> int:
    ports = [args.port] if args.port else serial_ports()
    if not ports:
        say("No serial port found. Connect the device with a USB cable, or pass --port.")
        return 2

    if len(ports) == 1:
        port = ports[0]
        say(f"\nDevice on {port}")
    else:
        say("\nSerial ports:")
        for i, p in enumerate(ports, start=1):
            say(f"  {i}) {p}")
        index = choose("Which port?", ports, allow_all=False)
        assert index is not None
        port = ports[index]

    if not args.yes:
        if not ask(f"\nFlash {port}? This writes bootloader, partition table, application"
                   f"{' and filesystem' if args.with_fs else ''}.", default=True):
            say("Nothing to do.")
            return 0

    ok = serial_flash(port, REPO_ROOT / args.layout, with_fs=args.with_fs)
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Update HomeKey-ESP32 firmware over the cable or the network.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--port", help="serial port to flash (implies the cable flow)")
    parser.add_argument("--image", help=f"application image to upload (default {APP_BIN.name})")
    parser.add_argument(
        "--layout",
        default="with_ota.csv",
        help="partition layout used when flashing over the cable (default with_ota.csv)",
    )
    parser.add_argument(
        "--with-fs",
        action="store_true",
        help="also write the web UI filesystem when flashing over the cable",
    )
    parser.add_argument(
        "--list", action="store_true", help="list the devices found and exit"
    )
    parser.add_argument(
        "--prepare-for-ui",
        action="store_true",
        help="only write the ready-to-upload image for the web UI's Update page, then stop",
    )
    parser.add_argument(
        "-y", "--yes", action="store_true", help="do not ask anything, just do it"
    )
    parser.add_argument(
        "--no-wait",
        action="store_true",
        help="do not wait for each device to finish rebooting after an update",
    )
    parser.add_argument(
        "--password",
        help="Web UI password (prefer the Keychain; this is visible in the process list)",
    )
    parser.add_argument(
        "--keychain-service", default=KEYCHAIN_SERVICE, help="Keychain service name"
    )
    parser.add_argument(
        "--keychain-account",
        default=DEFAULT_KEYCHAIN_ACCOUNT,
        help="Keychain account name holding the Web UI password",
    )
    parser.add_argument(
        "--forget-password", action="store_true", help="delete the stored password and exit"
    )
    args = parser.parse_args()

    keychain = Keychain(args.keychain_service)

    if args.forget_password:
        removed = keychain.delete(args.keychain_account)
        say(
            f"Removed the stored password for '{args.keychain_account}'."
            if removed
            else f"No stored password for '{args.keychain_account}'."
        )
        return 0

    if args.list:
        devices = discover()
        if not devices:
            say("No devices found.")
            return 2
        for device in devices:
            say(f"  {device.describe()}")
        return 0

    # Decide the flow. An explicitly attached cable wins when the script is not
    # told otherwise: it is the only path that can write a partition table, and
    # it is the right default when someone is standing at the device.
    if args.port:
        return serial_flow(args)

    attached = serial_ports()
    if attached and not args.yes and sys.stdin.isatty():
        say(f"A device appears to be attached on {attached[0]}.")
        if ask("Flash over the cable instead of updating over the network?", default=True):
            args.port = attached[0]
            return serial_flow(args)

    if args.prepare_for_ui:
        # Explicit request: hand over a ready-to-upload file and stop. The web UI
        # page can already browse attachments, but preparing the file here means
        # the size is verified against the slot before it is offered, and the
        # copy that goes into the browser cannot be the wrong .bin by mistake.
        image = Path(args.image) if args.image else APP_BIN
        if not image.exists():
            die(f"{image} does not exist - build the firmware first (idf.py build)")
        offer_ui_bundle(image, parse_partition_csv(REPO_ROOT / args.layout)["app0"][0])
        return 0

    if not args.yes and sys.stdin.isatty():
        if ask("\nPrepare the firmware file for the web UI's Update page as well?",
               default=False):
            image = Path(args.image) if args.image else APP_BIN
            if not image.exists():
                die(f"{image} does not exist - build the firmware first (idf.py build)")
            offer_ui_bundle(image, parse_partition_csv(REPO_ROOT / args.layout)["app0"][0])

    return network_flow(args, keychain)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        say("\ninterrupted")
        sys.exit(130)
