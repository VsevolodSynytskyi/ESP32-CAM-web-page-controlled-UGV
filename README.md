# ESP32-CAM web page controlled UGV

Tank-style tracked UGV: AI-Thinker ESP32-CAM on an ESP32-CAM-MB USB shield,
two DC motors through a TB6612FNG, 2S 18650 pack through a 5V buck converter.
Streams MJPEG video to a phone and takes real-time bidirectional throttle over a
WebSocket.

The build is split into stages. **Each stage is its own PlatformIO environment**,
so every stage stays flashable forever as a debugging tool. When Stage 4
misbehaves, reflash Stage 1a or Stage 2 to isolate hardware from firmware in one
upload.

---

## Quick start

`pio` is not on `PATH`:

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
cd /Users/vsevolodsynytskyi/Documents/PlatformIO/Projects/esp32

pio run -e stage0_blink -t upload -t monitor
```

| Environment | What it does |
|---|---|
| `stage0_blink` | LED + serial. Proves the upload path and the strapping pins. |
| `stage1a_camera_probe` | Camera only, no WiFi. The real "check the ESP" milestone. |
| `stage1b_stream` | MJPEG over station mode, viewed on a laptop. |
| `stage2_motors_serial` | Motors from serial. No camera, no WiFi. Wheels off the ground. |
| `stage3_ap_video` | SoftAP + web page + video on the phone. No control yet. |
| `stage4_full` | Shipping firmware: video + WebSocket throttle. |

`default_envs` in `platformio.ini` is `stage0_blink`; change it as you progress,
or always pass `-e`.

---

## Wiring

TB6612FNG in the **4-pin scheme**: `PWMA`, `PWMB`, `STBY` and `VCC` are tied
permanently to **3.3V**, and the PWM lives on the direction pins instead.

| ESP32-CAM | TB6612FNG |
|---|---|
| GPIO13 | AIN1 (left) |
| GPIO14 | AIN2 (left) |
| GPIO15 | BIN1 (right) |
| GPIO2 | BIN2 (right) |
| 3V3 | PWMA, PWMB, STBY, VCC |
| GND | GND (star point at the battery hub) |
| — | VM ← 2S+, with 1000 µF bulk cap |

**GPIO12 and GPIO16 must be left unconnected.**

### Why these four pins

The camera occupies GPIO 0, 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36,
39. GPIO1/3 are the MB shield's serial. GPIO4 is the flash LED. **GPIO16 is the
PSRAM chip-select** — driving it corrupts the camera framebuffer. That leaves
2, 12, 13, 14, 15, and we use four of them.

GPIO13/14 are unencumbered. GPIO15 is a strapping pin, but low is harmless (it
only silences the ROM boot log — cosmetic; your own serial output is
unaffected). For the fourth pin **GPIO2 beats GPIO12**: both are strapping pins,
but GPIO2 high at boot merely refuses download mode, whereas GPIO12 high at boot
sets VDD_SDIO to 1.8V and the board looks dead. Swap to GPIO12 by changing one
line in `include/config.h` if Stage 0 shows a problem.

### Power

```
2S 18650 (7.4V nom / 8.4V max)
  ├─► TB6612 VM ──┬── 1000µF electrolytic (at the driver, short leads)
  │               └── 0.1µF ceramic
  └─► 5V buck ──► ESP32-CAM 5V pin ──► onboard LDO ──► 3.3V ──► TB6612 logic
```

Star ground at the battery hub, not daisy-chained. 0.1 µF across each motor's
terminals and from each terminal to the can.

**Brownout is the most common failure in these builds** — motor inrush dips the
rail, the ESP32 resets mid-stream, and it looks like a firmware bug. Every stage
prints `esp_reset_reason()` at boot so you can tell the difference. Never feed 5V
into the 3.3V pin.

**USB and the buck must not both drive the 5V pin.** On the bench, power the ESP
from USB only and feed VM from the battery with a common ground — USB never
sources motor current that way. In the field, buck only, USB unplugged.

---

## Bring-up order

### Stage 0 — flash it twice

1. With **nothing** on the motor pins. Confirms port, baud, auto-reset, upload.
2. With the TB6612FNG wired. This is the strapping-pin proof. Before powering
   up, ohmmeter GPIO2 and GPIO15 to 3.3V and GND — TB6612FNG inputs are normally
   pulled *down*, which is what we want. An external pull-up on GPIO2 breaks
   download mode; move BIN2 to GPIO12.

Then measure the motors: inline ammeter, bench supply at 6V, record free-run and
stall current. **The TB6612FNG is 1.2 A continuous / 3.2 A peak per channel**, and
a stalled 6V TT motor pulls around 1.5 A — stall current is the binding
constraint, not voltage. If stall exceeds ~1.5 A the driver is undersized for
this chassis.

### Stage 1a → 1b

1a proves the OV2640, SCCB bus, PSRAM and JPEG encoder with WiFi entirely out of
the picture. Only then bring up the radio.

For 1b, copy `include/secrets.h.example` to `include/secrets.h` and fill in your
**2.4 GHz** network (the ESP32 cannot see a 5 GHz-only network).

### Stage 2 — wheels off the ground

Current-limit the supply to just above the measured free-run current.

Prove all four side × direction combinations before any web UI can obscure a
wiring fault. Then run the sweeps `1`/`2`/`3`/`4` and record where each track
*first* starts turning into the four `MOTOR_MIN_MOVE_*` values in
`include/config.h`. **Four numbers, not two** — forward and reverse differ per
side, and a single per-side figure makes the vehicle pull to one side in reverse.

Press `k` to kill the pilot and watch the failsafe fire. Then reflash Stage 1a to
confirm the camera still initialises with the driver wired.

### Stage 3 → 4

Join `TankCam` / `tankcam1234` (change these in `config.h`) and open
`http://192.168.4.1/`. Your phone will warn about "no internet" — expected. Turn
mobile data off, or Android may silently fall back to cellular.

---

## Controls (Stage 4)

Two full-height vertical sliders, one per track, no mixing.

```
 top    = +100  full throttle FORWARD
 middle =    0  stop (± MOTOR_DEADBAND)
 bottom = -100  full throttle BACKWARD
 release ->  0  springs back to centre
```

Both up to go forward, opposed to pivot in place. Releasing the screen *is* the
stop, which is why there is no stop button to fumble for.

### Layered stopping, weakest assumption last

| # | Trigger | Mechanism |
|---|---|---|
| 1 | Release a slider | zero sent out of band immediately |
| 2 | Screen off / backgrounded | `visibilitychange` zeroes and sends |
| 3 | Socket closes | `close_fn` stops the motors at once |
| 4 | Nothing arrives for `CMD_TIMEOUT_MS` | firmware deadman in `motors_tick()` |

Only #4 trusts nothing. Worst-case stopping time is the 300 ms detection window
plus roughly 100 ms of slew-limited ramp-down. Going much below ~200 ms invites
nuisance stops from ordinary SoftAP jitter.

---

## Architecture notes

**Two HTTP servers, not one.** `:80` serves the UI and the WebSocket; `:81`
serves MJPEG. An MJPEG response never ends — on a single instance it permanently
occupies the worker and the control endpoint stops answering. Each instance also
needs its own `ctrl_port`; both default to 32768, so the second would silently
fail to start.

**Signed end to end.** Slider value → WebSocket payload → duty are all signed,
and the sign alone selects direction. Nothing carries a separate direction flag,
so a direction bit can never disagree with a magnitude.

**One writer to the PWM registers.** `motors_set()` only updates targets and
refreshes the failsafe timer; a dedicated 50 Hz task at priority 6 (above the
httpd tasks) is the sole caller of `ledcWrite`, so streaming a heavy frame can
never delay a throttle update.

**Slow decay by default.** With the PWM on the direction pins, holding one pin
high and modulating the other with the *inverted* duty puts the off phase in
short-brake, so current recirculates and low-speed torque stays linear. The
inversion is the easiest bug to introduce here — get it backwards and the motor
runs fastest at zero throttle. Coast mode is one flag away for comparison.

**Transmission is timer-owned.** `pointermove` fires up to 120 Hz on a modern
phone; only the 20 Hz timer transmits, plus one out-of-band send each on
touch-down and release. Frames are sent even when unchanged — deduplicating them
looks free but would let the deadman trip while holding a steady throttle.

---

## Known constraints

**No battery monitoring is possible via ADC.** All ADC1 pins are consumed by the
camera, and ADC2 is hardware-blocked whenever the WiFi radio is active. Since 2S
cells must not go below 2.5 V each, use a standalone low-voltage alarm buzzer on
the balance leads.

**The platform version is pinned deliberately.** `espressif32 @ 7.0.1` pins
`framework-arduinoespressif32` to `~3.20017.0`, i.e. Arduino-ESP32 core **2.0.17**,
which uses `ledcSetup`/`ledcAttachPin`/`ledcWrite(channel, duty)`. Core 3.x
replaced these with `ledcAttach`/`ledcWrite(pin, duty)`. `motors.cpp` has an
`#error` guard so this fails loudly rather than subtly.

`esp32-camera` ships prebuilt inside the core, so there are **no `lib_deps`**.

**No OTA slot.** `huge_app.csv` gives 3 MB of app and no OTA partition. The build
is ~870 KB, so switching to `min_spiffs.csv` (1.9 MB × 2) would buy OTA
comfortably once the USB port becomes awkward to reach.

---

## Layout

```
platformio.ini              six stage environments, pinned platform
include/
  config.h                  every tunable: pins, PWM, limits, calibration, AP
  secrets.h.example         copy to secrets.h for Stage 1b
src/
  board.{h,cpp}             serial banner, reset reason, status LED
  camera_pins.h             AI-Thinker OV2640 map
  camera.{h,cpp}            sensor init with PSRAM fallback, JPEG sanity check
  motors.{h,cpp}            LEDC, signed control, slew limiting, failsafe
  net.{h,cpp}               station + SoftAP bring-up
  stream_server.{h,cpp}     httpd :81, MJPEG
  web_server.{h,cpp}        httpd :80, page + WebSocket control
  web_page.h                the mobile UI, one self-contained string
  stages/                   one main per stage
```
