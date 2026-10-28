# iss — Instant Space Switcher

Keeps the native finger-following preview when swiping between macOS spaces, accelerates the release, supports multi-space long drags, and clamps at the edges.

## Swipe behavior

- A quick flick previews and lands on one adjacent space.
- A sustained drag (at least 180 ms) previews the first adjacent space, then lands after 0.125 units of travel. Further travel advances in centered steps: each step consumes another 0.125 units, waits for the Dock to confirm the landing, and keeps the space centered for at least 120 ms before advancing again. Subsequent steps do not reopen a partial preview.
- Small peeks and reversals settle using the Dock's native behavior.
- Outward movement at the first or last space is flattened to zero. Reversing responds immediately without undoing the discarded overshoot.
- Vertical swipes, including Mission Control and App Exposé, pass through.

The thresholds live in `swipe.h`. Distances use native gesture progress units, not pixels.

## How it works

The session-level event tap edits the **original native events**. Ordinary Begin/Changed events retain their native progress and device metadata. On macOS 27, edited phases, progress and velocity are synchronized into the embedded IOHID queue as well as the outer CGEvent fields. Early End events receive a velocity child if the original Changed event had none. A committed release gets a high velocity with its progress preserved; it is not replaced with a near-zero-progress synthetic gesture.

This matters on macOS 27: generated gestures have a different direction encoding that also depends on Natural Scrolling. Native events already include that preference. The native path retains the original queue header and device metadata and derives the embedded direction from the incoming event. It does not generate a separate synthetic swipe sequence. The macOS 26/27 input convention is positive progress toward a higher space index. Releases use Spaceflick’s outer velocity of 999999; the embedded signed 16.16 velocity saturates at 32767 rather than overflowing.

For a long drag, a Changed sample becomes an early End. Further samples are held until the display's `Current Space` reports the expected destination. After confirmation and the centered dwell, continued travel starts the next step. Subsequent steps use near-zero progress and high release velocity to keep each landed space centered instead of reopening a fractional preview. There are no bursts of synthetic Begin/Changed/End sequences and no speculative index increments. If confirmation fails within 600 ms, continuation stops until the next physical gesture. The real terminal event still passes through with zero progress/velocity after an early finish, closing the hardware gesture on macOS 27.

Boundary checks use the ordered spaces of the display under the pointer, including fullscreen spaces. With one managed display, the active index comes from `CGSGetActiveSpace`, as in noswoosh; the dictionary’s `Current Space` remained stale throughout the captured native swipes on macOS 27.2. A changed space list or unexpected destination stops continuation. If the initial private space query is unavailable, the entire gesture passes through natively instead of disabling switching or guessing a boundary.

The release behavior follows the approach in [spaceflick](https://github.com/julienR2/spaceflick). [noswoosh](https://github.com/mmathys/noswoosh) documents display-aware boundary checks and the separate direction encoding of generated macOS 27 events.

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
