# iss — Instant Space Switcher

Keeps the native finger-following preview when swiping between macOS spaces, accelerates the release, supports multi-space long drags, and clamps at the edges.

## Swipe behavior

- The first landing completes the full remaining slide over 60 ms, then commits immediately to avoid a long ease-out and delayed focus.
- The first animation starts during the swipe after at least 40 ms and 0.0625 units of travel, or on release for an earlier flick. Further travel advances in centered steps: each step consumes another 0.125 units, waits for the Dock to confirm the landing, and keeps the space centered for at least 120 ms before advancing again. Subsequent steps do not reopen a partial preview.
- Small peeks and reversals settle using the Dock's native behavior.
- Outward gestures at the first or last space are suppressed from Begin through End. A deliberate inward reversal can start a new segment; release jitter cannot fling back to the previous space.
- Vertical swipes, including Mission Control and App Exposé, pass through.

The gesture thresholds live in `swipe.h`; the initial slide duration lives in `slide.h`. Distances use native gesture progress units, not pixels.

## How it works

The session-level event tap preserves the native preview. When the first transition commits, a run-loop timer drives Changed frames linearly over 60 ms, completing the remaining travel to the adjacent space. A full space is `N/(N-1)` progress units for a display with `N` spaces. At the final frame, End preserves the full destination progress with a minimal nonzero velocity, following FasterSwiper’s segmented commit path. It does not reset the completed slide back to near-zero progress. Timer frames are tagged to bypass our event tap, and hardware movement during the animation is retained for subsequent steps.

This follows the timed-position animation and immediate commit approach in [FasterSwiper](https://github.com/mgbowen/FasterSwiper), replacing the earlier velocity-only first landing. The frame timing and position conversion are implemented independently in C. The prior 100 ms setting was observed at approximately 101–104 ms in live tests, with prompt focus recovery; the default is now 60 ms. Actual frame delivery still depends on scheduling and display refresh.

On macOS 27, phase, progress, timestamp and velocity are synchronized in both the outer CGEvent and embedded native IOHID queue. Posted frames use the Natural Scrolling direction convention. The signed 16.16 velocity saturates at 32767 rather than overflowing. Subsequent centered steps retain the existing high-velocity finish.

For a long drag, a Changed sample becomes an early End. Further samples are held until the active-space query reports the expected destination. After confirmation and the centered dwell, continued travel starts the next step. Subsequent steps use near-zero progress and high release velocity to keep each landed space centered instead of reopening a fractional preview. There are no bursts of synthetic Begin/Changed/End sequences and no speculative index increments. If confirmation fails within 600 ms, continuation stops until the next physical gesture. The real terminal event still passes through with zero progress/velocity after an early finish, closing the hardware gesture on macOS 27.

Boundary checks use the ordered spaces of the display under the pointer, including fullscreen spaces. The active index comes from the per-display managed-space API (with `CGSGetActiveSpace` as a single-display fallback), rather than the stale `Current Space` dictionary. The ordered space IDs and count are rechecked while dragging, before release, and on every timed animation frame. Adding, removing, or reordering a space cancels an in-flight animation and clears its pending target. The next gesture reads the new layout and recomputes the slide distance. If the initial private space query is unavailable, that gesture is suppressed until the next gesture can obtain valid bounds.

High-velocity finishes follow the approach in [spaceflick](https://github.com/julienR2/spaceflick). [noswoosh](https://github.com/mmathys/noswoosh) documents display-aware boundary checks and the separate direction encoding of generated macOS 27 events.

No SIP disable or code injection is required. The event fields and CGS space APIs are private. Build and event tests cannot verify the Dock's live animation; that requires testing with a trackpad on the target macOS release.

## Build and test

```
make
make test
```

Tests cover both native directions, preview and release progress, first/last-space clamping, reversal, confirmed multi-space drags, delayed or failed landings, rapid subsequent gestures, cancellation, and single-space displays. The CGEvent tests check serialized native events, embedded phase/velocity updates, insertion of missing velocity children, opposite payload signs, and overflow prevention without posting gestures to the desktop.

## Install

```
# builds, installs to ~/.local/bin/iss, starts launch agent
make install
```

Runs automatically at login via launchd. Grant Accessibility permission when prompted.

## Diagnostics

With the installed agent stopped, use one foreground instance:

```
# Log native input and the changes made by iss:
./iss --verbose

# Or observe native gestures without modifying them:
./iss --probe
```

Both modes log horizontal space gestures and space indices; the probe also records their serialized event payloads. Stop with Ctrl+C. Avoid running another space-switching utility at the same time when testing.

## Uninstall

```
make uninstall
```
