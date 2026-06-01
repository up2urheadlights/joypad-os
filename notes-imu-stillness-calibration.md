# IMU Calibration via Stillness Detection

Captured insight (to fold into a future IMU calibration branch).

## The idea

Steam's controller calibration UI doesn't ask the user to hold the controller
still — it just waits for the gyro/accel readings to be steady, then takes
that as the "at rest" baseline. The user can be moving the controller when
the calibration starts; the dongle/firmware just waits patiently and
calibrates whenever the controller is set down and stops moving.

We should adopt the same pattern for joypad-os's IMU calibration. Whenever a
motion-capable controller is paired and reporting, the firmware continuously
watches for a stillness window. When detected, it captures the current gyro
readings as the bias and applies that bias to all subsequent samples.

## Why this matters

Previously we'd been thinking calibration was a one-shot thing tied to a
specific moment ("at pairing time" or "via factory calibration data
retrieval"), each with its own problems:

- **At pairing time:** the controller is likely being held / moved during
  pairing. Captures bad bias.
- **Via factory data (DS5 0x05 feature report):** requires a feature-receive
  infrastructure that's blocked by joypad-os's current BT-stack architecture
  (hid_host owns the HID PSMs; we can't intercept feature responses without
  a stack restructure). Documented as future work in the IMU PR.

**Stillness detection sidesteps both.** No specific moment is required —
calibration happens organically the first time the user sets the controller
down. It also keeps working: if the user power-cycles the controller, or if
the sensor's bias drifts with temperature, the next stillness window
re-calibrates. It's robust to all the things that make a one-shot calibration
fragile.

## How it would work, roughly

1. **Maintain a rolling window of recent gyro samples** (e.g., last 500 ms
   to 1 s at the controller's sample rate — for DS5 at ~250 Hz that's
   125-250 samples).
2. **Compute the standard deviation** or peak-to-peak range of each axis
   over the window.
3. **Stillness threshold:** when the standard deviation is below some small
   threshold (a few LSBs, e.g., < 3 LSB per axis at DS5's scale) for the
   full window, consider the controller still.
4. **On detection:** capture the mean of the window as the gyro bias.
   Apply the bias correction subtractively to all subsequent gyro samples.
5. **Continuous refinement:** keep watching for stillness. Each new
   stillness window updates the bias. Drift / temperature changes are
   handled automatically.

Optionally for accel: detect when the gravity vector is stable (magnitude
near 1g and direction unchanging) and use that to characterize sensor
sensitivity, though this is much less important than gyro bias correction
since accelerometers don't drift the way gyros do.

## What this depends on / unblocks

- **Depends on:** canonical-frame contract being established (commit ca8a55f
  on feature/imu-normalization, already landed). The bias-correction layer
  applies to canonical-frame int16 values before they reach output drivers.
- **Unblocks:** dropping the "DS5 feature report 0x05 calibration" line of
  investigation entirely. No need to wrangle BT-stack feature-receive
  infrastructure for calibration purposes. (The 0x05 path may still be
  worth pursuing eventually for sub-1% accuracy in high-precision
  applications, but for game use cases stillness detection is sufficient.)
- **Compatible with:** any motion-capable controller, not just DS5. DS3,
  DS4, Switch Pro 1, future Switch 2 — all benefit from the same
  stillness-detection layer. Hardware-agnostic.

## Where it fits architecturally

Probably as a layer between the input driver and the canonical-frame event:

- Input driver reads native IMU bytes, transforms to canonical frame,
  declares ranges.
- **New: calibration layer** applies stillness detection, captures bias on
  detection, subtracts bias from gyro values.
- Calibrated event reaches router and output drivers.

The calibration layer could live in `src/core/imu_calibration.c` (new file)
or as helpers in `src/core/imu_frame.h` (where the contract is documented).
Per-device state — one calibration context per (dev_addr, instance) tuple,
keyed off the input event. State persists across the session but doesn't
need flash persistence — re-calibration is fast (well under a second of
stillness usually).

## Suggested future branch

`feature/imu-stillness-calibration` — once IMU PR lands and we have a real
need for calibrated values reaching outputs.

## Provenance

Insight from user (Tommy Duffy / Duffy Land Development Engineering) during
joypad-os/feature-sinput-dynamic-capabilities work session, Jun 2026. Based
on observed Steam controller calibration UI behavior.
