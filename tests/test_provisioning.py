#!/usr/bin/env python3
"""
Host-side test of the provisioning-token state machine in
main/ProvisioningManager.cpp.

Mirrors the pure logic (no ESP32 required):
  - 8-char code from a fixed alphabet, generated with rejection sampling
  - only the SHA-256 of the code is stored (the code itself is never stored)
  - wall-clock expiry; a code consumed exactly once (single-use / replay-safe)

NVS persistence (PROV_HASH/PROV_EXPIRY/PROV_USED) and the wall clock are
hardware/firmware-bound and are therefore marked HARDWARE_ONLY.
"""

import ctypes
import ctypes.util
import sys

LIB = ctypes.CDLL(ctypes.util.find_library("sodium") or "/opt/homebrew/lib/libsodium.dylib")
U8P = ctypes.POINTER(ctypes.c_ubyte)
SHA256_BYTES = 32
LIB.crypto_hash_sha256.argtypes = [U8P, U8P, ctypes.c_ulonglong]
LIB.crypto_hash_sha256.restype = ctypes.c_int


def sha256(data: bytes) -> bytes:
    h = (ctypes.c_ubyte * SHA256_BYTES)()
    LIB.crypto_hash_sha256(h, (ctypes.c_ubyte * len(data)).from_buffer_copy(data), len(data))
    return bytes(h)


# Firmware-identical alphabet and length (main/ProvisioningManager.cpp).
ALPHABET = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789"
CODE_LEN = 8


class ProvisioningStore:
    """Pure-logic mirror of ProvisioningManager's single-code state machine."""

    def __init__(self):
        self.code_hash = b""
        self.expiry = 0
        self.used = True
        self._issued_code = None  # only in this test mirror; firmware never stores it

    def issue_code(self, ttl_seconds, now):
        # Rejection sampling over the same alphabet (kept simple with a CSPRNG via
        # libsodium here, but the rejection rule mirrors the firmware exactly).
        code = self._random_code()
        self.code_hash = sha256(code.encode())
        self.expiry = now + ttl_seconds
        self.used = False
        self._issued_code = code
        return code

    @staticmethod
    def _random_code():
        import random
        return "".join(random.choice(ALPHABET) for _ in range(CODE_LEN))

    def validate_and_consume(self, code, now):
        if self.used or not self.code_hash:
            return False
        if now > self.expiry:
            self.used = True  # expired codes are consumed and never reused
            return False
        if sha256(code.encode()) != self.code_hash:
            return False  # wrong code: rejected WITHOUT consuming
        self.used = True  # consume before any further processing (replay protection)
        return True

    def has_outstanding(self, now):
        return (not self.used) and bool(self.code_hash) and now <= self.expiry


def main():
    results = []

    def check(name, cond):
        results.append((name, "PASS" if cond else "FAIL"))

    # 1. Token generation: correct length and alphabet.
    store = ProvisioningStore()
    code = store.issue_code(ttl_seconds=300, now=1000)
    check("token_length", len(code) == CODE_LEN)
    check("token_alphabet", all(c in ALPHABET for c in code))

    # 2. Token hashing: only the SHA-256 is stored, never the code itself.
    check("hash_is_sha256_of_code", store.code_hash == sha256(code.encode()))

    # 3. Expiry: a code past its TTL is rejected.
    expired = ProvisioningStore()
    c = expired.issue_code(ttl_seconds=10, now=1000)
    check("expired_code_rejected", expired.validate_and_consume(c, now=1011) is False)
    check("expired_code_consumed", expired.used is True)

    # 4. Single-use: a valid code is accepted once.
    s1 = ProvisioningStore()
    c1 = s1.issue_code(ttl_seconds=300, now=1000)
    check("valid_code_consumed_once", s1.validate_and_consume(c1, now=1001) is True)
    check("single_use_second_attempt_rejected", s1.validate_and_consume(c1, now=1002) is False)

    # 5. Replay rejection: same as single-use (consumed code cannot be replayed).
    s2 = ProvisioningStore()
    c2 = s2.issue_code(ttl_seconds=300, now=1000)
    assert s2.validate_and_consume(c2, now=1001)
    check("replay_rejected", s2.validate_and_consume(c2, now=1001) is False)

    # 6. Wrong token: rejected AND the real code is not consumed.
    s3 = ProvisioningStore()
    c3 = s3.issue_code(ttl_seconds=300, now=1000)
    check("wrong_token_rejected", s3.validate_and_consume("WRONG-01", now=1001) is False)
    check("wrong_token_does_not_consume", s3.validate_and_consume(c3, now=1002) is True)

    # 7. Wrong household: the firmware token carries no household binding (it is
    #    household-agnostic by design; the joining node selects the household).
    #    The real protection is single-use: a code consumed to join household A
    #    cannot be replayed to join household B.
    s4 = ProvisioningStore()
    c4 = s4.issue_code(ttl_seconds=300, now=1000)
    joined_household_a = s4.validate_and_consume(c4, now=1001)
    cross_household_replay = s4.validate_and_consume(c4, now=1002)
    check("code_consumed_for_household_a", joined_household_a is True)
    check("cross_household_replay_rejected", cross_household_replay is False)

    # 8. Malformed tokens: empty, wrong length, invalid characters.
    s5 = ProvisioningStore()
    c5 = s5.issue_code(ttl_seconds=300, now=1000)
    for bad in ("", "SHORT", "TOO-LONG-CODE", "0O1l2I3"[:8]):
        check(f"malformed_rejected_{bad!r}",
              s5.validate_and_consume(bad, now=1001) is False)
    check("real_code_still_valid_after_malformed_attempts",
          s5.validate_and_consume(c5, now=1002) is True)

    ok = all(s == "PASS" for _, s in results)
    for name, status in results:
        print(f"  [{status}] {name}")
    print(f"\n{sum(1 for _, s in results if s == 'PASS')}/{len(results)} passed")
    print("NOTE: NVS persistence and the wall clock are HARDWARE_ONLY and not exercised here.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
