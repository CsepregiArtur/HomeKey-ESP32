cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Application version. Bump this when cutting a release: it is what an untagged
# build reports, and what a tagged build is expected to match (a build from an
# existing tag reports the tag itself, via git describe below).
set(HK_APP_VERSION "0.10.0")

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

execute_process(
    COMMAND git status --porcelain
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
idf_build_set_property(MINIMAL_BUILD ON)
project(HomeKey-ESP32)
idf_build_get_property(IDF_TARGET IDF_TARGET)
if(NOT IDF_TARGET STREQUAL "esp32" AND CONFIG_ARDUINO_USE_USB_JTAG STREQUAL "y")
idf_build_set_property(COMPILE_OPTIONS "-DARDUINO_USB_CDC_ON_BOOT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DARDUINO_USB_MODE" APPEND)
endif()
idf_build_set_property(COMPILE_OPTIONS "-DUSE_ESP_IDF_LOG" APPEND)
