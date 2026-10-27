# Spec: Secure, multi-transport control interface

**Status:** Implemented (unverified on hardware)
**Created:** 2026-10-15

## Problem statement

The dock controller currently accepts commands from anyone who joins its open
Wi-Fi access point, with no authentication of any kind. A stranger in range can
extend the lift, open the box and interfere with the drone. Separately, the
plain-HTTP control page cannot be opened on an unmodified iPhone — Safari's
HTTPS-Only setting refuses the request before it reaches the board — so the
current interface is simultaneously too open for a product and unusable on the
operator's actual phone. The dock also depends on its own access point, which
forces the operator's phone off the internet and offers no way to work over a
network the operator already has, such as their phone's hotspot.

## User scenarios

### Scenario 1: Authorised operator controls the dock
- **Given** the operator has the official app, paired with this dock
- **When** they open the app near the vehicle and command "extend"
- **Then** the lift extends, and the app shows live status

### Scenario 2: Stranger attempts control
- **Given** a stranger is within radio range and has joined or discovered the dock
- **When** they send any command by any means — browser, `curl`, replayed request
- **Then** the dock rejects it, does not move, and records the attempt

### Scenario 3: Operator's phone keeps its internet
- **Given** the operator is running a phone hotspot
- **When** the dock is configured to join that hotspot
- **Then** the app controls the dock over the hotspot while the phone keeps cellular data

### Scenario 4: No infrastructure available
- **Given** the dock is in a field with no hotspot and no known network
- **When** the operator opens the app
- **Then** the dock is still reachable, because it has fallen back to its own access point

### Scenario 5: Controlling client disappears mid-move
- **Given** the lift is extending under app control
- **When** the app is killed, the phone goes out of range, or the link drops
- **Then** the actuator stops within a bounded time, with no operator action

### Scenario 6: Commissioning a new network
- **Given** the dock has never seen the operator's hotspot
- **When** the operator provisions the credentials from the app
- **Then** the dock stores them, joins on this and every future boot, and the
  credentials never travel in the clear

### Scenario 7: Service access
- **Given** a technician with a USB cable and no app
- **When** they open the serial console
- **Then** they retain full local control for diagnosis

## Functional requirements

**Access control**
1. The system must reject any command that does not carry valid proof of authorisation.
2. The system must not accept a captured valid command replayed later.
3. The system must log rejected attempts to the serial console.
4. The system must keep its authorisation secret out of version control.
5. The system must allow its key to be rotated without reflashing. Each dock generates its own key on first boot; it is handed to the app once through a deliberately opened pairing window.

**Transport**
6. The control transport must work on an unmodified, current iPhone and Android phone, with no browser settings changed and no developer mode.
7. The system must provide a transport that does not require the phone to leave its existing network — either BLE, or Wi-Fi station mode where the dock joins the operator's hotspot.
8. The system must preserve the dead-man behaviour on every control transport: if the controlling client stops communicating while the motor runs, the actuator stops within 2 seconds.
9. The system must expose the same command set on every transport, via the existing single command path.

**Networking**
10. The system must be able to join an existing Wi-Fi network, including a phone hotspot, using credentials supplied at runtime.
11. The system must persist those credentials across reboot and power loss.
12. The system must fall back to its own access point when it cannot join a configured network, so it is never unreachable.
13. The system must be discoverable on a joined network without the operator knowing its IP address.

**Safety and resources**
14. Every network command, including stop, must require authorisation. Unauthenticated stop is **rejected**: an open stop channel would let a stranger block drone recovery. Physical safety is a wired E-stop that cuts actuator power, outside this spec.
15. The firmware must fit the flash available on the board, with the partition table adjusted and the board's real flash size verified.
16. The serial console must remain fully functional for service.

## Acceptance criteria

Walked on 2026-10-15 after Phase 5. **Nothing has been run on hardware yet**,
so most criteria are implemented but unverified. They are left unticked
deliberately — an unticked box is information, and ticking them from code
review would make this file untrustworthy.

| # | Criterion | Status |
| --- | --- | --- |
| 1 | A command from the paired app moves the actuator | ⏳ app never compiled; `dock_cli.py` is the stand-in |
| 2 | Unsigned `curl` is rejected with no movement | ⚠️ implemented, logic host tested (A07–A11), **not run on hardware** (W13) |
| 3 | A replayed command is rejected | ⚠️ implemented, host tested (A08, teeth verified), **not run on hardware** (W15) |
| 4 | Rejected attempts appear on serial with a reason | ⚠️ implemented, not observed |
| 5 | No secret in tracked content | ❌ **partially failed — see finding below** |
| 6 | Works from an unmodified iPhone | ⏳ blocked on the app |
| 7 | Works from an unmodified Android phone | ⏳ blocked on the app |
| 8 | Phone keeps cellular data on its own hotspot | ⏳ not tested (N04) |
| 9 | Killing the client mid-move stops within 2 s, every transport | ⚠️ implemented in `session/`, not measured (W21, B17) |
| 10 | Provisioning survives power cycle, password never in clear | ⚠️ implemented, not verified (B08, B10) |
| 11 | Reachable on its own AP within 30 s of boot | ⚠️ AP is unconditional, not timed (N01) |
| 12 | Reachable by name on a joined network | ⚠️ mDNS built, not resolved from a client (N03) |
| 13 | Serial retains full control | ⚠️ implemented, not verified (W20) |
| 14 | Build fits with ≥10 % headroom on verified flash | ✅ **passed** — 74 % free of 4 MB, 16 MB flash verified |
| 15 | Host tests pass, new logic covered | ✅ **passed** — 31 tests across `command` and `auth` |

### Finding against criterion 5

`git grep` found no device key, and none can be committed — it is generated by
`esp_fill_random()` on first boot and lives only in NVS. But:

```
firmware/drone_box/components/board_config/include/board_config.h:
    #define BOARD_WIFI_AP_PASSWORD  "liftmotor"
```

**The SoftAP password is a credential in version control**, so every dock built
from this repository shares it. The constitution says secrets never enter git,
and this does. It is milder than a leaked device key — commands still require a
signature, so knowing the AP password buys an attacker network access, not
control — but the criterion is not met.

Remedy, deferred to a follow-up task: derive the AP password from the device
key, so each dock differs and nothing credential-shaped is tracked.

## Edge cases & error handling

- **Hotspot disappears mid-move** — treated as client-gone: stop, then fall back to AP.
- **Two clients connected at once** — last command wins and both see shared state; the dock is single-operator by convention, not by lock. Each authorised session has its own dead-man timer.
- **Wrong credentials provisioned** — must not brick reachability; fall back to AP and report the failure.
- **No real-time clock** — the S3 has no battery-backed RTC, so replay protection cannot rely on wall-clock time; it must use a nonce or counter.
- **Secret compromised** — the rotation story from FR5.
- **Flash exhausted by the BLE stack** — must be discovered before implementation, not after.
- **Repeated failed auth** — rate limiting, or at minimum logging, to avoid an open brute-force channel.
- **Power loss mid-move** — on reboot the dead-reckoned level is unknown; the dock must not assume a position.

## Non-goals

- Cloud connectivity, fleet management or remote telemetry beyond the local link
- OTA firmware updates
- Controlling the drone itself — this is the dock only
- User accounts, roles or multi-tenant access
- Certified or audited security; this is a student proof of concept, and the
  spec aims at "a stranger cannot drive it", not at resisting a determined attacker
- Protecting against an attacker with physical access to the board

## Decisions taken

Resolved 2026-10-15:

| Question | Decision |
| --- | --- |
| Primary transport | **Both BLE and Wi-Fi.** BLE for pairing, provisioning and short-range control; signed HTTP over Wi-Fi for the main control link |
| Key model | **Pairing window.** Dock generates its own key, hands it over once through a deliberately opened window |
| Unauthenticated STOP | **Rejected.** Physical E-stop is the safety mechanism |
| Second client | Allowed; last command wins |

## Open questions

None. The last one — flash size — was resolved on 2026-10-15: the board has
**16 MB** (manufacturer 0x68, device 0x4018). The app partition is now 4 MB with
the current build using 20 % of it, so FR15 is satisfied with wide margin and
the BLE + Wi-Fi scope is deliverable.
