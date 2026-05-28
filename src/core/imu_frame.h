#ifndef IMU_FRAME_H
#define IMU_FRAME_H

/* ============================================================================
 * joypad-os canonical IMU frame
 *
 * This header defines the single, canonical motion frame that all INPUT
 * drivers normalize their controller's IMU into, and that all OUTPUT drivers
 * consume from. It is the contract between input and output: input drivers
 * convert from their controller's native frame into this one; output drivers
 * convert from this one into their target controller's frame.
 *
 * The fields below (event->accel[3], event->gyro[3], event->gyro_range,
 * event->accel_range, event->has_motion) live on input_event_t. This header
 * documents what they MEAN — the axes, units, and conventions.
 *
 *
 * AXES (right-handed; controller held in front of you, face toward you)
 *
 *     +Y (up, out of face)
 *      |
 *      |
 *      +------ +X (right)
 *     /
 *    /
 *  +Z (toward user / closer)
 *
 *   +X = right
 *   +Y = up (out of the face/touchpad/buttons)
 *   +Z = toward the user (closer)
 *
 * This matches the SDL / SInput sensor-frame convention, which is the
 * de-facto standard for game-controller IMUs on PC.
 *
 *
 * ACCELEROMETER
 *
 *   event->accel[0]  = X (right)     int16, signed
 *   event->accel[1]  = Y (up)        int16, signed
 *   event->accel[2]  = Z (toward user) int16, signed
 *
 *   Includes gravity. A device at rest face-up reads +1g on +Y.
 *
 *   Scale is implicit in event->accel_range:
 *     event->accel_range is the full-scale magnitude in milli-g.
 *     int16 value 32767  ==  +accel_range mg
 *     int16 value -32768 == -accel_range mg
 *   e.g., accel_range = 4000 means +/-4g, so each LSB = 4000/32768 mg.
 *
 *
 * GYROSCOPE
 *
 *   event->gyro[0]  = pitch (rotation about +X) int16, signed
 *   event->gyro[1]  = yaw   (rotation about +Y) int16, signed
 *   event->gyro[2]  = roll  (rotation about +Z) int16, signed
 *
 *   Positive rotation = counter-clockwise when viewed from the positive end
 *   of the rotation axis (right-hand rule).
 *
 *   Scale is implicit in event->gyro_range:
 *     event->gyro_range is the full-scale magnitude in degrees/second.
 *     int16 value 32767  ==  +gyro_range dps
 *     int16 value -32768 == -gyro_range dps
 *   e.g., gyro_range = 2048 means +/-2048 dps, so each LSB = 2048/32768 dps.
 *
 *
 * VALIDITY
 *
 *   event->has_motion must be set to true by the input driver when accel[],
 *   gyro[], accel_range, and gyro_range all contain meaningful current data.
 *   Output drivers must NOT consume motion fields when has_motion is false.
 *
 *
 * DECLARED RANGES vs CALIBRATION
 *
 *   The declared range is the controller's nominal full-scale, taken from
 *   manufacturer documentation. The raw int16 values, interpreted via that
 *   range, give physical units (dps, mg) accurate to the controller's
 *   manufacturing tolerance — typically within ~1% of the nominal sensitivity.
 *
 *   Per-device factory calibration (where supported by the controller, e.g.
 *   DualSense feature report 0x05, Switch Pro SPI 0x6020) corrects for the
 *   per-unit deviation from nominal AND for resting bias. When applied, the
 *   raw int16 values become accurate to the per-device calibration. The
 *   declared range remains the same (calibration is normalization to the
 *   nominal scale, not a change in range).
 *
 *   Input drivers SHOULD apply per-device calibration when the controller's
 *   calibration data is accessible. When it is not (yet) available, drivers
 *   use the documented nominal ranges. See each driver for status.
 *
 *
 * KNOWN PER-CONTROLLER VALUES (for input drivers normalizing INTO this frame)
 *
 *   DualSense (DS5):
 *     accel_range = 4000  (+/-4g,    nominal 8192 LSB/g)
 *     gyro_range  = 2048  (+/-2048 dps, nominal 1024 LSB/dps)
 *     Native frame == canonical (no axis transform).
 *     Per-device calibration: feature report 0x05 (pending receive-path work).
 *
 *   DualShock 4 (DS4):
 *     accel_range = 4000  (+/-4g)
 *     gyro_range  = 2048  (+/-2048 dps)
 *     (verify axes against canonical; transform if needed)
 *
 *   DualShock 3 (DS3):
 *     accel_range = 2000  (+/-2g)
 *     gyro_range  = 100   (single-axis ~100 dps)
 *     (legacy hardware; lower precision)
 *
 *   Switch Pro (1st gen):
 *     accel_range = 8000  (+/-8g, 4096 LSB/g)
 *     gyro_range  = 2000  (+/-2000 dps)
 *     Native frame: +X = toward top edge, +Y = toward left edge, +Z = up.
 *     Transform INTO canonical (both accel & gyro):
 *       canon_X(right)       = -native_Y
 *       canon_Y(up)          = +native_Z
 *       canon_Z(toward user) = -native_X
 *
 *
 * OUTPUT DRIVER CONTRACT
 *
 *   Output drivers convert FROM canonical to their target. For example:
 *     - sinput: consumes canonical directly (sinput IS the canonical frame);
 *               no axis transform, declared ranges pass through.
 *     - Switch Pro USB output: inverse of the Switch Pro INPUT transform.
 *     - PS3/PS4/PS5 USB output: per-target axis transform + scale to target
 *       LSB/unit using accel_range and gyro_range.
 *
 *
 * INVARIANTS
 *
 *   1. Input drivers MUST set accel_range and gyro_range whenever they set
 *      has_motion=true. The default values in init_input_event are a fallback
 *      only; every driver that emits motion declares its actual range.
 *
 *   2. Output drivers MUST use event->accel_range and event->gyro_range to
 *      interpret the int16 values. Hard-coded assumptions about LSB-per-unit
 *      are forbidden.
 *
 *   3. The canonical frame is fixed (this header). Drivers do not "negotiate"
 *      a frame. The contract is one direction: input normalizes IN, output
 *      converts OUT.
 * ============================================================================
 */

/* Canonical axis indices, for code clarity at call sites. */
#define IMU_AXIS_X        0  /* right */
#define IMU_AXIS_Y        1  /* up */
#define IMU_AXIS_Z        2  /* toward user */

#define IMU_GYRO_PITCH    0  /* rotation about +X */
#define IMU_GYRO_YAW      1  /* rotation about +Y */
#define IMU_GYRO_ROLL     2  /* rotation about +Z */

#endif /* IMU_FRAME_H */
