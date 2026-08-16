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
| GPIO15 | AIN2 — channel A |
| GPIO12 | BIN1 — channel B (motor 2) |
| GPIO2 | BIN2 — channel B |
| 3V3 | PWMA, PWMB, STBY, VCC |
| GND | GND (star point at the battery hub) |
| — | VM ← 2S+, with 1000 µF bulk cap |

**GPIO13 and GPIO16 must be left unconnected.**

The pin pairing is deliberate — see below. No pull-down resistors are needed.

### ESP32-CAM pin reference

Every pin the board actually breaks out. Everything not listed — GPIO 5, 18, 19,
21, 22, 23, 25, 26, 27, 32, 34, 35, 36, 39 — is consumed by the camera.

| Label | GPIO | Safe to use? | Reason | Our use |
|---|---|:---:|---|---|
| D0 | 0 | ⚠️ | must be HIGH during boot and LOW for flashing | camera XCLK |
| TX0 | 1 | ❌ | Tx pin, used for flashing and debugging | MB shield serial |
| D2 | 2 | ⚠️ | must be LOW during boot, cannot be used when microSD card is present | **BIN2** |
| RX0 | 3 | ❌ | Rx pin, used for flashing and debugging | MB shield serial |
| D4 | 4 | ⚠️ | connected to the on-board Flash LED, cannot be used when microSD card is present | free — future headlight |
| D12 | 12 | ⚠️ | must be LOW during boot, cannot be used when microSD card is present | **BIN1** |
| D13 | 13 | ⚠️ | cannot be used when microSD card is present | free — candidate for STBY |
| D14 | 14 | ⚠️ | cannot be used when microSD card is present | **AIN1** |
| D15 | 15 | ⚠️ | must be HIGH during boot, prevents startup log if pulled LOW, cannot be used when microSD card is present | **AIN2** |
| RX2 | 16 | ✅ | — | left unconnected, see note |

**Keep the microSD slot empty.** Six of these pins are the SD_MMC lines. An
inserted card loads them and changes their levels at reset — which is exactly
what the boot-state pairing above depends on. A card in the slot can silently
turn a safe `(H,H)` pair into a `(H,L)` drive command.

**GPIO16 is disputed.** The table marks it usable and says nothing about PSRAM,
but on modules carrying 4 MB PSRAM it is widely reported to be the PSRAM
chip-select, in which case driving it would corrupt the camera framebuffer. We
don't need it, so it stays unconnected rather than settling the argument.

**GPIO15's "must be HIGH" is softer than it sounds** — the stated consequence of
pulling it low is only losing the ROM startup log, which is cosmetic. That is why
it is a viable home for `STBY` if we add a hardware kill line.

Four comfortably usable pins is exactly why the driver runs in the 4-pin scheme.
The conventional 6-pin wiring would force spending GPIO16 or GPIO4 as well.

### The pins are paired by boot state — don't reshuffle them

Before `motors_begin()` runs, these pins are inputs sitting at whatever level
reset leaves them at, and the TB6612 is already awake because `STBY` and
`PWMA`/`PWMB` are tied to 3V3. Whatever that level happens to be *is* a command
to the driver. And the levels aren't uniform:

| **LOW** at reset | **HIGH** at reset |
|---|---|
| GPIO2, GPIO4, GPIO12, GPIO13 | GPIO14, GPIO15, GPIO16 |

**These were measured, not taken from the datasheet.** GPIO13 is documented as
having a reset pull-up and does not on this board — trusting that cost us a
motor running at full throttle through every boot. `main.cpp` prints this survey
at startup; if you change pins, pair from what it reports.

So each channel gets a **matched** pair, which the truth table reads as a stop:

| Channel | Pins | Boot state | Result |
|---|---|---|---|
| A | 14 + 15 | `(H, H)` | short brake |
| B | 12 + 2 | `(L, L)` | coast |

Neither turns a motor. This is why there are no pull-down resistors: the pin
choice does it for free, and it holds through power-on, watchdog resets,
brownouts, and the **entire duration of a firmware upload** — when the chip sits
in download mode and never reaches our code at all.

Mixing a high pin with a low pin on one channel gives `(H, L)`, which is a
full-throttle drive command. Put the vehicle on the floor, hit Upload, and it
drives off — and a brownout under load becomes a reset, which becomes full
throttle, which deepens the brownout. Keep `{14,15}` together and `{2,12}`
together.

**This does not cover holding RST down.** While EN is held low the core never
comes out of reset, so no pull is applied at all and every pin floats — the
TB6612 then sees garbage and a motor may run. Tapping RST or a normal reset is a
millisecond of this; holding it is unbounded. Closing that gap needs one
external resistor, and the place to put it is `STBY`: wire `STBY` to GPIO13
(now free) with 4.7 kΩ to GND, and have `motors_begin()` raise it. `STBY` low
disables the driver outright regardless of all four inputs. Not currently done.

Both strapping pins landed on channel B, and both want to be **low** at boot —
which is exactly what this layout gives them:

| Pin | If high at boot |
|---|---|
| GPIO2 | chip refuses download mode, uploads fail |
| GPIO12 | VDD_SDIO drops to 1.8 V, board appears dead |

Measured 0.6 MΩ from every TB6612 input to VCC on this build, so nothing pulls
them up. If the board ever stops booting or stops accepting uploads once the
driver is connected, suspect these two first — 10 kΩ to GND on each removes all
doubt, though it shouldn't be necessary.

`motors_begin()` is still the first statement in `setup()`, as defence in depth.

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
