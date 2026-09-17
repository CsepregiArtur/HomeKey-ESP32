#!/usr/bin/env bash
#
# Expose an existing ESP-IDF checkout as a PlatformIO "framework-espidf" package.
#
# Why this is needed: the newest ESP-IDF available as a PlatformIO registry
# package (`platformio/framework-espidf`) is 5.5.3, which lacks
# `httpd_uri_t::ws_post_handshake_cb` used by main/WebServerManager.cpp. The
# project is built with ESP-IDF 5.5.5 (same as upstream CI).
#
# PlatformIO can use a local folder as a package (`symlink://`), but that folder
# needs a `package.json` manifest - ESP-IDF does not ship one, so this script
# writes it (and nothing else) into the checkout. Pointing PlatformIO directly at
# the checkout matters: a directory of symlinks confuses ESP-IDF's git submodule
# check and its toolchain response-file handling.
#
# It also prepares the matching GCC toolchain that ESP-IDF installed through
# idf_tools.py (`$IDF_TOOLS_PATH`, default ~/.espressif). ESP-IDF 5.5.5 refuses to
# build with any other compiler build, and PlatformIO ships a different one
# (esp-14.2.0_20251107 vs esp-14.2.0_20260121).
#
# Usage:
#   scripts/setup_local_idf_package.sh [<esp-idf-dir>]
#
# Default: $IDF_PATH or ~/esp/esp-idf
#
# Afterwards make sure platformio.ini uses these lines (with your paths):
#   platform_packages =
#       platformio/framework-espidf @ symlink:///Users/you/esp/esp-idf
#       platformio/toolchain-xtensa-esp-elf @ symlink:///Users/you/.espressif/tools/xtensa-esp-elf/<ver>/xtensa-esp-elf

set -euo pipefail

idf_dir="${1:-${IDF_PATH:-$HOME/esp/esp-idf}}"
idf_tools_path="${IDF_TOOLS_PATH:-$HOME/.espressif}"

if [ ! -f "$idf_dir/tools/cmake/version.cmake" ]; then
	echo "error: '$idf_dir' does not look like an ESP-IDF checkout" >&2
	exit 1
fi

major=$(sed -n 's/^set(IDF_VERSION_MAJOR \([0-9]*\))$/\1/p' "$idf_dir/tools/cmake/version.cmake")
minor=$(sed -n 's/^set(IDF_VERSION_MINOR \([0-9]*\))$/\1/p' "$idf_dir/tools/cmake/version.cmake")
patch=$(sed -n 's/^set(IDF_VERSION_PATCH \([0-9]*\))$/\1/p' "$idf_dir/tools/cmake/version.cmake")
# PlatformIO encodes the ESP-IDF version as "<scheme>.<major><minor><patch>.<rev>",
# e.g. ESP-IDF 5.5.3 -> 3.50503.0
pio_version="3.$(printf '%d%02d%02d' "$major" "$minor" "$patch").0"

cat >"$idf_dir/package.json" <<EOF
{
  "name": "framework-espidf",
  "version": "$pio_version",
  "title": "Espressif IoT Development Framework",
  "description": "ESP-IDF $major.$minor.$patch (local checkout)",
  "keywords": ["framework", "esp32", "espressif"],
  "homepage": "https://docs.espressif.com/projects/esp-idf/en/latest/esp32/",
  "license": "Apache-2.0",
  "repository": {
    "type": "git",
    "url": "https://github.com/espressif/esp-idf"
  }
}
EOF

echo "Wrote $idf_dir/package.json (ESP-IDF $major.$minor.$patch -> $pio_version)"

# Prepare the matching GCC toolchain installed by idf_tools.py, so ESP-IDF's
# tool version check passes in PlatformIO.
toolchain_dir="$(ls -d "$idf_tools_path"/tools/xtensa-esp-elf/*/xtensa-esp-elf 2>/dev/null | tail -1 || true)"
if [ -n "$toolchain_dir" ]; then
	tc_release="$(basename "$(dirname "$toolchain_dir")")"   # esp-14.2.0_20260121
	tc_version="${tc_release#esp-}"                          # 14.2.0_20260121
	tc_version="${tc_version/_/+}"                           # 14.2.0+20260121
	cat >"$toolchain_dir/package.json" <<EOF
{
  "name": "toolchain-xtensa-esp-elf",
  "version": "$tc_version",
  "title": "GCC for ESP32 / ESP-IDF",
  "description": "xtensa-esp-elf GCC ($tc_release) installed by idf_tools.py",
  "keywords": ["toolchain", "gcc", "esp32", "xtensa"],
  "license": "GPL-3.0-or-later"
}
EOF
	echo "Wrote $toolchain_dir/package.json ($tc_release -> $tc_version)"
else
	echo "warning: no xtensa-esp-elf toolchain found below $idf_tools_path" >&2
fi

echo
echo "Use these lines in platformio.ini:"
echo "  platform_packages ="
echo "      platformio/framework-espidf @ symlink://$idf_dir"
if [ -n "${toolchain_dir:-}" ]; then
	echo "      platformio/toolchain-xtensa-esp-elf @ symlink://$toolchain_dir"
fi
