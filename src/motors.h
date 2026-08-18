#pragma once

#include <stdbool.h>
#include <stdint.h>

// ===========================================================================
//  TB6612FNG dual motor control, 4-pin scheme.
//
//  PWMA, PWMB and STBY are tied to 3.3V on the vehicle; the PWM lives on the
//  four direction pins. See config.h for the pin map and the reasoning.
//
//  Everything here is SIGNED end to end:
//
//      +MOTOR_SCALE  full throttle forward
//                 0  stop
//      -MOTOR_SCALE  full throttle reverse
//
//  The sign alone selects direction. Nothing carries a separate direction flag,
//  so a direction bit can never disagree with a magnitude.
// ===========================================================================

// Forces all four driver inputs low, then configures the four LEDC channels and
// leaves both sides coasting.
//
// Call this as the FIRST statement in setup(), before Serial or anything else.
// Until it runs, the ESP32's reset defaults hold GPIO13/14/15 weakly high and
// GPIO2 weakly low, and any mismatched pair reads to the TB6612 as a drive
// command. Every millisecond spent before this call is a millisecond a motor
// may be running at full battery voltage. It deliberately prints nothing, so
// that parking the pins never waits on Serial.
void motors_begin();

// Sets the desired throttle per side. Clamped, inverted per MOTOR_INVERT_*,
// and deadbanded. Does NOT touch the hardware - motors_tick() does that, so
// there is exactly one writer to the PWM registers.
//
// Also refreshes the failsafe timer: this is what tells the vehicle someone is
// still at the controls.
void motors_set(int16_t left, int16_t right);

// Immediate stop, bypassing the slew limiter. For every disconnect path.
// brake=true energises both low side FETs (short brake); brake=false releases
// both sides (coast).
void motors_stop(bool brake);

// Call at MOTOR_TICK_HZ. Applies the asymmetric slew limit, the per-side
// calibration and the duty ceiling, then writes the PWM registers. Also
// enforces the CMD_TIMEOUT_MS failsafe.
void motors_tick();

// Spawns the task that calls motors_tick() at MOTOR_TICK_HZ, above the HTTP
// server's priority. Call once, after motors_begin() and after Serial is up.
//
// Its own task rather than loop(): the failsafe is only as good as the tick
// that enforces it, and loop() sits below the streaming task. A stall there
// would leave the last throttle latched on the motors for the duration.
void motors_start_task();

// --- introspection, for telemetry ------------------------------------------

int16_t motors_target_left();
int16_t motors_target_right();
int16_t motors_applied_left();   // after slew limiting
int16_t motors_applied_right();

// True while the failsafe is holding the motors stopped because no command has
// arrived within CMD_TIMEOUT_MS.
bool motors_failsafe_active();

// Slow decay (default) recirculates current through the low-side FETs: far more
// linear torque per unit duty, and usable crawl speed. Coast/fast decay leaves
// the bridge high-impedance during the off phase. Switchable so the two can be
// compared on the bench.
void motors_set_slow_decay(bool on);
bool motors_slow_decay();
