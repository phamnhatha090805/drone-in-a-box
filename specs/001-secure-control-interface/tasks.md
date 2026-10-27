# Tasks: Secure, multi-transport control interface

**Plan:** ./plan.md
**Spec:** ./spec.md

Ordered. T1 is blocking — do not start T2 until it is answered.

## Phase 0 — make room

- [x] **T1 — Verify the board's real flash size** ✅ 2026-10-15
      **16 MB** (manufacturer 0x68, device 0x4018), not the 2 MB the build
      assumed. BLE + Wi-Fi is comfortably deliverable; no re-scope needed.
      Implements FR15.

- [x] **T2 — Partition table with a larger app partition** ✅ 2026-10-15
      `partitions.csv`: nvs 64 K, phy_init 4 K, factory app 4 M.
      `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` and `CONFIG_PARTITION_TABLE_CUSTOM` set
      in `sdkconfig.defaults`. Build: 843 KB of 4 MB, **80 % free**.
      ~11 MB left unallocated for OTA or data later.
      Still to verify on hardware: `idf.py erase-flash` then `idf.py flash`,
      because the flash-size and partition layout both changed. Implements FR15.

## Phase 1 — authorisation

- [x] **T3 — `auth` component, hardware-free** ✅ 2026-10-15
      `auth_canonical()`, `auth_nonce_acceptable()`, `auth_verify()` with the
      HMAC function injected, so the host build needs no mbedTLS. No ESP-IDF
      headers. See Interfaces. Implements FR1, FR2.

- [x] **T4 — Host unit tests for `auth`** ✅ 2026-10-15
      12 tests (A01–A12), all passing. Teeth verified: removing the nonce check
      from `auth_verify()` makes `test_replayed_request_is_rejected` fail with
      `Expected FALSE Was TRUE`.
      Canonical string is stable and unambiguous; nonce ordering accepts
      increasing and rejects equal, lower and wildly-ahead values; signature
      comparison is constant time; a one-bit change anywhere in method, path,
      query or nonce invalidates the signature. Prove the replay test fails
      against a build that skips the nonce check. Implements FR2, constitution
      testing rules.

- [x] **T5 — `secrets` component over NVS** ✅ 2026-10-15
      Generate a 32-byte key with `esp_random()` on first boot; store and read
      the nonce high-water mark with the lazy-persist scheme from Risks; store
      Wi-Fi credentials; `secrets_rotate_key()`. Implements FR4, FR5, FR11.

- [x] **T6 — Gate the HTTP API behind `auth`** ✅ 2026-10-15
      Verify `X-Dock-Nonce` and `X-Dock-Sig` before dispatch; reject with 401
      and log the reason and peer; keep the page itself unauthenticated but
      serve it only on the AP interface. Unauthenticated stop is rejected like
      any other command. Implements FR1, FR3, FR14.

- [x] **T7 — `session` component: dead-man across transports** ✅ 2026-10-15
      Move the heartbeat out of `webserver` into `session`, keyed by link, 2 s
      timeout, armed on first authorised message of any transport. Implements
      FR8.

- [x] **T8 — Bench-test Phase 1** ✅ 2026-10-15 (tooling), ⏳ awaiting hardware run
      `firmware/drone_box/tools/dock_cli.py` signs requests and has a
      `selftest` that runs W13–W17 and prints PASS/FAIL. W13–W23 added to the
      firmware README. **Still to run on hardware**: flash with
      `idf.py erase-flash` first (partition layout changed), press `K` for the
      key, then `dock_cli.py selftest` and `deadman`. Implements FR1, FR2, FR8.

## Phase 2 — joining other networks

- [x] **T9 — `wifi_link`: APSTA with fallback** ✅ 2026-10-15
      Join the stored SSID when present; keep the SoftAP up regardless; retry
      with backoff; never leave the dock unreachable. Report state on serial.
      Implements FR7, FR10, FR12.

- [x] **T10 — mDNS responder** ✅ 2026-10-15
      Advertise `dock.local` so the app never needs an IP address. Implements FR13.

- [x] **T11 — Serial provisioning command** ✅ 2026-10-15
      Set and clear Wi-Fi credentials from the console, so Phase 2 is testable
      before BLE exists. Implements FR10, FR16.

- [x] **T12 — Bench-test Phase 2** ✅ 2026-10-15 (cases written), ⏳ awaiting hardware run
      N01–N10 added to the firmware README, covering provisioning, dock.local,
      hotspot join, bad credentials, reboot persistence and AP survival.
      Implements FR10–FR13.

## Phase 3 — BLE pairing and provisioning

- [x] **T13 — `ble_link` skeleton on NimBLE** ✅ 2026-10-15
      Advertising as `dock-xxxx`, one GATT service, encryption required on both
      characteristics. **Flash: 872 KB → 1082 KB, so NimBLE cost ~210 KB** —
      close to the ~200 KB the plan estimated, and proof that T1/T2 were
      necessary: this build would not fit the original 1 MB partition.
      Now 74 % free of 4 MB. Implements FR6.

- [x] **T14 — Pairing window and key handover** ✅ 2026-10-15
      `ble_link_open_pairing(60)`, triggered from the serial console; the
      pairing characteristic is readable only while open; log loudly on open,
      read and close. Implements FR5, FR3.

- [x] **T15 — Wi-Fi provisioning over BLE** ✅ 2026-10-15
      Write SSID and password to the provision characteristic over an encrypted
      link; store via `secrets`; trigger a station re-join without a reboot.
      Implements FR10, FR11.

## Phase 4 — BLE control

- [x] **T16 — Command and status characteristics** ✅ 2026-10-15
      Command writes go through the same `auth`/`controller_handle()` path;
      status notifies the same JSON as HTTP. Feed `session_touch(LINK_BLE)` so
      the dead-man covers BLE too. Implements FR8, FR9.

- [x] **T17 — Bench-test Phase 4** ✅ 2026-10-15 (cases + tooling), ⏳ awaiting hardware run
      B13–B18 added, including B14 (bonded-but-unsigned refused) and B17
      (dead-man over BLE). `dock_cli.py blesign` emits the hex for a signed
      write so the cases are runnable from nRF Connect without the app.
      Implements FR8, FR9.

## Phase 5 — the app

- [x] **T18 — KMP `DockClient`: signed HTTP** ✅ 2026-10-15 — written, **never compiled**
      Nonce counter, HMAC signing, mDNS discovery, 400 ms status poll as the
      heartbeat. Android cleartext config and iOS `NSAllowsLocalNetworking` +
      `NSLocalNetworkUsageDescription`. Implements FR6, FR9.

- [~] **T19 — KMP BLE transport and pairing UI** — framing and UUIDs done in `commonMain`; `DockBle` is still an `expect` with no `actual`
      Scan, bond, read the key during the pairing window, store it in the
      platform keystore, provision Wi-Fi. Implements FR5, FR6.

- [x] **T20 — Update documentation** ✅ 2026-10-15
      Rewrite `docs/architecture/control-interface.md` for the two authorised
      transports; update the firmware README commands and test tables; walk the
      spec's acceptance criteria and tick what actually passes. Implements the
      Validate phase.

## Notes

- ~~T1 is genuinely blocking.~~ Resolved: 16 MB, so nothing downstream is at
  risk from flash.
- T3/T4 can proceed in parallel with T2 — `auth` is hardware-free and needs
  neither a partition table nor a board.
- Phases 1 and 2 are independent of BLE. If the semester runs short, stopping
  after T12 leaves a secure, hotspot-capable dock and an honest report.
- Every phase ends in a bench test task on purpose: each one is a row in the
  test report, and none of them depends on the phase after it.

## Deviation from the plan

**BLE writes are signed, which the plan did not specify.** The plan relied on
`BLE_GATT_CHR_F_WRITE_ENC` for access control. That is wrong: the dock pairs
"just works", so *any* device in range can bond and obtain an encrypted link —
encryption gives confidentiality, not authorisation. Spec FR1 says only an
authorised client may command the dock, and encryption alone does not deliver
it. `auth_verify_blob()` was added and both writable characteristics now
require a signature. Covered by A13–A15 and B14.

The spec and plan interfaces have been updated to match; this is recorded
rather than quietly fixed because the plan was wrong, not just incomplete.

## Lessons recorded during implementation

- **The flash estimate held.** NimBLE added ~210 KB against a planned ~200 KB.
  The plan's largest risk was sized correctly, and Phase 0 was not wasted work:
  the Phase 3 build is 1082 KB and the original partition was 1024 KB.

- **mDNS is not bundled with ESP-IDF 6.0.** It is a registry component
  (`espressif/mdns`), pulled in via `idf_component.yml`. The first build solves
  and downloads it, so that build needs network access; `dependencies.lock` is
  committed to keep later builds reproducible, and `managed_components/` is
  ignored.
- **ESP-IDF 6.0 ships mbedTLS 4.** The legacy `mbedtls_md_hmac()` one-shot is
  gone and `mbedtls/md.h` does not exist; HMAC goes through PSA Crypto
  (`psa_crypto_init`, `psa_import_key`, `psa_mac_compute`). Any HMAC example
  found online will almost certainly use the removed API.

- **A "teeth check" must prove the build succeeded.** The first attempt at
  verifying T4 removed the nonce check, which left `high_water` unused;
  `-Wextra -Werror` failed the compile, the stale binary ran, and all tests
  "passed" — appearing to prove the test had no teeth when in fact nothing had
  been rebuilt. Always break the code in a way that still compiles, and never
  suppress build output while doing it.
- **CMake can miss sub-second edits.** Restoring a file and rebuilding
  immediately can reuse the old object. `touch` is not always enough; delete the
  build directory when the result matters.

## Follow-up tasks raised during validation

- [ ] **T21 — Derive the SoftAP password from the device key**
      Acceptance criterion 5 failed: `BOARD_WIFI_AP_PASSWORD "liftmotor"` in
      `components/board_config/include/board_config.h` is a
      credential in git, shared by every dock built from this repo. Derive it
      from the device key (first bytes, base32) so each unit differs and
      nothing credential-shaped is tracked. Print it on the console at boot and
      hand it to the app during pairing. Implements FR4.

- [ ] **T22 — Rate-limit failed authorisation attempts**
      Currently logged but not throttled, leaving an unmetered guessing channel.
      Edge case listed in the spec and never implemented.

- [ ] **T23 — Run Phases 1–4 on hardware**
      Every bench case (W13–W23, N01–N10, B01–B18) is written and none has been
      executed. Until this is done, 11 of 15 acceptance criteria remain
      unverified. Start with `idf.py erase-flash`, since the partition layout
      changed.

- [ ] **T24 — Implement `DockBle` actuals**
      Android `BluetoothGatt` and iOS `CoreBluetooth`, plus the permissions and
      ATS entries listed in `app/README.md`.
