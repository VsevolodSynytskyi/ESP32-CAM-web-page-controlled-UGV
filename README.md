# ESP32-CAM web page controlled UGV

Tank-style tracked UGV: AI-Thinker ESP32-CAM on an ESP32-CAM-MB USB shield, two
DC motors through a TB6612FNG, 2S 18650 pack through a 5V buck converter. The
vehicle hosts its own WiFi network and streams live video to a phone browser.

**Working:** camera, SoftAP, MJPEG video at VGA.
**Next:** the web UI with twin spring-to-centre throttle sliders over WebSocket.
Motor control, slew limiting and failsafe are written and tested; the driver
inputs are parked at boot until the UI exists.

## Build

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
pio run -t upload -t monitor
```

Join `TankCam` (password `tankcam1234`), open `http://192.168.4.1/`.

Video is served from **port 81**, the page from port 80.

---

## Read this before changing the camera clock

`CAM_XCLK_HZ` is **24 MHz**, not the conventional 20. This is the least obvious
and most expensive thing in the project.

Every multiple of the pixel clock radiates from the camera ribbon, which is
close to a quarter wave at 2.4 GHz. Harmonics repeat every XCLK MHz and a WiFi
channel is 20 MHz wide — so **any clock at or below 20 MHz has gaps too narrow
to fit a channel in, and a harmonic must land inside whichever channel you
pick.**

| XCLK | ch1 (2412) | ch6 (2437) | ch11 (2462) |
|---|---|---|---|
| ≤ 22 MHz | jammed | jammed | jammed |
| **24 MHz** | clear | clear | jammed |
| 25 MHz | clear | clear | clear |

At 20 MHz the board jams its own radio: **0.12 fps and 16 kbps**, against
**19–25 fps and ~2.7 Mbit/s** at 24 MHz with nothing else changed. The stock
Arduino `CameraWebServer` suffers the same way — it also defaults to 20 MHz.

Two traps if you revisit this:

- **25 MHz looks better on paper and measured ~1.5× worse than 24.** At the
  ~100th harmonic a 0.1% clock error moves the harmonic 2.4 MHz, which is wider
  than the margin. The table is directional only; measure.
- **The clock and the channel are coupled.** `xclk_harmonic_in_channel()` in
  [net.cpp](src/net.cpp) penalises any channel a harmonic falls into, and the
  boot log says which are clear. Change one, re-check the other.

Symptoms that this is happening: the sensor is perfect in isolation, the radio
is perfect in isolation, throughput collapses only when both run, and it is
completely indifferent to channel congestion.

## Two HTTP servers, not one

An MJPEG response never ends, and `esp_http_server` dispatches every handler
from **a single task per instance** — so a stream sharing an instance with the
UI owns that task forever and nothing else gets served. Collapsing these into
one instance cost a measured 15×.

| port | |
|---|---|
| 80 | viewer page |
| 81 | `/stream` |

Each instance also needs its own `ctrl_port`; they all default to 32768, so a
second instance silently fails to start unless it is bumped.

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

Route the motor wires out the **camera end**. The PCB antenna is at the U.FL
end, and wires near it measurably cost throughput (5249 vs 3501 kbps).

### The pins are paired by boot state — don't reshuffle

Before `motors_begin()` runs, these pins hold whatever reset leaves them at, and
the TB6612 is already awake because `STBY` and `PWMA`/`PWMB` are tied to 3V3.
Whatever level is on a pin *is* a command to the driver. **Measured** on this
board:

| **LOW** at reset | **HIGH** at reset |
|---|---|
| GPIO2, GPIO4, GPIO12, GPIO13 | GPIO14, GPIO15, GPIO16 |

GPIO13 is documented as having a reset pull-up and **does not** here. Trusting
the datasheet cost a motor running at full throttle through every boot.

Each channel therefore gets a **matched** pair, which the truth table reads as a
stop:

| Channel | Pins | Boot state | Result |
|---|---|---|---|
| A | 14 + 15 | `(H, H)` | short brake |
| B | 12 + 2 | `(L, L)` | coast |

A high pin with a low pin gives `(H, L)` — a full-throttle drive command that
persists through power-on, watchdog resets, brownouts and **the entire duration
of a firmware upload**. `main.cpp` prints this survey at startup and shouts if a
pair is mismatched; pair from what it reports, not from documentation.

This does not cover holding RST down, when every pin floats. Closing that gap
needs one resistor: `STBY` to GPIO13 with 4.7 kΩ to GND, raised in
`motors_begin()`. Not currently done.

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

## Known constraints

**No battery monitoring is possible.** All ADC1 pins are consumed by the camera
and ADC2 is hardware-blocked while WiFi is active. Since 2S cells must not drop
below 2.5 V each, use a standalone low-voltage alarm buzzer on the balance leads.

**The platform version is pinned.** `espressif32 @ 7.0.1` pins the Arduino core
to 2.0.17, which uses `ledcSetup`/`ledcAttachPin`/`ledcWrite(channel, duty)`.
Core 3.x replaced these; `motors.cpp` has an `#error` guard so it fails loudly.

`esp32-camera` ships prebuilt inside the core — there are **no `lib_deps`**.

## Layout

```
platformio.ini              single environment, pinned platform
include/
  config.h                  every tunable: pins, PWM, limits, camera, AP
  secrets.h.example         copy to secrets.h for station mode
src/
  main.cpp                  boot order and wiring-together
  board.{h,cpp}             serial banner, reset reason, status LED
  camera_pins.h             AI-Thinker OV2640 map
  camera.{h,cpp}            sensor init and profile
  motors.{h,cpp}            LEDC, signed control, slew limiting, failsafe
  net.{h,cpp}               station + SoftAP, harmonic-aware channel choice
  stream_server.{h,cpp}     viewer page on :80, MJPEG on :81
```
