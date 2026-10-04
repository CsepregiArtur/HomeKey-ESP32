#!/bin/sh
# Build a firmware image for an ESP32-C3 on macOS/Homebrew hosts.
#
# WHY THIS EXISTS
#
# `idf.py set-target esp32c3` fails on a Homebrew CMake host with:
#
#     clang: error: unknown argument: '--traditional-format'
#     clang: error: unsupported argument 'rv32imafdc_zicsr_zifencei' to option '-march='
#
# That message is misleading. The RISC-V GCC driver is fine and is the one running;
# what fails is the ASSEMBLER step. IDF's `riscv32-esp-elf-as` is not binutils but a
# small Rust dispatcher that picks between `riscv32-esp-elf-as-xespv1/v2p1/v2p2`
# (the real GNU assembler). When it cannot resolve its target variant it exits
# non-zero, GCC's `-print-prog-name=as` reports the bare name `as`, and the OS then
# resolves that to `/usr/bin/as`, which is Apple clang. clang is handed RISC-V
# flags and rejects them.
#
# The fix is to put a directory on PATH containing an `as` that points straight at
# the real GNU assembler, bypassing the dispatcher entirely. `ld` is shimmed for
# the same reason.
#
# This is a HOST workaround and does not change the project. It is used instead of
# `idf.py set-target esp32c3` (the target switch itself is safe; the assembler
# resolution is what breaks).
#
# USAGE
#   ./scripts/build_esp32c3.sh set-target   # configure the build for esp32c3
#   ./scripts/build_esp32c3.sh build        # compile

set -eu

IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf}"
TOOLCHAIN_DIR="$HOME/.espressif/tools/riscv32-esp-elf"

if [ ! -d "$IDF_DIR" ]; then
  echo "error: ESP-IDF not found at $IDF_DIR (set IDF_PATH)" >&2
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

# The real GNU assembler and linker. The `-xespv2p2` build is the ISA variant this
# ESP32-C3 (rv32imc, no F/D, no xesp extension) needs; `as-xespv2p1` is an earlier
# ISA revision kept for older chips and is picked automatically by the dispatcher
# when it works.
AS_BIN="$TC/riscv32-esp-elf-as-xespv2p2"
LD_BIN="$TC/riscv32-esp-elf-ld"
if [ ! -x "$AS_BIN" ]; then
  echo "error: $AS_BIN not found - the toolchain install is incomplete" >&2
  exit 1
fi

# Shim directory. Recreated every run so a stale or wrong link cannot linger.
SHIM="${TMPDIR:-/tmp}/hk-riscv-shim"
rm -rf "$SHIM"
mkdir -p "$SHIM"
ln -s "$AS_BIN" "$SHIM/as"
[ -x "$LD_BIN" ] && ln -s "$LD_BIN" "$SHIM/ld"

echo "toolchain : $TC"
echo "assembler : $AS_BIN"
echo "shim      : $SHIM"
echo

# Put the shim first, then the toolchain, then let export.sh add the rest.
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

ACTION="${1:-build}"
case "$ACTION" in
  set-target)
    # The root sdkconfig is the reason `rm -rf build` alone is not enough.
    #
    # `idf.py set-target` applies the requested target through build/CMakeCache and a
    # generated sdkconfig *only when there is no sdkconfig to prefer*. This tree keeps a
    # checked-in sdkconfig at the repo root (a classic-ESP32 one), and IDF reads that in
    # preference to the target argument. So a plain `rm -rf build && idf.py set-target
    # esp32c3` silently builds for esp32 instead, while build/flash_args keeps the C3
    # offsets from the generate step - two files in the same directory describing
    # different chips. That combination is worse than either: the offset lookup reads
    # flash_args, so it would write an esp32 bootloader to 0x0, which is where a C3
    # bootloader goes and where an esp32 one does not.
    #
    # Moving it aside (not deleting it) keeps the configured values recoverable and
    # lets set-target write a fresh sdkconfig for the new target.
    rm -rf build
    if [ -f sdkconfig ]; then
      backup="sdkconfig.esp32.bak"
      mv sdkconfig "$backup"
      echo "existing sdkconfig moved to $backup so the target switch can take effect"
    fi
    exec idf.py set-target esp32c3
    ;;
  build|"")
    # reconfigure so the target switch is picked up even after a failed attempt
    idf.py reconfigure
    exec idf.py build
    ;;
  *)
    echo "usage: $0 [set-target|build]" >&2
    exit 2
    ;;
esac
