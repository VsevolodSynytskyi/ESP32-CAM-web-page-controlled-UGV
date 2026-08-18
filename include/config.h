#pragma once

// ===========================================================================
//  ESP32-CAM web page controlled UGV - central configuration
//
//  Board: AI-Thinker ESP32-CAM on an ESP32-CAM-MB USB shield.
//  Driver: TB6612FNG, wired in the 4-pin scheme (see below).
//
//  This header intentionally includes nothing, so it is safe from any
//  translation unit. Enum-valued knobs (CAM_FRAMESIZE) expand inside files
//  that already include the relevant driver header.
// ===========================================================================

// ---------------------------------------------------------------------------
//  Motor driver pins - TB6612FNG 4-pin scheme
//
//  PWMA, PWMB, STBY and VCC are tied permanently to 3.3V on the vehicle; the
//  PWM lives on the direction pins instead. That keeps GPIO16 free (it is the
//  reportedly the PSRAM chip-select on modules with 4 MB PSRAM, in which case
//  driving it would corrupt the camera
//  framebuffer) and keeps GPIO12 free (pulled high at boot it sets VDD_SDIO to
//  1.8V and the board appears dead).
//
//  The AI-Thinker camera occupies GPIO 0, 5, 18, 19, 21, 22, 23, 25, 26, 27,
//  32, 34, 35, 36, 39. GPIO1/3 are the MB shield's serial port. GPIO4 is the
//  flash LED. That leaves exactly 2, 12, 13, 14, 15 - and we use four of them.
// ---------------------------------------------------------------------------
//  PAIRED BY BOOT STATE - do not reshuffle these without re-reading the note.
//
//  Before motors_begin() runs, these pins are inputs sitting at whatever level
//  reset leaves them at, and the TB6612 is already awake (STBY and PWMA/B are
//  tied to 3V3). Whatever that level happens to be IS a command to the driver.
//
//  These levels were MEASURED on this board by reading the pins at the top of
//  setup(), not taken from the datasheet - GPIO13's documented pull-up does not
//  hold here, and trusting the documentation cost us a motor that ran at full
//  throttle through every boot:
//
//      HIGH: 14, 15, 16          LOW: 2, 4, 12, 13
//
//  So each channel gets a MATCHED pair, which the TB6612 truth table reads as a
//  stop rather than a drive:
//
//      channel A = 14 + 15 -> (H,H) -> short brake
//      channel B = 12 +  2 -> (L,L) -> coast
//
//  Neither turns a motor. This is why no external pull-down resistors are
//  needed: the pin choice does the job on its own, for free, and it holds
//  through power-on, watchdog resets, brownouts, and the whole duration of a
//  firmware upload (when the chip sits in download mode and never reaches our
//  code at all).
//
//  Mixing a high pin with a low pin on the same channel gives (H,L), a
//  full-throttle drive command. That is the failure this layout exists to
//  prevent, so keep {14,15} together and {2,12} together.
//
//  If you ever change these pins, re-run the boot-level survey printed by
//  main.cpp and pair from what it reports. Do not pair from a datasheet.
#define PIN_AIN1 14  // motor A (motor 1, on AO1/AO2) direction 1
#define PIN_AIN2 15  // motor A (motor 1, on AO1/AO2) direction 2
#define PIN_BIN1 12  // motor B (motor 2, on BO1/BO2) direction 1  <- strapping, see below
#define PIN_BIN2 2   // motor B (motor 2, on BO1/BO2) direction 2  <- strapping, see below

// GPIO13 is now unused and free. Leave it unconnected.
//
// GPIO15 is a strapping pin, but the only documented consequence of it being
// low at boot is losing the ROM startup log - cosmetic, and it measures high
// here anyway. We only drive it after boot, so the strap is never affected.

// Both strapping pins on channel B want to be LOW at boot, which is exactly the
// state this layout puts them in:
//
//   GPIO2  high at boot -> the chip refuses download mode and uploads fail
//   GPIO12 high at boot -> VDD_SDIO drops to 1.8V and the board appears dead
//
// Measured 0.6 Mohm from every TB6612 input to VCC on this build, i.e. no
// pull-up anywhere, so both sit low on their internal pull-downs. If the board
// ever stops booting or stops accepting uploads once the driver is connected,
// suspect these two pins first - a 10k resistor to GND on each removes all
// doubt, though it should not be necessary.

// ---------------------------------------------------------------------------
//  LEDC (hardware PWM) channels
//
//  Channels 0/1 share timer 0 and channels 2/3 share timer 1. All four run at
//  the same frequency, so sharing timers is harmless.
//
//  The camera driver also needs an LEDC timer and channel to generate XCLK.
//  camera.cpp gives it timer 3 / channel 7 so it can never collide with these.
//  If you add more PWM later, avoid channels 6, 7, 14 and 15 - Arduino maps
//  those onto timer 3, which the camera owns.
// ---------------------------------------------------------------------------
#define LEDC_CH_AIN1 0
#define LEDC_CH_AIN2 1
#define LEDC_CH_BIN1 2
#define LEDC_CH_BIN2 3

// 20 kHz is above the audible range, and 20000 * 1024 = 20.5 MHz stays under
// the 80 MHz APB clock, so 10-bit resolution is legal at this frequency.
#define MOTOR_PWM_FREQ_HZ 20000
#define MOTOR_PWM_BITS 10
#define MOTOR_PWM_MAX ((1 << MOTOR_PWM_BITS) - 1)  // 1023

// ---------------------------------------------------------------------------
//  Motor control scale and limits
//
//  Everything is signed end to end: slider value, WebSocket payload, and duty.
//  The sign alone selects direction, so a direction bit can never disagree
//  with a magnitude.
//
//      +MOTOR_SCALE = full throttle forward
//                 0 = stop
//      -MOTOR_SCALE = full throttle reverse
// ---------------------------------------------------------------------------
#define MOTOR_SCALE 1000

// Duty ceiling. 2S is 8.4V fresh off the charger; the common 3-6V TT
// gearmotors will cook at full duty. 0.55 * 8.4V ~= 4.6V average.
//
//  >>> Raise this only after measuring the motors' voltage rating and stall
//  >>> current. The TB6612FNG is 1.2A continuous / 3.2A peak per channel.
#define MOTOR_MAX_DUTY 0.55f

// Band around zero treated as a hard stop, in MOTOR_SCALE units. Makes
// "slider centre = stop" a reliable band rather than a single pixel.
#define MOTOR_DEADBAND 40

// Slow decay (default): current recirculates through the low-side FETs, so
// torque per unit duty is far more linear and crawl speed is usable. Set false
// for coast/fast decay to compare the two on the bench.
#define MOTOR_SLOW_DECAY true

// Brake (both direction pins high) instead of coast (both low) when stopped.
// Coast is gentler on the gearbox and lets you push the vehicle by hand.
#define MOTOR_BRAKE_WHEN_STOPPED false

// ---------------------------------------------------------------------------
//  Slew rate limiting - applied in motors_tick()
//
//  Asymmetric on purpose. Ramping up slowly is what stops motor inrush from
//  browning out the ESP32; ramping down fast is what makes releasing a slider
//  feel instant. Since the sliders spring back to centre, deceleration is the
//  common case.
// ---------------------------------------------------------------------------
#define MOTOR_TICK_HZ 50                  // 20 ms per tick
#define MOTOR_SLEW_UP_PER_TICK 50         // ~400 ms from stop to full throttle
#define MOTOR_SLEW_DOWN_PER_TICK 200      // ~100 ms from full throttle to stop

// Failsafe. If no command arrives within this window the motors stop. This is
// the backstop that does not trust the browser at all - it covers walking out
// of WiFi range, a locked phone, and a crashed tab alike.
#define CMD_TIMEOUT_MS 300

// ---------------------------------------------------------------------------
//  Per-track calibration - fill in from a signed duty sweep on the bench
// ---------------------------------------------------------------------------

// Minimum duty at which each track actually starts turning, in MOTOR_SCALE
// units. Non-zero commands are remapped onto [min .. MOTOR_SCALE] so even the
// gentlest slider movement produces motion instead of a stall whine.
//
// Four values, not two: forward and reverse differ per side because of gearbox
// preload and brush wear. A single per-side figure makes the vehicle pull to
// one side in reverse.
#define MOTOR_MIN_MOVE_L_FWD 0
#define MOTOR_MIN_MOVE_L_REV 0
#define MOTOR_MIN_MOVE_R_FWD 0
#define MOTOR_MIN_MOVE_R_REV 0

// Flip a track in software if its motor is wired backwards, instead of
// resoldering. Verify on the bench before trusting the web UI.
#define MOTOR_INVERT_L false
#define MOTOR_INVERT_R false

// ---------------------------------------------------------------------------
//  Onboard LEDs
// ---------------------------------------------------------------------------
#define PIN_STATUS_LED 33  // small red LED next to the antenna. ACTIVE LOW.
#define PIN_FLASH_LED 4    // high-power white LED. Extremely bright. Also SD D1.

// ---------------------------------------------------------------------------
//  Camera
//
//  Settings captured 2026-08-18 by tuning against a live image, then measured.
//  Only XCLK, frame size and quality matter for throughput; the sensor profile
//  further down is picture quality only.
// ---------------------------------------------------------------------------

// 24 MHz, not the conventional 20, and this is the most important line here.
//
// Every multiple of the pixel clock radiates from the camera ribbon, which is
// close to a quarter wave at 2.4 GHz. Harmonics repeat every XCLK MHz and a
// WiFi channel is 20 MHz wide - so any clock at or below 20 MHz has gaps too
// narrow to fit a channel in, and a harmonic MUST land inside whichever channel
// you pick. At 20 MHz that is 2420 in ch1, 2440 in ch6, 2460 in ch11: nowhere
// to hide. It cost a ~20x throughput collapse that took days to find, and it
// hits the stock CameraWebServer too, which also defaults to 20 MHz.
//
// Measured here: 0.12 fps at 20 MHz, 19-25 fps at 24 MHz, all else equal.
// 25 MHz was tried and came out ~1.5x worse than 24 despite looking equivalent
// on paper - at the ~100th harmonic a 0.1% clock error moves it 2.4 MHz, which
// is wider than the margin, so measurement beats arithmetic here.
//
// net.cpp penalises any channel a harmonic falls into. Change this and the
// channel choice moves with it.
#define CAM_XCLK_HZ 24000000

// Frame size dominates throughput - bytes scale with pixel count.
#define CAM_FRAMESIZE FRAMESIZE_VGA  // 640x480

// 10-63. LOWER is BETTER quality and BIGGER frames. The link saturates around
// 2.7 Mbit/s either way, so this is purely a detail-vs-frame-rate trade:
// q16 gave ~14 kB frames at 22-25 fps, q4 gave ~50 kB at 7-11 fps.
#define CAM_JPEG_QUALITY 8

// Two, matching the stock CameraWebServer. GRAB_LATEST needs at least two.
#define CAM_FB_COUNT 2

// Sensor profile - picture only, no bearing on throughput.
#define CAM_BRIGHTNESS 0      // -2..2
#define CAM_CONTRAST 0        // -2..2
#define CAM_SATURATION 0      // -2..2
#define CAM_SHARPNESS 0       // -2..2  (OV2640 ignores this)
#define CAM_SPECIAL_EFFECT 0  // 0 none, 1 negative, 2 grayscale, 3 red, 4 green, 5 blue, 6 sepia
#define CAM_WB_MODE 0         // 0 auto, 1 sunny, 2 cloudy, 3 office, 4 home
#define CAM_AWB 1             // auto white balance
#define CAM_AWB_GAIN 1
#define CAM_AEC 1             // auto exposure
#define CAM_AEC2 0            // DSP auto exposure ("night mode")
#define CAM_AE_LEVEL 0        // -2..2
#define CAM_AEC_VALUE 168     // 0..1200, inert while CAM_AEC is 1
#define CAM_AGC 1             // auto gain
#define CAM_AGC_GAIN 0        // 0..30, only used when CAM_AGC is 0
#define CAM_GAINCEILING 0     // 0..6 == 2x..128x
#define CAM_BPC 0             // black pixel correction
#define CAM_WPC 1             // white pixel correction
#define CAM_RAW_GMA 1         // gamma correction
#define CAM_LENC 1            // lens shading correction
#define CAM_DCW 1             // downsize enable
#define CAM_COLORBAR 0        // test pattern

// Set these once the camera is bolted to the chassis.
#define CAM_VFLIP 0    // 1 = flip vertically
#define CAM_HMIRROR 0  // 1 = mirror horizontally

// ---------------------------------------------------------------------------
//  Networking
// ---------------------------------------------------------------------------

// SoftAP - the field mode. The phone joins this network directly, so no router
// is involved and the vehicle works anywhere.
#define AP_SSID "TankCam"
#define AP_PASSWORD "tankcam1234"  // WPA2 requires at least 8 characters

// 0 = scan the band at boot and pick the quietest of 1 / 6 / 11.
// 1-13 = force that channel.
//
// Worth leaving on auto. 2.4 GHz is usually crowded, and channel congestion is
// invisible to RSSI: signal strength says how loudly the other end hears you,
// not whether anyone else is talking. A contended channel gives clear windows
// alternating with multi-second stalls at perfect signal - which reads as a
// broken stream rather than a busy band.
#define AP_CHANNEL 0

#define AP_MAX_CONN 1  // one pilot; extra clients only steal bandwidth

// The video server takes HTTP_PORT + 1 - see the two-instance note in
// stream_server.h.
#define HTTP_PORT 80

// How long net_begin_sta() waits for a DHCP lease before giving up.
#define STA_CONNECT_TIMEOUT_MS 20000
