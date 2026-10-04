cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Application version. Bump this when cutting a release: it is what an untagged
# build reports, and what a tagged build is expected to match (a build from an
# existing tag reports the tag itself, via git describe below).
set(HK_APP_VERSION "0.13.0")

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git describe --tags
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE GIT_EXACT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# `git describe --tags` yields the tag only when HEAD is exactly on it; one commit
# later it yields "v0.12.0-1-gabc1234", which a prefix test cannot tell apart. Ask git
# directly, so the release branch below cannot mistake a build *after* a tag for the
# tag itself. Empty output (and a non-zero exit, which ERROR_QUIET swallows) means
# HEAD is not on a tag.
execute_process(
    COMMAND git describe --tags --exact-match
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE GIT_RELEASE_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    # --ignore-submodules: several components under components/ are third-party
    # submodules (pn532_cxx, msgpack-c, HomeSpan) pinned at a SHA in someone else's
    # repository. This project cannot commit into them and cannot push them, so a
    # local change there is not something a released version should advertise - it
    # would turn every build into "-dirty" for a file the release has no way to
    # capture. Only changes to files this repository owns count.
    COMMAND git status --porcelain --ignore-submodules
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE GIT_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(GIT_EXACT_TAG MATCHES "^dev" AND GIT_COMMIT_HASH)
    # The development release is tagged just "dev", which on its own says nothing about
    # which tree it came from. Report it the same way as any other untagged build, so the
    # version still identifies the release line and the exact commit - and so a device
    # running a development build is visibly different from one on a stable tag such as
    # v0.10.0. The OTA page and HomeKit both surface this string.
    set(PROJECT_VER "${HK_APP_VERSION}-dev+${GIT_COMMIT_HASH}")
elseif(GIT_RELEASE_TAG MATCHES "^v[0-9]")
    # HEAD is exactly on a release tag, so report that tag. Without this branch the
    # version falls through to IDF's default, which is `git describe --dirty`, and
    # that counts submodule drift this project cannot commit - so a released image
    # would announce itself as "v0.12.0-dirty". A local edit to a file this
    # repository owns still appends -dirty, because that is a genuine deviation
    # from what the tag contains.
    set(PROJECT_VER "${GIT_RELEASE_TAG}")
    if(GIT_DIRTY)
        string(APPEND PROJECT_VER "-dirty")
    endif()
elseif(NOT GIT_EXACT_TAG AND GIT_COMMIT_HASH)
    # No tag to describe: this is a build straight from a branch, so report the
    # version the tree is based on plus the exact commit rather than a bare hash.
    # The value surfaces in the Web UI (OTA page and device info) and in HomeKit as
    # the firmware revision, which is how a running device is identified.
    set(PROJECT_VER "${HK_APP_VERSION}-dev+${GIT_COMMIT_HASH}")
    if(GIT_DIRTY)
        string(APPEND PROJECT_VER "-dirty")
    endif()
endif()

# ---------------------------------------------------------------------------
# Re-apply the local ACK diagnostic to the pinned pn532_cxx submodule.
#
# `waitForAck()` compares the six-byte ACK against the expected frame and returns
# INVALID_FRAME with no other detail. That single error code covers two situations that
# need opposite fixes:
#
#   * a floating MISO line - nothing is driving it - reads back all 00 or all FF
#   * a real but wrong frame means the chip IS talking, just not the SPI protocol
#
# Logging the bytes actually received tells the two apart immediately, which matters
# because the pin numbers differ per chip and a mis-wired bus looks identical to a
# protocol mismatch from the caller's side.
#
# It cannot live in components/pn532_cxx: that directory is a submodule of
# rednblkx/pn532_cxx, the working tree sits on a detached HEAD, and this project has no
# push access - so a commit there would be unreachable and discarded by the next
# `git submodule update`. Applying it here instead keeps the submodule at its pinned
# SHA, keeps the change visible in this repository's history, and makes it survive a
# submodule update. Idempotent: the marker check means a second configure is a no-op.
# ---------------------------------------------------------------------------
set(_pn532_txn "${CMAKE_CURRENT_LIST_DIR}/components/pn532_cxx/src/transaction.cpp")
if(EXISTS "${_pn532_txn}")
    file(READ "${_pn532_txn}" _pn532_src)
    if(NOT _pn532_src MATCHES "ACK mismatch: got")
        string(REPLACE
            "#include \"pn532_cxx/transport.hpp\""
            "#include \"pn532_cxx/transport.hpp\"\n#include \"esp_log.h\""
            _pn532_src "${_pn532_src}")
        string(REPLACE
            "    if (ack_buf[i] != EXPECTED_ACK[i]) {\n      _valid = false;"
            "    if (ack_buf[i] != EXPECTED_ACK[i]) {\n      // Print what actually came back. The ACK is only ever six specific bytes, so a\n      // mismatch means either a floating bus (all 00 or all FF - nothing is driving\n      // MISO) or a real-but-wrong frame (the chip is talking, just not SPI). Those\n      // need opposite fixes and the error code alone cannot tell them apart.\n      ESP_LOGE(\"pn532\",\n               \"ACK mismatch: got %02X %02X %02X %02X %02X %02X, expected 00 00 FF 00 FF 00\",\n               ack_buf[0], ack_buf[1], ack_buf[2], ack_buf[3], ack_buf[4], ack_buf[5]);\n      _valid = false;"
            _pn532_src "${_pn532_src}")
        file(WRITE "${_pn532_txn}" "${_pn532_src}")
        message(STATUS "pn532_cxx: applied the ACK diagnostic patch (submodule left at its pinned SHA)")
    endif()
endif()

idf_build_set_property(MINIMAL_BUILD ON)
project(HomeKey-ESP32)
idf_build_get_property(IDF_TARGET IDF_TARGET)
if(NOT IDF_TARGET STREQUAL "esp32" AND CONFIG_ARDUINO_USE_USB_JTAG STREQUAL "y")
idf_build_set_property(COMPILE_OPTIONS "-DARDUINO_USB_CDC_ON_BOOT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DARDUINO_USB_MODE" APPEND)
endif()
idf_build_set_property(COMPILE_OPTIONS "-DUSE_ESP_IDF_LOG" APPEND)
