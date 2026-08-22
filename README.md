# ESP32-CAM web page controlled UGV

Two-motor UGV: AI-Thinker ESP32-CAM on an ESP32-CAM-MB USB shield, two DC
motors through a TB6612FNG, 2S 18650 pack through a 5V buck converter. The
vehicle hosts its own WiFi network and streams live video to a phone browser.

Chassis is undecided - the differential drive and the twin-slider control scheme
suit tracks or skid-steer wheels equally, so nothing here assumes either.

**Working:** camera, SoftAP, MJPEG video at VGA, and hold-to-run motor test
buttons on the page — forward and back per motor, enough to check the wiring,
the direction of each side and the battery under load, all at once.
**Next:** replacing those buttons with twin spring-to-centre throttle sliders.
The firmware side is already the real thing; only `web_page.h` changes.

## Build

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
pio run -t upload -t monitor
```

Join `UGV` (password `letmecontrolit`), open `http://192.168.4.1/`.

Video is served from **port 81**, the page and control socket from port 80.

### The test buttons

Four buttons, forward and back for motor A and motor B, labelled to match the
TB6612FNG silkscreen — when a motor turns the wrong way, the label has to name
the thing you rewire. Press both forwards together to drive straight; press
opposite ones to pivot.

**Hold to run — nothing latches.** Releasing the button, sliding a thumb off it,
backgrounding the page or losing WiFi all mean stop, and `motors_tick()` stops
the vehicle by itself after `CMD_TIMEOUT_MS` (300 ms) if no command arrives. The
page re-sends the held state at 10 Hz to stay ahead of that.

Full press is `MOTOR_SCALE`, which `MOTOR_MAX_DUTY` then caps at 55% duty. The
slew limiter ramps up over ~400 ms, so first power-on shouldn't brown out — if
it does, that is the bulk capacitor, not the code.

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
| 80 | viewer page, and `/control` WebSocket |
| 81 | `/stream` |

The control socket lives with the page rather than with the video for the same
reason: it has to answer while a frame is going out. Handlers on port 80 must
stay quick — they share one dispatch task with each other.

Commands are **two signed bytes, left then right, each −100…+100**, with the
sign carrying direction. That is the format the sliders will use unchanged.

Each instance also needs its own `ctrl_port`; they all default to 32768, so a
second instance silently fails to start unless it is bumped.

---

## Wiring

TB6612FNG in the **4-pin scheme**: `PWMA`, `PWMB`, `STBY` and `VCC` tied
permanently to **3.3V**, with the PWM on the direction pins.

**Five wires** run from the ESP32-CAM to the driver:

| ESP32-CAM | TB6612FNG | LEDC ch |
|---|---|---|
| GPIO14 | `AIN1` — channel A (motor 1, on AO1/AO2) | 0 |
| GPIO15 | `AIN2` — channel A | 1 |
| GPIO12 | `BIN1` — channel B (motor 2, on BO1/BO2) | 2 |
| GPIO2 | `BIN2` — channel B | 3 |
| 3V3 | `VCC`, with `STBY`/`PWMA`/`PWMB` jumpered to it on the driver | — |

**`GND` is deliberately not one of them.** The two boards share a ground at the
star point, not through a wire between them — see [Power](#power). Adding that
sixth wire creates a second return path, and motor current then divides into the
ESP's ground pin.

### Same wiring, from the driver's side

Every pin on the breakout, for when you have the board in front of you. Most
TB6612FNG modules carry these in two rows — power and motor outputs along one
edge, logic along the other:

| TB6612FNG | connects to | |
|---|---|---|
| `VM` | battery `+`, through the switch | motor rail, 7.4–8.4 V. 1000 µF here |
| `VCC` | ESP32-CAM `3V3` | logic supply, ~1 mA |
| `GND` | the star point at battery `−` | |
| `STBY` | ESP32-CAM `3V3` | **tie high or nothing moves** |
| `PWMA` | ESP32-CAM `3V3` | tied high — PWM is on the direction pins |
| `AIN1` | ESP32-CAM GPIO14 | |
| `AIN2` | ESP32-CAM GPIO15 | |
| `AO1` | motor A | |
| `AO2` | motor A | |
| `PWMB` | ESP32-CAM `3V3` | tied high |
| `BIN1` | ESP32-CAM GPIO12 | |
| `BIN2` | ESP32-CAM GPIO2 | |
| `BO1` | motor B | |
| `BO2` | motor B | |

`VCC`, `STBY`, `PWMA` and `PWMB` are jumpered together **on the driver**, so
they cost one wire back to the ESP32, not four.

Breakouts usually expose two or three `GND` pins. They are one net internally —
use a single one, wired to the star.

**`STBY` low is the silent failure.** The driver sits in standby with every
output high-impedance: the page's buttons work, the firmware logs normally, no
error appears anywhere, and the motors simply never turn. `PWMA`/`PWMB` do the
same thing per channel.

**`VM` and `VCC` sit next to each other** on most boards. `VM` on 3V3 gives the
motor 0.55 × 3.3 ≈ 1.8 V — it buzzes and won't turn. `VCC` on the pack is worse.

To reverse a motor, swap its two output wires (`AO1`↔`AO2`) or flip
`MOTOR_INVERT_L`/`MOTOR_INVERT_R` in [config.h](include/config.h). Don't do both.

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
   +  ── switch ──┬──► TB6612 VM ──┬── 1000 µF electrolytic (at the driver)
                  │                └── 0.1 µF ceramic
                  └──► buck IN+ ──► buck OUT+ ──► ESP32-CAM 5V pin
                                                       │
                                                  onboard LDO
                                                       │
                                                     3V3 ──► TB6612 VCC/STBY/PWMA/PWMB

   -  ◄── THE STAR ──┬──── TB6612 GND
                     └──── buck IN-  ──(buck ground)──  buck OUT- ──► ESP32-CAM GND
```

**Two wires on `+`, two on `−`, and nothing daisy-chained.** The star is the
battery negative terminal itself. The ESP32's ground reaches it *through the
buck* — `IN-` and `OUT-` are the same copper — which is why there is no ground
wire between the ESP32 and the driver. Motor return current must never share
a wire with the ESP's, or 20 kHz PWM edges land on its ground reference.

0.1 µF across each motor's terminals and from each terminal to the can. Never
feed 5 V into 3V3. **USB and the buck must not both drive the 5 V pin** — they
are the same node with nothing between them. On the bench, USB for the ESP and
battery for VM only; the grounds are already common through the buck.

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

**The tick runs in its own task, above the HTTP servers.** The failsafe is only
worth as much as the tick that enforces it, and `loop()` sits *below* the
streaming task — a stall there would leave the last throttle latched on the
motors for the duration. `motors_start_task()` pins it to core 1 at priority 6.

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
  web_server.{h,cpp}        page and /control WebSocket on :80
  web_page.h                the page: video, test buttons, control link
  stream_server.{h,cpp}     MJPEG on :81
```
