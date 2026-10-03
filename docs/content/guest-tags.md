---
title: "Guest NFC Tags"
weight: 16
---

Guest tags let **ordinary NFC cards** (NTAG213/215/216 stickers, key fobs, …) unlock a
node, the same way a HomeKey tap does, with an optional validity window. They exist for
people who should be able to get in for a while — a cleaner, a dog walker, a guest — but
who do not have a HomeKey credential on an Apple device.

> [!TIP]
> Guest tags can be managed and monitored from Home Assistant — teach, revoke and cancel
> a card write, see the card list and set a default validity window. Managing them needs
> the integration's **direct (TLS) transport**. See
> [Home Assistant Integration → Guest tags](/HomeKey-ESP32/home-assistant/#61-guest-tags).

> [!IMPORTANT]
> **A guest tag is not a HomeKey credential, and it cannot be made into one.**
> HomeKey is Apple's protocol: the authentication is signed with Apple-issued issuer
> keys held in the device's secure element. No third-party tag can produce a valid
> HomeKey authentication. A guest tag is a **separate, locally verified credential** with
> a different (weaker) threat model. Read the [Security model](#security-model) section
> before deciding whether it is appropriate for your door.

## What a guest tag does

* **Unlocks locally.** A verified tap drives the lock exactly like a HomeKey tap.
  No MQTT, no Home Assistant, no internet is involved in a tap — only in *configuring*
  and *distributing* tags.
* **Can expire.** A tag can have a validity window (`valid_from` / `valid_until`), so a
  key fob can stop working on its own.
* **Can be revoked.** Revoking a tag in the firmware removes it immediately; the card
  becomes an unknown card again.
* **Can be shared across the household.** A tag taught on one node is published to the
  household MQTT namespace, so the other nodes can verify the same card offline.
* **Is off by default.** Nothing is accepted until *guest access* is enabled.

## Offline behaviour

**The validity window lives on the node, not in Home Assistant.** It is stored in the
node's NVS guest table (`GuestTagRecord.valid_from` / `valid_until`) and the same window
is written into the card payload. Verification reads the stored record and compares it
against the node's own clock — `GuestTagManager::verify()` — so a tap is decided entirely
on the device. Home Assistant, MQTT and the internet are involved only in *teaching*,
*configuring* and *distributing* tags; never in evaluating one.

What that means in practice:

| Situation | Unbounded tag (no expiry) | Time-bounded tag |
|---|---|---|
| Node online, HA/MQTT/broker down | ✅ works | ✅ works |
| Node offline (no network), running | ✅ works | ✅ works (clock already synced) |
| Node rebooted, clock retained | ✅ works | ✅ works |
| Node cold-booted, no network yet | ✅ works | ❌ refused with `NO_CLOCK` |

**Why the last row.** The ESP32 has no RTC battery and this firmware's only clock source
is SNTP (see `startNetworkTime()` in `main.cpp`). `GuestTagManager::wallClockNow()`
returns 0 until the clock is real, and `verify()` then refuses a time-bounded tag with
`NO_CLOCK` rather than guessing — deliberately, because comparing a calendar window
against uptime would silently accept expired cards.

A normal reboot usually keeps the clock: `CONFIG_NEWLIB_TIME_SYSCALL_USE_RTC_HRT=y`, so
the epoch is held in RTC slow memory and survives a software reset. A full **power cut**
does not. So a time-bounded guest card can stop working after a power failure while the
network is still down — and it will start working again as soon as SNTP answers.

If that is not acceptable for your door, the options are:

* **Teach unbounded tags** (`valid_days` unset and the default validity at `0`). They are
  enforced with no clock at all, and are revoked rather than expiring.
* **Keep the node on a UPS**, or give it a route to an NTP server.
* Accept that a time-bounded tag needs a clock — which is inherent to an *absolute*
  expiry window on a device with no battery-backed time source.

## Hardware requirements

| Reader | Read/verify a guest tag | Teach (write) a guest tag |
|---|---|---|
| **PN532** | ✅ | ✅ |
| PN7160 / PN7161 | ✅ | ❌ |
| ST25R3916 | ✅ | ❌ |

**Writing requires the PN532.** Teaching uses raw ISO14443A Type 2 page writes, and only
the PN532 backend exposes that path. On the other readers guest tags can still be read
and verified — you just cannot create one from that device. Teach it on a PN532 node, or
let another node distribute it.

**Cards:** use **NTAG213** (144 bytes) or larger — NTAG215/216 are needed only if you want
more headroom, the payload is 80 bytes. MIFARE Classic and 125 kHz cards are **not**
supported. A card needs at least 80 bytes of user memory, which MIFARE Ultralight
(48 bytes) does not have.

## Enabling and configuring

Guest access is configured over the **Home Assistant direct API** (TLS + device
credential), or over MQTT.

### HTTP API

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/ha/guest` | GET | Guest access state, default validity, the tag table (no secrets) and the card-writer state |
| `/api/ha/guest/config` | POST | `{"enabled": true}` and/or `{"default_validity_seconds": 86400}` |
| `/api/ha/guest/teach` | POST | Mint a tag and arm a card write |
| `/api/ha/guest/revoke` | POST | `{"tag_id": "A1B2C3D4"}` |
| `/api/ha/guest/cancel` | POST | Cancel an armed card write |

All of these require the device credential over TLS, exactly like `/api/ha/lock`.
Teaching a card mints a credential, so none of it is reachable unauthenticated.

### MQTT

For convenience — and so Home Assistant works without the custom integration — the
following plain topics exist:

| Topic | Direction | Payload |
|---|---|---|
| `<CLIENT_ID>/guest/set_enabled` | subscribe | `1`/`0`, `on`/`off`, `true`/`false` |
| `<CLIENT_ID>/guest/set_validity` | subscribe | Default validity in **seconds** (`0` = no expiry) |
| `<CLIENT_ID>/guest/status` | publish (retained) | Token-free status JSON (this is what HA reads) |
| `homekey/household/<hid>/nodes/<nid>/guest/status` | publish (retained) | Same status, household namespace |
| `homekey/household/<hid>/nodes/<nid>/guest/table` | publish + subscribe | Full table **including per-tag tokens**, for household sync |
| `<CLIENT_ID>/homekey/auth` | publish | Guest taps, with `"guest": true` |

> [!WARNING]
> The `set_enabled` / `set_validity` topics are protected only by broker access — the same
> protection the legacy `homekit/set_target_state` unlock topic has. Prefer the HTTP API
> (authenticated, TLS) for anything security-relevant.

Home Assistant discovery publishes two extra sensors, **Guest access**
(`enabled`/`disabled`) and **Guest tags** (count). Because discovery is
state-only for these, use the HTTP API (or the plain topics) to change them.

### Guest tap payload

A guest tap is published to `<CLIENT_ID>/homekey/auth`:

```json
{
  "guest": true,
  "accepted": true,
  "reason": "ACCEPTED",
  "tagId": "A1B2C3D4",
  "label": "Cleaner",
  "readerId": "A1B2C3D4E5F6"
}
```

Refusals are reported too, with the reason, so an automation can tell an expired card from
an unknown one:

| `reason` | Meaning |
|---|---|
| `ACCEPTED` | Verified, in date, enabled |
| `NO_RECORD` | This card was never taught to this node |
| `DISABLED` | Guest access is off, or this tag is disabled |
| `NOT_YET_VALID` | `valid_from` is in the future |
| `EXPIRED` | `valid_until` has passed |
| `BAD_PAYLOAD` | No guest payload, or it belongs to a different card (UID mismatch) |
| `NO_CLOCK` | The tag is time-bounded but the node has no wall clock yet |

`NO_RECORD` is not published (it would fire for every unknown card); everything else is.

The household `last_auth` sensor also reports `"type": "Guest"` with the guest's label,
so the activity log distinguishes a guest card from an Apple device.

## Teaching a card

Teaching is deliberately **two-phase**, because the NFC reader is owned by the polling
task and cannot be driven from an HTTP or MQTT callback:

1. Call `POST /api/ha/guest/teach` with `{"label": "Cleaner", "valid_days": 7}` (or
   `valid_from`/`valid_until` as Unix seconds).
2. The node answers immediately with the new `tag_id` and arms the write. It also starts
   polling quickly.
3. **Present the card to that node's reader**, within 60 seconds.
4. The node writes the payload, reads it back to verify, and only then stores the
   credential. The result is published on MQTT (`guest/table` re-publish) and reflected in
   `/api/ha/guest` (`write.last_result` = `success` / `failed` / `timeout`).

If no card is presented in time the arm expires and is reported as `timeout`. An expired
arm is never applied to the next card tapped — a HomeKey tap that follows is a normal tap.

Writing fails, with a reason, when the card is not a Type 2 tag, is too small, or the
read-back does not match. A card is only ever considered taught if it verified.

Once the credential is committed, the household table is re-published so the other nodes
import the tag. Each node verifies the same card **locally and offline** afterwards.

## Revoking a tag

`POST /api/ha/guest/revoke {"tag_id": "A1B2C3D4"}` removes it from that node. If the node
is enrolled in a household, the removal propagates to the other nodes. Revoking does not
erase the card — it stays an inert card, and re-teaching overwrites it.

A lost card is therefore handled by revoking, not by re-writing it. Because the payload
is bound to the card's UID, a payload copied onto a *different* card is rejected
(`BAD_PAYLOAD`).

## Security model

This is the honest description of what a guest tag is and is not. It was chosen
deliberately over a stronger design that needs different tags (see
[Upgrade path](#upgrade-path)).

**What the card holds.** An 80-byte payload at the first user page: a magic header, a
random 24-byte nonce, and a `CardPlaintext` blob (tag id, validity window, label)
encrypted with XChaCha20-Poly1305 and authenticated against the card's UID.

**Key.** The encryption key is `BLAKE2b-256(key = tag_token, "HK-GUEST-TAG-v1")`, where
`tag_token` is a random 16-byte secret generated per tag and stored on each node (never
on the card). A card's payload cannot be decrypted — or forged — without the token.

**Binding.** The card UID is the AEAD associated data, so a payload lifted onto a
different card fails to decrypt. Copying the *whole* card (UID and memory, e.g. a "magic"
card) still works, which is the limitation below.

**What is stored where.**

| Item | Where | Notes |
|---|---|---|
| Per-tag token | NVS on each enrolled node | Distributed over the household MQTT topic |
| Card payload | Card user memory | Encrypted; not readable without the token |
| Tag table | NVS, bounded to 16 tags | ~1.2 KiB total |

> [!WARNING]
> **A guest tag is cloneable by anyone who can read the card.** NTAG memory is *static* —
> the tag has no crypto engine, so it cannot do a challenge-response. Anyone with a
> commodity NFC reader can copy a guest card, including its UID on a "magic" card, and the
> clone will work. This is inherent to the card, not to this implementation.
>
> Treat guest tags as **convenience, not security**. For anything you actually rely on
> for access control, use HomeKey, which is cryptographically bound to an Apple device.

> [!WARNING]
> **Wall clock matters.** A time-bounded tag is refused with `NO_CLOCK` until the node has
> synced NTP. Tags without a validity window are unaffected. This is deliberate: comparing
> a calendar window against uptime would silently accept expired tags.

Note also that the guest feature stores its tokens in the same NVS partition as the rest
of the configuration, which is **plaintext at rest** on this firmware (flash encryption
is a deferred, one-way rollout — see
[Security Rollout Plan: Path 1 → Path 2](path2_security_rollout)).

## Limits

* **16 guest tags per node.** NVS is the tightest resource on this device, so the table is
  fixed-size. Revoke a tag to free a slot. A household-wide limit does not exist — each
  node holds its own copy of the table.
* **Labels are 15 characters.**
* **Non-ISO14443A cards are not seen at all.** 125 kHz (HID/EM4100) and ISO14443B tags are
  not polled by any supported reader.
* **Guest tags and HomeKey do not share a slot.** A card that verifies as a guest tag is
  not also reported as a generic tag.

## Upgrade path

If cloneability is not acceptable for your use case, the design leaves room to move to
**NTAG 424 DNA** with SUN/SDM, which does real AES-based challenge-response on the card
and is genuinely anti-clone. That requires those specific tags and a per-tag AES key
provisioning step; the storage layer (`GuestTagRecord`, the tag table and the verify
entry point) is already shaped to accept it. The current `CardPlaintext` codec would be
replaced, not the managers around it.

The middle ground — no code change — is to keep guest tags for low-stakes access and keep
HomeKey as the credential that actually protects the door.
