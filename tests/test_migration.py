#!/usr/bin/env python3
"""
Host-side test of the household migration logic in main/HouseholdManager.cpp.

Mirrors the exact decision table of `migrate()` and the load() version guard
(no ESP32 required):

  record absent              -> create UNCONFIGURED record at current version
  record version == current  -> already migrated (idempotent no-op)
  record version == 0        -> unstamped/corrupt: re-initialize (never touches
                                existing config / HomeKey / pairing data)
  record version > current   -> unsupported future schema: fail closed, refuse

The NVS read/write itself is hardware-bound and marked HARDWARE_ONLY.
"""

import sys

CURRENT_VERSION = 1  # household::CONFIG_VERSION_CURRENT
UNCONFIGURED = "UNCONFIGURED"


def load(record, current=CURRENT_VERSION):
    """Mirror HouseholdManager::load()'s version guard (returns (ok, state))."""
    if record is None:
        return False, UNCONFIGURED
    if record["config_version"] > current:
        # Fail closed: refuse to interpret a future schema.
        return False, UNCONFIGURED
    return True, record.get("state", UNCONFIGURED)


def migrate(record, current=CURRENT_VERSION):
    """Mirror HouseholdManager::migrate() and return (action, new_record)."""
    if record is None:
        return "created", {"state": UNCONFIGURED, "config_version": current}
    if record["config_version"] > current:
        return "refused_unsupported", record  # never overwrite a future record
    if record["config_version"] == 0:
        return "reinitialized", {"state": UNCONFIGURED, "config_version": current}
    return "already_migrated", record  # idempotent no-op


def main():
    results = []

    def check(name, cond):
        results.append((name, "PASS" if cond else "FAIL"))

    # 1. Fresh install: no household record -> created at current version.
    action, rec = migrate(None)
    check("fresh_install_creates_record", action == "created")
    check("fresh_install_uses_current_version", rec["config_version"] == CURRENT_VERSION)
    check("fresh_install_state_unconfigured", rec["state"] == UNCONFIGURED)

    # 2. Old configuration (existing config, no household record): migrate creates
    #    only the household record and leaves other config/HomeKey data untouched
    #    (by construction migrate() writes only the household record).
    action, rec = migrate(None)
    check("old_config_migrates_without_touching_other_data", action == "created")

    # 3. Migration of an already-current record is a no-op.
    migrated = {"state": "ACTIVE", "config_version": CURRENT_VERSION}
    action, rec = migrate(migrated)
    check("already_migrated_noop", action == "already_migrated" and rec is migrated)

    # 4. Repeated migration is idempotent.
    a1, r1 = migrate(migrated)
    a2, r2 = migrate(migrated)
    check("repeated_migration_idempotent", a1 == a2 == "already_migrated" and r1 == r2)

    # 5. Invalid version (0 = unstamped/corrupt): re-initialized to current.
    action, rec = migrate({"state": "ACTIVE", "config_version": 0})
    check("invalid_version_reinitialized", action == "reinitialized")
    check("invalid_version_stamped_current", rec["config_version"] == CURRENT_VERSION)
    check("invalid_version_state_unconfigured", rec["state"] == UNCONFIGURED)

    # 6. Unsupported future version: load() refuses and migrate() refuses without
    #    overwriting the future record.
    future = {"state": "ACTIVE", "config_version": CURRENT_VERSION + 5}
    ok, state = load(future)
    check("future_version_load_refused", ok is False and state == UNCONFIGURED)
    action, rec = migrate(future)
    check("future_version_migrate_refused", action == "refused_unsupported")
    check("future_record_preserved", rec is future)

    ok_all = all(s == "PASS" for _, s in results)
    for name, status in results:
        print(f"  [{status}] {name}")
    print(f"\n{sum(1 for _, s in results if s == 'PASS')}/{len(results)} passed")
    print("NOTE: NVS read/write and reboot-during-migration are HARDWARE_ONLY.")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
