# iss — Instant Space Switcher

Keeps the native finger-following preview when swiping between macOS spaces, accelerates the release, supports multi-space long drags, and clamps at the edges.

## Swipe behavior

- Every landing completes a full slide over 60 ms, then commits immediately to avoid a long ease-out and delayed focus.
- The first animation starts during the swipe after at least 40 ms and 0.0625 units of travel, or on release for an earlier flick. Further travel advances in centered steps: each step consumes another 0.16 units, waits for the Dock to confirm the landing, and keeps the space centered for at least 150 ms before advancing again. Each subsequent step plays the same 60 ms slide from the centered position.
- Reversing a continued drag needs only 0.04 units of backward travel, discards queued forward movement, and bypasses the centered pause once the current landing is confirmed. Each reverse switch still plays the 60 ms slide.
- Small initial peeks and reversals settle using the Dock's native behavior.
- Outward gestures at the first or last space are suppressed from Begin through End. A deliberate inward reversal can start a new segment; release jitter cannot fling back to the previous space.
- Vertical swipes, including Mission Control and App Exposé, pass through.

These are the default settings. The native app lets you change them without rebuilding. Distances use native gesture progress units, not pixels.

## How it works

The session-level event tap preserves the native preview. When any transition commits, a run-loop timer drives Changed frames linearly over 60 ms, completing the remaining travel to the adjacent space. A full space is `N/(N-1)` progress units for a display with `N` spaces. At the final frame, End preserves the full destination progress with a minimal nonzero velocity, following FasterSwiper’s segmented commit path. It does not reset the completed slide back to near-zero progress. Timer frames are tagged to bypass our event tap, and hardware movement during the animation is retained for subsequent steps.

This follows the timed-position animation and immediate commit approach in [FasterSwiper](https://github.com/mgbowen/FasterSwiper), replacing the earlier velocity-only first landing. The frame timing and position conversion are implemented independently in C. The prior 100 ms setting was observed at approximately 101–104 ms in live tests, with prompt focus recovery; the default is now 60 ms. Actual frame delivery still depends on scheduling and display refresh.

On macOS 27, phase, progress, timestamp and velocity are synchronized in both the outer CGEvent and embedded native IOHID queue. Posted frames use the Natural Scrolling direction convention. The signed 16.16 velocity saturates at 32767 rather than overflowing. Every step finishes at its full destination progress with minimal terminal velocity.

For a long drag, a Changed sample becomes an early End. Further samples are held until the active-space query reports the expected destination. After confirmation and the centered dwell, continued travel starts the next step. Subsequent steps begin centered and use the same timed full-slide animation before settling on the next space. There are no bursts of synthetic Begin/Changed/End sequences and no speculative index increments. If confirmation fails within 600 ms, continuation stops until the next physical gesture. The real terminal event still passes through with zero progress/velocity after an early finish, closing the hardware gesture on macOS 27.

Boundary checks use the ordered spaces of the display under the pointer, including fullscreen spaces. The active index comes from the per-display managed-space API (with `CGSGetActiveSpace` as a single-display fallback), rather than the stale `Current Space` dictionary. The ordered space IDs and count are rechecked while dragging, before release, and on every timed animation frame. Adding, removing, or reordering a space cancels an in-flight animation and clears its pending target. The next gesture reads the new layout and recomputes the slide distance. If the initial private space query is unavailable, that gesture is suppressed until the next gesture can obtain valid bounds.

High-velocity finishes follow the approach in [spaceflick](https://github.com/julienR2/spaceflick). [noswoosh](https://github.com/mmathys/noswoosh) documents display-aware boundary checks and the separate direction encoding of generated macOS 27 events.

No SIP disable or code injection is required. The event fields and CGS space APIs are private. Build and event tests cannot verify the Dock's live animation; that requires testing with a trackpad on the target macOS release.

## Build and test

```
make
make test
make test-app
```

Tests cover both native directions, preview and release progress, first/last-space clamping, reversal, confirmed multi-space drags, delayed or failed landings, rapid subsequent gestures, cancellation, and single-space displays. Settings tests cover clamping and invalid files, altered gesture thresholds, persistence, reset, and app pause/resume/quit using a harmless dummy helper. The CGEvent tests check serialized native events, embedded phase/velocity updates, insertion of missing velocity children, opposite payload signs, and overflow prevention without posting gestures to the desktop.

## Native settings app and menu bar

```sh
make app
open build/ISS.app
```

The settings window has three pages:

- **Swiping:** slide duration, activation distance and delay, and flick sensitivity. Try Slide previews the timing without changing your desktop.
- **Long drags:** distance per space, centered pause, long-drag delay, reversal distance, and inward reversal distance at an edge.
- **General:** instant focus across Spaces, launch at login, Accessibility permission, switcher status, restart, and local diagnostics.

Changes save automatically and apply between physical gestures, after any current slide finishes. Every setting has a reset button; Restore Defaults returns to the tuning above. Closing the window leaves ISS in the menu bar. The menu offers pause/resume, settings, launch at login, Accessibility settings, and quit. Pausing or quitting returns gestures to macOS.

To install the app in `~/Applications` and open it:

```sh
# If you installed the old command-line background service, remove it first:
make uninstall
make install-app
```

Stop any foreground `./iss` process with Ctrl+C before opening the app. The new engine prevents overlapping instances, and the app detects the legacy launch-agent installation. Grant **ISS** Accessibility access in System Settings → Privacy & Security → Accessibility. If macOS lists **iss-engine** separately, allow that as well. Click Retry if needed. Local builds are ad-hoc signed; macOS may require re-enabling permission after a rebuild.

Enable **Launch at login** in the app once it is in its permanent location. ISS uses macOS’s built-in login-item registration. If approval is required, the app provides a link to Login Items. Turn this setting off before moving or removing the app.

Settings are saved in `~/Library/Application Support/ISS/settings.plist`. The app and engine share their default values and supported ranges through `settings.h`; the app’s schema is generated during the build. The helper validates the file and defers reloads throughout a gesture and its animation. It exits if the app quits or crashes. The app stays offline and does not send diagnostics automatically.

Normal background operation has no repeating status or settings-file poll. The engine waits on kernel notifications for file edits, app exit, and termination signals. The UI refreshes system status when opened/activated and publishes only actual changes. A two-second permission retry runs only while Accessibility is missing. Timers run only during a swipe, a short pending focus request, or cursor restoration after a cross-display jump.

### Instant app and window focus

**General → Instant focus across Spaces** is enabled by default. It skips the native Space transition when Command+Tab, the AltTab app, an existing Option+Tab shortcut, or a Dock click focuses a window on another Space. Your switcher still chooses the destination; ISS does not bind Option+Tab or replace its UI, and cannot remove a delay inside a third-party switcher's own interface. Windows already on a visible Space are left alone.

This uses yabai's [SIP-compatible focus approach](https://github.com/asmvik/yabai/blob/master/src/space_manager.c), with synthetic gestures carrying the macOS 27 IOHID payload. No Dock injection or SIP changes are used. The focus observer coalesces activation and window-focus notifications, reads the actual focused window, and rechecks both focus and the ordered Space layout immediately before posting. It prepares the complete jump up front and ignores duplicate requests; it never appends extra steps based on a Space query during the native transition. A physical swipe cancels any pending request. If focus or the layout changes during preparation, it leaves the switch to macOS. Trackpad gestures keep the configured slide duration. On another display, the cursor is briefly routed to that display and restored if you have not moved it.

The underlying APIs are private; compatibility and perceived timing require live testing on each macOS version. Run `./iss --focus-check` to check symbol availability without changing focus. See `THIRD_PARTY_NOTICES.md` for reference code licences.

For a UI-only preview with no switcher or Accessibility prompt:

```sh
build/ISS.app/Contents/MacOS/ISS --preview
```

## Command-line installation (optional)

```sh
# Installs ~/.local/bin/iss and starts the original launch agent:
make install
```

Use either this service or the app. The command-line service still uses the original defaults unless started with `--config`. The native app is not required for command-line operation.

## Diagnostics

With the installed agent stopped, use one foreground instance:

```
# Log native input and the changes made by iss:
./iss --verbose

# Use settings saved by the app (quit the app first):
./iss --verbose --config "$HOME/Library/Application Support/ISS/settings.plist"

# Or observe native gestures without modifying them:
./iss --probe
```

Both modes log horizontal space gestures and space indices; the probe also records their serialized event payloads. Stop with Ctrl+C. Avoid running another space-switching utility at the same time when testing.

## Uninstall

For the app, disable Launch at login, quit ISS from the menu bar, and remove `~/Applications/ISS.app`. Your settings remain in Application Support. For the command-line service:

```
make uninstall
```
