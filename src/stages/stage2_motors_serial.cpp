// ===========================================================================
//  Stage 2 - motors driven from serial. No camera, no WiFi.
//
//  *** WHEELS OFF THE GROUND. Current-limit the bench supply to just above the
//  *** free-run current you measured in Stage 0.
//
//  The point of this stage is to prove all four side x direction combinations
//  independently, before any web UI exists to obscure a wiring fault.
//
//  Keys (single keypress, no Enter needed):
//
//    w / s     both tracks forward / reverse at the preset throttle
//    a / d     pivot left / right  (tracks opposed - the bidirectional check
//              that matters most for a tank)
//    x, space  stop
//    + / -     adjust the preset throttle by 100
//    m         toggle slow-decay vs coast, to feel the low-speed difference
//    p         print state
//    k         kill the pilot: stop re-asserting commands, so you can watch the
//              CMD_TIMEOUT_MS failsafe fire. Any other key re-arms it.
//    1 / 2     sweep LEFT track forward / reverse
//    3 / 4     sweep RIGHT track forward / reverse
//    ?         help
//
//  Typed commands (need Enter):
//
//    L<signed> exact left throttle,  e.g.  L-800
//    R<signed> exact right throttle, e.g.  R450
//
//  The sweeps are how you get the four MOTOR_MIN_MOVE_* numbers for config.h.
//  Each ramps one track in one direction from zero, printing the command value.
//  Watch the wheel and note the value at which it FIRST starts turning; that is
//  the figure to record. Forward and reverse differ per side, so all four
//  matter - a single per-side number makes the vehicle pull to one side in
//  reverse. Press any key to abort a sweep.
//
//  Also re-run Stage 1a afterwards to confirm the camera still initialises with
//  the driver wired. With GPIO16 unconnected, PSRAM should stay clean.
// ===========================================================================

#include <Arduino.h>

#include "../board.h"
#include "../motors.h"
#include "config.h"

// What the "pilot" is currently asking for. Re-asserted every tick, because
// motors_set() is also what feeds the failsafe timer: a control channel that
// goes quiet must look like a control channel that goes quiet.
static int16_t g_want_l = 0;
static int16_t g_want_r = 0;
static int16_t g_preset = 500;
static bool g_pilot_alive = true;

// Sweep state. 0 = idle, otherwise 1..4 matching the keys.
static int g_sweep = 0;
static int16_t g_sweep_value = 0;
static uint32_t g_sweep_last_ms = 0;

static const int16_t kSweepStep = 10;
static const uint32_t kSweepIntervalMs = 200;
static const int16_t kSweepMax = MOTOR_SCALE;

static char g_line[16];
static uint8_t g_line_len = 0;

static void print_help() {
  Serial.println();
  Serial.println(F("  w/s both fwd/rev   a/d pivot L/R   x or space stop"));
  Serial.println(F("  +/- preset +-100   m decay mode    p state    ? help"));
  Serial.println(F("  k   kill pilot (watch the failsafe fire)"));
  Serial.println(F("  1/2 sweep LEFT fwd/rev    3/4 sweep RIGHT fwd/rev"));
  Serial.println(F("  L<signed> / R<signed> then Enter, e.g. L-800"));
  Serial.println();
}

static void print_state() {
  Serial.printf("target %+5d/%+5d | applied %+5d/%+5d | preset %d | %s decay | %s%s\n",
                motors_target_left(), motors_target_right(), motors_applied_left(),
                motors_applied_right(), g_preset,
                motors_slow_decay() ? "slow" : "coast",
                motors_failsafe_active() ? "FAILSAFE" : "armed",
                g_pilot_alive ? "" : " (pilot killed)");
}

static void stop_sweep() {
  if (g_sweep != 0) {
    Serial.printf("[sweep] aborted at %+d\n", g_sweep_value);
    g_sweep = 0;
    g_sweep_value = 0;
    g_want_l = 0;
    g_want_r = 0;
  }
}

static void start_sweep(int which) {
  g_sweep = which;
  g_sweep_value = 0;
  g_sweep_last_ms = millis();
  g_want_l = 0;
  g_want_r = 0;

  const char *side = (which <= 2) ? "LEFT" : "RIGHT";
  const char *dir = (which == 1 || which == 3) ? "FORWARD" : "REVERSE";
  Serial.println();
  Serial.printf("[sweep] %s %s - note the value when the wheel FIRST moves.\n", side, dir);
  Serial.printf("[sweep] that number goes in MOTOR_MIN_MOVE_%c_%s in config.h\n",
                (which <= 2) ? 'L' : 'R', (which == 1 || which == 3) ? "FWD" : "REV");
  Serial.println(F("[sweep] any key aborts."));
}

static void tick_sweep() {
  if (g_sweep == 0) return;

  const uint32_t now = millis();
  if (now - g_sweep_last_ms < kSweepIntervalMs) return;
  g_sweep_last_ms = now;

  g_sweep_value += kSweepStep;
  if (g_sweep_value > kSweepMax) {
    Serial.println(F("[sweep] reached full scale without movement - check the wiring,"));
    Serial.println(F("[sweep] the VM supply, and that STBY is actually tied high."));
    g_sweep = 0;
    g_sweep_value = 0;
    g_want_l = g_want_r = 0;
    return;
  }

  const int16_t signed_value =
      (g_sweep == 1 || g_sweep == 3) ? g_sweep_value : (int16_t)-g_sweep_value;

  if (g_sweep <= 2) {
    g_want_l = signed_value;
    g_want_r = 0;
  } else {
    g_want_l = 0;
    g_want_r = signed_value;
  }

  Serial.printf("[sweep] %+d\n", signed_value);
}

// Returns true if the character was a complete no-argument command.
static bool handle_key(char c) {
  switch (c) {
    case 'w': g_want_l = g_preset;  g_want_r = g_preset;  break;
    case 's': g_want_l = (int16_t)-g_preset; g_want_r = (int16_t)-g_preset; break;
    case 'a': g_want_l = (int16_t)-g_preset; g_want_r = g_preset; break;
    case 'd': g_want_l = g_preset;  g_want_r = (int16_t)-g_preset; break;

    case 'x':
    case ' ':
      g_want_l = 0;
      g_want_r = 0;
      // Immediate, bypassing the slew limiter - this is the panic key.
      motors_stop(false);
      Serial.println(F("stop"));
      break;

    case '+':
      g_preset += 100;
      if (g_preset > MOTOR_SCALE) g_preset = MOTOR_SCALE;
      Serial.printf("preset %d\n", g_preset);
      break;

    case '-':
      g_preset -= 100;
      if (g_preset < 0) g_preset = 0;
      Serial.printf("preset %d\n", g_preset);
      break;

    case 'm':
      motors_set_slow_decay(!motors_slow_decay());
      g_want_l = 0;
      g_want_r = 0;
      Serial.printf("decay mode: %s\n", motors_slow_decay() ? "slow (brake)" : "coast (high-Z)");
      break;

    case 'p': print_state(); break;
    case '?': print_help(); break;

    case 'k':
      g_pilot_alive = false;
      Serial.println(F("pilot killed - motors_set() will stop being called."));
      Serial.printf("expect FAILSAFE within %d ms, then a slew-limited stop.\n", CMD_TIMEOUT_MS);
      return true;

    case '1': start_sweep(1); return true;
    case '2': start_sweep(2); return true;
    case '3': start_sweep(3); return true;
    case '4': start_sweep(4); return true;

    default: return false;
  }
  return true;
}

static void process_line() {
  g_line[g_line_len] = '\0';
  if (g_line_len == 0) return;

  const char kind = g_line[0];
  if (kind == 'L' || kind == 'l' || kind == 'R' || kind == 'r') {
    const long v = atol(&g_line[1]);
    const int16_t clamped =
        (int16_t)(v > MOTOR_SCALE ? MOTOR_SCALE : (v < -MOTOR_SCALE ? -MOTOR_SCALE : v));
    if (kind == 'L' || kind == 'l') {
      g_want_l = clamped;
    } else {
      g_want_r = clamped;
    }
    Serial.printf("%c = %+d\n", (kind == 'L' || kind == 'l') ? 'L' : 'R', clamped);
  } else {
    Serial.printf("unrecognised: \"%s\"  (? for help)\n", g_line);
  }
}

static void poll_serial() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();

    // Any keypress aborts a running sweep, so you can stop the moment the wheel
    // twitches rather than fumbling for a specific key.
    if (g_sweep != 0) {
      stop_sweep();
      if (c == '\r' || c == '\n') continue;
      // fall through and let the key act normally too
    }

    if (c != '\r' && c != '\n' && !g_pilot_alive) {
      g_pilot_alive = true;
      Serial.println(F("pilot re-armed"));
    }

    // Single-key commands act immediately, so driving on the bench does not need
    // an Enter after every keystroke. L/R fall through to the line buffer.
    if (g_line_len == 0 && handle_key(c)) continue;

    if (c == '\r' || c == '\n') {
      process_line();
      g_line_len = 0;
      continue;
    }

    if (g_line_len < sizeof(g_line) - 1) {
      g_line[g_line_len++] = c;
    } else {
      g_line_len = 0;  // overlong garbage, start over
    }
  }
}

void setup() {
  board_begin("Stage 2: motors via serial");
  board_led_begin();

  Serial.println(F("*** WHEELS OFF THE GROUND ***"));
  Serial.println(F("Current-limit the supply to just above the measured free-run current."));
  Serial.println();

  motors_begin();

  if (MOTOR_MIN_MOVE_L_FWD == 0 && MOTOR_MIN_MOVE_R_FWD == 0) {
    Serial.println(F("[mot] MOTOR_MIN_MOVE_* are all zero - run the 1/2/3/4 sweeps"));
    Serial.println(F("[mot] and record the four values into config.h."));
  }

  print_help();
}

void loop() {
  poll_serial();
  tick_sweep();

  // Fixed-rate control tick. The pilot re-asserts its command every tick so
  // that "no commands arriving" genuinely means the control link is gone.
  static uint32_t last_tick = 0;
  const uint32_t tick_period = 1000 / MOTOR_TICK_HZ;
  const uint32_t now = millis();
  if (now - last_tick >= tick_period) {
    last_tick = now;

    if (g_pilot_alive) motors_set(g_want_l, g_want_r);
    motors_tick();
  }

  // Fast blink whenever the tracks are actually being driven.
  const bool moving = motors_applied_left() != 0 || motors_applied_right() != 0;
  board_led_heartbeat(moving ? 200 : 1500);

  static uint32_t last_report = 0;
  if (now - last_report >= 1000) {
    last_report = now;
    if (moving || motors_target_left() || motors_target_right()) print_state();
  }
}
