#include "motors.h"

#include <Arduino.h>

#include "config.h"

// This file uses the Arduino-ESP32 core 2.x LEDC API. Core 3.x replaced
// ledcSetup/ledcAttachPin with ledcAttach and changed ledcWrite to take a pin
// instead of a channel, so it would compile to something quite different or not
// at all. platformio.ini pins the platform for this reason.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#error "motors.cpp targets Arduino-ESP32 core 2.x (ledcSetup/ledcAttachPin/ledcWrite(channel,duty)). \
Either restore the platform pin in platformio.ini or port this file to ledcAttach/ledcWrite(pin,duty)."
#endif

// Desired throttle, written by motors_set() from whichever task owns the
// control channel. Read by motors_tick().
static int16_t s_target_l = 0;
static int16_t s_target_r = 0;

// Throttle actually being applied, after slew limiting. Owned entirely by
// motors_tick().
static int16_t s_applied_l = 0;
static int16_t s_applied_r = 0;

static uint32_t s_last_cmd_ms = 0;
static bool s_failsafe = true;  // start latched until the first real command
static bool s_slow_decay = MOTOR_SLOW_DECAY;

// motors_set() and motors_tick() run on different tasks (the WebSocket handler
// and the motor task). The spinlock keeps the left/right pair consistent, so a
// tick can never see a new left throttle alongside a stale right one - which
// would make the vehicle veer for one tick on every command.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
//  Low-level output
// ---------------------------------------------------------------------------

// Writes one motor's pin pair for a signed command already in output space
// (calibrated, ceiling-limited, deadbanded). See the TB6612FNG truth table with
// the PWM pin tied high:
//
//      IN1  IN2   result
//       H    L    drive one way
//       L    H    drive the other way
//       L    L    high-Z  -> coast (fast decay)
//       H    H    short brake -> slow decay
//
static void drive_pair(uint8_t ch1, uint8_t ch2, int16_t cmd) {
  if (cmd == 0) {
    if (MOTOR_BRAKE_WHEN_STOPPED) {
      ledcWrite(ch1, MOTOR_PWM_MAX);  // both high -> short brake
      ledcWrite(ch2, MOTOR_PWM_MAX);
    } else {
      ledcWrite(ch1, 0);  // both low -> high-Z, wheels free to turn
      ledcWrite(ch2, 0);
    }
    return;
  }

  const int32_t mag = cmd > 0 ? cmd : -cmd;
  int32_t duty = (mag * (int32_t)MOTOR_PWM_MAX) / (int32_t)MOTOR_SCALE;
  if (duty > MOTOR_PWM_MAX) duty = MOTOR_PWM_MAX;
  if (duty < 0) duty = 0;

  // Which pin leads depends only on the sign - forward and reverse are exact
  // mirrors of each other.
  const uint8_t lead = (cmd > 0) ? ch1 : ch2;
  const uint8_t follow = (cmd > 0) ? ch2 : ch1;

  if (s_slow_decay) {
    // Hold the leading pin high and modulate the other with the INVERTED duty.
    // The off phase then has both pins high, i.e. short brake, so current keeps
    // circulating and low-speed torque stays linear.
    //
    // The inversion is the easiest bug to introduce in this whole project: get
    // it backwards and the motor runs fastest at zero throttle.
    ledcWrite(lead, MOTOR_PWM_MAX);
    ledcWrite(follow, MOTOR_PWM_MAX - duty);
  } else {
    // Modulate the leading pin, hold the other low. The off phase is high-Z,
    // so the motor coasts between pulses.
    ledcWrite(lead, duty);
    ledcWrite(follow, 0);
  }
}

// ---------------------------------------------------------------------------
//  Command conditioning
// ---------------------------------------------------------------------------

static int16_t clamp_scale(int32_t v) {
  if (v > MOTOR_SCALE) return MOTOR_SCALE;
  if (v < -MOTOR_SCALE) return -MOTOR_SCALE;
  return (int16_t)v;
}

// Remaps a non-zero command from (MOTOR_DEADBAND, MOTOR_SCALE] onto
// [min_move, MOTOR_SCALE], so the gentlest slider movement produces motion
// rather than a stall whine.
//
// min_move must be measured with the same MOTOR_MAX_DUTY in place as it will
// run with, or the two numbers are not self-consistent.
static int16_t apply_min_move(int16_t cmd, int16_t min_move) {
  if (cmd == 0) return 0;
  if (min_move <= 0) return cmd;

  const int32_t mag = cmd > 0 ? cmd : -cmd;
  const int32_t in_span = MOTOR_SCALE - MOTOR_DEADBAND;
  if (in_span <= 0) return cmd;

  const int32_t out_span = MOTOR_SCALE - min_move;
  int32_t mapped = min_move + ((mag - MOTOR_DEADBAND) * out_span) / in_span;
  if (mapped > MOTOR_SCALE) mapped = MOTOR_SCALE;
  if (mapped < 0) mapped = 0;

  return (int16_t)(cmd > 0 ? mapped : -mapped);
}

// Applies the duty ceiling. Every path to the hardware goes through here, so
// the limit cannot be bypassed by a caller. The ESP32 has a single-precision
// FPU, so the float multiply is cheap and avoids fixed-point rounding games.
static int16_t apply_ceiling(int16_t cmd) {
  return (int16_t)((float)cmd * MOTOR_MAX_DUTY);
}

// Asymmetric slew. Growing the magnitude is what draws inrush current and
// browns out the ESP32, so that is rate-limited hard. Shrinking it - including
// crossing zero, where the first job is to get to zero - runs much faster,
// because with spring-to-centre sliders deceleration is the common case and has
// to feel immediate.
static int16_t slew_toward(int16_t applied, int16_t target) {
  if (applied == target) return applied;

  const int32_t a = applied < 0 ? -applied : applied;
  const int32_t t = target < 0 ? -target : target;
  const bool same_sign = (applied >= 0) == (target >= 0);
  const bool magnitude_rising = same_sign && (t > a);

  const int32_t rate = magnitude_rising ? MOTOR_SLEW_UP_PER_TICK : MOTOR_SLEW_DOWN_PER_TICK;

  if (target > applied) {
    const int32_t next = (int32_t)applied + rate;
    return next > target ? target : (int16_t)next;
  }
  const int32_t next = (int32_t)applied - rate;
  return next < target ? target : (int16_t)next;
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

void motors_begin() {
  const uint8_t channels[4] = {LEDC_CH_AIN1, LEDC_CH_AIN2, LEDC_CH_BIN1, LEDC_CH_BIN2};
  const uint8_t pins[4] = {PIN_AIN1, PIN_AIN2, PIN_BIN1, PIN_BIN2};

  // FIRST, before anything else: force all four inputs low. Until this runs the
  // pins are inputs holding the ESP32's reset defaults - GPIO13/14/15 weakly
  // high, GPIO2 weakly low - and a mismatched pair reads as a drive command.
  // Nothing here allocates, logs or waits, so the unsafe window closes in
  // microseconds rather than after the serial banner.
  for (int i = 0; i < 4; i++) {
    pinMode(pins[i], OUTPUT);
    digitalWrite(pins[i], LOW);
  }

  for (int i = 0; i < 4; i++) {
    ledcSetup(channels[i], MOTOR_PWM_FREQ_HZ, MOTOR_PWM_BITS);
    // Park at zero before the pin is handed to LEDC, so attaching cannot
    // produce a momentary kick.
    ledcWrite(channels[i], 0);
    ledcAttachPin(pins[i], channels[i]);
  }

  s_target_l = s_target_r = 0;
  s_applied_l = s_applied_r = 0;
  s_failsafe = true;
  s_last_cmd_ms = 0;

  drive_pair(LEDC_CH_AIN1, LEDC_CH_AIN2, 0);
  drive_pair(LEDC_CH_BIN1, LEDC_CH_BIN2, 0);
}

void motors_set(int16_t left, int16_t right) {
  int16_t l = clamp_scale(left);
  int16_t r = clamp_scale(right);

  if (MOTOR_INVERT_L) l = (int16_t)-l;
  if (MOTOR_INVERT_R) r = (int16_t)-r;

  // A thumb resting near the middle of a slider must mean stop, not a stall
  // whine. This is what makes "centre = stop" a band rather than a pixel.
  if (l > -MOTOR_DEADBAND && l < MOTOR_DEADBAND) l = 0;
  if (r > -MOTOR_DEADBAND && r < MOTOR_DEADBAND) r = 0;

  portENTER_CRITICAL(&s_mux);
  s_target_l = l;
  s_target_r = r;
  s_last_cmd_ms = millis();
  s_failsafe = false;
  portEXIT_CRITICAL(&s_mux);
}

void motors_stop(bool brake) {
  portENTER_CRITICAL(&s_mux);
  s_target_l = 0;
  s_target_r = 0;
  s_applied_l = 0;
  s_applied_r = 0;
  portEXIT_CRITICAL(&s_mux);

  // Deliberately immediate rather than slew-limited: this is the panic path.
  if (brake) {
    ledcWrite(LEDC_CH_AIN1, MOTOR_PWM_MAX);
    ledcWrite(LEDC_CH_AIN2, MOTOR_PWM_MAX);
    ledcWrite(LEDC_CH_BIN1, MOTOR_PWM_MAX);
    ledcWrite(LEDC_CH_BIN2, MOTOR_PWM_MAX);
  } else {
    ledcWrite(LEDC_CH_AIN1, 0);
    ledcWrite(LEDC_CH_AIN2, 0);
    ledcWrite(LEDC_CH_BIN1, 0);
    ledcWrite(LEDC_CH_BIN2, 0);
  }
}

void motors_tick() {
  int16_t target_l, target_r;
  uint32_t last_cmd;

  portENTER_CRITICAL(&s_mux);
  target_l = s_target_l;
  target_r = s_target_r;
  last_cmd = s_last_cmd_ms;
  portEXIT_CRITICAL(&s_mux);

  // Failsafe. The backstop that trusts nothing: not the browser, not the WiFi
  // link, not the phone staying awake. Zeroing the target rather than slamming
  // the output means the stop is still slew-limited, which takes about 100 ms
  // on top of the detection window.
  if (last_cmd == 0 || (millis() - last_cmd) > CMD_TIMEOUT_MS) {
    if (!s_failsafe) {
      s_failsafe = true;
      Serial.println(F("[mot] FAILSAFE - no command within timeout, stopping"));
    }
    target_l = 0;
    target_r = 0;
  }

  s_applied_l = slew_toward(s_applied_l, target_l);
  s_applied_r = slew_toward(s_applied_r, target_r);

  const int16_t out_l = apply_ceiling(apply_min_move(
      s_applied_l, s_applied_l > 0 ? MOTOR_MIN_MOVE_L_FWD : MOTOR_MIN_MOVE_L_REV));
  const int16_t out_r = apply_ceiling(apply_min_move(
      s_applied_r, s_applied_r > 0 ? MOTOR_MIN_MOVE_R_FWD : MOTOR_MIN_MOVE_R_REV));

  drive_pair(LEDC_CH_AIN1, LEDC_CH_AIN2, out_l);
  drive_pair(LEDC_CH_BIN1, LEDC_CH_BIN2, out_r);
}

int16_t motors_target_left() { return s_target_l; }
int16_t motors_target_right() { return s_target_r; }
int16_t motors_applied_left() { return s_applied_l; }
int16_t motors_applied_right() { return s_applied_r; }
bool motors_failsafe_active() { return s_failsafe; }

void motors_set_slow_decay(bool on) {
  s_slow_decay = on;
  // Re-assert the outputs so the change takes effect on the next tick with the
  // correct polarity rather than leaving a stale duty on the follow pin.
  s_applied_l = 0;
  s_applied_r = 0;
  drive_pair(LEDC_CH_AIN1, LEDC_CH_AIN2, 0);
  drive_pair(LEDC_CH_BIN1, LEDC_CH_BIN2, 0);
}

bool motors_slow_decay() { return s_slow_decay; }
