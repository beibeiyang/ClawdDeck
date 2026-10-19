# Voice, Clock, and Level Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a cohesive Precision Black redesign for Voice, Clock, and Level, including persistent Level sensor-plane calibration and verified gravity behavior.

**Architecture:** Keep UI construction and state binding inside each existing app. Extract Level's gravity projection and calibration accumulation into a pure C++ header for host tests, while `imu_hal` owns persisted sensor bias. Restore the existing SDL simulator as the visual iteration loop, then prove final behavior on the 480×480 hardware.

**Tech Stack:** C++17/Arduino ESP32, LVGL 9, PlatformIO, native SDL2 simulator, ESP32 Preferences/NVS, host C++ tests, pytest for daemon regression coverage.

**Spec:** `docs/superpowers/specs/2026-10-18-voice-clock-level-polish-design.md`

## Global Constraints

- Use the approved **Precision Black** direction: opaque near-black roots, restrained solid geometry, and no full-screen gradients in Voice, Clock, or Level.
- Preserve Voice capture, BLE transport, Whisper, injection, hold/release debounce, and BOOT PTT behavior.
- Preserve Clock's host-fed time/date data path and 12/24-hour setting.
- Level must use real gravity: flat=center; physical low side controls ball direction; upright=6 o'clock; opening pose is never learned as level.
- Level calibration begins only after a 1.5-second BOOT hold on a known-flat surface and persists across app exits/reboots.
- Use only existing firmware fonts, LVGL features, and dependencies.
- Support the 480×480 reference board and the existing compact breakpoint below 400 px.
- Leave no diagnostic-only commands or hooks in release source.

## Review Focus

- Voice's longest help/error/transcript strings must remain inside their fixed region at both 480×480 and 368×448.
- Level calibration cancellation or failure must preserve the last known-good bias.
- Level's gravity direction must remain correct through all four frozen display rotations.
- Clock must never show green `Synced from Mac` while BLE is disconnected or before host time is valid.
- RGB565 hardware screenshots—not desktop CSS/mockups—are the final authority for banding, clipping, contrast, and centering.

---

### Task 1: Restore the Native Visual Test Loop

**Files:**
- Modify: `firmware/src/shell/launcher.cpp:1-25`
- Modify: `.gitignore`

**Interfaces:**
- Consumes: existing `env:sim` and `SIM_AUTOSHOT_*` controls.
- Produces: a compiling `firmware/.pio/build/sim/program` capable of scripted `open <app>` screenshots.

- [ ] **Step 1: Reproduce the simulator compiler failure**

Run: `/Users/barbara/.platformio/penv/bin/pio run -d firmware -e sim`

Expected: FAIL at `launcher.cpp` because `struct timespec` is used before `<time.h>` is included.

- [ ] **Step 2: Move the standard time include before the first `timespec` use**

Put `#include <time.h>` with the other standard includes at the top of `launcher.cpp` and remove its late duplicate. Add `/.superpowers/` to `.gitignore` so local design-session state cannot enter a release commit.

- [ ] **Step 3: Verify the simulator build**

Run: `/Users/barbara/.platformio/penv/bin/pio run -d firmware -e sim`

Expected: `sim SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/shell/launcher.cpp .gitignore
git commit -m "build: restore native simulator on macOS"
```

### Task 2: Add Tested Gravity Projection and Persistent Calibration

**Files:**
- Create: `firmware/src/apps/level/level_model.h`
- Create: `firmware/test/test_level_model/test_main.cpp`
- Modify: `firmware/src/hal/imu_hal.h`
- Modify: `firmware/src/hal/imu_hal.cpp`
- Modify: `firmware/src/boards/sim/shim/Preferences.h`
- Modify: `firmware/src/boards/sim/arduino_shim.cpp`

**Interfaces:**
- Produces: `LevelProjection level_project(float panel_x, float panel_y, uint8_t rotation, float max_offset)` with `x`, `y`, and `tilt_deg` fields.
- Produces: `void level_calibration_begin(LevelCalibration *cal)`, `bool level_calibration_add(LevelCalibration *cal, float x, float y, float *bias_x, float *bias_y, int *pct_out)`, and a fixed `LEVEL_CAL_SAMPLES` sample target. Incomplete accumulators never emit a replacement bias.
- Produces: `imu_hal_gravity_cal_begin()`, `imu_hal_gravity_cal_step(int *pct_out)`, `imu_hal_gravity_cal_cancel()`, and `imu_hal_gravity_ready()`; successful completion persists X/Y bias, cancellation/failure does not overwrite it.
- Consumes: board-frame `imu_hal_read_accel_live()` samples and ESP32 `Preferences` storage.

- [ ] **Step 1: Write the failing pure model tests**

In `test_main.cpp`, add checks named:

- `flat_projects_to_center` — zero corrected X/Y returns `x == 0`, `y == 0`, `tilt_deg == 0`.
- `upright_projects_to_six_oclock` — a unit panel-down gravity vector returns `x == 0`, positive saturated `y`, and approximately `90°`.
- `upright_sensor_maps_to_six_oclock` — the mounted accelerometer's observed positive X direction maps to the 6-o'clock edge.
- `flat_sensor_residual_stays_centered` — a small combined X/Y residual remains inside the radial flat deadzone.
- `any_clear_tilt_reaches_the_gravity_edge` — non-flat gravity reaches the track perimeter instead of scaling travel by degrees.
- `physical_low_edge_survives_all_rotations` — expected X/Y signs for rotation quadrants 0–3.
- `travel_saturates_at_track_radius` — values beyond the usable radius never exceed `max_offset`.
- `calibration_averages_mount_bias` — the specified sample count averages deterministic X/Y samples.
- `incomplete_calibration_emits_no_bias` — an incomplete accumulator cannot report a replacement result.

- [ ] **Step 2: Run the pure test and verify it fails**

Run: `c++ -std=c++17 -Ifirmware/src firmware/test/test_level_model/test_main.cpp -o /tmp/clawddeck-level-model-test && /tmp/clawddeck-level-model-test`

Expected: FAIL because `apps/level/level_model.h` does not exist.

- [ ] **Step 3: Implement the pure model interfaces**

Convert calibrated QMI8658 samples to panel space using the observed board-axis signs, rotate through the frozen display quadrant, derive tilt with `asin(clamp(magnitude, 0, 1))`, and place any vector outside a small radial flat deadzone at `max_offset`. Keep calibration sample count and accumulator state in the pure header so tests do not require Arduino or LVGL.

- [ ] **Step 4: Run the pure test and verify it passes**

Run: `c++ -std=c++17 -Ifirmware/src firmware/test/test_level_model/test_main.cpp -o /tmp/clawddeck-level-model-test && /tmp/clawddeck-level-model-test`

Expected: exit 0 and `all level model checks passed`.

- [ ] **Step 5: Wire persistent calibration into `imu_hal`**

Load the last-good X/Y correction lazily from a dedicated Preferences namespace, store fixed-point milli-g integers, and write only after a complete calibration. Extend the simulator Preferences shim with the exact signed-integer getters/setters used by the HAL. Preserve the existing noise deadzone after bias subtraction.

- [ ] **Step 6: Verify both native and hardware compilation**

Run: `/Users/barbara/.platformio/penv/bin/pio run -d firmware -e sim`

Run: `/Users/barbara/.platformio/penv/bin/pio run -d firmware -e waveshare_amoled_216`

Expected: both environments report `SUCCESS`.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/apps/level/level_model.h firmware/test/test_level_model/test_main.cpp firmware/src/hal/imu_hal.h firmware/src/hal/imu_hal.cpp firmware/src/boards/sim/shim/Preferences.h firmware/src/boards/sim/arduino_shim.cpp
git commit -m "level: add persistent calibrated gravity model"
```

### Task 3: Rebuild Level as a Precision Gravity Instrument

**Files:**
- Modify: `firmware/src/apps/level/app.cpp`
- Modify: `firmware/src/boards/sim/imu.cpp`

**Interfaces:**
- Consumes: `level_project(...)` and persistent calibration APIs from Task 2.
- Consumes: `input_hal_is_held(INPUT_BTN_PRIMARY)` for the guarded 1.5-second calibration gesture.
- Produces: flat/tilted/vertical/calibrating visual states on one stable layout.

- [ ] **Step 1: Add a simulator-controlled gravity test seam**

Make the sim IMU accept deterministic pitch/roll from `SIM_IMU_PITCH_DEG` and `SIM_IMU_ROLL_DEG` while retaining animated defaults when unset. Add host/simulator checks for flat, 30° tilt, and 90° standing inputs so screenshot scenarios are reproducible.

- [ ] **Step 2: Replace Level's decorative and motion layers**

Use an opaque black root, one dark dial, one track, two subtle crosshairs, a center mark, and the terra-cotta ball. Remove F/B/L/R labels, full-screen gradient, layered glass gradients, and redundant rings. Put degree readout above and the bounded state/help label below.

- [ ] **Step 3: Bind real gravity behavior**

Remove the 12° spirit-level scale. Feed corrected panel gravity through `level_project(...)`, retain restrained exponential smoothing, and ensure a vertical unit vector reaches 6 o'clock at the track edge.

- [ ] **Step 4: Implement the calibration interaction**

Consume the initial BOOT press, arm while the button stays held, begin calibration at 1.5 seconds, show bounded progress, keep the ball stable, and cancel without mutation if released before the hold or if sampling fails. On success, return to live gravity using the newly persisted bias.

- [ ] **Step 5: Verify Level math and deterministic screenshots**

Run the Task 2 pure test, build `env:sim`, and capture scripted Level screenshots for flat, tilted, and upright inputs.

Expected: flat ball centered; tilted ball on the matching low side; upright ball at 6 o'clock; no stripes, clipped text, or decorative axis letters.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/apps/level/app.cpp firmware/src/boards/sim/imu.cpp
git commit -m "ui: turn Level into a calibrated gravity instrument"
```

### Task 4: Give Voice Collision-Proof Premium States

**Files:**
- Create: `firmware/src/apps/voice/voice_layout.h`
- Create: `firmware/test/test_voice_layout/test_main.cpp`
- Modify: `firmware/src/apps/voice/app.cpp`

**Interfaces:**
- Produces: `VoiceLayout voice_layout(int width, int height, int statusbar_h, int foot_h)` with explicit mic, state, meter, and transcript rectangles.
- Consumes: the existing Voice state machine and audio peak value without transport changes.

- [ ] **Step 1: Write failing layout tests**

Test 480×480 and 368×448 layouts. Assert each rectangle is inside the screen, transcript bottom stays above the home-pill keep-out, and mic/state/meter/transcript rectangles have non-negative separation.

- [ ] **Step 2: Run the tests and verify they fail**

Run: `c++ -std=c++17 -Ifirmware/src firmware/test/test_voice_layout/test_main.cpp -o /tmp/clawddeck-voice-layout-test && /tmp/clawddeck-voice-layout-test`

Expected: FAIL because `voice_layout.h` does not exist.

- [ ] **Step 3: Implement the pure layout helper**

Calculate a smaller circular control and fixed state/meter/transcript zones from available height. Pin transcript width and height; reserve room for three Inter-16 lines on 480 px and a compact equivalent below 400 px.

- [ ] **Step 4: Run the tests and verify they pass**

Expected: exit 0 and `all voice layout checks passed`.

- [ ] **Step 5: Rebuild the Voice presentation**

Use a solid black root and dark graphite mic control with one terra-cotta live accent. Apply explicit coordinates from `VoiceLayout`; shorten idle help to two lines; set transcript long mode to clip/wrap inside its fixed box. Distinguish `Listening`, `Mac voice`, `Transcribing`, `Processing`, and ready/error states without moving widgets.

- [ ] **Step 6: Exercise functional and visual regression checks**

Run: `PYTHONPATH=daemon daemon/.venv/bin/python -m pytest daemon/tests/test_voice_code.py -q`

Build and capture the deterministic idle simulator screen. Exercise listening,
transcribing, a multi-line transcript, and error copy during the physical Voice
smoke test in Task 6.

Expected: Voice codec tests pass and no text intersects the mic at either supported width.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/apps/voice/app.cpp firmware/src/apps/voice/voice_layout.h firmware/test/test_voice_layout/test_main.cpp
git commit -m "ui: give Voice collision-proof premium states"
```

### Task 5: Make Clock Typography the Interface

**Files:**
- Modify: `firmware/src/apps/clock/app.cpp`

**Interfaces:**
- Consumes: `clock_src_*()`, `ble_get_state()`, and existing Tiempos/Inter fonts.
- Produces: direct hero time/date layout and honest synchronized/waiting status.

- [ ] **Step 1: Remove wallpaper/card material layers**

Replace the wallpaper, washes, glass face, rim, and shadow hierarchy with one opaque black root. Keep only time, seconds, date, sync dot, sync copy, and home-pill keep-out.

- [ ] **Step 2: Rebuild metric-stable typography**

Pin the hero group to a known center region, measure/position hours-minutes and seconds without stale-width drift, and verify both `23:59:59` and the widest 12-hour rendering fit on 368 px and 480 px widths.

- [ ] **Step 3: Bind truthful sync state**

Show green `Synced from Mac` only when clock data is valid and BLE is connected. Otherwise show a neutral dot and `Waiting for Mac`; refresh on the existing one-second tick and app BLE callback.

- [ ] **Step 4: Capture visual cases**

Use simulator scenarios for 12-hour, 24-hour, disconnected, and connected states.

Expected: centered, unclipped time/date; no large card or root gradient; correct status color/copy.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/apps/clock/app.cpp
git commit -m "ui: make Clock typography-first"
```

### Task 6: Integrate Documentation and Prove the Release on Hardware

**Files:**
- Modify: `README.md`
- Commit: `docs/superpowers/specs/2026-10-18-voice-clock-level-polish-design.md`
- Commit: `docs/superpowers/plans/2026-10-18-voice-clock-level-polish.md`

**Interfaces:**
- Consumes: all prior tasks.
- Produces: exact-source build/test evidence, physical screenshots, calibrated motion evidence, and a clean release commit history.

- [ ] **Step 1: Verify the Clawdmeter attribution**

Ensure the first `Clawdmeter` mention links to `https://github.com/HermannBjorgvin/Clawdmeter` and no unrelated README prose changed.

- [ ] **Step 2: Run the complete software gate fresh**

Run:

```bash
c++ -std=c++17 -Ifirmware/src firmware/test/test_level_model/test_main.cpp -o /tmp/clawddeck-level-model-test && /tmp/clawddeck-level-model-test
c++ -std=c++17 -Ifirmware/src firmware/test/test_voice_layout/test_main.cpp -o /tmp/clawddeck-voice-layout-test && /tmp/clawddeck-voice-layout-test
PYTHONPATH=daemon daemon/.venv/bin/python -m pytest daemon/tests -q
/Users/barbara/.platformio/penv/bin/pio run -d firmware -e sim
/Users/barbara/.platformio/penv/bin/pio run -d firmware -e waveshare_amoled_216
git diff --check
```

Expected: both pure suites pass, daemon suite has zero failures, both PlatformIO environments report `SUCCESS`, and `git diff --check` emits nothing.

- [ ] **Step 3: Flash the exact clean-source firmware**

Run: `/Users/barbara/.platformio/penv/bin/pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem2101`

Expected: upload and hash verification succeed.

- [ ] **Step 4: Perform physical visual QA**

Capture all three screens over the existing serial screenshot command. Check RGB565 output at original resolution for horizontal bands, text overlap, clipped glyphs, off-center hero elements, and home-pill collisions. Rework and repeat the relevant task gate if any defect remains.

- [ ] **Step 5: Perform physical Level QA**

Calibrate with a 1.5-second BOOT hold on a known-flat table, reopen the app to prove persistence, then test flat and each physical low edge. Record: center error/readout when flat; ball direction at four edges; 6-o'clock position and near-90° readout while upright.

- [ ] **Step 6: Perform Voice functional smoke test**

Record a short phrase through the physical microphone and confirm the existing BLE/Whisper/injection path still completes while every Voice state remains inside its assigned zone.

- [ ] **Step 7: Commit documentation and final fixes**

```bash
git add README.md docs/superpowers/specs/2026-10-18-voice-clock-level-polish-design.md docs/superpowers/plans/2026-10-18-voice-clock-level-polish.md
git commit -m "docs: link Clawdmeter and record app polish design"
```

- [ ] **Step 8: Audit the release tree**

Run: `git status --short && git log --oneline -6`

Expected: no untracked diagnostics or temporary hooks; only intentional commits from this plan.
