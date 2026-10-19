# Voice, Clock, and Level Polish Design

## Intent

ClawdDeck's Voice, Clock, and Level apps should feel like finished instruments,
not demonstrations of LVGL effects. The selected visual direction is
**Precision Black**: true-black or near-black fields, direct typography,
restrained depth, and one functional accent per app. This specifically removes
the horizontal color bands visible in the current 16-bit full-screen gradients
and eliminates layout collisions such as Voice copy overlapping the microphone.

The redesign must preserve each app's existing purpose. Voice remains a dual
path for on-device dictation and Mac voice-mode push-to-talk. Clock remains a
host-synchronized wall clock. Level remains gravity-driven: flat places the ball
at center, tilting pulls it toward the physical low edge, and standing the deck
upright places it at 6 o'clock. Level must never redefine the opening pose as
flat.

## Goals

- Make all three apps read as one deliberate product family.
- Eliminate full-screen gradient banding on the RGB565 AMOLED target.
- Give every dynamic Voice string a bounded region that cannot overlap the mic.
- Make Clock's time—not a decorative card—the visual hero.
- Correct Level's flat-table offset while retaining real gravity behavior.
- Provide calm, responsive state changes with no ornamental looping animation.
- Preserve usable compact layouts on boards narrower than 400 px.

## Non-goals

- No changes to Voice capture, BLE transport, Whisper, or text injection.
- No launcher-wide, status-bar, home-pill, or unrelated app redesign.
- No new bitmap assets, external libraries, or fonts.
- No automatic Level zeroing on each app entry.
- No fake physics or bouncing-ball game behavior.

## Shared Visual System

The three roots use an opaque `THEME_BG`/near-black fill with no root gradient.
Depth comes from a few solid, low-contrast shapes, hairlines, and local surfaces;
no large alpha or luma ramp spans the display. This is intentionally compatible
with RGB565 and should show no horizontal gradient steps on hardware.

Primary text is warm white. Secondary text is neutral gray. Each app gets one
semantic accent:

- Voice: terra-cotta for live/listening state.
- Clock: green only for the small connected/synchronized indicator.
- Level: terra-cotta for the gravity ball and green for the exact-level lock.

Type uses the fonts already compiled into firmware. Inter carries interface
copy; Tiempos carries Clock's hero numerals. Surfaces use one-pixel neutral rims
and shallow shadows only where they explain a control or instrument boundary.
Decorative glass gradients and large translucent cards are removed.

All geometry is computed from board width and height, status-bar height, and the
home-pill keep-out. The 480×480 board is the visual reference; the existing
`compact` breakpoint below 400 px remains supported with scaled dimensions and
no clipped text.

## Voice

### Layout

Voice is divided into non-overlapping vertical zones:

1. Status bar and a compact `Voice` title.
2. A centered circular microphone control, smaller than the current 240 px
   circle so it does not consume the copy region.
3. One single-line state label directly below the control.
4. A slim, dedicated input-level indicator shown only while listening.
5. A fixed transcript/help region above the home-pill keep-out.

The transcript region has an explicit width and height, wraps within that box,
and clips overflow. Idle help is reduced to two short lines: hold the mic to
dictate; hold BOOT for Mac voice mode. A returned transcript may use up to three
visible lines; it cannot move the microphone, state, or level indicator.

### States

- **Ready:** dark microphone control, neutral `Hold to speak`, concise help.
- **Dictating:** terra-cotta accent/ring, live input level, `Listening`.
- **Mac voice PTT:** same live accent, `Mac voice` so the two modes are distinct.
- **Transcribing/processing:** stable control, understated progress treatment,
  `Transcribing` or `Processing`; no indeterminate looping spectacle.
- **Result/error:** state returns to ready and transcript region shows the result
  or actionable error without altering layout.

Existing hold/release semantics, touch debounce, BOOT behavior, and transport
state machine remain unchanged.

## Clock

Clock removes the large glass card and app title. The time sits directly on the
black field and owns the center of the screen. Hours and minutes use large
Tiempos numerals; seconds remain smaller and dimmer on the same baseline. Date
appears beneath in Inter with generous separation.

A small green dot plus `Synced from Mac` sits above the home-pill keep-out. It is
the only decorative color. When the BLE host is unavailable, the dot becomes
neutral and the copy becomes `Waiting for Mac`; the UI must not claim a live
sync while disconnected. The 12/24-hour selection and host-fed time/date path
remain unchanged. Label widths are pinned and aligned from measured font metrics
so the seconds update cannot cause drift or clipping.

## Level

### Motion Model

Level is a gravity instrument, not an opening-pose bubble level:

- Flat on a table: ball at the dial center, readout approximately `0.0°`.
- Tilted: ball follows the physical low direction with smooth damping.
- Standing upright: ball reaches the track at 6 o'clock, readout approximately
  `90.0°`.
- Rotating the deck while vertical moves the ball around the track consistently
  with the frozen display orientation.

The current QMI8658-to-panel axis mapping follows the physical direction
observed on the board; calibration is applied in sensor/panel space before
display rotation. A small radial deadzone keeps sensor noise on a flat table
from pulling the ball off center. Once the calibrated vector clears that
deadzone, the ball settles at the track edge in the low-side direction;
smoothing controls how quickly it gets there. Tilt magnitude remains available
for the degree readout, but does not strand the ball partway across the dial.

### Calibration

Opening Level uses the last stored sensor-mount correction and never learns the
entry pose. Holding the primary/BOOT button for 1.5 seconds while the deck is
resting on a known-flat surface starts a short multi-sample calibration; a
normal press does nothing destructive. During calibration, the fixed readout
region shows progress and the ball remains stable. Successful calibration
persists the X/Y correction across app exits and reboots. The app ships with a
neutral default and exposes concise help when no persisted calibration exists.
Calibration failure leaves the prior correction intact.

This replaces the current behavior that clears calibration on app entry and on
primary-button press without ever collecting calibration samples.

### Presentation

The dial is a dark instrument ring with two subtle crosshairs, one track, and a
clear center mark. F/B/L/R labels and layered decorative rings are removed.
The terra-cotta ball is the dominant mark. The degree readout sits above the
dial; a compact state (`LEVEL`, `TILTED`, `VERTICAL`, or calibration help) sits
below. When within the level threshold, the center/label changes to green
without flashing the whole screen.

## Implementation Boundaries

- App presentation and state binding stay in:
  - `firmware/src/apps/voice/app.cpp`
  - `firmware/src/apps/clock/app.cpp`
  - `firmware/src/apps/level/app.cpp`
- Sensor calibration behavior belongs in `firmware/src/hal/imu_hal.*`, with
  platform-compatible persistence or board-level support as required.
- Pure gravity transformation/calibration math should be isolated so native
  tests can cover flat, tilted, vertical, rotation, and saturation cases.
- The existing desktop simulator should compile again if a small portability
  include is required, because screenshot verification is part of this work.
- The README change links the first Clawdmeter mention to
  `https://github.com/HermannBjorgvin/Clawdmeter`.

## Acceptance Criteria

1. Physical 480×480 screenshots of all three apps show no full-screen horizontal
   banding and no text/control overlap.
2. Voice idle, listening, transcribing, transcript, and error copy stay inside
   their allocated zones; the microphone circle never intersects text.
3. Clock time, seconds, date, and sync indicator remain centered and unclipped
   in both 12- and 24-hour formats.
4. With the hardware placed flat after calibration, Level settles at the center
   and reads within 0.5°.
5. With the hardware standing upright, Level settles at 6 o'clock and reads near
   90°; each physical low edge produces the matching on-screen direction.
6. Level calibration is deliberate, averaged, persistent, and never runs merely
   because the app opened.
7. The simulator build and the `waveshare_amoled_216` firmware build succeed.
8. Existing daemon tests remain green, and new pure tests cover gravity mapping
   and calibration behavior.
9. Final hardware screenshots and motion checks are performed from the exact
   release source, with no diagnostic-only hooks left in the commit.

## Builder Calls

The builder may choose exact pixel dimensions, opacities, damping constants,
calibration sample count, persistence namespace/keys, and the smallest clean
math-test seam, provided the behavior and acceptance thresholds above remain
true. These choices must favor legibility and stability over ornamental detail.
