# drone_box

Bench firmware for a single linear actuator driven through a **ZK-5AD** dual
H-bridge, controlled over the serial console. It is the first firmware step
towards the dock controller: it proves the actuator, the driver module and the
ESP32-S3 PWM path all work together before any state machine is built on top.

Target: **ESP32-S3**, **ESP-IDF v6.0.2**.

> Ported from [`../actuator_test/actuator_test.ino`](../actuator_test/actuator_test.ino).
> Behaviour is deliberately identical; see [Arduino parity](#arduino-parity).

## Hardware

| ESP32-S3 | ZK-5AD | Notes |
| --- | --- | --- |
| GPIO4 | D2 | Motor 2 input A — "extend" |
| GPIO5 | D3 | Motor 2 input B — "retract" |
| GND | GND | Must be common, or nothing works |

The ZK-5AD takes its motor supply separately. Its inputs read high at 2.2–6.0 V,
so 3.3 V logic is in spec. D0/D1 drive motor 1 and are unused here.

> **Pin caveat:** GPIO4/5 are `ADC1_CH3`/`ADC1_CH4`. Wi-Fi owns ADC2 on the S3,
> so ADC1 (GPIO1–10) is the only ADC available once SoftAP is running, and
> actuator position feedback will need those channels. Move the PWM outputs to
> GPIO38+ when the wiring is redone. Avoid GPIO19/20 (native USB) and
> GPIO26–37 (flash/PSRAM).

## Configuration

Every pin, radio name and tunable timing lives in one header:

```
components/board_config/include/board_config.h
```

Nothing else holds a value you are meant to change. The component headers keep
their own names — `WIFI_AP_SSID`, `TRAVEL_MS`, `SPEED_MIN_PCT` — but each is
defined from the `BOARD_*` value in `board_config.h`, so there is exactly one
place to edit and the components keep documenting their own contracts.

| Setting | What it is |
| --- | --- |
| `BOARD_ACTUATOR_PIN_A`, `BOARD_ACTUATOR_PIN_B` | The two PWM outputs to the driver |
| `BOARD_ACTUATOR_PWM_FREQ_HZ`, `BOARD_ACTUATOR_PWM_BITS` | Carrier frequency and duty resolution |
| `BOARD_PAIR_BUTTON_GPIO`, `..._DEBOUNCE_MS` | The BOOT button that gates pairing |
| `BOARD_WIFI_AP_SSID`, `BOARD_WIFI_AP_PASSWORD` | The dock's own access point |
| `BOARD_WIFI_AP_CHANNEL`, `BOARD_WIFI_AP_MAX_CONN` | SoftAP radio settings |
| `BOARD_WIFI_HOSTNAME` | mDNS name, so the app can reach `dock.local` |
| `BOARD_WIFI_RETRY_MIN_MS`, `..._MAX_MS` | Station reconnect backoff |
| `BOARD_BLE_NAME_PREFIX` | Advertised name stem, e.g. `dock-3f8a` |
| `BOARD_BLE_PAIRING_WINDOW_S` | How long the pairing window stays open |
| `BOARD_BLE_UUID_BASE` | The 15 shared bytes of the five service UUIDs |
| `BOARD_TRAVEL_MS`, `BOARD_LEVELS`, `BOARD_STEP_SPEED_PCT` | Stroke, level count, timed-move speed |
| `BOARD_HOME_OVERTRAVEL_PCT` | Overtravel into the end stop when homing |
| `BOARD_SPEED_MIN_PCT` | Stall floor; slower requests clamp up to it |
| `BOARD_SESSION_TIMEOUT_MS` | Dead-man switch window |

Two rules keep the file usable:

* **Plain integers and string literals only — no `#include`, no ESP-IDF enums
  such as `GPIO_NUM_4` or `LEDC_TIMER_0`.** That is what lets `command` and
  `auth` include it and still build with the host compiler in `test/host`, with
  no board and no IDF. Where a configured number has to become a driver enum —
  `BOARD_ACTUATOR_PWM_BITS` → `LEDC_TIMER_8_BIT` — the `#if` ladder that does
  it lives in `actuator.c`, and an unhandled value is a compile error rather than a
  silent fallback.
* **Buffer sizes and protocol constants do not belong here.** `AUTH_SIG_LEN`,
  `WIFI_SSID_TEXT_MAX`, `CONTROLLER_REPLY_MAX` and the canonical-form limits
  stay with the code that has to agree with the clients about them. Changing a
  value in `board_config.h` must never change the wire format.

There are deliberately **no station credentials** in the file. The network the
dock joins is provisioned at runtime — over the serial console, or over an
encrypted BLE link — and kept in NVS by `secrets`, so a Wi-Fi password never
enters this repository and differs per unit.

## Build, flash, monitor

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py -p /dev/cu.usbmodem5B8E1411611 flash monitor
```

In VS Code, use **Build, Flash and Monitor** (🔥) — it sequences the port
handoff. A separate Serial Monitor holding the port makes flashing fail with
`Resource busy`; stop it first.

### Console transport

The console follows `menuconfig` → *Component config* → *ESP System Settings* →
*Channel for console output*:

| Setting | Use when the cable is in | Transport |
| --- | --- | --- |
| `UART` (default) | the board's UART-bridge port (CH343/CP2102) | UART0, 115200 |
| `USB Serial/JTAG` | the S3's native USB port | native USB |

`main.c` compiles against whichever is selected — no source edits needed.
The default lives in [`sdkconfig.defaults`](sdkconfig.defaults).

> `sdkconfig.defaults` is only read when `sdkconfig` does not exist. After
> changing it, **delete `sdkconfig`** and rebuild, or the change is ignored.

## Commands

| Key | Action |
| --- | --- |
| `e` | Extend — PWM on GPIO4, GPIO5 low |
| `r` | Retract — PWM on GPIO5, GPIO4 low |
| `s` | Stop — both pins low (coast) |
| `5`–`9` | Speed 50 %–90 % |
| `0` | Full speed (100 %) |
| `1`–`4` | Clamped to 50 % — below that the actuator stalls under load |
| `h` | Home: drive to the fully **retracted** end, set level 0 |
| `t` | Top: drive to the fully **extended** end, set level 5 |
| `+` / `-` | Move one level and stop |
| `p` | Print the current level |
| `d` | Toggle decay mode (slow ⇄ fast) |
| `A` | Diagnostic: hold GPIO4 at full scale, GPIO5 low |
| `B` | Diagnostic: hold GPIO5 at full scale, GPIO4 low |

Anything else is ignored, including CR and LF. Each accepted command echoes the
duty LEDC actually holds, e.g. `Direction: e, speed: 100%  [A=256 B=0]`.

## Web UI

The board runs its **own access point**, so no network infrastructure is needed
— which matters because the dock lives on a moving vehicle, and because at a
demo the assessor just joins it from their own phone with nothing to install.

The dock runs **APSTA**: its own access point is always up, *and* it joins a
stored network when one is in range. Station mode is strictly additive —
losing the hotspot can never make the dock unreachable, which is the whole
reason the AP is unconditional.

| | |
| --- | --- |
| Own AP | `drone-in-a-box` / `liftmotor`, always at `http://192.168.4.1/` — both in `board_config.h` |
| Joined network | whatever you provision; the app finds it as `dock.local` |
| Hostname | `dock` (mDNS, advertised on both interfaces) |

Provision from the serial console:

| Key | Action |
| --- | --- |
| `W` | Set the Wi-Fi network — prompts for SSID then password (masked). **Needs the terminal's line ending set to LF**; with "None" the prompt never completes |
| `X` | Forget it and go back to AP only |
| `K` | Print the device key, for the bench client |
| `P` | Open the BLE pairing window for 60 s |

Credentials live in NVS and survive power cycles. A failed join retries with
backoff from 1 s up to 30 s and never gives up, so a hotspot switched on later
is picked up without a reboot.

Joining your phone's hotspot is the useful case: the dock gets a network, and
**your phone keeps its cellular data** — the problem that made the dock's own
AP awkward to use from a phone in the first place.

| Endpoint | Method | Auth | Purpose |
| --- | --- | --- | --- |
| `/` | GET | no | The setup and control page (embedded in the binary, no filesystem) |
| `/api/pair` | GET | **window + AP** | Hands over the device key; see below |
| `/api/net` | GET | yes | Which network the dock is on |
| `/api/wifi?ssid=…&password=…` | POST | yes | Join a network, without a reboot |
| `/api/status` | GET | **yes** | Current state as JSON; an authorised poll is the heartbeat |
| `/api/cmd?c=X` | POST | **yes** | Apply one command byte, returns the new state |
| `/api/nonce` | GET | no | Current replay high-water mark, so a client can pick its next nonce |

### Authorisation

Every API request carries two headers:

```
X-Dock-Nonce: 1734
X-Dock-Sig:   <base64 HMAC-SHA256 over the canonical string>
```

The signature covers method, path, query and nonce, so none can be altered in
flight; the nonce must exceed everything already accepted, so a captured
request cannot be replayed. See `components/auth/` and spec FR1/FR2.

The device key is generated on first boot by `esp_fill_random()` and stored in
NVS. It is never in source and never in git. `/api/nonce` is deliberately open:
the high-water mark is not a secret, and a client needs it to choose a usable
nonce after reinstalling.

**Stop is not privileged.** An open stop endpoint would let anyone in range
block drone recovery; physical safety is a wired E-stop.

### Pairing, and why it is a button

Open the page, press **BOOT** on the board, tap **Pair**. The browser stores the
key and signs its own requests from then on — no key is ever typed.

`/api/pair` has two gates, and neither alone is sufficient:

| Gate | Why |
| --- | --- |
| A pairing window, opened only by the **BOOT button** | Something nobody in radio range can do remotely |
| The caller is on the dock's **own AP**, not a joined network | A device on your home Wi-Fi has no business taking the key |

Note what is deliberately *not* a gate: **knowing the Wi-Fi password**. It is a
compile-time constant in this repository, shared by every dock built from it, so
"is on our network" proves nothing. A physical button press does.

The window closes on the first successful handover, and the same window serves
BLE pairing — `secrets` owns it, because both transports hand the key over.

### The page signs its own requests

`crypto.subtle` is only exposed in **secure contexts**, and the dock serves plain
HTTP, so the page cannot use it. It carries its own HMAC-SHA256 instead, which
makes it the **fourth** implementation of the wire format — alongside `auth.c`,
`dock_cli.py` and `DockCrypto.kt`. All four are cross-checked against one set of
vectors by `tools/check-wire-format.sh`.

### Wi-Fi setup is in the page

The form posts to `/api/wifi`, signed. Credentials travel in the **signed query**
rather than a request body: the signature covers method, path and query, so a
body would be alterable in flight. They are never logged. `W` and `X` on the
serial console still work as the service path.

**The web client sends the same single-character commands as the serial
console.** Both land in `controller_handle()`, so there is exactly one place
where a command changes the machine's state, and `command_parse()` — the part
under host unit test — is shared by both. Adding a third client (the KMP app)
means writing a client, not reinterpreting what `e` means.

## BLE pairing and provisioning

The dock advertises as **`dock-xxxx`** (last two bytes of its BLE address) with
one custom GATT service:

| Characteristic | Access | Payload |
| --- | --- | --- |
| Pairing `…5e02` | read, encrypted | the 32-byte device key — only while the window is open |
| Provision `…5e03` | write, encrypted + **signed** | `ssid\0password` |
| Command `…5e04` | write, encrypted + **signed** | one command byte |
| Status `…5e05` | read + notify, encrypted | the same JSON as `/api/status` |

### Signed BLE writes

Writes are framed **`nonce (8 bytes, little-endian) ‖ signature (32) ‖ payload`**,
signed exactly like an HTTP request but over the raw payload bytes.

**Link encryption is not enough on its own.** The dock pairs "just works", so
any device in range can bond and obtain an encrypted link — `WRITE_ENC` means
"any bonded device", not "my app". Only a client that received the device key
through the pairing window can sign, so the signature is what actually enforces
"only my app", on BLE exactly as on Wi-Fi.

The payload is signed as **bytes, not as a C string**: Wi-Fi credentials are
`ssid\0password`, and a `strlen`-based canonical form would sign the SSID and
leave the password unprotected — an attacker could swap it for a network they
control. Covered by host tests A13–A15.

A command write is answered by a **notify on the status characteristic**, so the
app sees the result immediately rather than polling. An authorised write also
pets the dead-man, so `session/` covers BLE identically to Wi-Fi: lose the
link mid-move and the actuator stops.

**Pairing is opened from the serial console with `P`, never remotely.** Opening
it over the network would let an attacker create their own way in. The window
is 60 s and **closes on the first successful read** — the app needs the key once,
and leaving it open afterwards widens the hole for nothing.

Provisioning over BLE solves a chicken-and-egg problem: handing over
credentials for a network the dock has not joined yet. BLE needs no network,
and its link encryption keeps the Wi-Fi password off the air in the clear. The
dock rejoins immediately, without a reboot.

> **Pairing is "just works".** The dock has no display or keypad, so there is no
> passkey to confirm. The link is encrypted and bonded but **not authenticated**:
> an attacker in range *during the window* could man-in-the-middle it. That is
> why the window is short, operator-opened and one-shot. It is listed in the
> spec's non-goals rather than hidden.

### Bench client

Phase 1 cannot be tested with `curl` alone — valid requests need a signature.
[`tools/dock_cli.py`](tools/dock_cli.py) signs them. Stdlib only, no install.

```bash
# press 'K' on the serial console to print the device key, then:
export DOCK_KEY=<64 hex chars>
tools/dock_cli.py selftest          # W13-W17, moves nothing
tools/dock_cli.py cmd e --yes       # signed command, DOES move the actuator
tools/dock_cli.py deadman --yes     # starts a move, then goes silent
tools/dock_cli.py blesign cmd e     # hex for a signed BLE write
```

`blesign` prints the exact bytes to paste into a BLE app such as nRF Connect,
so the BLE cases are testable before the KMP app exists. It takes its nonce
from the dock over Wi-Fi — APSTA keeps the AP up even when BLE is the transport
under test.

`selftest` prints a PASS/FAIL line per case, so it drops straight into the test
report. Its `canonical()` mirrors `auth_canonical()` and is asserted against the
same vector as `test_canonical_is_length_prefixed` — if the firmware's canonical
form ever changes, the client fails loudly rather than silently mis-signing.

> `K` prints the device key over serial. It needs physical USB access, which the
> spec puts out of scope, but it is a deliberate hole that the BLE pairing
> window (task T14) replaces.

### The heartbeat is a dead-man switch

An authorised poll of `/api/status` **is** the heartbeat. If a client that has
been polling goes quiet for more than `SESSION_TIMEOUT_MS` (1500 ms) while the
actuator is energised, the firmware stops it itself.

This lives in `components/session/`, not in the webserver, because BLE needs the
identical guarantee — it moved before BLE arrived rather than being duplicated
afterwards. A link arms itself on its first authorised message; the stop fires
only when every armed link has gone quiet, so two clients watching at once
behave sensibly. **Only an authorised message pets it**, or a stranger could
hold the dead-man open after the operator walked away.

Serial deliberately does not arm it: a cable means someone is physically at the
bench, and a technician must not have the actuator stopped by a timer.

This is not optional for remote control of something that moves. A browser tab
*will* be closed mid-cycle, a laptop lid *will* be shut, a phone *will* walk out
of range — and without this the actuator keeps driving. The firmware is
authoritative; the client is never trusted to send the stop.

The watchdog stays disarmed until a web client has polled at least once, so a
serial-only session is unaffected.

> Credentials are compile-time constants in `components/webserver/include/webserver.h`.
> This is an open bench network with no authentication on the endpoints: anyone
> on the AP can drive the actuator. Fine for a bench and a demo, not fine for
> anything mounted on a vehicle in public.

## Code structure

```
main/main.c              wiring only: init the parts, pump the serial loop
components/
  board_config/          every pin, radio name and tunable timing (header only)
  command/               byte -> command, duty and clamp arithmetic
  actuator/              the two PWM pins, LEDC, decay modes
  positioner/            dead-reckoned levels, non-blocking timed moves
  controller/            the one command path every client shares
  auth/                  request signing, canonical forms, replay window
  secrets/               device key, replay high-water mark, Wi-Fi creds in NVS
  session/               the dead-man switch, shared by every remote transport
  serial_io/             UART0 or native USB, whichever menuconfig selects
  wifi_link/             APSTA: own AP always up, joins a stored network, mDNS
  ble_link/              NimBLE: pairing and Wi-Fi provisioning
  pair_button/           BOOT button: the physical gate on handing out the key
  webserver/             signed REST endpoints and the embedded control page
managed_components/
  espressif__mdns/       fetched by the component manager, for dock.local
```

`espressif/mdns` is the one external dependency, declared in
`components/wifi_link/idf_component.yml` and pinned in `dependencies.lock`.
`wifi_link.c` includes it behind `#if __has_include("mdns.h")`, so a build
without it still comes up — it just prints *"mDNS unavailable in this build —
use the IP address"* instead of answering to `dock.local`.

The split is along one line: **`command/` and `auth/` touch no hardware and
include no ESP-IDF headers.** They are plain C and compile with a bare `gcc`,
so everything that can be got wrong by reasoning — the duty full-scale
correction, the stall-floor clamp, level bounds, the parse table, the canonical
forms, the replay window — is unit tested on a host with no board attached. See
[Unit tests (host)](#unit-tests-host). Everything that needs a chip lives in
the other components.

That is the seam the dock controller will need for its unit-test deliverable,
and it exists now rather than being retrofitted later.

`board_config/` is a third kind: a header and nothing else. It includes
nothing, links nothing and declares no functions, which is what lets the two
host-testable components read their tunables from it without giving up the
property that makes them testable.

| Component | Public API | Depends on |
| --- | --- | --- |
| `board_config` | `BOARD_*` constants only; no code, no link dependency | nothing |
| `command` | `command_parse()`, `speed_clamp()`, `speed_is_clamped()`, `speed_to_duty()`, `level_step_valid()`, `command_url_decode()` | `board_config` |
| `actuator` | `actuator_init/set/hold_raw/set_slow_decay/slow_decay`, duty, frequency and pin-name getters | `command`, `board_config`, LEDC, GPIO |
| `positioner` | `positioner_init/complete/go_end/step/cancel/level/moving` | `actuator`, `board_config`, `esp_timer` |
| `controller` | `controller_init/handle/status_json/emergency_stop/reveal_key` | `command`, `actuator`, `positioner`, `secrets` |
| `auth` | `auth_init()`, `auth_canonical()`, `auth_canonical_blob()`, `auth_verify()`, `auth_verify_blob()`, `auth_nonce_acceptable()`, `auth_equal_ct()` | nothing |
| `secrets` | `secrets_init/device_key/key_is_new/rotate_key`, nonce high-water, Wi-Fi get/set/clear, pairing window, `secrets_hmac_sha256()` | `auth`, `board_config`, NVS, mbedTLS |
| `session` | `session_init()`, `session_touch()`, `session_any_live()` | `controller`, `actuator`, `board_config`, `esp_timer` |
| `serial_io` | `serial_io_init/read_byte/read_line/transport_name` | UART, USB-Serial-JTAG |
| `wifi_link` | `wifi_link_start/reprovision/station_joined/station_ip/scan/auth_name` | `secrets`, `board_config`, `esp_wifi`, `esp_netif` |
| `ble_link` | `ble_link_start/open_pairing/close_pairing/pairing_open` | `auth`, `controller`, `secrets`, `session`, `wifi_link`, `board_config`, NimBLE (`bt`) |
| `pair_button` | `pair_button_start()` | `secrets`, `board_config`, GPIO, `esp_timer` |
| `webserver` | `webserver_start()` | `auth`, `command`, `controller`, `actuator`, `secrets`, `session`, `wifi_link`, `esp_http_server`, mbedTLS |
| `main` | `app_main()` | all of them |

Two components own a decision the rest defer to. `controller_handle()` is a
single switch over `cmd_kind_t` and **returns** its reply rather than printing
it, so the serial loop can print it, the HTTP handler can put it in a response
and `ble_link` can notify it on a characteristic. Adding a command means one
enum value, one line in `command_parse()`, and one case — and the parse half is
testable without hardware. `session` holds the dead-man switch for every remote
transport at once, so a new transport inherits the guarantee by calling
`session_touch()` instead of reimplementing it.

`secrets` is the only component that touches the device key, and `auth` is the
only one that decides whether a request is authentic — which is why `auth`
takes its HMAC as a function pointer (`auth_init()`) rather than linking
mbedTLS itself. That indirection is what keeps it host-testable.

> **Component names are global in ESP-IDF.** This component set originally
> called `serial_io` "console", which shadowed IDF's own `console` component and
> broke `protocomm` with a `esp_console.h: No such file` error in an unrelated
> part of the tree. Prefix or pick distinctive names.

### No delays, no polling

`serial_read()` blocks on `portMAX_DELAY`. The RX interrupt fills the driver's
ring buffer and wakes the task; between commands the CPU is free and the idle
task runs.

Timed level moves use an `esp_timer` one-shot rather than sleeping: `start_move()`
arms it and returns immediately, and `move_done()` stops the actuator from the timer
task when the step is due. So the console stays responsive mid-move, and there is
no `vTaskDelay()` and no busy-wait anywhere in the program.

### PWM configuration

Both channels share `LEDC_TIMER_0` at **20 kHz, 8-bit**, in
`LEDC_LOW_SPEED_MODE` (the S3 has no high-speed channels). 20 kHz is above
hearing, so the actuator does not whine. One shared timer guarantees both
channels run at the same frequency — and on the S3 all low-speed timers share a
clock source, so using two timers invites cross-interaction for no benefit.

### Drive modes: slow vs fast decay

How you PWM an H-bridge matters as much as the duty, and the two modes are not
equivalent under load.

| | Off-phase | Pins (extend @ duty `d`) | Behaviour |
| --- | --- | --- | --- |
| **Fast decay** | both inputs low → coast | `A = d`, `B = 0` | Torque collapses below full speed; through a gearbox the actuator does not move at all |
| **Slow decay** (default) | both inputs high → brake | `A = 256`, `B = 256 - d` | Current keeps circulating, low-speed torque holds up, speed tracks duty |

The original Arduino sketch used fast decay. It appeared to work only because
every test ran at 100 %, where both modes collapse to the same thing: one input
high, the other low. The first time a speed key was pressed, the actuator
stopped responding — which is how this was found.

Press `d` to toggle and compare on the bench. Stop (`s`) always drives both
inputs low, i.e. coast, in either mode.

### Query values arrive encoded

`httpd_query_key_value()` returns the value **still percent-encoded**. `?c=%2B`
yields the three characters `%2B`, not `+`. Every command byte that is an
unreserved URL character survives untouched, which is why `e`, `r`, `s`, `h`,
`t` and the digits all worked while **Step +** silently did nothing: the handler
was passing `'%'` to the parser, which correctly ignored it.

`command_url_decode()` lives in the `command` component rather than the web
handler, because it is pure string logic and belongs where it can be unit
tested (U14–U16).

### The full-scale gotcha

**At 8-bit resolution, LEDC's duty range is 0…256, not 0…255.** A duty of 255 is
99.6 %: the pin dips low for ~195 ns every 50 µs period. Only **256** holds the
pin solidly high.

Arduino's `ledcWrite()` hides this — it bumps a full-scale value to
`max_duty + 1` internally, so `analogWrite(pin, 255)` really programs 256. A
direct port that writes 255 produces a near-DC signal with a narrow notch, and
the ZK-5AD's forward input ignores it entirely while the reverse input tolerates
it. The result was "`r` works, `e` does not" with provably correct firmware.

`duty()` therefore returns `256` at 100 %. This is the single most important
line in the file and the one most likely to be "simplified" by mistake later.

### Arduino parity

Identical: pins, 20 kHz, 8-bit duty scale, command set, output strings, and
actuator-off-at-boot. Different: the blocking interrupt-driven read instead of
`Serial.available()` polling, the explicit full-scale correction that Arduino
applies silently, and the duty readback added to each echo.

## Levels and travel calibration

The stroke is split into **5 steps**. `+` and `-` move exactly one step and
stop; `h` and `t` drive to the end stops and establish the datum.

There is **no position feedback** on this rig, so position is dead-reckoned
from actuator run time. That holds only if every timed move runs at the same
speed — `STEP_SPEED_PCT`, 100 % — and starts from a known datum. Any manual
jog (`e`, `r`, `s`, `A`, `B`) therefore marks the level **unknown** until you
home again.

Timed moves are non-blocking: an `esp_timer` one-shot stops the actuator when
the step is due, so the console stays responsive and there is still no `delay()`
anywhere in the program.

### You must calibrate `TRAVEL_MS`

In `components/board_config/include/board_config.h`:

```c
#define BOARD_TRAVEL_MS         10000
```

The default is a **placeholder**. Measure it:

1. Press `h` and wait for `Level: 0/5`.
2. Press `t` and time with a stopwatch until the actuator reaches the stop.
3. Put that figure in `BOARD_TRAVEL_MS`, rebuild, reflash.

`STEP_MS` is `TRAVEL_MS / LEVELS` and `HOME_MS` is
`TRAVEL_MS × BOARD_HOME_OVERTRAVEL_PCT / 100` — both derived in
`positioner.h`, so they cannot drift out of step with the stroke. The shipped
130 % is deliberate overtravel, so homing always reaches the end stop
regardless of starting position.

> Homing drives the actuator into its internal limit switch and holds there for
> the remaining overtravel. That is a stall. It is brief and at the end stop it
> is how homing works, but do not extend `HOME_MS` beyond what you need.

## Unit tests (host)

The `command` component is hardware-free, so it is tested on the development
machine — no board, no flashing, no serial port, runs in under a second.

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh    # only to locate Unity
cmake -S test/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Two suites: `command` (16 tests) and `auth` (12 tests). Adding another
hardware-free component to the runner is one line — `host_test(<name>)` in
`test/host/CMakeLists.txt` — provided the component keeps ESP-IDF headers out.

Unity comes from the ESP-IDF install rather than a vendored copy, so host tests
and any future on-target tests use the same framework. `IDF_PATH` must be set;
the CMake file fails with a clear message if it is not.

| ID | Test | Asserts |
| --- | --- | --- |
| U01 | `test_parse_directions` | `e`/`r`/`s` map to `CMD_DIRECTION` with the right value |
| U02 | `test_parse_levels_and_modes` | `h`,`t`,`+`,`-`,`p`,`d`,`A`,`B` map correctly |
| U03 | `test_parse_digits` | Digits map to percent; `0` is **100 %**, not 0 % |
| U04 | `test_parse_ignores_noise` | CR, LF, space, `z`, `E`, `0x00`, `0xFF` are ignored — case matters |
| U05 | `test_speed_clamp_raises_weak_speeds` | 10 %, 30 %, 0 %, −5 % all come out at the floor |
| U06 | `test_speed_clamp_passes_usable_speeds` | 50/70/100 pass through; 150 caps to 100 |
| U07 | `test_speed_is_clamped_reports_accurately` | Boundary at exactly 50 % |
| U08 | `test_full_speed_is_full_scale_not_255` | **100 % → 256, never 255** — the full-scale regression |
| U09 | `test_duty_matches_arduino_map` | 50/60/70/80/90 % → 127/153/178/204/229 |
| U10 | `test_duty_applies_the_clamp` | A weak speed yields the floor's duty, not a stalling one |
| U11 | `test_duty_never_exceeds_full_scale` | Sweeps −20…150 %: never above 256, never a silent 0 |
| U12 | `test_level_steps_inside_range` | Steps within 0…5 are allowed |
| U13 | `test_level_steps_refuse_past_the_ends` | Steps past either end are refused |
| U14 | `test_decode_plus_reaches_the_parser` | **`%2B` decodes to `+` and parses as a step** — the Step + regression |
| U15 | `test_decode_handles_ordinary_values` | Unreserved chars pass through; `%41%42`, lowercase hex |
| U16 | `test_decode_is_robust_to_junk` | `+`→space, truncated `%`, `%A`, `%ZZ`, short-buffer overrun, zero-length buffer |

### `auth` suite — request authorisation (spec FR1, FR2)

| ID | Test | Asserts |
| --- | --- | --- |
| A01 | `test_canonical_is_length_prefixed` | Exact canonical bytes: `4:POST8:/api/cmd3:c=e1:7` |
| A02 | `test_canonical_resists_field_splicing` | Two different requests cannot produce the same bytes to sign |
| A03 | `test_canonical_refuses_to_truncate` | Oversized input returns 0 rather than a truncated string |
| A04 | `test_nonce_must_increase` | Equal, lower and zero nonces rejected — the replay case |
| A05 | `test_nonce_skip_is_capped` | `UINT64_MAX` rejected, so one request cannot lock the device out forever |
| A06 | `test_nonce_from_zero` | Fresh device accepts nonce 1, rejects 0 |
| A07 | `test_valid_request_is_accepted` | The happy path |
| A08 | `test_replayed_request_is_rejected` | **Valid signature, seen nonce → refused** |
| A09 | `test_any_tampering_invalidates` | Method, path, query, nonce, key, one flipped signature bit |
| A10 | `test_stop_is_not_privileged` | Unauthenticated stop refused, per the spec decision |
| A11 | `test_verify_without_init_fails_closed` | Missing HMAC primitive refuses rather than allows |
| A12 | `test_constant_time_compare_is_correct` | First-byte and last-byte differences both detected |

**A08 has teeth** — removing the nonce check from `auth_verify()` makes it fail
with `Expected FALSE Was TRUE`, verified by doing exactly that.

> When breaking code to check a test has teeth, make sure the build still
> succeeds. The first attempt here left a parameter unused, `-Werror` failed the
> compile, and the stale binary ran green — which looks identical to a test with
> no teeth.

**U08 has teeth** — reverting the `256` correction in `speed_to_duty()` makes it
fail, which was verified by doing exactly that. A regression test that cannot
fail is decoration.

**U14 is honest about its limits.** It proves the decoder works; it cannot prove
the HTTP handler *calls* it, because that needs a chip. W11 covers that half.
Two tests, one bug, because the bug lived in the seam between them.

The remaining components need a chip, so they are covered by the bench tests
below rather than unit tests. Splitting them further — a mockable
`IActuatorDriver` behind `positioner` — is the natural next step if you want
level logic under test too.

## Test cases

Bench tests, run on hardware over the serial console; the host unit tests above
cover the arithmetic. `[A=.. B=..]` is the duty readback in
the echo line. Reset the board before each group. Calibrate `TRAVEL_MS` before
running the level tests, or the timings will not match.

### Boot and console

| ID | Purpose | Input | Expected output | Expected behaviour |
| --- | --- | --- | --- | --- |
| T01 | Boot banner | reset with monitor open | `Ready:`, `Diag:`, `Decay:`, `Level:` lines | — |
| T02 | PWM frequency and transport | reset | `PWM: 20000 Hz on UART0` (or `USB-Serial-JTAG`) | — |
| T03 | Actuator off at boot | reset | — | Actuator does not move |
| T04 | Level unknown at boot | reset, `p` | `Level: unknown (press h to home)` | — |
| T05 | Invalid char ignored | `z` | No output | No state change |
| T06 | Line endings ignored | send with CRLF | One echo line only | No spurious commands |
| T07 | Console survives reconnect | close/reopen monitor, `s` | Echo appears | Commands still accepted |

### Direction and speed

| ID | Purpose | Input | Expected output | Expected behaviour |
| --- | --- | --- | --- | --- |
| T08 | Extend, full speed | `0` `e` | `Direction: e, speed: 100%  [A=256 B=0]` | Extends |
| T09 | Retract, full speed | `r` | `Direction: r, speed: 100%  [A=0 B=256]` | Retracts |
| T10 | Stop coasts | `s` | `Direction: s, speed: 100%  [A=0 B=0]` | Stops, both inputs low |
| T11 | Full scale is 256 | `0` `e` | `A=256`, **not** `A=255` | Regression test for the full-scale gotcha |
| T12 | Extend at 50 % | `5` `e` | `Direction: e, speed: 50%  [A=256 B=129]` | Extends, visibly slower |
| T13 | Retract at 50 % | `5` `r` | `Direction: r, speed: 50%  [A=129 B=256]` | Retracts, visibly slower |
| T14 | Speed change while moving | `e` then `7` | `speed: 70%  [A=256 B=78]` | Speeds up without stopping |
| T15 | Low speed clamped | `1` | `10% is below the 50% stall floor -- clamped`, then `speed: 50%` | Does not stall |
| T16 | Direction reversal | `e` then `r` | `[A=0 B=256]` | Reverses |

### Drive mode

| ID | Purpose | Input | Expected output | Expected behaviour |
| --- | --- | --- | --- | --- |
| T17 | Decay toggle | `d` | `Decay: fast (coast off-phase)` | Mode switches, re-applied immediately |
| T18 | Fast decay stalls at low speed | `d` `5` `e` | `[A=127 B=0]` | Actuator stalls — documents why slow decay is the default |
| T19 | Both modes agree at 100 % | `0` `e`, then `d` `e` | `[A=256 B=0]` both times | Identical behaviour |

### End stops (maximum and minimum)

| ID | Purpose | Input | Expected output | Expected behaviour |
| --- | --- | --- | --- | --- |
| T20 | Home to minimum | `h` | `Homing to retracted end (...)` then `Level: 0/5` | Reaches the retracted stop and stops |
| T21 | Drive to maximum | `t` | `Homing to extended end (...)` then `Level: 5/5` | Reaches the extended stop and stops |
| T22 | Cannot step past maximum | at level 5, `+` | `Already at extended end (level 5/5)` | No movement |
| T23 | Cannot step past minimum | at level 0, `-` | `Already at retracted end (level 0/5)` | No movement |
| T24 | Stroke time matches `TRAVEL_MS` | `h`, then time `t` | — | Stopwatch within ~10 % of `TRAVEL_MS`; recalibrate if not |
| T25 | Homing is idempotent | `h` twice | `Level: 0/5` both times | Second run just holds at the stop |

### Levels

| ID | Purpose | Input | Expected output | Expected behaviour |
| --- | --- | --- | --- | --- |
| T26 | Step up one level | `h` then `+` | `Level 0 -> 1 (...)` then `Level: 1/5` | Moves one step, **stops by itself** |
| T27 | Walk the whole range | `h` then `+` ×5 | Levels 1,2,3,4,5 in turn | Five roughly equal steps, ends at the extended stop |
| T28 | Step down | at level 5, `-` ×5 | Levels 4,3,2,1,0 | Returns to the retracted end |
| T29 | Step without homing | reset, `+` | `Level unknown -- press h or t first` | No movement |
| T30 | Manual jog invalidates position | `h`, `+`, `e`, `s`, `p` | `Level: unknown` | Dead reckoning correctly discarded |
| T31 | Console stays live during a move | during `+`, press `p` | `Level: moving...` immediately | Actuator still running — proves non-blocking |
| T34 | Arrival is announced | `h` | `Level: 0/5` when the move ends | Printed by the completion callback, not polled |
| T35 | Stepping mid-move is refused | during `+`, press `+` | `Move in progress -- wait for it, or press s to abort` | Second move not started; dead reckoning stays sound |

### Web UI

| ID | Purpose | Input | Expected output | Expected behaviour |
| --- | --- | --- | --- | --- |
| W01 | AP comes up | reset, scan for Wi-Fi | SSID `drone-in-a-box` visible | Joinable with the password |
| W02 | Page serves | open `http://192.168.4.1/` | Control page renders | Works on a phone with nothing installed |
| W03 | Status poll | watch the page idle | Values refresh ~2.5×/s | No page reload, button state preserved |
| W04 | Command from web | tap **Extend** | Actuator extends | Serial log shows `web: e -> ...` |
| W05 | Both clients agree | tap **Extend**, then press `s` on serial | Page shows stopped within 400 ms | One state, two views |
| W06 | Levels from web | tap **Home**, then **Step +** | Level display tracks | Same dead reckoning as serial |
| W07 | **Heartbeat stop** | tap **Extend**, then close the browser tab | Actuator stops within ~1.5 s | Serial prints `STOP: web client stopped responding` |
| W08 | Heartbeat ignores serial-only | never open the page, drive with `e` | Actuator keeps running | Watchdog stays disarmed |
| W09 | Out of range | tap **Extend**, walk the phone out of range | Actuator stops | Same path as W07 |
| W10 | Bad request | `curl -X POST "http://192.168.4.1/api/cmd"` | HTTP 400 | No state change |
| W11 | **Encoded command** | tap **Step +** | Level advances | `?c=%2B` must decode; see U14 |
| W12 | Unknown command logged | authorised request with `c=z` | `web: ignored byte 0x7a` in serial | No state change |
| W13 | **Unsigned request refused** | `curl -X POST "http://192.168.4.1/api/cmd?c=e"` | HTTP 401, `AUTH: refused /api/cmd -- missing X-Dock-Nonce` | Actuator does not move |
| W14 | Bad signature refused | valid nonce, wrong signature | 401, `-- bad signature` | No movement |
| W15 | Replay refused | capture a valid request, resend it | 401, `-- replayed or out-of-window nonce` | No movement |
| W16 | Unauthenticated status refused | `curl http://192.168.4.1/api/status` | 401 | Watchdog not petted |
| W17 | Nonce endpoint is open | `curl http://192.168.4.1/api/nonce` | `{"nonce":N}` | Readable without auth, by design |
| W18 | Page shows locked state | open `http://192.168.4.1/` | "locked" plus the explanation | Browser cannot command |
| W19 | Key survives reboot | note nonce, power cycle, reuse an old nonce | 401 | High-water mark persisted |
| W20 | Serial keeps control | send `e` over serial with no app | Actuator moves | Service access unaffected |
| W21 | Dead-man covers authorised clients | `tools/dock_cli.py deadman --yes` | `STOP: web client stopped responding` | Stops ~1.5 s after the last poll |
| W22 | Unauthorised poll cannot pet the dead-man | start a move, then poll unsigned | Stop still fires | 401s do not count as liveness |
| W23 | Serial does not arm the dead-man | drive with `e` over serial only, wait | No stop | Technician is not interrupted |

### Networking (Phase 2)

| ID | Purpose | Input | Expected output | Expected behaviour |
| --- | --- | --- | --- | --- |
| N01 | AP up with no stored network | erase-flash, boot | `AP "drone-in-a-box" up`, `no stored network -- AP only` | Reachable at 192.168.4.1 |
| N02 | Provision over serial | `W`, enter hotspot SSID + password | `station configured`, then `station joined, address …` | Password echoes as asterisks, never in plaintext |
| N03 | Reachable by name | `tools/dock_cli.py --host dock.local status` | Signed status returned | No IP address needed |
| N04 | Phone keeps cellular | dock on phone hotspot, app controls it | Commands work | Phone still has internet — the point of station mode |
| N05 | Credentials survive reboot | power cycle after N02 | Rejoins automatically | Stored in NVS |
| N06 | Wrong password does not brick it | provision a bad password | Retries with backoff, AP stays up | Dock never becomes unreachable (FR12) |
| N07 | Hotspot appears later | provision, switch the hotspot off, boot, switch it on | Joins within ~30 s | Backoff caps rather than giving up |
| N08 | Forget network | `X` | `credentials cleared -- AP only` | Station stops, AP unaffected |
| N09 | AP survives station loss | join, then kill the hotspot mid-session | AP client stays connected | Local control is never infrastructure-dependent |
| N10 | Both interfaces at once | connect one device to the AP and one via the hotspot | Both see the same state | APSTA working |

### BLE (Phase 3)

| ID | Purpose | Input | Expected output | Expected behaviour |
| --- | --- | --- | --- | --- |
| B01 | Advertising | boot, scan with a BLE app | `dock-xxxx` visible | Service UUID `…5e01` present |
| B02 | Key read refused when closed | read pairing characteristic without `P` | `key read refused -- pairing window is closed` | Read denied |
| B03 | Pairing window opens | press `P` | `*** BLE PAIRING OPEN for 60 s ***` | Window announced loudly |
| B04 | Key handover | bond, then read the pairing characteristic | 32 bytes returned, `pairing closed (key handed over)` | Matches what `K` prints |
| B05 | Window is one-shot | read a second time after B04 | Refused | Closed on first read |
| B06 | Window expires | press `P`, wait 60 s | `pairing closed (window expired)` | No read needed |
| B07 | Unencrypted access refused | read/write without bonding | Insufficient authentication | Encryption required by the characteristic |
| B08 | Provision over BLE | write `ssid\0password` | `provisioned "…" -- joining` | Station joins; password never logged |
| B09 | Malformed provision rejected | write with no NUL separator | Invalid length error | No credentials stored |
| B10 | Credentials survive reboot | power cycle after B08 | Rejoins automatically | Stored in NVS |
| B11 | Pairing cannot be opened remotely | authorised `P` over HTTP | `pairing is opened from the serial console only` | Window stays shut |
| B12 | BLE and Wi-Fi coexist | provision over BLE while an app polls over Wi-Fi | Both work | One 2.4 GHz radio shared |
| B13 | Signed command over BLE | `dock_cli.py blesign cmd e`, write the hex to `…5e04` | `BLE: e -> Direction: e…` | Actuator extends |
| B14 | **Bonded but unsigned is refused** | write a raw `e` byte to `…5e04` | `refused /ble/cmd -- bad signature` | Proves encryption alone is not authorisation |
| B15 | BLE replay refused | write the same signed frame twice | Second refused | Shared nonce window with HTTP |
| B16 | Status notifies on command | subscribe to `…5e05`, send B13 | Notification with fresh JSON | No polling needed |
| B17 | **Dead-man covers BLE** | signed extend, then disconnect | Stops ~1.5 s later | `session/` is transport-agnostic |
| B18 | Control with Wi-Fi off | forget network, power the AP down in menuconfig or move out of range | BLE control still works | Phase 4 goal |

### Pairing and setup UI

| ID | Purpose | Input | Expected output | Expected behaviour |
| --- | --- | --- | --- | --- |
| P01 | Pairing refused when closed | `curl http://192.168.4.1/api/pair` | HTTP 403, `press the BOOT button on the dock` | No key handed over |
| P02 | **Button opens the window** | press BOOT | `PAIR: BOOT button pressed`, `*** PAIRING OPEN for 60 s ***` | Window announced |
| P03 | Pair from the page | press BOOT, tap **Pair** | Controls appear; `PAIR: key handed over via web` | No key typed anywhere |
| P04 | Window is one-shot | tap Pair twice | Second attempt 403 | Closes on first handover |
| P05 | Window expires | press BOOT, wait 60 s | `PAIR: closed (window expired)` | No handover needed |
| P06 | **Not from a joined network** | pair while on the dock's station network | 403, `join the dock's own Wi-Fi to pair` | AP-only gate holds |
| P07 | Key persists in the browser | reload the page | Controls still shown | Stored in localStorage |
| P08 | Unpair | tap **Unpair browser**, reload | Pair card returns | Key removed |
| P09 | Wi-Fi setup from the page | enter SSID + password, **Join network** | `WiFi: provisioned "…" from the web UI` | Station joins; AP stays up |
| P10 | Credentials not logged | watch the console during P09 | SSID shown, password never | Password stays out of scrollback |
| P11 | Signed page requests | tap any control after pairing | Command executes | Page is a real authorised client |
| T32 | New command cancels a move | during `+`, press `s` | `Direction: s ... [A=0 B=0]` | Stops at once, level unknown |
| T33 | Step size is `TRAVEL_MS / 5` | measure one `+` | — | Within ~10 % of `STEP_MS` |

### Duty mapping reference

`d` is the commanded duty from `duty()`. In slow decay the direction input sits
at 256 and the other carries `256 - d`, so the readback does not show `d`
directly. Keys `1`–`4` clamp to 50 %.

| Key | Speed | `d` | Extend `[A B]` | Retract `[A B]` |
| --- | --- | --- | --- | --- |
| `5` | 50 % | 127 | `[256 129]` | `[129 256]` |
| `7` | 70 % | 178 | `[256 78]` | `[78 256]` |
| `9` | 90 % | 229 | `[256 27]` | `[27 256]` |
| `0` | 100 % | **256** | `[256 0]` | `[0 256]` |

Integer truncation is intentional — it matches Arduino's `map()`.

## Known limitations

This is bench firmware, not the dock controller. It deliberately does **not**
have:

- **Position feedback.** Levels are dead-reckoned from time. Any slip, load
  change or supply sag makes the reported level a lie, and nothing detects it.
  The real controller wants a potentiometer or encoder on ADC1.
- **Limit-switch inputs.** Nothing tells the firmware an end stop was reached;
  homing just drives for longer than the stroke and relies on the actuator's
  internal switches. Repeated stalls at full duty are a plausible way to damage
  a half-bridge.
- **Stall or current detection.** No feedback of any kind.
- **A state machine.** Commands act immediately, with no interlocks and no
  notion of a safe position.
- **Unit tests beyond `command` and `auth`.** `actuator` and `positioner` talk
  to hardware
  directly, so the level arithmetic and timed-move sequencing are only covered
  by bench tests. Putting a mockable driver interface behind `positioner` would
  bring them under host test too.
- **Calibrated speed.** Duty is not linear in rod speed. The 50 % clamp is a
  measured-by-hand floor, not a characterised one.

All of these belong in the dock controller, with the logic in a hardware-free
component so it can be unit tested on the host.
