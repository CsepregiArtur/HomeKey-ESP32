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
# Apply the local PN532 driver patches to the pinned submodules.
#
# components/pn532_cxx and components/pn532_hal are submodules of rednblkx/pn532_cxx
# and rednblkx/esp-hal-pn532. Their working trees sit on a detached HEAD and this
# project has no push access - so a commit there would be unreachable and discarded by
# the next `git submodule update`. Instead the local driver changes (including the ACK
# diagnostic that used to be applied inline here) are kept as patch files under
# patches/ and applied at configure time: the submodules stay at their pinned SHAs, the
# changes are visible in this repository's history, and they survive a submodule
# update.
#
# Idempotent: a patch whose content is already present makes `git apply --check` fail,
# so it is skipped and a second configure is a no-op.
# ---------------------------------------------------------------------------
foreach(_hk_submodule pn532_cxx pn532_hal)
    set(_hk_patch "${CMAKE_CURRENT_LIST_DIR}/patches/${_hk_submodule}.patch")
    set(_hk_subdir "${CMAKE_CURRENT_LIST_DIR}/components/${_hk_submodule}")
    if(EXISTS "${_hk_patch}" AND IS_DIRECTORY "${_hk_subdir}")
        execute_process(
            COMMAND git apply --check "${_hk_patch}"
            WORKING_DIRECTORY "${_hk_subdir}"
            RESULT_VARIABLE _hk_check
            OUTPUT_QUIET ERROR_QUIET)
        if(_hk_check EQUAL 0)
            execute_process(
                COMMAND git apply "${_hk_patch}"
                WORKING_DIRECTORY "${_hk_subdir}"
                RESULT_VARIABLE _hk_applied
                OUTPUT_QUIET ERROR_QUIET)
            if(_hk_applied EQUAL 0)
                message(STATUS "${_hk_submodule}: applied local patch (submodule left at its pinned SHA)")
            else()
                message(WARNING "${_hk_submodule}: failed to apply patches/${_hk_submodule}.patch")
            endif()
        endif()
    endif()
endforeach()

idf_build_set_property(MINIMAL_BUILD ON)
project(HomeKey-ESP32)
idf_build_get_property(IDF_TARGET IDF_TARGET)
if(NOT IDF_TARGET STREQUAL "esp32" AND CONFIG_ARDUINO_USE_USB_JTAG STREQUAL "y")
idf_build_set_property(COMPILE_OPTIONS "-DARDUINO_USB_CDC_ON_BOOT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DARDUINO_USB_MODE" APPEND)
endif()
idf_build_set_property(COMPILE_OPTIONS "-DUSE_ESP_IDF_LOG" APPEND)
