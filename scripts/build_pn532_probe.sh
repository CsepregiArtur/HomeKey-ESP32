#!/bin/sh
# Build (and optionally flash / monitor) the standalone PN532 SPI probe.
#
# This builds the standalone ESP32-C3 Mini PN532 diagnostic project. IDF's
# `riscv32-esp-elf-as` is a Rust
# dispatcher that fails to resolve its real assembler here, so the OS falls back to
# Apple's `as`, which rejects RISC-V flags. A shim directory containing an `as` that
# points straight at the real GNU assembler bypasses the dispatcher.
#
# The probe is pure ESP-IDF and uses one C source file. It does not build or link
# application components.
#
# USAGE
#   ./scripts/build_pn532_probe.sh                 # configure + build
#   ./scripts/build_pn532_probe.sh flash /dev/cu.usbmodem1101
#   ./scripts/build_pn532_probe.sh monitor /dev/cu.usbmodem1101
#
# Port defaults to the first /dev/cu.usbmodem* that exists.

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROBE_DIR="$REPO_ROOT/tools/pn532_probe"

# This diagnostic is intentionally fixed to the ESP32-C3 Mini target.
PROBE_BUILD="$REPO_ROOT/build_probe_esp32c3"

IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf}"
TOOLCHAIN_DIR="$HOME/.espressif/tools/riscv32-esp-elf"

if [ ! -d "$IDF_DIR" ]; then
  echo "error: ESP-IDF not found at $IDF_DIR (set IDF_PATH)" >&2
  exit 1
fi
if [ ! -f "$PROBE_DIR/CMakeLists.txt" ]; then
  echo "error: $PROBE_DIR does not look like the probe project" >&2
  exit 1
fi

# Newest installed RISC-V toolchain.
TC=""
for candidate in $(ls -d "$TOOLCHAIN_DIR"/*/ 2>/dev/null | sort -r); do
  if [ -x "${candidate}riscv32-esp-elf/bin/riscv32-esp-elf-gcc" ]; then
    TC="${candidate}riscv32-esp-elf/bin"
    break
  fi
done

if [ -z "$TC" ]; then
  echo "error: the RISC-V toolchain is not installed. Run:" >&2
  echo "  python \"\$IDF_PATH/tools/idf_tools.py\" --non-interactive install riscv32-esp-elf" >&2
  exit 1
fi

AS_BIN="$TC/riscv32-esp-elf-as-xespv2p2"
LD_BIN="$TC/riscv32-esp-elf-ld"
if [ ! -x "$AS_BIN" ]; then
  echo "error: $AS_BIN not found - the toolchain install is incomplete" >&2
  exit 1
fi

SHIM="${TMPDIR:-/tmp}/pn532-c3-riscv-shim"
rm -rf "$SHIM"
mkdir -p "$SHIM"
ln -s "$AS_BIN" "$SHIM/as"
[ -x "$LD_BIN" ] && ln -s "$LD_BIN" "$SHIM/ld"

export PATH="$SHIM:$TC:$PATH"

if [ ! -f "$IDF_DIR/export.sh" ]; then
  echo "error: $IDF_DIR/export.sh not found" >&2
  exit 1
fi
# shellcheck disable=SC1091
. "$IDF_DIR/export.sh" >/dev/null

export PATH="$SHIM:$TC:$PATH"
export CI=true
export CMAKE_POLICY_VERSION_MINIMUM=3.5

# confgen writes sdkconfig from this variable, not from set-target's argument; with it
# unset the generated sdkconfig silently reverts to Kconfig's default of esp32 while
# CMake records the requested target, and the build then fails with a target mismatch.
export IDF_TARGET=esp32c3

ACTION="${1:-build}"
PORT="${2:-}"

if [ -z "$PORT" ]; then
  for p in /dev/cu.usbmodem*; do
    [ -e "$p" ] && PORT="$p" && break
  done
fi

cd "$PROBE_DIR"

# First build only: create the ESP32-C3 build directory.
if [ ! -f "$PROBE_BUILD/CMakeCache.txt" ]; then
  echo "configuring standalone probe for esp32c3 ..."
  idf.py -B "$PROBE_BUILD" set-target esp32c3
fi

case "$ACTION" in
  build)
    exec idf.py -B "$PROBE_BUILD" build
    ;;
  flash)
    [ -n "$PORT" ] || { echo "error: no serial port found; pass one" >&2; exit 1; }
    idf.py -B "$PROBE_BUILD" build || exit 1
    exec idf.py -B "$PROBE_BUILD" -p "$PORT" flash
    ;;
  monitor)
    [ -n "$PORT" ] || { echo "error: no serial port found; pass one" >&2; exit 1; }
    exec idf.py -B "$PROBE_BUILD" -p "$PORT" monitor
    ;;
  *)
    echo "usage: $0 [build|flash|monitor] [port]" >&2
    exit 2
    ;;
esac
