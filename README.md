# ESP32-CAM web page controlled UGV

Tank-style tracked UGV: AI-Thinker ESP32-CAM on an ESP32-CAM-MB USB shield, two
DC motors through a TB6612FNG, 2S 18650 pack through a 5V buck converter.

**Current state: motor wiring and control verification.** The firmware runs a
looping four-phase motor pattern so the wiring can be confirmed by watching the
wheels. Camera streaming and the phone web UI come next.

---

## Build and run

`pio` is not on `PATH`:

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
cd "/Users/vsevolodsynytskyi/Documents/PlatformIO/Projects/ESP32-CAM web page controlled UGV"

pio run -t upload -t monitor
```

### What it does

Loops forever, with a 1.5 s stop between phases so transitions are obvious:

| Phase | Duration | Expect |
|---|---|---|
| 1/4 both forward | 5 s | both wheels turning the **same** way |
| 2/4 both backward | 5 s | both reversed, still matching |
| 3/4 A fwd, B back | 5 s | wheels turning **opposite** ways |
| 4/4 A back, B fwd | 5 s | opposite again, both flipped |

Five-second arming countdown at boot before the first movement.

| Key | Action |
|---|---|
| `SPACE` | pause / resume (pause stops immediately, bypassing the slew limiter) |
| `+` / `-` | throttle ±100 |
| `r` | restart at phase 1 |
| any other | emergency stop |

### Fixing what you see

| Symptom | Fix |
|---|---|
| A motor runs the wrong way | `MOTOR_INVERT_L` / `MOTOR_INVERT_R` in `include/config.h` |
| Wrong track responds | swap motor leads between AO and BO, or swap the `PIN_AIN*` / `PIN_BIN*` pairs |
| Buzzes but doesn't turn | below its start threshold — press `+`, then record `MOTOR_MIN_MOVE_*` |

---

## Wiring

TB6612FNG in the **4-pin scheme**: `PWMA`, `PWMB`, `STBY` and `VCC` tied
permanently to **3.3V**, with the PWM on the direction pins instead.

| ESP32-CAM | TB6612FNG |
|---|---|
| GPIO14 | AIN1 — channel A (motor 1) |
| GPIO2 | AIN2 — channel A |
| GPIO15 | BIN1 — channel B (motor 2) |
| GPIO13 | BIN2 — channel B |
| 3V3 | PWMA, PWMB, STBY, VCC |
| GND | GND (star point at the battery hub) |
| — | VM ← 2S+, with 1000 µF bulk cap |
| 4.7 kΩ each | AIN1, AIN2, BIN1, BIN2 → GND (**required**, see below) |

**GPIO12 and GPIO16 must be left unconnected.**

Which GPIO drives which input is arbitrary — all four are plain LEDC outputs.
Rewire as convenient and edit the four `PIN_*` lines in `include/config.h`.

### Why only these pins are available

The camera occupies GPIO 0, 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36,
39. GPIO1/3 are the MB shield's serial. GPIO4 is the flash LED. **GPIO16 is the
PSRAM chip-select** — driving it corrupts the camera framebuffer. That leaves
2, 12, 13, 14, 15, and GPIO12 high at boot sets VDD_SDIO to 1.8 V and the board
appears dead.

Four usable pins is exactly why the driver runs in the 4-pin scheme. The
conventional 6-pin wiring would force spending GPIO16 (losing PSRAM, so no VGA
framebuffer) or GPIO4 (a strobing headlight).

### ⚠️ The pull-down resistors are not optional

The TB6612FNG does not pull its inputs down, and the ESP32's reset defaults are
not uniform: GPIO13/14/15 come up weakly **high**, GPIO2 comes up weakly **low**.
Any mismatched pair reads as a drive command, so one track runs at **full
throttle** from power-on until `motors_begin()` executes — including for the
entire duration of every firmware upload, since the chip sits in download mode
and never reaches your code. Put the vehicle on the floor, hit Upload, and it
drives off.

4.7 kΩ from each input to GND beats the ESP's ~45 kΩ internal pull-ups (0.31 V,
well under the 0.8 V threshold) and costs 0.7 mA when driven. No arrangement of
GPIO 2/13/14/15 avoids this — three default high, only one defaults low.

Until they are fitted, use this order every time:

1. **VM disconnected** from the battery
2. Upload, wait for the banner
3. **Then** connect VM

`motors_begin()` is the first statement in `setup()` for the same reason, which
shrinks the window to bootloader time — but firmware cannot help during an
upload.

### Power

```
2S 18650 (7.4 V nom / 8.4 V max)
  ├─► TB6612 VM ──┬── 1000 µF electrolytic (at the driver, short leads)
  │               └── 0.1 µF ceramic
  └─► 5V buck ──► ESP32-CAM 5V pin ──► onboard LDO ──► 3V3 ──► TB6612 logic
```

Star ground at the battery hub, not daisy-chained. 0.1 µF across each motor's
terminals and from each terminal to the can.

**Brownout is the most common failure in these builds** — motor inrush dips the
rail, the ESP32 resets, and it looks like a firmware bug. The boot banner prints
`esp_reset_reason()` so you can tell the difference. Never feed 5 V into 3V3.

**USB and the buck must not both drive the 5 V pin.** On the bench: USB for the
ESP, battery for VM only, common ground. In the field: buck only, USB unplugged.

### Motors are still unmeasured

`MOTOR_MAX_DUTY` is `0.55f`, holding the average at ~4.6 V — safe for almost
anything. But stall current is unknown against the TB6612FNG's **1.2 A
continuous / 3.2 A peak** limit, and stall current is the binding constraint,
not voltage. Measure winding resistance across the motor terminals and compute
`I_stall ≈ 8.4 V ÷ R`. Over ~1.5 A means the driver is undersized.

Touch the driver chip periodically. Warm is fine; too hot to hold means stop.

---

## Design notes

**Signed end to end.** Throttle is signed `-1000 … +1000` through the whole
chain, and the sign alone selects direction. Nothing carries a separate
direction flag, so a direction bit can never disagree with a magnitude.

**One writer to the PWM registers.** `motors_set()` only updates targets and
refreshes the failsafe timer; `motors_tick()` at 50 Hz is the sole caller of
`ledcWrite`.

**Slow decay by default.** With the PWM on the direction pins, holding one pin
high and modulating the other with the *inverted* duty puts the off phase in
short-brake, so current recirculates and low-speed torque stays linear. That
inversion is the easiest bug to introduce here — get it backwards and the motor
runs fastest at zero throttle.

**Asymmetric slew limiting.** Ramping up is rate-limited to stop inrush browning
out the ESP; ramping toward zero runs ~4× faster so stops feel immediate. A
command dragged from full forward to full reverse passes through zero rather
than reversing the bridge instantly.

**Failsafe.** `motors_tick()` stops the motors if no command has arrived within
`CMD_TIMEOUT_MS` (300 ms). It exists so that a control channel going quiet can
never mean "keep doing what you were doing".

---

## Known constraints

**No battery monitoring via ADC.** All ADC1 pins are consumed by the camera, and
ADC2 is hardware-blocked while WiFi is active. Since 2S cells must not drop
below 2.5 V each, use a standalone low-voltage alarm buzzer on the balance leads.

**The platform version is pinned.** `espressif32 @ 7.0.1` pins the Arduino core
to 2.0.17, which uses `ledcSetup`/`ledcAttachPin`/`ledcWrite(channel, duty)`.
Core 3.x replaced these; `motors.cpp` has an `#error` guard so it fails loudly.

`esp32-camera` ships prebuilt inside the core, so there are **no `lib_deps`**.

---

## Layout

```
platformio.ini              single environment, pinned platform
include/
  config.h                  every tunable: pins, PWM, limits, calibration, AP
  secrets.h.example         copy to secrets.h when station mode is needed
src/
  main.cpp                  current firmware: motor wiring check
  board.{h,cpp}             serial banner, reset reason, status LED
  motors.{h,cpp}            LEDC, signed control, slew limiting, failsafe
  camera_pins.h             AI-Thinker OV2640 map          ] written and
  camera.{h,cpp}            sensor init, PSRAM fallback    ] compiling, but
  net.{h,cpp}               station + SoftAP bring-up      ] not yet called
  stream_server.{h,cpp}     httpd :81, MJPEG               ] from main.cpp —
  web_server.{h,cpp}        httpd :80, page + WebSocket    ] the linker
  web_page.h                mobile UI, self-contained      ] strips them
```

The camera and networking modules are complete but inert until `main.cpp` calls
them. They add nothing to the binary in the meantime: the build is 298 KB either
way.
