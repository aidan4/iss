import SwiftUI

private let accent = Color(nsColor: NSColor(name: nil) { appearance in
    appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        ? NSColor(srgbRed: 0.57, green: 0.64, blue: 1.0, alpha: 1)
        : NSColor(srgbRed: 0.30, green: 0.39, blue: 0.88, alpha: 1)
})

struct SettingsView: View {
    @ObservedObject var model: SettingsModel
    @State private var showingReset = false

    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 28) {
                HStack(spacing: 12) {
                    Image(systemName: "rectangle.on.rectangle")
                        .font(.system(size: 24, weight: .semibold)).foregroundStyle(.white)
                        .frame(width: 48, height: 48)
                        .background(accent.gradient, in: RoundedRectangle(cornerRadius: 13))
                    VStack(alignment: .leading, spacing: 2) {
                        Text("ISS").font(.system(size: 23, weight: .bold, design: .rounded))
                        Text("Instant Space Switcher").font(.system(size: 10)).foregroundStyle(.secondary)
                    }
                }.padding(.top, 10)
                VStack(spacing: 6) {
                    ForEach(SettingsPage.allCases) { page in
                        Button { model.page = page } label: {
                            HStack(spacing: 11) {
                                Image(systemName: page.symbol).frame(width: 20)
                                Text(page.rawValue).fontWeight(.medium)
                                Spacer()
                            }.padding(.horizontal, 12).padding(.vertical, 11)
                                .foregroundStyle(model.page == page ? accent : .primary)
                                .background(model.page == page ? accent.opacity(0.11) : .clear,
                                            in: RoundedRectangle(cornerRadius: 9))
                                .contentShape(Rectangle())
                        }.buttonStyle(.plain)
                            .accessibilityAddTraits(model.page == page ? .isSelected : [])
                    }
                }
                Spacer()
                VStack(alignment: .leading, spacing: 10) {
                    HStack(spacing: 7) {
                        Circle().fill(model.isRunning ? Color.green : Color.secondary).frame(width: 7, height: 7)
                        Text(model.status).font(.system(size: 12, weight: .medium)).lineLimit(2)
                    }
                    Toggle("Enable ISS", isOn: Binding(get: { model.enabled }, set: { model.setEnabled($0) }))
                        .toggleStyle(.switch).controlSize(.small)
                    Text("Your spaces, at your pace.").font(.system(size: 11)).foregroundStyle(.secondary)
                }
            }.padding(22).frame(width: 225).background(Color(nsColor: .underPageBackgroundColor))
            Divider()
            VStack(alignment: .leading, spacing: 0) {
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 7) {
                        Text(model.page.rawValue).font(.system(size: 27, weight: .bold))
                        Text(model.page.subtitle).font(.system(size: 12)).foregroundStyle(.secondary)
                    }
                    Spacer()
                }.padding(.horizontal, 30).padding(.top, 28).padding(.bottom, 20)
                ScrollView {
                    VStack(alignment: .leading, spacing: 18) {
                        if let error = model.error {
                            notice(error, symbol: "exclamationmark.triangle", color: .orange) {
                                Button("Retry") { model.retry() }
                                Button("Dismiss") { model.error = nil }
                            }
                        }
                        if !model.accessibility {
                            notice("Allow Accessibility so ISS can respond to your trackpad gestures.", symbol: "hand.raised", color: accent) {
                                Button("Open Accessibility Settings") { model.requestAccessibility() }
                            }
                        }
                        switch model.page {
                        case .swiping: swiping
                        case .longDrag: longDrag
                        case .general: general
                        }
                    }.padding(.horizontal, 30).padding(.bottom, 24)
                }
                Divider()
                HStack {
                    Text(model.preview ? "Preview · no gestures are changed" : "Saved automatically · applies to your next swipe")
                        .font(.system(size: 11)).foregroundStyle(.secondary)
                    Spacer()
                    Button("Restore Defaults…") { showingReset = true }.controlSize(.small)
                }.padding(.horizontal, 24).padding(.vertical, 15)
            }.background(Color(nsColor: .windowBackgroundColor))
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .tint(accent)
        .frame(minWidth: 840, minHeight: 710)
        .alert("Restore swipe settings?", isPresented: $showingReset) {
            Button("Cancel", role: .cancel) {}
            Button("Restore Defaults", role: .destructive) { model.restoreDefaults() }
        } message: { Text("This restores your 60 ms slide and original gesture tuning. Launch at login and pause settings stay as they are.") }
    }

    private var releaseOnly: Bool { (model.values["releaseOnly"] ?? 0) >= 0.5 }

    private var swiping: some View {
        VStack(alignment: .leading, spacing: 18) {
            card("SWIPE BEHAVIOR") {
                Picker("Finish the swipe", selection: Binding(
                    get: { releaseOnly }, set: { model.set("releaseOnly", $0 ? 1 : 0) })) {
                    Text("During gesture").tag(false)
                    Text("On release").tag(true)
                }.pickerStyle(.segmented)
                Text(releaseOnly
                     ? "Follow your fingers, then slide to the next Space in the direction of momentum when you lift. Keep dragging to move across more Spaces."
                     : "Start the slide early and keep moving through Spaces with a long drag.")
                    .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            SlidePreview(duration: model.values["slideSeconds"] ?? 0.06)
            card("THE SLIDE") {
                setting("slideSeconds", "Slide duration", "Every space switch plays the full slide. Lower is faster.", milliseconds: true)
            }
            if releaseOnly {
                card("FOLLOW YOUR FINGERS") {
                    setting("gestureMultiplier", "Gesture speed multiplier", "Multiply preview movement while dragging. 1× is normal speed; 2× travels twice as far.", suffix: "×")
                    Divider()
                    setting("gestureAcceleration", "Acceleration multiplier", "Boost fast swipes on top of your gesture speed. 0 turns acceleration off; higher values add more boost.")
                    Text("Reverse to move back through your Spaces. Without momentum, the nearest Space wins. There are no automatic steps or centered pauses.")
                        .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
            } else {
            card("THE FIRST SWITCH") {
                setting("initialDistance", "Activation distance", "How far to drag before the first slide starts. Lower starts sooner.")
                Divider()
                setting("initialSeconds", "Activation delay", "Minimum time before starting a slide while your fingers are down.", milliseconds: true)
                Divider()
                setting("flickVelocity", "Flick threshold", "Minimum release speed for a quick flick. Lower catches gentler flicks.")
            }
            }
        }
    }
    private var longDrag: some View {
        VStack(alignment: .leading, spacing: 18) {
            if releaseOnly {
                Label("Long-drag stepping is off in On release mode. Change the mode in Swiping to use these settings.", systemImage: "hand.draw")
                    .font(.system(size: 12)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            Group {
            card("KEEP MOVING") {
                setting("spaceDistance", "Distance per space", "Travel between continued switches. Higher spreads spaces farther apart.")
                Divider()
                setting("centerSeconds", "Centered pause", "Time to rest on each space before moving forward again.", milliseconds: true)
                Divider()
                setting("longSeconds", "Long-drag delay", "Minimum gesture age before continuing past the first space.", milliseconds: true)
            }
            card("CHANGE DIRECTION") {
                setting("reverseDistance", "Reversal distance", "Backward travel to reverse. Skips the centered pause after landing.")
                Divider()
                setting("edgeReverseDistance", "Reverse from an edge", "Inward travel to resume after swiping against the first or last space.")
            }
            }.disabled(releaseOnly)
            Label("Each step uses your slide duration. Edges always stay clamped.", systemImage: "arrow.left.and.right")
                .font(.system(size: 11)).foregroundStyle(.secondary)
        }
    }
    private var general: some View {
        VStack(alignment: .leading, spacing: 18) {
            card("WINDOW SWITCHING") {
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Instant focus across Spaces").fontWeight(.medium)
                        Text("Skip the Space animation when Command+Tab, AltTab, or an existing Option+Tab shortcut focuses a window.")
                            .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer()
                    Toggle("Instant focus across Spaces", isOn: Binding(
                        get: { (model.values["instantFocus"] ?? 1) >= 0.5 },
                        set: { model.set("instantFocus", $0 ? 1 : 0) }))
                        .labelsHidden().toggleStyle(.switch)
                }
                Text("Keeps your current switcher and trackpad slide timing. Works with SIP enabled.")
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
            card("ON YOUR MAC") {
                HStack {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Launch at login").fontWeight(.medium)
                        Text("Keep ISS in your menu bar when you sign in.").font(.system(size: 11)).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Toggle("Launch at login", isOn: Binding(get: { model.loginEnabled }, set: { model.setLogin($0) }))
                        .labelsHidden().toggleStyle(.switch)
                }
                if model.loginNeedsApproval {
                    Button("Approve in Login Items…") { model.openLoginSettings() }
                }
                Divider()
                HStack {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Accessibility").fontWeight(.medium)
                        Text(model.accessibility ? "Permission granted" : "Permission required to change swipes")
                            .font(.system(size: 11)).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("Open Settings…") { model.requestAccessibility() }
                }
            }
            card("TOOLS") {
                HStack {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Switcher status").fontWeight(.medium)
                        Text(model.status).font(.system(size: 11)).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("Restart Switcher") { model.retry() }.disabled(!model.enabled)
                }
                Divider()
                HStack {
                    Button("Copy Diagnostics") { model.copyDiagnostics() }
                    Button("Show Settings File") { model.revealConfig() }
                }
                Text("Diagnostics include your settings and switcher status. Nothing is sent automatically.")
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 8) {
                Text("ISS 1.0").font(.system(size: 13, weight: .semibold))
                Text("A native preview, a fast slide, and a clean landing. Closing this window keeps ISS in the menu bar. Quit from the menu bar to return to normal macOS swipes.")
                    .font(.system(size: 12)).foregroundStyle(.secondary).lineSpacing(3)
                Text("Distance and speed values use trackpad gesture units, not pixels. The same tuning applies on all displays.")
                    .font(.system(size: 11)).foregroundStyle(.secondary).lineSpacing(3)
            }.padding(4)
        }
    }
    private func card<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title).font(.system(size: 10, weight: .semibold)).tracking(1.2).foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 16, content: content)
                .padding(18).frame(maxWidth: .infinity, alignment: .leading)
                .background(Color(nsColor: .controlBackgroundColor), in: RoundedRectangle(cornerRadius: 12))
                .overlay(RoundedRectangle(cornerRadius: 12).stroke(.primary.opacity(0.07), lineWidth: 1))
        }
    }
    private func setting(_ key: String, _ title: String, _ detail: String, milliseconds: Bool = false, suffix: String = "") -> some View {
        let spec = model.schema[key]!
        let binding = model.binding(key)
        return VStack(alignment: .leading, spacing: 7) {
            HStack(alignment: .firstTextBaseline) {
                Text(title).font(.system(size: 13, weight: .medium))
                Spacer()
                SettingNumberField(value: binding, spec: spec, title: title,
                                   scale: milliseconds ? 1000 : 1, suffix: milliseconds ? "ms" : suffix)
                    .id(key)
                Button { model.set(key, spec.defaultValue) } label: { Image(systemName: "arrow.counterclockwise").font(.system(size: 10)) }
                    .buttonStyle(.borderless).foregroundStyle(.secondary)
                    .help("Reset \(title.lowercased())").accessibilityLabel("Reset \(title)")
            }
            // The slider offers a convenient range; text entry keeps the exact
            // value, including valid values outside that range.
            Slider(value: Binding(get: { min(spec.maximum, max(spec.minimum, binding.wrappedValue)) },
                                  set: { binding.wrappedValue = spec.sliderValue($0, scale: milliseconds ? 1000 : 1) }), in: spec.minimum...spec.maximum)
                .accessibilityLabel(title)
            Text(detail).font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }
    private func notice<Actions: View>(_ message: String, symbol: String, color: Color, @ViewBuilder actions: () -> Actions) -> some View {
        HStack(alignment: .top, spacing: 11) {
            Image(systemName: symbol).foregroundStyle(color)
            VStack(alignment: .leading, spacing: 10) {
                Text(message).font(.system(size: 12)).fixedSize(horizontal: false, vertical: true)
                HStack(content: actions).controlSize(.small)
            }
            Spacer(minLength: 0)
        }.padding(14).background(color.opacity(0.08), in: RoundedRectangle(cornerRadius: 10))
    }
}

private struct SettingNumberField: View {
    @Binding var value: Double
    let spec: SettingSpec
    let title: String
    let scale: Double
    let suffix: String
    @State private var draft = ""
    @State private var invalid = false
    @State private var savedDraft = ""
    @FocusState private var focused: Bool

    var body: some View {
        VStack(alignment: .trailing, spacing: 3) {
            HStack(spacing: 4) {
                TextField(title, text: $draft)
                    .textFieldStyle(.roundedBorder).multilineTextAlignment(.trailing)
                    .monospacedDigit().font(.system(size: 12)).frame(width: 115)
                    .focused($focused).onSubmit { commit() }
                    .onExitCommand { refresh(); focused = false }
                    .accessibilityLabel(title).help("Enter a decimal value, then press Return or leave the field to save.")
                if !suffix.isEmpty { Text(suffix).font(.system(size: 11)).foregroundStyle(.secondary) }
            }
            if invalid {
                Text(spec.inputHint).font(.system(size: 10)).foregroundStyle(.red)
            }
        }
        .onAppear { refresh() }
        .onChange(of: focused) { _, editing in if !editing { commit() } }
        .onChange(of: value) { _, _ in refresh() }
    }
    private func refresh() {
        draft = spec.text(for: value, scale: scale)
        savedDraft = draft
        invalid = false
    }
    private func commit() {
        guard draft != savedDraft else { invalid = false; return }
        guard let parsed = spec.parse(draft, scale: scale) else { invalid = true; return }
        value = parsed
        savedDraft = draft
        invalid = false
    }
}

private struct SlidePreview: View {
    let duration: Double
    @State private var secondSpace = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    var body: some View {
        HStack(spacing: 22) {
            GeometryReader { geometry in
                HStack(spacing: 0) {
                    desktop("1", color: accent)
                    desktop("2", color: Color(red: 0.20, green: 0.57, blue: 0.56))
                }.frame(width: geometry.size.width * 2)
                    .offset(x: secondSpace ? -geometry.size.width : 0)
            }.frame(width: 180, height: 94).clipShape(RoundedRectangle(cornerRadius: 10))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(.primary.opacity(0.1)))
                .accessibilityLabel("Slide preview, space \(secondSpace ? 2 : 1)")
            VStack(alignment: .leading, spacing: 9) {
                Text("Make your next move.").font(.system(size: 15, weight: .semibold))
                Text("Preview the slide timing here. Your desktop stays put.")
                    .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Button("Try Slide", systemImage: "play.fill") {
                    withAnimation(reduceMotion ? nil : .linear(duration: duration)) { secondSpace.toggle() }
                }.controlSize(.small)
            }
            Spacer(minLength: 0)
        }.padding(17).frame(maxWidth: .infinity, alignment: .leading)
            .background(accent.opacity(0.055), in: RoundedRectangle(cornerRadius: 13))
    }
    private func desktop(_ number: String, color: Color) -> some View {
        ZStack {
            Rectangle().fill(color.gradient)
            RoundedRectangle(cornerRadius: 5).fill(.white.opacity(0.9))
                .frame(width: 103, height: 60).shadow(color: .black.opacity(0.12), radius: 4, y: 3)
            VStack(spacing: 6) {
                Capsule().fill(color.opacity(0.25)).frame(width: 70, height: 4)
                Capsule().fill(color.opacity(0.13)).frame(width: 70, height: 4)
                Capsule().fill(color.opacity(0.13)).frame(width: 45, height: 4).frame(width: 70, alignment: .leading)
            }
            Text(number).font(.system(size: 9, weight: .bold)).foregroundStyle(.white)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing).padding(8)
        }.frame(width: 180, height: 94)
    }
}
