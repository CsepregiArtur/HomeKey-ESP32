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

def rel(path: Path) -> str:
    """Path relative to the repo when it is inside it, else the absolute path.

    `Path.relative_to` raises when the path is outside REPO_ROOT, which would turn a
    clear error message into a traceback. BUILD_DIR is normally inside the repo, but
    an overridden or relocated build directory should not crash the error path.
    """
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def flash_layout() -> dict[str, int]:
    """Where each image belongs, as the current build decided.

    The bootloader offset is NOT the same on every chip: a classic ESP32 puts it at
    0x1000, while an ESP32-C3 (and the other RISC-V parts) put it at 0x0. Writing a C3
    bootloader to 0x1000 leaves the chip with no valid bootloader and it prints
    `invalid header: 0xffffffff` forever.

    There is deliberately no fallback offset table. `flash_args` is written by IDF in
    the same directory as the images, so its absence means the build is not a finished
    IDF build, and guessing 0x1000 for an unknown chip is exactly the mistake this
    function exists to prevent. A missing file is an error, not a default.
    """
    args = BUILD_DIR / "flash_args"
    if not args.exists():
        die(
            f"{rel(args)} is missing, so the flash offsets for this chip are unknown. "
            f"Build the firmware first (idf.py build or ./scripts/build_esp32c3.sh "
            f"build); the bootloader offset differs between chips and cannot be "
            f"guessed safely."
        )

    layout: dict[str, int] = {}
    known = {
        "bootloader/bootloader.bin": "bootloader",
        "partition_table/partition-table.bin": "partition-table",
    }
    for line in args.read_text().splitlines():
        line = line.strip()
        if line.startswith("--") or not line:
            continue
        parts = line.split()
        if len(parts) != 2:
            continue
        offset, path = parts
        key = known.get(path)
        if key:
            layout[key] = int(offset, 0)

    missing = [k for k in ("bootloader", "partition-table") if k not in layout]
    if missing:
        die(
            f"{rel(args)} does not name the {', '.join(missing)} image(s), so the build "
            f"is incomplete. Rebuild before flashing."
        )
    return layout


def build_target() -> str | None:
    """The target the current build directory was actually configured for."""
    desc = BUILD_DIR / "project_description.json"
    if not desc.exists():
        return None
    try:
        return json.loads(desc.read_text()).get("target")
    except (ValueError, OSError):
        return None


def check_build_consistency() -> None:
    """Refuse to flash when the build directory describes two different chips.

    `flash_args` and `project_description.json` are both generated, but by different
    steps, and they can disagree: this tree keeps a checked-in root `sdkconfig`, which
    IDF prefers over the target passed to `set-target`. That makes a target switch
    silently revert while `flash_args` keeps the offsets from the other chip's generate
    step - so the offset lookup would take a bootloader offset from a build that is not
    the build being written. The mismatch is exactly what writes a bootloader to the
    wrong address, and there is no way to tell from the numbers alone which file is
    right, so this stops rather than picks one.
    """
    target = build_target()
    if target is None:
        return

    layout = flash_layout()
    boot = layout["bootloader"]
    # A C3 (and the other RISC-V parts) boot from 0x0; every Xtensa part uses 0x1000.
    expect_zero = target.startswith("esp32c") or target in ("esp32h2", "esp32p4")
    if expect_zero and boot != 0x0:
        die(
            f"build/flash_args places the bootloader at 0x{boot:X} but the build targets "
            f"'{target}', which boots from 0x0. The build directory describes two "
            f"different chips; write to it would put the bootloader at the wrong "
            f"address. Rebuild from clean:\n"
            f"    rm -rf build && ./scripts/build_esp32c3.sh set-target && "
            f"./scripts/build_esp32c3.sh build"
        )
    if not expect_zero and boot == 0x0:
        die(
            f"build/flash_args places the bootloader at 0x0 but the build targets "
            f"'{target}', which boots from 0x1000. The build directory is inconsistent; "
            f"rebuild from clean before flashing."
        )


# Every image the cable flow writes. Checked once, before anything is detected or
# written, so all four entry points fail with the same sentence instead of a
# FileNotFoundError from whichever .stat() happens to run first.
REQUIRED_IMAGES = (("application", APP_BIN), ("bootloader", BOOTLOADER_BIN),
                   ("partition table", PARTITION_BIN))


def require_build_artifacts() -> None:
    """Fail early and clearly when the tree has not been built (or built fully).

    An image that exists but is empty is treated as missing: a failed link leaves a
    zero-byte `HomeKey-ESP32.elf` behind and `idf.py build` can still exit 0, so a
    plain `exists()` check would pass and the flash would write nothing at that offset.
    """
    missing = [
        (label, path)
        for label, path in REQUIRED_IMAGES
        if not path.exists() or path.stat().st_size == 0
    ]
    if not missing:
        return
    details = "\n".join(f"    {label:<16} {rel(path)}" for label, path in missing)
    die(
        f"the build is missing or empty for {len(missing)} image(s):\n{details}\n"
        f"  Build the firmware first. Nothing was written.\n"
        f"  For an ESP32-C3 use ./scripts/build_esp32c3.sh build (a plain "
        f"idf.py set-target fails on this host; see that script for why)."
    )


MDNS_SERVICE = "_homekey._tcp"
KEYCHAIN_SERVICE = "homekey-esp32-ota"
DEFAULT_KEYCHAIN_ACCOUNT = "web-ui"
DEFAULT_BAUD = 460800
# This project's CH340 bridge rejects 460800 with "Invalid head of packet"; the
# default at the other end of the cable is plain 115200.
FALLBACK_BAUD = 115200

# Targets this project is set up to build. esp32 (Xtensa) builds directly; esp32c3 works
# on macOS hosts after the RISC-V toolchain is installed AND an assembler shim is used,
# see scripts/build_esp32c3.sh for why. Everything else would need both plus review of the
# pin defaults, since the NFC wiring differs per chip.
SUPPORTED_TARGETS = ("esp32", "esp32c3")

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
    """Yes/no question. `default` decides what a bare Enter means.

    Falls back to `default` when there is no terminal to read from, so a scripted run
    (piped input, cron, a captured log) does not die with EOFError halfway through its
    work. Passing no default and having no terminal is a programming error, not a user
    error, so it raises.
    """
    if not sys.stdin.isatty():
        if default is None:
            die(f"cannot ask '{prompt}' without a terminal; pass the matching flag")
        return default

    if default is None:
        suffix = "[y/n]"
    else:
        suffix = "[Y/n]" if default else "[y/N]"
    while True:
        try:
            answer = input(f"{prompt} {suffix} ").strip().lower()
        except EOFError:
            if default is None:
                raise
            return default
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


@dataclass
class ChipInfo:
    """What the attached device actually is, as reported by esptool."""

    chip: str  # canonical esptool name, e.g. "esp32" or "esp32c3"
    description: str
    features: str = ""
    mac: str = ""
    flash_size: str = ""

    def describe(self) -> str:
        bits = [self.description or self.chip]
        if self.flash_size:
            bits.append(f"{self.flash_size} flash")
        if self.mac:
            bits.append(self.mac)
        return "  ".join(bits)


def detect_chip(esptool: list[str], port: str) -> ChipInfo | None:
    """Ask esptool what is on the other end of the cable.

    This is what makes the cable path work on any board: the chip name decides the
    `--chip` argument, and the flash size decides the `write_flash` header. Neither
    can be guessed from the host build, because a project can ship more than one
    target's image (this one builds for esp32 and esp32c3 from the same tree).

    `--no-stub` is deliberately not passed: the ROM bootloader reports the chip
    before any stub is uploaded, so a low baud is enough and a device that is
    already running a sketch still answers after a reset.
    """
    proc = subprocess.run(
        esptool
        + [
            "--port",
            port,
            "--baud",
            "115200",
            "--before",
            "default_reset",
            "--after",
            "hard_reset",
            "chip_id",
        ],
        capture_output=True,
        text=True,
    )
    output = ((proc.stdout or "") + (proc.stderr or "")).replace("\r", "")
    # Only the exit code decides whether detection failed. A non-zero exit means no
    # usable answer; the parsers below already return None when no chip name is found,
    # so a text guard here would only reject outputs that actually parsed fine.
    if proc.returncode != 0:
        return None

    description = ""
    m = re.search(r"Chip is (.+)", output)
    if m:
        description = m.group(1).strip()

    # "Detecting chip type... ESP32-C3" is the canonical short name esptool wants.
    chip = ""
    m = re.search(r"Detecting chip type\.\.\.\s*(\S+)", output)
    if m:
        chip = m.group(1).strip().lower().replace("-", "")
    if not chip:
        # Older esptool only prints the full description.
        if description:
            chip = (
                description.split()[0].lower().replace("-", "").split("(")[0].strip()
            )
    if not chip:
        return None

    features = ""
    m = re.search(r"Features:\s*(.+)", output)
    if m:
        features = m.group(1).strip()

    flash_size = ""
    m = re.search(r"Embedded Flash\s+(\d+\s*MB)", features)
    if m:
        flash_size = m.group(1).replace(" ", "")
    else:
        m = re.search(r"Flash size:\s*(\d+\s*MB)", output)
        if m:
            flash_size = m.group(1).replace(" ", "")

    mac = ""
    m = re.search(r"MAC:\s*([0-9a-f:]{17})", output, re.I)
    if m:
        mac = m.group(1)

    return ChipInfo(
        chip=chip, description=description, features=features, mac=mac, flash_size=flash_size
    )


def serial_flash(port: str, layout: Path, *, with_fs: bool, chip: str | None = None) -> bool:
    esptool = find_esptool()
    if esptool is None:
        die("esptool not found. Install it, or run this from a shell with "
            "~/esp/esp-idf/export.sh sourced.")

    require_build_artifacts()

    detected = detect_chip(esptool, port)
    if detected is None:
        if chip is None:
            die(
                f"could not read the chip on {port}. Is the device in the bootloader "
                f"(hold BOOT), the right port chosen, and not held open by a serial "
                f"monitor? Pass --chip to proceed without detection."
            )
        # --chip is the documented escape hatch for a board that will not answer
        # chip_id; honour it rather than dying and telling the user to pass the flag
        # that was already passed.
        say(f"Nothing answered on {port}; trusting --chip {chip} as requested.")
        detected = ChipInfo(chip=chip, description=f"assumed {chip} (detection skipped)")
    elif chip and chip != detected.chip:
        say(f"Note: --chip says {chip} but the device is {detected.chip}; using the device's own.")

    say(f"\nChip: {detected.describe()}")
    if detected.features:
        say(f"      {detected.features}")

    # The image must match the chip. Building for the wrong target produces an
    # image the ROM refuses to boot, and the failure is a silent reset loop, so it
    # is worth one comparison here instead of a puzzled half hour later.
    project_version = None
    desc = BUILD_DIR / "project_description.json"
    if desc.exists():
        try:
            project_version = json.loads(desc.read_text()).get("target")
        except (ValueError, OSError):
            project_version = None
    if project_version and project_version != detected.chip:
        die(
            f"the build in build/ targets '{project_version}' but the device is "
            f"'{detected.chip}'. Rebuild for this chip:\n"
            f"    idf.py set-target {detected.chip} && idf.py build\n"
            f"(or run ./scripts/ota_update.py --target {detected.chip})"
        )

    table = parse_partition_csv(layout)
    if "app0" not in table:
        die(f"{layout} has no app0 partition")
    app_offset = table["app0"][0]

    # Chip-specific: the bootloader is at 0x0 on a C3 and 0x1000 on a classic ESP32.
    offsets = flash_layout()
    # flash_args and project_description.json can disagree when a target switch did not
    # take effect; a mismatch means the offsets do not describe the build being written.
    check_build_consistency()

    writes = [
        (offsets["bootloader"], BOOTLOADER_BIN),
        (offsets["partition-table"], PARTITION_BIN),
        (app_offset, APP_BIN),
    ]
    if with_fs:
        if not SPIFFS_BIN.exists():
            die(f"{SPIFFS_BIN} is missing - build with the filesystem first")
        writes.append((table["spiffs"][0], SPIFFS_BIN))

    # "keep" leaves detection to esptool; pinning it when the size is known stops a
    # mismatched board from being written with a header describing the wrong flash.
    flash_size = detected.flash_size or "keep"

    say(f"\nWriting to {port} using {layout.name}:")
    for offset, path in writes:
        say(f"  0x{offset:06X}  {rel(path)}  ({human(path.stat().st_size)})")

    for baud in (DEFAULT_BAUD, FALLBACK_BAUD):
        cmd = esptool + [
            "--chip",
            detected.chip,
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
            flash_size,
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


def rebuild_for(target: str) -> bool:
    """Switch the build to `target` and compile it.

    esp32c3 needs the assembler shim described in scripts/build_esp32c3.sh, because
    IDF's `riscv32-esp-elf-as` is a Rust dispatcher that fails to resolve its real
    assembler on this host and the OS then falls back to Apple's `as`, which rejects
    RISC-V flags. Calling `idf.py set-target` directly would fail, so the helper is
    used when it exists.
    """
    if target != "esp32":
        helper = REPO_ROOT / "scripts" / f"build_{target}.sh"
        if helper.exists():
            for step in ("set-target", "build"):
                if subprocess.run([str(helper), step]).returncode != 0:
                    return False
            return True
    if subprocess.run(["idf.py", "set-target", target]).returncode != 0:
        return False
    return subprocess.run(["idf.py", "build"]).returncode == 0


def flash_over_cable(args: argparse.Namespace) -> int:
    """Detect the board, make sure the build matches it, then write it."""
    # Checked before anything touches the device: with --no-build there is nothing
    # left that could produce these files, so failing here saves a pointless port
    # probe and, more importantly, reports the real problem first.
    if args.no_build:
        require_build_artifacts()

    ports = [args.port] if args.port else serial_ports()
    if not ports:
        say("No serial port found. Connect the device with a USB cable, or pass --port.")
        return 2

    if len(ports) == 1:
        port = ports[0]
    else:
        say("\nSerial ports:")
        for i, p in enumerate(ports, start=1):
            say(f"  {i}) {p}")
        index = choose("Which port?", ports, allow_all=False)
        assert index is not None
        port = ports[index]

    esptool = find_esptool()
    if esptool is None:
        die("esptool not found. Run this from a shell with ~/esp/esp-idf/export.sh sourced.")

    detected = detect_chip(esptool, port)
    if detected is None:
        if args.chip is None:
            die(f"nothing answered on {port}. Check the cable, the port, and that no serial "
                f"monitor is holding it open. Pass --chip to proceed without detection.")
        say(f"Nothing answered on {port}; trusting --chip {args.chip} as requested.")
        detected = ChipInfo(
            chip=args.chip, description=f"assumed {args.chip} (detection skipped)"
        )

    say(f"\nDevice on {port}: {detected.describe()}")

    # The build has to match the board. Rather than asking the user to notice, switch
    # the target here: `idf.py set-target` is a build-directory operation, so it never
    # touches the device, and the image it produces is the one this board needs.
    target = BUILD_DIR / "project_description.json"
    built_for = None
    if target.exists():
        try:
            built_for = json.loads(target.read_text()).get("target")
        except (ValueError, OSError):
            built_for = None

    if built_for and built_for != detected.chip:
        say(f"The build in build/ targets '{built_for}' but this device is '{detected.chip}'.")
        if args.no_build:
            die(
                f"--no-build was given, so the build was not switched from '{built_for}' "
                f"to '{detected.chip}'. Nothing was written. Either drop --no-build to "
                f"rebuild automatically, or switch it yourself:"
                f"\n    ./scripts/build_esp32c3.sh set-target   # or: idf.py set-target "
                f"{detected.chip}"
            )
        if detected.chip not in SUPPORTED_TARGETS:
            die(
                f"firmware for '{detected.chip}' cannot be built from this tree as it is "
                f"configured. Supported targets: {', '.join(SUPPORTED_TARGETS)}.\n"
                f"  To add one, install its toolchain and export it before running idf.py:\n"
                f"      python \"$IDF_PATH/tools/idf_tools.py\" install riscv32-esp-elf\n"
                f"      idf.py set-target {detected.chip} && idf.py build\n"
                f"  then re-run this script. (This board reports itself correctly; it is "
                f"the host build that does not match, so nothing was written.)"
            )
        if not args.yes and not ask(f"Rebuild for {detected.chip} now?", default=True):
            say("Nothing to do.")
            return 0
        if not rebuild_for(detected.chip):
            die(f"could not build for {detected.chip}")
        say("")

    if not args.yes:
        extra = " and filesystem" if args.with_fs else ""
        if not ask(f"Flash {port}? This writes bootloader, partition table, application{extra}.",
                   default=True):
            say("Nothing to do.")
            return 0

    ok = serial_flash(
        port, REPO_ROOT / args.layout, with_fs=args.with_fs, chip=args.chip
    )
    if ok:
        offer_ui_bundle(APP_BIN, parse_partition_csv(REPO_ROOT / args.layout)["app0"][0])
    return 0 if ok else 1


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
    if args.image:
        if not image.exists():
            die(f"{image} does not exist")
    else:
        # The default image is the build output, so check the whole build at once
        # rather than letting the user discover a missing bootloader mid-flash.
        require_build_artifacts()

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


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Update HomeKey-ESP32 firmware over the cable or the network.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--port", help="serial port to flash (implies the cable flow)")
    parser.add_argument("--image", help=f"application image to upload (default {APP_BIN.name})")
    parser.add_argument(
        "--chip",
        help="chip to assume when flashing over the cable; by default the device is asked",
    )
    parser.add_argument(
        "--target",
        help="rebuild (set-target + build) for this chip before flashing, e.g. esp32c3",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="never run idf.py; fail instead when the build does not match the device",
    )
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

    # Rebuilding for a specific chip is a build-directory operation, so it happens
    # before anything is chosen or flashed.
    if args.target and not args.no_build:
        if args.target not in SUPPORTED_TARGETS:
            die(
                f"'{args.target}' is not a target this tree builds. Supported: "
                f"{', '.join(SUPPORTED_TARGETS)}. See the compile-targets note in "
                f"docs/content/updates.md before adding one."
            )
        say(f"Rebuilding for {args.target} ...")
        if not rebuild_for(args.target):
            die(f"could not build for {args.target}")
        say("")

    # Decide the flow. An explicitly attached cable wins when the script is not
    # told otherwise: it is the only path that can write a partition table, and
    # it is the right default when someone is standing at the device.
    if args.port or args.chip:
        return flash_over_cable(args)

    attached = serial_ports()
    if attached and not args.yes and sys.stdin.isatty():
        say(f"A device appears to be attached on {attached[0]}.")
        if ask("Flash over the cable instead of updating over the network?", default=True):
            args.port = attached[0]
            return flash_over_cable(args)

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
