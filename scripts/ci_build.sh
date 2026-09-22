#!/usr/bin/env bash
#
# CI build helper for the HomeKey-ESP32 fork.
#
# The fork enables flash encryption + Secure Boot V1 + NVS encryption, which
# means Secure Boot needs a signing key at `keys/secure_boot_signing_key.pem`
# (configured via CONFIG_SECURE_BOOT_SIGNING_KEY). That file is gitignored, so
# this script creates one when it is missing.
#
# ⚠️  The fallback key is EPHEMERAL: it is generated fresh on every CI run, so
#     images built with it will only boot on a device whose eFuses have not been
#     burned with a different key. To publish actually flashable releases, set
#     the `SECURE_BOOT_SIGNING_KEY` repository secret to the base64-encoded PEM
#     of your real key (see docs/content/security.md).
#
# Runs inside the espressif/esp-idf-ci-action container, where espsecure.py is
# already available.
set -euo pipefail

KEY_PATH="keys/secure_boot_signing_key.pem"

mkdir -p keys

if [ ! -f "${KEY_PATH}" ]; then
  echo "::notice title=Ephemeral signing key::${KEY_PATH} not found; generating a throwaway ECDSA-P256 Secure Boot V1 key. Set the SECURE_BOOT_SIGNING_KEY secret to build flashable releases."
  espsecure.py generate_signing_key --version 1 "${KEY_PATH}"
else
  echo "Using the provided Secure Boot signing key."
fi

idf.py --ccache build
idf.py merge-bin
