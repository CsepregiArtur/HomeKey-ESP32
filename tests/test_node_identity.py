#!/usr/bin/env python3
"""
Host-side test of the node identity model in main/NodeIdentityManager.cpp.

Mirrors the pure logic (no ESP32 required):
  - node_id = "<ROLE_PREFIX>-<generation:03d>"  (generation starts at 1)
  - identity is minted exactly once; duplicate generation is refused
  - replacement always mints a NEW keypair and bumps generation (never clones)
  - cert_fingerprint = SHA-256(node_id || public_key)

The Ed25519 keypair and SHA-256 are real (libsodium via ctypes). NVS persistence
itself is hardware-bound and is therefore marked HARDWARE_ONLY below.
"""

import ctypes
import ctypes.util
import sys

LIB = ctypes.CDLL(ctypes.util.find_library("sodium") or "/opt/homebrew/lib/libsodium.dylib")
U8P = ctypes.POINTER(ctypes.c_ubyte)

PK_BYTES = 32
SK_BYTES = 64
SIGN_BYTES = 64
SHA256_BYTES = 32

LIB.crypto_sign_keypair.argtypes = [U8P, U8P]
LIB.crypto_sign_keypair.restype = ctypes.c_int
LIB.crypto_sign_detached.argtypes = [U8P, ctypes.POINTER(ctypes.c_ulonglong), U8P, ctypes.c_ulonglong, U8P]
LIB.crypto_sign_detached.restype = ctypes.c_int
LIB.crypto_sign_verify_detached.argtypes = [U8P, U8P, ctypes.c_ulonglong, U8P]
LIB.crypto_sign_verify_detached.restype = ctypes.c_int
LIB.crypto_hash_sha256.argtypes = [U8P, U8P, ctypes.c_ulonglong]
LIB.crypto_hash_sha256.restype = ctypes.c_int


def keypair():
    pk = (ctypes.c_ubyte * PK_BYTES)()
    sk = (ctypes.c_ubyte * SK_BYTES)()
    assert LIB.crypto_sign_keypair(pk, sk) == 0
    return bytes(pk), bytes(sk)


def sign_detached(message: bytes, sk: bytes) -> bytes:
    sig = (ctypes.c_ubyte * SIGN_BYTES)()
    siglen = (ctypes.c_ulonglong)()
    LIB.crypto_sign_detached(sig, ctypes.byref(siglen),
                             (ctypes.c_ubyte * len(message)).from_buffer_copy(message), len(message),
                             (ctypes.c_ubyte * len(sk)).from_buffer_copy(sk))
    return bytes(sig[:siglen.value])


def verify_detached(sig: bytes, message: bytes, pk: bytes) -> bool:
    return LIB.crypto_sign_verify_detached(
        (ctypes.c_ubyte * len(sig)).from_buffer_copy(sig),
        (ctypes.c_ubyte * len(message)).from_buffer_copy(message), len(message),
        (ctypes.c_ubyte * len(pk)).from_buffer_copy(pk)) == 0


def sha256(data: bytes) -> bytes:
    h = (ctypes.c_ubyte * SHA256_BYTES)()
    LIB.crypto_hash_sha256(h, (ctypes.c_ubyte * len(data)).from_buffer_copy(data), len(data))
    return bytes(h)


# Firmware-identical role prefix map (main/include/household_types.hpp).
PREFIX = {0: "GATE", 1: "HOUSE", 2: "SMALLHOUSE", 3: "GARAGE", 4: "WORKSHOP", 5: "NODE"}


def node_id(role: int, generation: int) -> str:
    return f"{PREFIX[role]}-{generation:03d}"


def fingerprint(node_id_str: str, public_key: bytes) -> bytes:
    return sha256(node_id_str.encode() + public_key)


class NodeIdentity:
    """Pure-logic mirror of NodeIdentityManager's identity state machine."""

    def __init__(self):
        self.node_id_str = ""
        self.public_key = b""
        self.private_key = b""
        self.generation = 0

    def has_identity(self):
        return bool(self.node_id_str and self.private_key)

    def generate_identity(self, role, name):
        if self.has_identity():
            return False  # duplicate identity rejected (firmware refuses)
        pk, sk = keypair()
        self.generation = 1
        self.node_id_str = node_id(role, self.generation)
        self.public_key = pk
        self.private_key = sk
        return True

    def create_replacement_identity(self, role, name, household_id, old_generation):
        if self.has_identity():
            return False  # a replacement must not clone an existing identity
        pk, sk = keypair()
        self.generation = old_generation + 1
        self.node_id_str = node_id(role, self.generation)
        self.public_key = pk
        self.private_key = sk
        return True


def main():
    results = []

    def check(name, cond):
        results.append((name, "PASS" if cond else "FAIL"))

    # 1. Unique identity generation: two fresh identities get distinct keypairs.
    a, b = NodeIdentity(), NodeIdentity()
    assert a.generate_identity(0, "Gate") and b.generate_identity(0, "Gate")
    check("unique_keypair_generated", a.public_key != b.public_key and a.private_key != b.private_key)

    # 2. Deterministic / stable node id format.
    check("stable_node_id_format", a.node_id_str == "GATE-001")

    # 3. Fingerprint is deterministic over (node_id || public_key).
    f1 = fingerprint(a.node_id_str, a.public_key)
    f2 = fingerprint(a.node_id_str, a.public_key)
    check("deterministic_fingerprint", f1 == f2 and len(f1) == 32)

    # 4. Replacement identity: new id (generation bumped), new keypair.
    old_pk, old_sk, old_id = a.public_key, a.private_key, a.node_id_str
    r = NodeIdentity()
    assert r.create_replacement_identity(0, "Gate", "HOUSE-7F42", old_generation=1)
    check("replacement_new_node_id", r.node_id_str == "GATE-002")
    check("replacement_new_keypair", r.public_key != old_pk and r.private_key != old_sk)

    # 5. Duplicate identity rejection: minting over an existing identity fails.
    check("duplicate_identity_rejected", a.generate_identity(0, "Gate") is False)
    check("replacement_over_existing_rejected",
          a.create_replacement_identity(0, "Gate", "HOUSE-7F42", 1) is False)

    # 6. Private-key non-cloning: an old key cannot produce signatures that the new
    #    key verifies, and vice versa.
    msg = b"household-command"
    sig_old = sign_detached(msg, old_sk)
    check("old_key_does_not_verify_under_new_pk", not verify_detached(sig_old, msg, r.public_key))
    sig_new = sign_detached(msg, r.private_key)
    check("new_key_does_not_verify_under_old_pk", not verify_detached(sig_new, msg, old_pk))
    check("replacement_signature_self_verifies", verify_detached(sig_new, msg, r.public_key))

    ok = all(s == "PASS" for _, s in results)
    for name, status in results:
        print(f"  [{status}] {name}")
    print(f"\n{sum(1 for _, s in results if s == 'PASS')}/{len(results)} passed")
    print("NOTE: NVS persistence (save/load/reboot) is HARDWARE_ONLY and not exercised here.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
