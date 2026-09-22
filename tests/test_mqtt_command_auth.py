#!/usr/bin/env python3
"""
Host-side tests for MQTT command authentication and provisioning-token hashing,
mirroring the libsodium constructions in main/MqttManager.cpp and
main/ProvisioningManager.cpp.

  command MAC = HMAC-SHA256(key, "{ts}{nonce}{req_id}{action}")
  provisioning hash = SHA-256(code)

These are pure constructions (no ESP32 required). Replay protection and expiry
state live in the firmware; the replay set is exercised here as pure logic.
"""

import ctypes
import ctypes.util
import os
import sys

LIB = ctypes.CDLL(ctypes.util.find_library("sodium") or "/opt/homebrew/lib/libsodium.dylib")
U8P = ctypes.POINTER(ctypes.c_ubyte)

LIB.crypto_auth_hmacsha256.argtypes = [U8P, U8P, ctypes.c_ulonglong, U8P]
LIB.crypto_auth_hmacsha256.restype = ctypes.c_int
LIB.crypto_auth_hmacsha256_verify.argtypes = [U8P, U8P, ctypes.c_ulonglong, U8P]
LIB.crypto_auth_hmacsha256_verify.restype = ctypes.c_int
LIB.crypto_hash_sha256.argtypes = [U8P, U8P, ctypes.c_ulonglong]
LIB.crypto_hash_sha256.restype = ctypes.c_int

MAC_BYTES = 32
SHA256_BYTES = 32

def hmac_sha256(key: bytes, message: bytes) -> bytes:
    mac = (ctypes.c_ubyte * MAC_BYTES)()
    rc = LIB.crypto_auth_hmacsha256(mac,
        (ctypes.c_ubyte * len(message)).from_buffer_copy(message), len(message),
        (ctypes.c_ubyte * len(key)).from_buffer_copy(key))
    assert rc == 0, rc
    return bytes(mac)

def hmac_verify(mac: bytes, message: bytes, key: bytes) -> bool:
    return LIB.crypto_auth_hmacsha256_verify(
        (ctypes.c_ubyte * len(mac)).from_buffer_copy(mac),
        (ctypes.c_ubyte * len(message)).from_buffer_copy(message), len(message),
        (ctypes.c_ubyte * len(key)).from_buffer_copy(key)) == 0

def sha256(data: bytes) -> bytes:
    h = (ctypes.c_ubyte * SHA256_BYTES)()
    LIB.crypto_hash_sha256(h, (ctypes.c_ubyte * len(data)).from_buffer_copy(data), len(data))
    return bytes(h)

def command_mac(key, ts, nonce, req_id, action):
    return hmac_sha256(key, f"{ts}{nonce}{req_id}{action}".encode())

def main():
    key = os.urandom(32)
    ts, nonce, req_id, action = 1760000000, "n0nCe123", "req-42", "unlock"
    mac = command_mac(key, ts, nonce, req_id, action)

    results = []

    def check(name, cond):
        results.append((name, "PASS" if cond else "FAIL"))

    # valid command verifies
    check("valid_command_accepts", hmac_verify(mac, f"{ts}{nonce}{req_id}{action}".encode(), key))

    # modified payload (action swapped) rejected
    check("modified_payload_rejected",
          not hmac_verify(mac, f"{ts}{nonce}{req_id}lock".encode(), key))

    # wrong key rejected
    check("wrong_key_rejected",
          not hmac_verify(mac, f"{ts}{nonce}{req_id}{action}".encode(), os.urandom(32)))

    # missing request id (empty) rejected
    check("missing_request_id_rejected",
          not hmac_verify(mac, f"{ts}{nonce}{action}".encode(), key))

    # provisioning token hashing: only the SHA-256 is stored, code never logged
    code = "aB3xY9zQ"
    stored_hash = sha256(code.encode())
    check("provisioning_hash_matches", sha256(code.encode()) == stored_hash)
    check("provisioning_wrong_code_rejected", sha256(b"WRONG-CODE") != stored_hash)

    # replay protection: a seen-nonce set rejects duplicates (pure logic mirror)
    seen = set()
    def accept(n):
        if n in seen:
            return False
        seen.add(n)
        return True
    check("replay_rejected", accept(nonce) and not accept(nonce))

    ok = all(s == "PASS" for _, s in results)
    for name, status in results:
        print(f"  [{status}] {name}")
    print(f"\n{sum(1 for _, s in results if s == 'PASS')}/{len(results)} passed")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
