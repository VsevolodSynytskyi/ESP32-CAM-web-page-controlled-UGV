# ESP32-CAM web page controlled UGV

Tank-style tracked UGV: AI-Thinker ESP32-CAM on an ESP32-CAM-MB USB shield, two
DC motors through a TB6612FNG, 2S 18650 pack through a 5V buck converter.

---

## ⚠️ Status: blocked on a faulty camera module

**The OV2640 on this board is defective.** It runs too hot to touch and collapses
WiFi throughput whenever the camera subsystem is active:

| Camera | Motors | `/speed` throughput |
|---|---|---|
| running | running | **132 kbps** |
| `esp_camera_deinit()` | running | **5204 kbps** |
| deinit | released | 5124 / 4949 kbps |

Same board, same client, same minute. **Streaming numbers are meaningless until
it is replaced.** Motor wiring, control and the boot-state design are all done
and verified; only the camera blocks progress.

### Already eliminated — don't re-investigate

Each of these was measured, not reasoned about:

| Suspect | Verdict |
|---|---|
| Radio / antenna | ✅ **5.2 Mbit/s**, repeatedly, even at −55 dBm |
| Antenna select jumper | ✅ correct — an empty U.FL could not reach 5 Mbit/s |
| Motor GPIOs / LEDC | ✅ **zero cost** — 5204 with them running vs 5124/4949 detached |
| Power supply | ✅ TX-power sweep showed no change; no brownout resets |
| Channel congestion | ✅ still bad at congestion score 0 |
| Client / browser | ✅ desktop Chrome and iPhone Safari behave identically |
| Transport | ✅ MJPEG, WebSocket push and per-frame polling all the same |
| Framework / lwip / core | ✅ matches the MJPEG2SD reference, which reaches 15 fps |
| **Camera module** | ❌ **39–250× penalty, runs hot, fails within seconds** |

One real secondary effect: physically **disconnecting the motor jumper wires**
gained ~33% (5249 vs 3501 kbps). That's parasitic antenna coupling from the
wires themselves — not the pin configuration — and it isn't fixable in software.
Route them out the **camera end**; the PCB trace antenna is at the U.FL end.

### Validating the replacement module

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
cd "/Users/vsevolodsynytskyi/Documents/PlatformIO/Projects/ESP32-CAM web page controlled UGV"
pio run -t upload -t monitor
```

1. Join `TankCam`, open `http://192.168.4.1/speed` → note the rate
2. Press **`c`** (camera stops), open `/speed` again
3. Compare

**If both are in the Mbit/s range, the module is good.** If the camera-running
number collapses again, the replacement is also faulty — or the fault is in the
ribbon or connector rather than the sensor.

Then expect roughly **11 fps at SVGA** and **17 at VGA** with the motor wires
attached; near 16 at SVGA with them routed away from the antenna.

---

## Build

Single environment, single `src/main.cpp`.

```bash
pio run -t upload -t monitor
```

### Endpoints

| | |
|---|---|
| `/` | viewer page, switches between all three transports |
| `/ws` | WebSocket frame push |
| `/jpg` | one JPEG per request |
| `/stream` | multipart MJPEG |
| `/speed` | 1 MB throughput test, no camera data in the payload |

### Serial keys

`1`–`5` frame size · `q`/`Q` quality · `a` adaptive quality · `c` stop/restart
camera · `v`/`h` flip/mirror · `p` status

### Reading the status line

```
[status] 4.2 fps | grab 0 send 238 GAP 210 ms | 6108 B/f q14 | 205 kbps | rssi -20 | streaming
```

`grab + send + GAP` accounts for the whole frame period, and each blames
something different:

- **`grab`** — the sensor or JPEG encoder
- **`send`** — the radio
- **`GAP`** — dead time waiting to be asked for the next frame: the client, or
  the server refusing connections

That split is what eventually isolated the camera, after a long detour through
transports, TCP tuning and channel selection.

---

## Wiring

TB6612FNG in the **4-pin scheme**: `PWMA`, `PWMB`, `STBY` and `VCC` tied
permanently to **3.3V**, with the PWM on the direction pins.

| ESP32-CAM | TB6612FNG |
|---|---|
| GPIO14 | AIN1 — channel A (motor 1) |
| GPIO15 | AIN2 — channel A |
| GPIO12 | BIN1 — channel B (motor 2) |
| GPIO2 | BIN2 — channel B |
| 3V3 | PWMA, PWMB, STBY, VCC |
| GND | GND (star point at the battery hub) |
| — | VM ← 2S+, with 1000 µF bulk cap |

**GPIO13 and GPIO16 must be left unconnected.** Keep the microSD slot empty —
those lines are shared with the motor pins.

### ESP32-CAM pin reference

Everything not listed — GPIO 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36,
39 — is consumed by the camera.

| Label | GPIO | Safe? | Reason | Our use |
|---|---|:---:|---|---|
| D0 | 0 | ⚠️ | HIGH during boot, LOW for flashing | camera XCLK |
| TX0 | 1 | ❌ | flashing and debugging | MB shield serial |
| D2 | 2 | ⚠️ | LOW during boot; conflicts with microSD | **BIN2** |
| RX0 | 3 | ❌ | flashing and debugging | MB shield serial |
| D4 | 4 | ⚠️ | on-board flash LED; conflicts with microSD | free — future headlight |
| D12 | 12 | ⚠️ | LOW during boot; conflicts with microSD | **BIN1** |
| D13 | 13 | ⚠️ | conflicts with microSD | free — candidate for STBY |
| D14 | 14 | ⚠️ | conflicts with microSD | **AIN1** |
| D15 | 15 | ⚠️ | HIGH during boot, else no startup log; microSD | **AIN2** |
| RX2 | 16 | ✅ | — | left unconnected |

### The pins are paired by boot state — don't reshuffle

Before `motors_begin()` runs, these pins hold whatever reset leaves them at, and
the TB6612 is already awake because `STBY` and `PWMA`/`PWMB` are tied to 3V3.
Whatever that level is *is* a command to the driver. **Measured** on this board:

| **LOW** at reset | **HIGH** at reset |
|---|---|
| GPIO2, GPIO4, GPIO12, GPIO13 | GPIO14, GPIO15, GPIO16 |

Note GPIO13 is documented as having a reset pull-up and **does not** here.
Trusting the datasheet cost a motor running at full throttle through every boot.
`main.cpp` prints this survey at startup; if you change pins, pair from what it
reports, not from documentation.

Each channel therefore gets a **matched** pair, which the truth table reads as a
stop:

| Channel | Pins | Boot state | Result |
|---|---|---|---|
| A | 14 + 15 | `(H, H)` | short brake |
| B | 12 + 2 | `(L, L)` | coast |

Mixing a high pin with a low pin gives `(H, L)` — a full-throttle drive command
that persists through power-on, watchdog resets, brownouts and **the entire
duration of a firmware upload**, when the chip never reaches our code at all.
Keep `{14,15}` together and `{12,2}` together.

**This does not cover holding RST down.** While EN is held low no pull is applied
and every pin floats. Closing that gap needs one resistor: `STBY` to GPIO13 with
4.7 kΩ to GND, raised in `motors_begin()`. Not currently done.

### Power

```
2S 18650 (7.4 V nom / 8.4 V max)
  ├─► TB6612 VM ──┬── 1000 µF electrolytic (at the driver, short leads)
  │               └── 0.1 µF ceramic
  └─► 5V buck ──► ESP32-CAM 5V pin ──► onboard LDO ──► 3V3 ──► TB6612 logic
```

Star ground at the battery hub. 0.1 µF across each motor's terminals and from
each terminal to the can. Never feed 5 V into 3V3. **USB and the buck must not
both drive the 5 V pin** — on the bench, USB for the ESP and battery for VM
only, grounds common.

### Motors are still unmeasured

`MOTOR_MAX_DUTY` is `0.55f` (~4.6 V average) — safe for almost anything, but
stall current is unknown against the TB6612FNG's **1.2 A continuous** limit, and
stall current is the binding constraint. Measure winding resistance across the
terminals and compute `I_stall ≈ 8.4 V ÷ R`. Over ~1.5 A means the driver is
undersized.

---

## Design notes

**Signed end to end.** Throttle is signed `-1000 … +1000` through the whole
chain and the sign alone selects direction. No separate direction flag exists,
so it cannot disagree with a magnitude.

**One writer to the PWM registers.** `motors_set()` only updates targets and
refreshes the failsafe timer; `motors_tick()` at 50 Hz is the sole caller of
`ledcWrite`.

**Slow decay by default.** With PWM on the direction pins, holding one high and
modulating the other with the *inverted* duty puts the off phase in short-brake,
keeping low-speed torque linear. That inversion is the easiest bug to introduce
here — get it backwards and the motor runs fastest at zero throttle.

**Asymmetric slew limiting.** Ramping up is rate-limited to stop inrush browning
out the ESP; ramping toward zero runs ~4× faster so stops feel immediate.

**Failsafe.** `motors_tick()` stops the motors if no command arrives within
`CMD_TIMEOUT_MS` (300 ms), so a control channel going quiet can never mean
"keep doing what you were doing".

---

## Known constraints

**No battery monitoring is possible.** All ADC1 pins are consumed by the camera
and ADC2 is hardware-blocked while WiFi is active. Since 2S cells must not drop
below 2.5 V each, use a standalone low-voltage alarm buzzer on the balance leads.

**The platform version is pinned.** `espressif32 @ 7.0.1` pins the Arduino core
to 2.0.17, which uses `ledcSetup`/`ledcAttachPin`/`ledcWrite(channel, duty)`.
Core 3.x replaced these; `motors.cpp` has an `#error` guard so it fails loudly.

`esp32-camera` ships prebuilt inside the core — there are **no `lib_deps`**.

**`CONFIG_LWIP_TCP_SND_BUF_DEFAULT` is 5760 bytes**, about one JPEG frame, and
is compiled into Arduino's prebuilt lwip. A frame larger than that costs two
window round trips instead of one. Not currently binding, but it's why
`TARGET_FRAME_BYTES` in the adaptive controller is 4000.

---

## Scaffolding to remove once the camera works

Built during the fault hunt, worth keeping only until a healthy baseline exists:

- three video transports — keep whichever measures best, drop the others
- `/speed`, the `c` key, `motors_release()`
- the adaptive quality controller (currently off by default)
- `set_nodelay()` is a deliberate no-op — re-enable it *with a measurement*

Then: the phone web UI with twin spring-to-centre throttle sliders over
WebSocket, which is the part this project actually set out to build.

---

## Layout

```
platformio.ini              single environment, pinned platform
include/
  config.h                  every tunable: pins, PWM, limits, calibration, AP
  secrets.h.example         copy to secrets.h for station mode
src/
  main.cpp                  camera bring-up + streaming
  board.{h,cpp}             serial banner, reset reason, status LED
  camera_pins.h             AI-Thinker OV2640 map
  camera.{h,cpp}            sensor init, PSRAM fallback, full stop/restart
  motors.{h,cpp}            LEDC, signed control, slew limiting, failsafe
  net.{h,cpp}               station + SoftAP, auto channel selection
  stream_server.{h,cpp}     httpd: /ws, /jpg, /stream, /speed, viewer page
  web_server.{h,cpp}        httpd :80 + WebSocket control    ] written, not yet
  web_page.h                mobile UI with throttle sliders   ] wired into main
```

The web UI modules are complete but not yet called from `main.cpp`. They add
nothing to the binary in the meantime — the linker strips them.
