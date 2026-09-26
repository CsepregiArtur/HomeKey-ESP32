#include "sdkconfig.h"

#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
// arduino-esp32 already defines this (weakly) when rollback is enabled, and HomeSpan
// resolves it against that definition. Nothing to add - see the #else branch.
#else
/**
 * @brief Satisfies HomeSpan's reference to `verifyRollbackLater()` on a no-OTA build.
 *
 * HomeSpan's `configureNetwork()` prints the rollback state and uses the symbol that
 * `HomeSpan.h` describes as "pre-defined Arduino-ESP32 version". arduino-esp32 only
 * defines it when `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on, because rollback is an
 * OTA concept: it exists so a freshly OTA'd image that fails to boot can be abandoned in
 * favour of the previous slot.
 *
 * The single-slot (no OTA) layout turns that option off, so nothing provides the symbol
 * and the link fails. Returning `false` means "do not defer verification": there is no
 * previous slot to fall back to and no OTA data partition recording the state, so the
 * sketch is valid the moment it runs. That is also why `HomeSpan::begin()`'s
 * "OTA REQUIRED BUT NOT ENABLED" rollback path has nothing left to do on this layout.
 *
 * A sketch that wants deferred verification includes `SpanRollback.h`, which defines this
 * to return true - that is part of the OTA feature this build no longer has.
 */
extern "C" bool verifyRollbackLater() { return false; }
#endif
