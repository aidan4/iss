import AppKit
import SwiftUI
import ApplicationServices
import ServiceManagement

struct SettingSpec: Decodable {
    let defaultValue: Double
    let minimum: Double
    let maximum: Double
    enum Domain: String, Decodable { case positive, nonnegative, toggle }
    let domain: Domain
    func accepts(_ value: Double) -> Bool {
        guard value.isFinite else { return false }
        switch domain {
        case .positive: return value > 0
        case .nonnegative: return value >= 0
        case .toggle: return value == 0 || value == 1
        }
    }
    func parse(_ text: String, scale: Double = 1, locale: Locale = .current) -> Double? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let normalized = locale.decimalSeparator == "," ? trimmed.replacingOccurrences(of: ",", with: ".") : trimmed
        guard scale.isFinite && scale > 0 else { return nil }
        // Shift the exponent for milliseconds before parsing, so conversion
        // neither overflows large durations nor loses tiny decimal values.
        let value: Double?
        if scale == 1000 { value = shifted(normalized, by: -3).flatMap(Double.init) }
        else { value = Double(normalized).map { $0 / scale } }
        guard let value, accepts(value) else { return nil }
        return value
    }
    // Only slider input is quantized. Typed decimals retain their precision.
    func sliderValue(_ value: Double, scale: Double = 1) -> Double {
        let digits = scale == 1000 ? 0 : maximum > 1 ? 2 : 4
        let displayed = min(maximum, max(minimum, value)) * scale
        let text = String(format: "%.*f", locale: Locale(identifier: "en_US_POSIX"), digits, displayed)
        return parse(text, scale: scale, locale: Locale(identifier: "en_US_POSIX")) ?? defaultValue
    }
    func text(for value: Double, scale: Double = 1, locale: Locale = .current) -> String {
        let displayed = value * scale
        let text: String
        if !displayed.isFinite { text = shifted(String(value), by: 3)! }
        else if scale == 1000 && parse(String(displayed.rounded()), scale: scale) == value {
            text = String(format: "%.0f", locale: Locale(identifier: "en_US_POSIX"), displayed)
        } else { text = String(displayed) }
        return locale.decimalSeparator == "," ? text.replacingOccurrences(of: ".", with: ",") : text
    }
    private func shifted(_ text: String, by shift: Int) -> String? {
        let parts = text.lowercased().split(separator: "e", omittingEmptySubsequences: false)
        guard !parts.isEmpty, parts.count <= 2, let exponent = parts.count == 2 ? Int(parts[1]) : 0 else { return nil }
        let adjusted = exponent.addingReportingOverflow(shift)
        guard !adjusted.overflow else { return nil }
        return "\(parts[0])e\(adjusted.partialValue)"
    }
    var inputHint: String { domain == .nonnegative ? "Enter zero or a positive number." : "Enter a positive number." }
}

enum SettingsPage: String, CaseIterable, Identifiable {
    case swiping = "Swiping", longDrag = "Long drags", general = "General"
    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .swiping: return "rectangle.on.rectangle"
        case .longDrag: return "hand.draw"
        case .general: return "gearshape"
        }
    }
    var subtitle: String {
        switch self {
        case .swiping: return "A little motion. A much quicker landing."
        case .longDrag: return "Find the rhythm for moving through your spaces."
        case .general: return "Keep ISS close, and ready when you are."
        }
    }
}

// System dependencies are kept together so lifecycle tests can use a harmless
// child process and isolated preferences, without installing an event tap.
@MainActor
struct EngineEnvironment {
    var executable: URL? = Bundle.main.executableURL?.deletingLastPathComponent().appendingPathComponent("iss-engine")
    var legacyAgent = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/LaunchAgents/com.instant-swipe.plist")
    var trusted: () -> Bool = { AXIsProcessTrusted() }
    var preferences: UserDefaults = .standard
}

@MainActor
final class SettingsModel: ObservableObject {
    let schema: [String: SettingSpec]
    let configURL: URL
    let preview: Bool
    @Published private(set) var values: [String: Double]
    @Published private(set) var enabled: Bool
    @Published private(set) var status = "Starting…"
    @Published private(set) var isRunning = false
    @Published private(set) var accessibility = false
    @Published private(set) var loginEnabled = false
    @Published private(set) var loginNeedsApproval = false
    @Published var error: String?
    @Published var page: SettingsPage = .swiping
    @Published private(set) var log = ""
    var onStatusChange: (() -> Void)?
    private let environment: EngineEnvironment
    private var process: Process?
    private var pipe: Pipe?
    private var permissionRetry: Timer?
    private var activationObserver: NSObjectProtocol?
    private var lastSavedValues: [String: Double]?
    private var saveTask: Task<Void, Never>?
    private var quitting = false
    private var stopping = false
    private var outputBuffer = ""
    private var runID = UUID()

    init(preview: Bool = false, configURL: URL? = nil, schemaURL: URL? = nil, environment: EngineEnvironment? = nil) throws {
        let environment = environment ?? EngineEnvironment()
        self.environment = environment
        accessibility = environment.trusted()
        self.preview = preview
        guard let schemaURL = schemaURL ?? Bundle.main.url(forResource: "settings-schema", withExtension: "json") else {
            throw CocoaError(.fileNoSuchFile)
        }
        schema = try JSONDecoder().decode([String: SettingSpec].self, from: Data(contentsOf: schemaURL))
        self.configURL = configURL ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("ISS/settings.plist")
        var loaded = schema.mapValues(\.defaultValue)
        if !preview && FileManager.default.fileExists(atPath: self.configURL.path) {
            let plist = try PropertyListSerialization.propertyList(from: Data(contentsOf: self.configURL), format: nil)
            guard let dictionary = plist as? [String: Any] else { throw CocoaError(.propertyListReadCorrupt) }
            for (key, spec) in schema {
                if let number = dictionary[key] as? NSNumber,
                   CFGetTypeID(number) != CFBooleanGetTypeID(), spec.accepts(number.doubleValue) {
                    loaded[key] = number.doubleValue
                }
            }
        }
        values = loaded
        enabled = preview || (environment.preferences.object(forKey: "switcherEnabled") as? Bool ?? true)
        if preview { status = "Preview"; accessibility = true }
    }

    func begin() {
        guard !preview else { return }
        refreshSystemStatus()
        if enabled { start() } else { setStatus("Paused") }
        activationObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { [weak self] _ in Task { @MainActor in self?.refreshSystemStatus() } }
        updatePermissionRetry()
    }
    func binding(_ key: String) -> Binding<Double> {
        Binding(get: { self.values[key] ?? self.schema[key]!.defaultValue }, set: { self.set(key, $0) })
    }
    func set(_ key: String, _ value: Double) {
        guard let spec = schema[key], spec.accepts(value), values[key] != value else { return }
        values[key] = value
        saveTask?.cancel()
        saveTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 180_000_000)
            guard !Task.isCancelled else { return }
            self?.saveReportingError()
        }
    }
    func restoreDefaults() {
        saveTask?.cancel()
        let defaults = schema.mapValues(\.defaultValue)
        if values != defaults { values = defaults }
        saveReportingError()
    }
    private func save() throws {
        guard !preview, lastSavedValues != values else { return }
        let data = try PropertyListSerialization.data(fromPropertyList: values, format: .xml, options: 0)
        try FileManager.default.createDirectory(at: configURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: configURL, options: .atomic)
        lastSavedValues = values
    }
    private func saveReportingError() {
        do { try save() }
        catch { self.error = "Couldn’t save settings: \(error.localizedDescription)" }
    }
    private func setStatus(_ text: String, running: Bool = false) {
        guard status != text || isRunning != running else { return }
        if status != text { status = text }
        if isRunning != running { isRunning = running }
        onStatusChange?()
    }
    func setEnabled(_ value: Bool) {
        guard enabled != value else { return }
        enabled = value
        if !preview { environment.preferences.set(value, forKey: "switcherEnabled") }
        error = nil
        if value { start() } else { stop() }
        updatePermissionRetry()
    }
    func start() {
        guard !preview, !quitting, process == nil else { return }
        if FileManager.default.fileExists(atPath: environment.legacyAgent.path) {
            setStatus("Background service installed")
            error = "Remove the old background service before using the app: run make uninstall in the ISS project folder, then click Retry. Your swipe settings will be kept."
            return
        }
        guard accessibility else { setStatus("Accessibility needed"); return }
        do {
            try save()
            guard let executable = environment.executable,
                  FileManager.default.isExecutableFile(atPath: executable.path) else {
                throw CocoaError(.fileNoSuchFile)
            }
            let child = Process()
            let output = Pipe()
            child.executableURL = executable
            child.arguments = ["--config", configURL.path, "--managed"]
            child.standardError = output
            child.standardOutput = FileHandle.nullDevice
            child.standardInput = FileHandle.nullDevice
            log = ""; outputBuffer = ""
            let id = UUID(); runID = id
            output.fileHandleForReading.readabilityHandler = { [weak self] handle in
                let data = handle.availableData
                guard !data.isEmpty else { handle.readabilityHandler = nil; return }
                let text = String(decoding: data, as: UTF8.self)
                Task { @MainActor in self?.received(text, id: id) }
            }
            child.terminationHandler = { [weak self] child in
                let code = child.terminationStatus
                Task { @MainActor in self?.exited(code, id: id) }
            }
            process = child; pipe = output
            setStatus("Starting…")
            do { try child.run() }
            catch {
                output.fileHandleForReading.readabilityHandler = nil
                process = nil; pipe = nil
                throw error
            }
        } catch {
            self.error = "Couldn’t start ISS: \(error.localizedDescription)"
            setStatus("Unable to start")
        }
    }
    private func received(_ text: String, id: UUID) {
        guard id == runID else { return }
        log = String((log + text).suffix(16000))
        guard !stopping, !quitting, process?.isRunning == true else { return }
        outputBuffer += text
        while let end = outputBuffer.firstIndex(of: "\n") {
            let line = String(outputBuffer[..<end])
            outputBuffer.removeSubrange(...end)
            if line.contains("swipe active") || line.contains("Accessibility restored") {
                setStatus("Active", running: true)
            } else if line.contains("Accessibility removed") { setStatus("Accessibility needed") }
        }
        outputBuffer = String(outputBuffer.suffix(16000))
    }
    private func exited(_ code: Int32, id: UUID) {
        guard id == runID else { return }
        pipe?.fileHandleForReading.readabilityHandler = nil
        pipe = nil; process = nil
        guard !quitting else { return }
        if stopping {
            stopping = false
            setStatus("Paused")
            if enabled { start() }
        } else if code == 73 {
            setStatus("Another ISS is running")
            error = "Stop the Terminal copy of ISS first. If you installed the old background service with make install, run make uninstall in the project folder, then click Retry."
        } else {
            let trusted = environment.trusted()
            if accessibility != trusted { accessibility = trusted }
            updatePermissionRetry()
            setStatus(accessibility ? "Stopped unexpectedly" : "Accessibility needed")
            error = code == 1
                ? "Allow ISS in System Settings → Privacy & Security → Accessibility, then click Retry. If iss-engine is listed separately, allow it too."
                : "The switcher stopped (code \(code)). Click Retry to start it again."
        }
    }
    private func stop() {
        guard let child = process else { setStatus("Paused"); return }
        stopping = true
        setStatus("Pausing…")
        if child.isRunning { child.terminate() }
    }
    func retry() {
        error = nil
        accessibility = environment.trusted()
        if process != nil { stop() } else if enabled { start() }
    }
    func requestAccessibility() {
        guard !preview else { return }
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        _ = AXIsProcessTrustedWithOptions(options)
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!)
    }
    private func updatePermissionRetry() {
        if preview || accessibility || !enabled {
            permissionRetry?.invalidate(); permissionRetry = nil
        } else if permissionRetry == nil {
            // Poll only during permission setup. The normal running app has
            // no repeating timer; opening its UI refreshes system status.
            permissionRetry = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.refreshSystemStatus() }
            }
            permissionRetry?.tolerance = 0.5
        }
    }
    func refreshSystemStatus() {
        guard !preview else { return }
        let previouslyTrusted = accessibility
        let trusted = environment.trusted()
        if accessibility != trusted { accessibility = trusted }
        let state = SMAppService.mainApp.status
        let login = state == .enabled || state == .requiresApproval
        let approval = state == .requiresApproval
        if loginEnabled != login { loginEnabled = login }
        if loginNeedsApproval != approval { loginNeedsApproval = approval }
        updatePermissionRetry()
        if !accessibility && enabled { setStatus("Accessibility needed") }
        if accessibility && !previouslyTrusted && enabled {
            if process == nil { error = nil; start() } else { setStatus("Active", running: true) }
        }
    }
    func setLogin(_ value: Bool) {
        guard !preview else { return }
        do {
            if value { try SMAppService.mainApp.register() }
            else { try SMAppService.mainApp.unregister() }
        } catch { self.error = "Couldn’t change launch at login: \(error.localizedDescription)" }
        refreshSystemStatus()
    }
    func openLoginSettings() { SMAppService.openSystemSettingsLoginItems() }
    func revealConfig() { NSWorkspace.shared.activateFileViewerSelecting([configURL]) }
    func copyDiagnostics() {
        let report = "ISS 1.0\nmacOS \(ProcessInfo.processInfo.operatingSystemVersionString)\nStatus: \(status)\nAccessibility: \(accessibility)\nSettings: \(values)\n\n\(log)"
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(report, forType: .string)
    }
    func shutdown() {
        quitting = true; saveTask?.cancel(); permissionRetry?.invalidate()
        if let activationObserver { NotificationCenter.default.removeObserver(activationObserver) }
        saveReportingError()
        if let process, process.isRunning { process.terminate() }
        // The engine finishes its current slide and removes its tap on SIGTERM.
        // --managed also exits if this app dies without reaching shutdown.
    }
}
