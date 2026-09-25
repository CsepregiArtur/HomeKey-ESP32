#!/usr/bin/env bash
#
# CI build helper for the HomeKey-ESP32 fork.
#
# The fork *supports* flash encryption + Secure Boot V1 + NVS encryption, but
# they are DISABLED in sdkconfig.defaults by default ("Path 1") so the board
# stays fully reversible. While they are off, `idf.py build` produces a normal
# plaintext, unsigned image and no signing key is needed at all.
#
# When the hardening is eventually enabled ("Path 2", see
# docs/content/PATH2_SECURITY_ROLLOUT.md), Secure Boot needs a signing key at
# `keys/secure_boot_signing_key.pem` (configured via
# CONFIG_SECURE_BOOT_SIGNING_KEY). That file is gitignored, so this script
# creates one when it is missing.
#
# ⚠️  The fallback key is EPHEMERAL: it is generated fresh on every CI run, so
#     images built with it will only boot on a device whose eFuses have not been
#     burned with a different key. To publish actually flashable releases once
#     Secure Boot is on, set the `SECURE_BOOT_SIGNING_KEY` repository secret to
#     the base64-encoded PEM of your real key (see docs/content/security.md).
#
# Runs inside the espressif/esp-idf-ci-action container, where espsecure.py is
# already available. Note that the action may invoke this file with `sh` rather
# than bash, so it must stay POSIX-compatible: no `pipefail`, no `[[ ]]`.
#
# The "throwaway key" branch below is only taken when the workflow step that
# installs the SECURE_BOOT_SIGNING_KEY secret did not leave a key behind. Keep
# the two messages visually distinct: a real key must never look like a fallback.
set -eu

KEY_PATH="keys/secure_boot_signing_key.pem"

mkdir -p keys

if [ -f "${KEY_PATH}" ]; then
  echo "::notice title=Secure Boot::SIGNED WITH YOUR KEY - using the provided ${KEY_PATH}."
  # Print a public fingerprint so a reader can tell two builds apart without
  # ever exposing the private key.
  if command -v openssl >/dev/null 2>&1; then
    echo "Secure Boot signing key fingerprint (SHA-256 of the public key):"
    openssl pkey -in "${KEY_PATH}" -pubout 2>/dev/null \
      | openssl dgst -sha256 2>/dev/null \
      | sed 's/^/    /'
  fi
else
  if grep -qE '^CONFIG_SECURE_BOOT=y' sdkconfig 2>/dev/null; then
    echo "::warning title=THROWAWAY SIGNING KEY::${KEY_PATH} not found but CONFIG_SECURE_BOOT=y, so this build is signed with a throwaway ECDSA-P256 key. It only boots on a device with unburned eFuses; set the SECURE_BOOT_SIGNING_KEY secret to publish flashable images."
  else
    echo "::notice title=Secure Boot::Secure Boot is disabled (Path 1), so no signing key is needed for this build."
  fi
  espsecure.py generate_signing_key --version 1 "${KEY_PATH}"
fi

idf.py --ccache build
idf.py merge-bin
