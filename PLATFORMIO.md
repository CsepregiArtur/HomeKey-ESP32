# Building & flashing with PlatformIO

`platformio.ini` (plus the two hooks in `scripts/`) allows building, flashing and
monitoring this project with PlatformIO instead of `idf.py`.

```bash
pio run                # build firmware (app + littlefs web UI image)
pio run -t upload      # flash bootloader, partition table, otadata, littlefs, app
pio device monitor     # 115200 baud, UART0
pio run -t erase       # erase the whole flash
pio run -t menuconfig  # ESP-IDF menuconfig
```

Requirements:

* PlatformIO Core/IDE,
* `node`/`npm` on `PATH` (only the first time, to build the web UI),
* a local **ESP-IDF v5.5.5** checkout, prepared once with

  ```bash
  scripts/setup_local_idf_package.sh          # uses $IDF_PATH or ~/esp/esp-idf
  ```

* the component submodules below `components/` checked out (see `.gitmodules`).

## Environment

| Item | Value | Note |
| --- | --- | --- |
| Board | `esp32dev` | ESP32, 4 MB flash |
| Platform | `espressif32@6.13.0` | last platform pairing ESP-IDF 5.5.x with GCC 14.2 |
| Framework | local ESP-IDF v5.5.5 | via `platform_packages`, see below |
| Toolchain | local `xtensa-esp-elf` (`esp-14.2.0_20260121`) | the compiler ESP-IDF 5.5.5 expects, also via `platform_packages` |
| Partition table | `with_ota.csv` | `board_build.partitions`, must match `sdkconfig.defaults` |
| sdkconfig | `sdkconfig.defaults`, `sdkconfig.defaults.esp32` | read by ESP-IDF itself |

### Why a local ESP-IDF

* Platform 7.x is unusable: it ships ESP-IDF 6.1, which no longer contains the
  `mqtt` and `json` components required by `main/CMakeLists.txt`.
* The newest ESP-IDF in the PlatformIO registry (`platformio/framework-espidf`
  `3.50503.0` = 5.5.3) is *also* too old: `httpd_uri_t::ws_post_handshake_cb`,
  used in `main/WebServerManager.cpp`, was added in 5.5.4.
* Therefore `platformio.ini` maps `framework-espidf` onto the local v5.5.5
  checkout with `platform_packages = platformio/framework-espidf @ symlink://…`.
  PlatformIO requires a `package.json` manifest in that folder, which
  `scripts/setup_local_idf_package.sh` writes (`~/esp/esp-idf/package.json`); the
  ESP-IDF tree itself is not modified otherwise. Pointing at the real checkout
  (not a directory of symlinks) matters - symlinks break ESP-IDF's git submodule
  check and its toolchain response-file handling.

The ESP-IDF component manager runs during CMake configure, so
`main/idf_component.yml` (arduino-esp32, joltwallet/littlefs, espp/serialization)
and the manifests of the local components are resolved into `managed_components/`
and pinned in `dependencies.lock` on the first build.

## What the PlatformIO hooks do

`scripts/pio_pre_idf.py` (loaded with `pre:`, i.e. before CMake configure):

1. builds the web UI (`data/dist`) with `npm` when it is missing,
2. sets `CI=true`, which makes `main/CMakeLists.txt` skip its `bun`-based `webui`
   target and consume the pre-built `data/dist` instead (same as upstream CI),
3. registers PlatformIO's "generic files" component (`__pio_env`) in
   `EXTRA_COMPONENT_DIRS`/`COMPONENTS` — without this, ESP-IDF's
   `MINIMAL_BUILD` filters the component out and the build stops with
   *"Failed to find the default IDF component with build information for generic files"*,
4. expands ESP-IDF's flag response files: since ESP-IDF 5.5.4 the toolchain keeps
   its global compiler flags in `@…/toolchain/cflags|cxxflags|asmflags` files that
   CMake references from the compile fragments. PlatformIO 6.13 hands those
   fragments to SCons' flag parser, which silently drops the unknown `@file`
   token — components that rely on it (mqtt, freertos, …) were compiled without
   `-mlongcalls` and linking failed with ~15 000
   `dangerous relocation: call8: call target out of range` errors. The hook
   substitutes the file contents before parsing, so the flags reach the compiler
   exactly as in an `idf.py` build.

`scripts/pio_post_flash_fs.py` (loaded with `post:`):

1. runs the ESP-IDF target `littlefs_spiffs_bin` — PlatformIO compiles with SCons
   and never executes ESP-IDF's ninja targets, so the littlefs image that
   `main/CMakeLists.txt` asks for (`littlefs_create_partition_image`, built with
   littlefs-python) would otherwise not exist. This produces
   `build/spiffs.bin`, identical to an `idf.py` build;
2. appends that image to the esptool command line at the offset of the `spiffs`
   partition (taken from `board_build.partitions`), because PlatformIO only
   flashes the bootloader, partition table, otadata and application by itself.

Result: `pio run -t upload` flashes the complete device in one command.


## Versioning

Two values have to be bumped together when cutting a release, and a tag keeps builds
identifiable on a running device:

* `HK_APP_VERSION` in the root `CMakeLists.txt` - the application version an untagged
  build reports.
* `version` in `data/package.json` - the web interface version, shown in the device info
  panel as `<version>+<commit>`.

What a build reports:

| Build | Version string |
| --- | --- |
| From a release tag (`v0.9.0`) | `v0.9.0` (via `git describe --tags`) |
| From a branch with no reachable tag | `0.9.0-dev+<commit>` |
| ... and with uncommitted changes | `0.9.0-dev+<commit>-dirty` |

The value is visible in the Web UI (OTA page and device info panel) and in HomeKit as the
firmware revision. Tag releases as `vX.Y.Z`: `wiki.yml` publishes every `v*` tag that
contains a `docs/` directory under `/<tag>/`, which is what the version switcher in
`docs/hugo.yaml` links to. Add a matching section to `CHANGELOG.md`.

## Web UI payload budget

The web UI is served from the `spiffs` partition (0x20000, 128 kB) as pre-compressed
files, and littlefs is mounted with 4 KiB blocks (esp_littlefs hardcodes that, because
ESP32 flash erases in 4 KiB sectors). That leaves roughly 108 kB for assets, so the
space is genuinely tight:

* The assets are brotli-compressed (`data/vite.config.ts`). Brotli is ~17% smaller
  than gzip here (91 kB vs 110 kB), which is the only reason the modern UI still fits.
  Shipping both encodings would need about 200 kB and is not an option.
* The firmware serves brotli, gzip or uncompressed files, whichever the flashed image
  contains (`resolveAsset()` in `main/WebServerManager.cpp`), so firmware and
  filesystem image can be updated independently - but updating *only* the image onto
  older firmware leaves the UI unable to load, because the old firmware only looks for
  `.gz`. Update the firmware first; this is documented under "Breaking changes" in
  `docs/content/updates.md`.
* `scripts/pio_post_flash_fs.py` prints the payload against the usable budget on every
  build and warns when the headroom gets thin. Without it, overshooting shows up as a
  bare `LittleFSError -28: LFS_ERR_NOSPC` from inside littlefs-python.
* After changing anything under `data/`, rebuild the assets before building the
  firmware, because the littlefs image is generated from `data/dist`:

  ```bash
  cd data && npm run build && find dist \( -name '*.css' -o -name '*.js' \) -delete
  ```

  (Only the compressed files are kept; the firmware never serves the originals.)

## Known differences to the `idf.py` build

* `build_unflags` removes the `-DFMT_THROW(x)=...` option that
  `main/CMakeLists.txt` propagates from the `fmt` target: PlatformIO passes
  compile options through a shell, which cannot represent that function-like
  macro (unescaped parentheses). Without it, fmt uses its own no-exceptions
  `FMT_THROW` fallback, which also ends in `fmt::assert_fail()`; only the text of
  the abort message differs (`idf.py` builds keep the override).
* `board_build.embed_txtfiles` lists the server certificates of `esp_insights`
  and `esp_rainmaker`. Those components embed them with ESP-IDF's
  `target_add_binary_data()`, which generates an assembly source that PlatformIO
  does not create; declaring the same files makes PlatformIO generate the very
  same assembly (same cmake script, same symbols).
* The web UI image is produced by ESP-IDF's own littlefs pipeline
  (`littlefs-python` through the `littlefs_spiffs_bin` target), not by
  PlatformIO's filesystem feature — `board_build.filesystem` is deliberately
  unset, so `pio run -t uploadfs` is *not* the way to update the web UI. Use the
  normal `pio run -t upload`, which flashes application and web UI together.
