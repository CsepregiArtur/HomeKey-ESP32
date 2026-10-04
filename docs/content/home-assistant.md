---
title: "Home Assistant Integration"
weight: 10
---

# Home Assistant Integration

> [!IMPORTANT]
> **Home Assistant is not required for local HomeKey unlocking.** The chain
> NFC → ESP32 → HomeKey authentication → lock works entirely on its own, with no
> Home Assistant, no MQTT broker and no internet. This integration is a
> **management and control plane**, not a dependency for local access. If your Home
> Assistant is down, or you never install this, the door still opens.

Upstream HomeKey-ESP32 exposes its MQTT topics for Home Assistant to consume with
MQTT discovery. This fork keeps that path unchanged and **adds a second transport**:
a custom integration that talks to the node directly over its own HTTPS API, with
the node's certificate pinned.

## 1. The component

The integration is a Home Assistant custom component called **`homekey_household`**.
It is a **separate repository** from the firmware, so it can be published through
HACS independently:

### **➡️ [CsepregiArtur/homekey-household](https://github.com/CsepregiArtur/homekey-household)**

> [!NOTE]
> This is deliberately *not* shipped inside this firmware repository. An earlier
> revision built a second, parallel component here; that would have given users two
> integrations claiming the same devices and the same entity ids. The work was moved
> into the existing integration as an additional transport instead. See
> [`HA_INTEGRATION_PLAN.md`](https://github.com/CsepregiArtur/HomeKey-ESP32/blob/main/HA_INTEGRATION_PLAN.md)
> in the firmware repository for the full design history.

## 2. Two transports, one set of entities

| Transport | How it works | Covers | Needs a broker |
| --- | --- | --- | --- |
| **`mqtt`** | The documented household MQTT API (see [MQTT Household API](/HomeKey-ESP32/mqtt_household_api/)), reusing Home Assistant's core MQTT integration | a **whole household** in one entry | yes |
| **`direct`** | The node's own HTTPS API (`/api/ha/*`), with its certificate pinned to an exact fingerprint | **one node** per entry | **no** |

```
HomeKey Household
├── Household (HOME-001)
│   ├── Gate        (GATE-001)
│   ├── Main House  (HOUSE-001)
│   ├── Small House (SMALL-001)
│   └── Garage      (GARAGE-001)
```

Both transports produce **identical coordinator state**, so each node appears as its
own Home Assistant device with a lock and the documented household entities either
way — and nothing downstream can tell which transport is in use. A `/api/ha/state`
response is restated as the messages the MQTT transport would have delivered and fed
to the *same* ingestion path, so the two cannot drift apart in how they read the same
firmware: there is only one reader.

## 3. Installation

### 3.1. HACS (recommended)

1. In Home Assistant, open **HACS → Integrations**.
2. Add `https://github.com/CsepregiArtur/homekey-household` as a custom repository
   (category: *Integration*).
3. Install **HomeKey Household** and restart Home Assistant.

### 3.2. Manual

1. Copy `custom_components/homekey_household/` into `<config>/custom_components/`.
2. Restart Home Assistant.

## 4. Configuration

### 4.1. Broker-less (`direct`) — the node is discovered automatically

This is the path for people who do not run an MQTT broker. The device advertises
itself over mDNS, so there is nothing to look up:

1. **Settings → Devices & Services**. A node advertising `_homekey._tcp` on the
   local network appears as a **discovered** card.
2. **Confirm the certificate fingerprint** shown matches the one on the node's own
   Web UI (**Misc → Security**, or the boot log). This is the trust decision.
3. Enter the **node's Web UI credentials**.

No broker, no household id to look up — the node reports its own identity.

> [!NOTE]
> The node must have **HTTPS enabled**. The integration refuses to configure a node
> that advertises `tls=0`, and the firmware's `haRequireTls()` refuses its own config
> and state endpoints over plain HTTP with `503`.

### 4.2. Over MQTT

1. Configure the core **MQTT** integration (this integration reuses it).
2. **Settings → Devices & Services → Add Integration → HomeKey Household**.
3. Choose the **MQTT** transport and enter the **Household ID**. To enable lock
   control, also enter the household **recovery secret** — it is used once to derive
   the HMAC command key and is **never stored or transmitted**.

Nodes are then discovered automatically from the household namespace; you do not add
them one by one.

## 5. Trust model of the broker-less transport

**Why it exists:** the MQTT path needs a broker running, correct and reachable before
a single node shows up. A node on the same LAN needs none of that.

**Trust — certificate pinning.** The node generates its own ECDSA P-256 key and a
self-signed certificate on first boot, so every unit is unique. There is no CA to
chain to and no subject that can match a DHCP address, which means the SHA-256
fingerprint shown in the node's Web UI and advertised over mDNS **is** the trust
anchor. The integration pins it: the certificate must match exactly, and the check is
**repeated on every Home Assistant start**, so a swapped or factory-reset device is
refused rather than trusted because it once was. Pinning is enforced by the TLS layer
itself — the confirmed certificate is loaded as the only trusted root with
`CERT_REQUIRED`, which is stronger than comparing fingerprints after the fact.

**Authorisation.** The node authorises commands with its own Web UI credential over
that pinned TLS connection — the same boundary that already protects
`/reboot_device`, `/backup/restore` and `/recovery/export`. The credential is stored
in the config entry (the same way the core MQTT integration stores a broker password),
because the node has to be authenticated on every poll. If it is ever rejected, Home
Assistant starts a **reauthentication flow** instead of failing silently.

**Polling.** MQTT pushes; this transport **polls every 30 s**. A single slow response
does not mark the node unavailable: the ESP32 serves TLS from the same chip that runs
HomeKit, so **three consecutive failures** are required before the entities go
unavailable, and the node's last known state is kept until then.

**What it does not do.** One entry covers one node, so a household over the direct
transport produces one entry per node. Audit and provisioning remain HTTP-only on the
firmware and are not exposed as entities.

## 6. Entities

| Entity | Platform | Unique id |
| --- | --- | --- |
| Lock | `lock` | `<hid>_<nid>_lock` |
| Node online | `binary_sensor` | `<hid>_<nid>_online` |
| Node health | `sensor` | `<hid>_<nid>_health` |
| Backup status | `sensor` | `<hid>_<nid>_backup` |
| Security status | `sensor` | `<hid>_<nid>_security` |
| Firmware version | `sensor` | `<hid>_<nid>_firmware` |
| Last HomeKey authentication | `sensor` | `<hid>_<nid>_last_auth` |
| Guest tags | `sensor` | `<hid>_<nid>_guest_tags` |
| Guest access | `switch` | `<hid>_<nid>_guest_access` |
| Guest default validity | `number` | `<hid>_<nid>_guest_validity` |
| Back up now | `button` | `<hid>_<nid>_backup_now` |
| Teach guest card | `button` | `<hid>_<nid>_guest_teach` |
| Cancel guest card write | `button` | `<hid>_<nid>_guest_cancel` |

**Services:** `create_backup`, `restore_backup`, `guest_teach`, `guest_revoke`,
`guest_cancel` — all under `homekey_household.`.

Device identity is `household_id + node_id`, **never** the MAC address or the HomeKit
`deviceID`, so a replacement node keeps its entities.

### 6.1. Guest tags

Guest tags are an ordinary NFC card that unlocks the same way a HomeKey tap does, with
an optional validity window — a **locally verified** credential, not a HomeKey one.
Managing them needs the **direct (TLS)** transport; the state arrives over both.

A node holds up to **16** guest cards, each with its own label and validity window.
One `guest_teach` call arms one write, so teaching three cards means arm-and-tap three
times. Re-teaching a card the node already knows **refreshes that card's entry
instead of spending a second slot** — so a lost card is revoked, not overwritten.

`sensor.<node>_guest_tags` has the count as its state and the cards as attributes
(`tags` with `tag_id`, `label`, `uid`, `enabled`, `expires`, `last_used_at`,
`use_count`, plus `capacity`, `default_validity_days`, `write_armed`,
`write_supported`, `last_write_result`, `last_write_message`, `node_has_wall_clock`).
A card's per-tag secret is in neither the sensor nor diagnostics — the node never
publishes it, so nothing readable from Home Assistant could clone a card. See
[Guest NFC Tags](/HomeKey-ESP32/guest-tags/).

### 6.2. Knowing who opened the door

A change asked for **from** Home Assistant carries your service call's context — which
is why the activity log names you. A change made **at the door** had nobody to
attribute to.

The firmware publishes `B/lock/last` immediately before the state it explains, saying
what asked for the change — `homekit`, `homekey`, `mqtt`, `api` or `device`. The
integration turns a device-originated change into a cause sharing the state change's
context, so the activity log reads *"Gate unlocked by HomeKit"* instead of *"No cause
was recorded"*.

A HomeKey tap can say more than the mechanism, because the node also publishes
`B/last_auth` with **the name you gave that controller** (set it in the node's Web UI
under Dashboard → HomeKey → an issuer — HomeKit never tells the accessory a name, only
an opaque pairing id and a public key). When the node stamps the authorisation and the
change it produced from the same reading of its clock, the log names the person:
*"Gate unlocked by Artur"*. If the stamps differ, the mechanism is named instead —
less specific, and never wrong. Only the **name** is sent, never the issuer id, and
only when you have given one.

The device is the authority: a cause is only used for the change it actually
describes. If the node reports a cause for a different change, or none at all on older
firmware, nothing is claimed rather than guessed at.

## 7. Backups and restores

A backup exists only as the reply to a request on the node's own HTTPS API, so the
integration asks on a schedule (**daily, newest 7 kept per node**) and on demand, and
keeps the copies. **The node keeps none** — only the time and hash of the last one.

| Shape | Restores | A replacement node needs |
| --- | --- | --- |
| **Configuration only** *(default)* | household membership, configuration, issuers | every device and tag **re-enrolled** |
| **With the node's keys** | the above **plus** its reader credential store and HomeKit pairing state | **nothing** — it comes back as the same device |

A credential-carrying copy is the keys to the door, so it is **off by default**. Turn
it on per entry under **Configure**, or ask for a single copy:

```yaml
action: homekey_household.create_backup
data:
  config_entry_id: 01J...
  include_credentials: true
```

A restore needs a stored copy **and** the household recovery secret — which is never
stored here, because it is both the key the backup was sealed with and the proof of the
right to rejoin:

```yaml
action: homekey_household.restore_backup
data:
  config_entry_id: 01J...      # which node
  recovery_secret: 9f2c...     # required, passed to the node and not kept
  # backup: 0107a1b2...       # optional; omitted = newest stored copy
```

The flow is visible in Home Assistant: `sensor.<node>_backup` exposes
`stored_backups`, `stored_includes_credentials`, `stored_backups_detail`,
`api_configured` and a `restore` block, and diagnostics adds a `backup_store` summary.
See [Backup and full restore](https://github.com/CsepregiArtur/homekey-household/blob/main/docs/BACKUP_AND_RESTORE.md)
for the step-by-step replacement-node procedure and its limits. The firmware side is in
[Household & Node Architecture](/HomeKey-ESP32/household/).

## 8. Topics used (MQTT transport)

Household base `B = homekey/household/<household_id>/nodes/<node_id>`:

| Direction | Topic |
| --- | --- |
| Subscribe | `B/state`, `B/status`, `B/health`, `B/security` |
| Subscribe | `B/backup/status`, `B/backup/last`, `B/last_auth` |
| Subscribe | `<CLIENT_ID>/status` (shared broker LWT availability only) |
| Publish | `B/command/lock`, `B/command/unlock` (HMAC-SHA256 authenticated) |

Reserved/not-implemented topics (`B/events`, `B/backup/{request,data}`,
`B/restore/*`) are never used: backup, restore, audit and provisioning are **HTTP-only
on the firmware**, and the integration reaches them over the node's own API rather than
over MQTT.

## 9. The direct transport's HTTP surface

Documented by the firmware; listed here so the contract the client implements is
visible in one place.

| Method | Path | Auth | Purpose |
| --- | --- | --- | --- |
| GET | `/api/ha/info` | **none** | Identity, protocol version, actual transport, port, certificate fingerprint |
| GET | `/api/ha/state` | Basic | Identity plus the documented `B/health` document |
| GET/POST | `/api/ha/config` | Basic | The Web UI's own configuration handlers |
| POST | `/api/ha/lock` | Basic | `{"action":"lock"}` or `{"action":"unlock"}` |

`/api/ha/info` is **deliberately unauthenticated**: it is what a client fetches to learn
the fingerprint it is about to ask its user to confirm, and it cannot ask for a password
before knowing what it is talking to. Everything it returns is already broadcast in the
mDNS TXT record. The **household id is not** among it — that is only reported once
authenticated, because it forms part of the MQTT topic path.

> [!NOTE]
> **HMAC command signing is deliberately not used on this path.** The MQTT transport
> signs commands with a key derived from the household recovery secret because a broker
> is untrusted. This path is already authenticated by TLS with a pinned peer plus a
> device credential; a second signature scheme over the same connection would add code
> without adding a guarantee — and it would mean asking the user for a recovery secret
> the transport does not need.

## 10. Firmware requirements

| Requirement | Why |
| --- | --- |
| `DiscoveryAdvertiser` (`_homekey._tcp`) | Without the mDNS advertisement nothing can ever be discovered |
| `deviceCert` (`ECDSA P-256`, self-signed) | The per-unit certificate that pinning anchors on |
| **HTTPS enabled** (`webHttpsEnabled`) | The direct transport refuses plaintext; API routes return `503` under HTTP |
| `webAuthEnabled` + credentials | Authorises state reads, config writes and lock commands |
| A configured household | Entities are keyed on `household_id + node_id`; a node with none is refused with an actionable message |

The mDNS advertisement is registered from the HomeSpan status callback, and the
`DiscoveryAdvertiser` deliberately **does not touch the hostname** when one is already
set, because HomeSpan owns it and HomeKit controllers resolve it.

## 11. Limitations

- **One node per direct entry.** A household of four nodes over the direct transport
  means four config entries; over MQTT it is one.
- **Audit and provisioning are HTTP-only** on the firmware and are not exposed as
  entities by either transport.
- **A factory reset changes the fingerprint**, so Home Assistant refuses to reconnect
  until you re-pair. That is correct behaviour — a new key is a new identity — but it
  means re-pairing is a manual step, not an opaque failure.
- **Polling, not push, on the direct transport.** State updates are up to 30 s behind
  an event, versus near-instant over MQTT.
- **The MQTT LWT clean-disconnect limitation** documented in the integration's own
  docs applies to the MQTT transport's availability reporting.

## 12. Related pages

- [MQTT Household API](/HomeKey-ESP32/mqtt_household_api/) — the MQTT contract this integration implements
- [MQTT API Contract Matrix](/HomeKey-ESP32/mqtt_api_contract_matrix/) — every topic, payload and auth requirement
- [Household & Node Architecture](/HomeKey-ESP32/household/) — households, node identity, backups
- [Guest NFC Tags](/HomeKey-ESP32/guest-tags/) — the credential type the guest entities manage
- [Security](/HomeKey-ESP32/security/) — the device-side trust model
- [HASS Automations](/HomeKey-ESP32/automations/) — automation examples
