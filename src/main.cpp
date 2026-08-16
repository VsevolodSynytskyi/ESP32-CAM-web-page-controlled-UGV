// ===========================================================================
//  ESP32-CAM web page controlled UGV
//
//  CURRENT SCOPE: motor wiring and control verification.
//
//  Runs a four-phase pattern on a loop so the wheels can be watched directly:
//
//      1/4   both forward           5 s
//      2/4   both backward          5 s
//      3/4   A forward, B backward  5 s   (pivot one way)
//      4/4   A backward, B forward  5 s   (pivot the other way)
//            ... repeat forever
//
//  with a short stop between phases so the transitions are unmistakable.
//
//  *** WHEELS OFF THE GROUND ***
//
//  VM can stay connected permanently, including during uploads. The motor pins
//  are paired by their boot-default levels so that each channel reads as a stop
//  before firmware runs - see the note above PIN_AIN1 in config.h.
//
//  Keys:
//      SPACE          pause / resume  (pausing stops the motors immediately)
//      + / -          throttle by 100
//      r              restart the sequence at phase 1
//      any other key  emergency stop
// ===========================================================================

#include <Arduino.h>

#include "board.h"
#include "config.h"
#include "motors.h"

// High enough to clearly turn an unmeasured gearmotor, low enough to be gentle.
// MOTOR_MAX_DUTY in config.h still applies on top of this.
#define START_THROTTLE 700

#define DRIVE_MS 5000
#define PAUSE_MS 1500
#define ARM_DELAY_MS 5000

struct Phase {
  const char *label;
  const char *expect;
  int8_t l;  // -1, 0 or +1, multiplied by the throttle
  int8_t r;
  uint32_t ms;
};

static const Phase kPhases[] = {
    {"1/4  BOTH FORWARD", "both wheels turning the SAME way", +1, +1, DRIVE_MS},
    {"     stop", nullptr, 0, 0, PAUSE_MS},
    {"2/4  BOTH BACKWARD", "both reversed, still matching each other", -1, -1, DRIVE_MS},
    {"     stop", nullptr, 0, 0, PAUSE_MS},
    {"3/4  PIVOT  A fwd / B back", "wheels turning OPPOSITE ways", +1, -1, DRIVE_MS},
    {"     stop", nullptr, 0, 0, PAUSE_MS},
    {"4/4  PIVOT  A back / B fwd", "opposite again, both the other way round", -1, +1, DRIVE_MS},
    {"     stop", nullptr, 0, 0, PAUSE_MS},
};
static const int kPhaseCount = sizeof(kPhases) / sizeof(kPhases[0]);

static bool g_running = false;  // set true once the arming countdown elapses
static bool g_armed = false;
static uint32_t g_boot_ms = 0;
static int g_phase = 0;
static uint32_t g_phase_start = 0;
static int16_t g_throttle = START_THROTTLE;

static void announce() {
  const Phase &p = kPhases[g_phase];
  if (p.expect != nullptr) {
    Serial.printf("\n[%s]  A=%+5d  B=%+5d\n", p.label, p.l * g_throttle, p.r * g_throttle);
    Serial.printf("      expect: %s\n", p.expect);
  } else {
    Serial.println(F("[     stop]"));
  }
}

static void enter_phase(int index) {
  g_phase = index % kPhaseCount;
  g_phase_start = millis();
  announce();
}

static void pause_motors(const char *why) {
  g_running = false;
  motors_stop(false);  // immediate, bypasses the slew limiter
  Serial.printf("\n*** %s *** press SPACE to resume\n", why);
}

static void poll_serial() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') continue;

    if (c == ' ') {
      if (g_running) {
        pause_motors("PAUSED");
      } else {
        g_running = true;
        g_armed = true;
        g_phase_start = millis();
        Serial.println(F("\n*** RUNNING ***"));
        announce();
      }
      continue;
    }

    if (c == '+') {
      g_throttle += 100;
      if (g_throttle > MOTOR_SCALE) g_throttle = MOTOR_SCALE;
      Serial.printf("throttle %d\n", g_throttle);
      continue;
    }
    if (c == '-') {
      g_throttle -= 100;
      if (g_throttle < 0) g_throttle = 0;
      Serial.printf("throttle %d\n", g_throttle);
      continue;
    }
    if (c == 'r') {
      enter_phase(0);
      Serial.println(F("restarted at phase 1"));
      continue;
    }

    pause_motors("STOPPED");
  }
}

// Levels sampled off the driver inputs before anything reconfigures them.
static int g_boot_ain1, g_boot_ain2, g_boot_bin1, g_boot_bin2;

// Every other pin this board breaks out, so the pairing can be chosen from
// measured levels rather than datasheet defaults. GPIO13's documented pull-up
// turned out not to hold here, so nothing is assumed any more.
static const uint8_t kSurveyPins[] = {4, 13, 16};
static int g_survey[sizeof(kSurveyPins)];
static bool g_survey_unstable[sizeof(kSurveyPins)];

static const char *pair_verdict(int in1, int in2) {
  if (in1 == in2) return in1 ? "(H,H) short brake - safe" : "(L,L) coast - safe";
  return "(H,L) or (L,H)  *** DRIVE COMMAND - motor runs during boot ***";
}

void setup() {
  // Sample the reset-default levels first. Nothing has configured these pins
  // yet, so this is exactly what the TB6612 has been seeing since the core came
  // out of reset. Four register reads, a few microseconds - it does not
  // meaningfully delay motors_begin() below.
  g_boot_ain1 = digitalRead(PIN_AIN1);
  g_boot_ain2 = digitalRead(PIN_AIN2);
  g_boot_bin1 = digitalRead(PIN_BIN1);
  g_boot_bin2 = digitalRead(PIN_BIN2);

  // Survey the spare pins too. Each is sampled repeatedly: a pin that keeps
  // changing is being driven by something else, which for GPIO16 would be
  // direct evidence that it really is the PSRAM chip-select.
  for (size_t i = 0; i < sizeof(kSurveyPins); i++) {
    const int first = digitalRead(kSurveyPins[i]);
    bool changed = false;
    for (int n = 0; n < 64; n++) {
      if (digitalRead(kSurveyPins[i]) != first) changed = true;
    }
    g_survey[i] = first;
    g_survey_unstable[i] = changed;
  }

  // Absolutely first after that, before Serial or anything else. Until this
  // runs, the pins sit at their reset defaults and a mismatched pair reads to
  // the TB6612 as a drive command. Every millisecond before this call is a
  // millisecond a track may be running at full battery voltage.
  motors_begin();

  board_begin("motor wiring check");
  board_led_begin();
  motors_log_config();

  // What the driver actually saw during the boot window. This is measured, not
  // assumed - the ESP32's documented reset defaults can be overridden by
  // anything else on the pin, and GPIO2/12/13/14 are also the SD_MMC lines, so
  // an inserted microSD card loads them.
  Serial.println();
  Serial.println(F("--- pin levels at reset, before firmware touched them ---"));
  Serial.printf("  A: AIN1 GPIO%-2d = %d   AIN2 GPIO%-2d = %d   %s\n", PIN_AIN1, g_boot_ain1,
                PIN_AIN2, g_boot_ain2, pair_verdict(g_boot_ain1, g_boot_ain2));
  Serial.printf("  B: BIN1 GPIO%-2d = %d   BIN2 GPIO%-2d = %d   %s\n", PIN_BIN1, g_boot_bin1,
                PIN_BIN2, g_boot_bin2, pair_verdict(g_boot_bin1, g_boot_bin2));
  Serial.println(F("  (a channel only stays still if its two inputs MATCH)"));
  Serial.println();
  Serial.println(F("  spare pins, for choosing a matched pair:"));
  for (size_t i = 0; i < sizeof(kSurveyPins); i++) {
    Serial.printf("    GPIO%-2d = %d %s\n", kSurveyPins[i], g_survey[i],
                  g_survey_unstable[i] ? "  <-- CHANGING, driven by something else" : "");
  }

  Serial.println();
  Serial.println(F("*** WHEELS OFF THE GROUND ***"));
  Serial.println();
  Serial.println(F("Looping sequence:"));
  Serial.println(F("   1/4  both forward           5 s"));
  Serial.println(F("   2/4  both backward          5 s"));
  Serial.println(F("   3/4  A forward, B backward  5 s"));
  Serial.println(F("   4/4  A backward, B forward  5 s"));
  Serial.println();
  Serial.println(F("SPACE pause/resume   +/- throttle   r restart   any other key = stop"));
  Serial.printf("Throttle %d of %d, duty ceiling %.0f%%.\n", g_throttle, MOTOR_SCALE,
                MOTOR_MAX_DUTY * 100.0f);
  Serial.println();
  Serial.printf("Connect VM to the battery now. Starting in %lu s...\n", ARM_DELAY_MS / 1000);

  g_boot_ms = millis();
  g_phase = 0;
  g_phase_start = g_boot_ms;
}

void loop() {
  poll_serial();

  const uint32_t now = millis();

  // Arming countdown, so there is time to connect VM and get hands clear
  // before anything turns.
  if (!g_armed) {
    static uint32_t last_count = 0;
    const uint32_t elapsed = now - g_boot_ms;
    if (elapsed >= ARM_DELAY_MS) {
      g_armed = true;
      g_running = true;
      Serial.println(F("\n*** RUNNING ***"));
      enter_phase(0);
    } else if (now - last_count >= 1000) {
      last_count = now;
      Serial.printf("   %lu...\n", (ARM_DELAY_MS - elapsed + 999) / 1000);
    }
  }

  if (g_running && (now - g_phase_start >= kPhases[g_phase].ms)) {
    enter_phase(g_phase + 1);
  }

  // Fixed-rate control tick. motors_set() is called every tick even when the
  // command is zero: it also feeds the failsafe timer, so going quiet has to
  // mean "the controller is gone", not "the controller wants a stop".
  static uint32_t last_tick = 0;
  const uint32_t tick_period = 1000 / MOTOR_TICK_HZ;
  if (now - last_tick >= tick_period) {
    last_tick = now;

    if (g_running) {
      const Phase &p = kPhases[g_phase];
      motors_set((int16_t)(p.l * g_throttle), (int16_t)(p.r * g_throttle));
    } else {
      motors_set(0, 0);
    }
    motors_tick();
  }

  const bool moving = motors_applied_left() != 0 || motors_applied_right() != 0;
  board_led_heartbeat(moving ? 150 : 1200);

  // Countdown, so a finished phase is distinguishable from a stalled motor.
  static uint32_t last_log = 0;
  if (g_running && now - last_log >= 1000) {
    last_log = now;
    if (kPhases[g_phase].expect != nullptr) {
      const uint32_t left_ms = kPhases[g_phase].ms - (now - g_phase_start);
      Serial.printf("      %lus   applied A %+5d  B %+5d\n", (left_ms + 999) / 1000,
                    motors_applied_left(), motors_applied_right());
    }
  }

  delay(5);
}
