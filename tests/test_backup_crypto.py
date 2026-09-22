#!/usr/bin/env python3
"""
Host-side test of the HomeKey Household backup cryptographic construction.

This test calls the REAL libsodium (via ctypes) to reproduce the exact
construction used on the ESP32 firmware (main/BackupManager.cpp):

  key    = BLAKE2b-256("HK-HOUSEHOLD-BACKUP-v1", recovery_secret || salt)
  cipher = XChaCha20-Poly1305-IETF(key, nonce, plaintext, ad=header)
  sig    = Ed25519-detached(header || ciphertext)

It validates round-trip behaviour and fail-closed behaviour for every
negative case required by the hardening plan. It does NOT require the ESP32.
"""

import ctypes
import ctypes.util
import os
import struct
import sys

# --- libsodium bindings -----------------------------------------------------

LIB = ctypes.CDLL(ctypes.util.find_library("sodium") or "/opt/homebrew/lib/libsodium.dylib")

U8P = ctypes.POINTER(ctypes.c_ubyte)
SIZE_T = ctypes.c_size_t
ULL = ctypes.c_ulonglong

LIB.crypto_generichash.argtypes = [U8P, SIZE_T, U8P, ctypes.c_ulonglong, U8P, SIZE_T]
LIB.crypto_generichash.restype = ctypes.c_int

LIB.crypto_aead_xchacha20poly1305_ietf_encrypt.argtypes = [U8P, ctypes.POINTER(ULL), U8P, ctypes.c_ulonglong, U8P, ctypes.c_ulonglong, U8P, U8P, U8P]
LIB.crypto_aead_xchacha20poly1305_ietf_encrypt.restype = ctypes.c_int
LIB.crypto_aead_xchacha20poly1305_ietf_decrypt.argtypes = [U8P, ctypes.POINTER(ULL), U8P, U8P, ctypes.c_ulonglong, U8P, ctypes.c_ulonglong, U8P, U8P]
LIB.crypto_aead_xchacha20poly1305_ietf_decrypt.restype = ctypes.c_int

LIB.crypto_sign_keypair.argtypes = [U8P, U8P]
LIB.crypto_sign_keypair.restype = ctypes.c_int
LIB.crypto_sign_detached.argtypes = [U8P, ctypes.POINTER(ULL), U8P, ctypes.c_ulonglong, U8P]
LIB.crypto_sign_detached.restype = ctypes.c_int
LIB.crypto_sign_verify_detached.argtypes = [U8P, U8P, ctypes.c_ulonglong, U8P]
LIB.crypto_sign_verify_detached.restype = ctypes.c_int

# Constants (must match libsodium and the firmware).
ABYTES = 16
NPUBBYTES = 24
SIGN_BYTES = 64
PK_BYTES = 32
SK_BYTES = 64

def generichash(data: bytes, key: bytes) -> bytes:
    out = (ctypes.c_ubyte * 32)()
    rc = LIB.crypto_generichash(out, 32, (ctypes.c_ubyte * len(data)).from_buffer_copy(data), len(data),
                                (ctypes.c_ubyte * len(key)).from_buffer_copy(key), len(key))
    assert rc == 0, rc
    return bytes(out)

def derive_key(secret: bytes, salt: bytes) -> bytes:
    return generichash(secret + salt, b"HK-HOUSEHOLD-BACKUP-v1")

def encrypt(key: bytes, nonce: bytes, plaintext: bytes, ad: bytes) -> bytes:
    c = (ctypes.c_ubyte * (len(plaintext) + ABYTES))()
    clen = (ctypes.c_ulonglong)()
    rc = LIB.crypto_aead_xchacha20poly1305_ietf_encrypt(
        c, ctypes.byref(clen),
        (ctypes.c_ubyte * len(plaintext)).from_buffer_copy(plaintext), len(plaintext),
        (ctypes.c_ubyte * len(ad)).from_buffer_copy(ad), len(ad),
        None,
        (ctypes.c_ubyte * len(nonce)).from_buffer_copy(nonce),
        (ctypes.c_ubyte * len(key)).from_buffer_copy(key))
    assert rc == 0, rc
    return bytes(c[:clen.value])

def decrypt(key: bytes, nonce: bytes, ciphertext: bytes, ad: bytes):
    m = (ctypes.c_ubyte * len(ciphertext))()
    mlen = (ctypes.c_ulonglong)()
    rc = LIB.crypto_aead_xchacha20poly1305_ietf_decrypt(
        m, ctypes.byref(mlen), None,
        (ctypes.c_ubyte * len(ciphertext)).from_buffer_copy(ciphertext), len(ciphertext),
        (ctypes.c_ubyte * len(ad)).from_buffer_copy(ad), len(ad),
        (ctypes.c_ubyte * len(nonce)).from_buffer_copy(nonce),
        (ctypes.c_ubyte * len(key)).from_buffer_copy(key))
    if rc != 0:
        raise ValueError("AEAD decryption failed")
    return bytes(m[:mlen.value])

def sign_detached(message: bytes, sk: bytes) -> bytes:
    sig = (ctypes.c_ubyte * SIGN_BYTES)()
    siglen = (ctypes.c_ulonglong)()
    rc = LIB.crypto_sign_detached(sig, ctypes.byref(siglen),
                                  (ctypes.c_ubyte * len(message)).from_buffer_copy(message), len(message),
                                  (ctypes.c_ubyte * len(sk)).from_buffer_copy(sk))
    assert rc == 0, rc
    return bytes(sig[:siglen.value])

def verify_detached(sig: bytes, message: bytes, pk: bytes) -> bool:
    return LIB.crypto_sign_verify_detached(
        (ctypes.c_ubyte * len(sig)).from_buffer_copy(sig),
        (ctypes.c_ubyte * len(message)).from_buffer_copy(message), len(message),
        (ctypes.c_ubyte * len(pk)).from_buffer_copy(pk)) == 0

# --- Firmware-identical header serialization --------------------------------

FORMAT_VERSION = 1

def build_header(meta: dict, salt: bytes, nonce: bytes) -> bytes:
    b = bytearray()
    b.append(meta["format_version"])
    hid = meta["household_id"].encode()[:24]
    b.append(len(hid)); b += hid
    nid = meta["node_id"].encode()[:24]
    b.append(len(nid)); b += nid
    b.append(meta["node_role"])
    b.append(meta["generation"])
    b += struct.pack("<I", meta["timestamp"])
    fw = meta["firmware_version"].encode()[:32]
    b.append(len(fw)); b += fw
    b += meta["node_public_key"]
    b += salt
    b += nonce
    return bytes(b)

def parse_header(blob: bytes):
    pos = 0
    fmt = blob[pos]; pos += 1
    hid_len = blob[pos]; pos += 1
    hid = blob[pos:pos+hid_len]; pos += hid_len
    nid_len = blob[pos]; pos += 1
    nid = blob[pos:pos+nid_len]; pos += nid_len
    role = blob[pos]; pos += 1
    gen = blob[pos]; pos += 1
    ts = struct.unpack("<I", blob[pos:pos+4])[0]; pos += 4
    fw_len = blob[pos]; pos += 1
    fw = blob[pos:pos+fw_len]; pos += fw_len
    pk = blob[pos:pos+32]; pos += 32
    salt = blob[pos:pos+16]; pos += 16
    nonce = blob[pos:pos+24]; pos += 24
    return {"format_version": fmt, "household_id": hid.decode(), "node_id": nid.decode(),
            "node_role": role, "generation": gen, "timestamp": ts,
            "firmware_version": fw.decode(), "node_public_key": pk}, salt, nonce, pos

def create_backup(secret, salt, sk, pk, meta, payload):
    key = derive_key(secret, salt)
    nonce = os.urandom(NPUBBYTES)
    header = build_header(meta, salt, nonce)
    ct = encrypt(key, nonce, payload, header)
    msg = header + ct
    sig = sign_detached(msg, sk)
    return msg + sig

def decrypt_backup(blob, secret):
    meta, salt, nonce, header_len = parse_header(blob)
    if meta["format_version"] != FORMAT_VERSION:
        raise ValueError("unsupported format version")
    if len(blob) < header_len + ABYTES + SIGN_BYTES:
        raise ValueError("truncated")
    ct = blob[header_len:-SIGN_BYTES]
    sig = blob[-SIGN_BYTES:]
    if not verify_detached(sig, blob[:-SIGN_BYTES], meta["node_public_key"]):
        raise ValueError("signature verification failed")
    key = derive_key(secret, salt)
    return decrypt(key, nonce, ct, blob[:header_len]), meta

# --- Test harness -----------------------------------------------------------

def main():
    pk = (ctypes.c_ubyte * PK_BYTES)()
    sk = (ctypes.c_ubyte * SK_BYTES)()
    assert LIB.crypto_sign_keypair(pk, sk) == 0
    pk_b = bytes(pk); sk_b = bytes(sk)

    secret = os.urandom(32)
    salt = os.urandom(16)
    payload = (b'{"household":{"household_id":"HOUSE-7F42","household_name":"Home"},'
               b'"node_config":{"mqtt_password":"secret-value-123"},"issuers":[]}')
    meta = {
        "format_version": FORMAT_VERSION,
        "household_id": "HOUSE-7F42",
        "node_id": "GATE-001",
        "node_role": 0,
        "generation": 1,
        "timestamp": 1760000000,
        "firmware_version": "0.9.0",
        "node_public_key": pk_b,
    }

    results = []

    def check(name, fn):
        try:
            fn()
            results.append((name, "PASS"))
        except Exception as e:
            results.append((name, f"FAIL ({e})"))

    def check_rejected(name, fn):
        """Negative test: fn must raise (fail closed) to pass."""
        try:
            fn()
            results.append((name, "FAIL (accepted when it should reject)"))
        except Exception:
            results.append((name, "PASS"))

    blob = create_backup(secret, salt, sk_b, pk_b, meta, payload)

    # Positive
    def round_trip():
        pt, m = decrypt_backup(blob, secret)
        assert pt == payload, "payload mismatch"

    # Negatives — must all fail closed
    def wrong_key():
        decrypt_backup(blob, os.urandom(32))

    def corrupted_ciphertext():
        b = bytearray(blob)
        header_len = parse_header(bytes(b))[3]
        b[header_len + 3] ^= 0xFF
        decrypt_backup(bytes(b), secret)

    def corrupted_tag():
        b = bytearray(blob)
        b[-SIGN_BYTES - 1] ^= 0xFF  # last byte of ciphertext (tag)
        decrypt_backup(bytes(b), secret)

    def corrupted_signature():
        b = bytearray(blob)
        b[-1] ^= 0xFF
        decrypt_backup(bytes(b), secret)

    def truncated():
        decrypt_backup(blob[:-1], secret)

    def unsupported_version():
        b = bytearray(blob)
        b[0] = 0
        decrypt_backup(bytes(b), secret)

    def future_version():
        b = bytearray(blob)
        b[0] = 255
        decrypt_backup(bytes(b), secret)

    check("round_trip", round_trip)
    check_rejected("wrong_key_rejected", wrong_key)
    check_rejected("corrupted_ciphertext_rejected", corrupted_ciphertext)
    check_rejected("corrupted_tag_rejected", corrupted_tag)
    check_rejected("corrupted_signature_rejected", corrupted_signature)
    check_rejected("truncated_rejected", truncated)
    check_rejected("unsupported_version_rejected", unsupported_version)
    check_rejected("future_version_rejected", future_version)

    ok = all(r[1] == "PASS" for r in results)
    for name, status in results:
        print(f"  [{status.split()[0]}] {name}: {status}")
    print(f"\n{sum(1 for _, s in results if s == 'PASS')}/{len(results)} passed")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
