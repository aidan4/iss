import Foundation

@main
struct SettingsModelTests {
    @MainActor static func main() async throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: folder) }
        let config = folder.appendingPathComponent("settings.plist")
        let schema = URL(fileURLWithPath: CommandLine.arguments[1])
        let model = try SettingsModel(configURL: config, schemaURL: schema)
        assert(model.values["slideSeconds"] == 0.06)
        model.set("slideSeconds", 0.105)
        model.set("reverseDistance", 0.055)
        try await Task.sleep(nanoseconds: 350_000_000)
        let reopened = try SettingsModel(configURL: config, schemaURL: schema)
        assert(reopened.values["slideSeconds"] == 0.105)
        assert(reopened.values["reverseDistance"] == 0.055)
        assert(reopened.values["spaceDistance"] == 0.16)
        model.set("slideSeconds", .infinity)
        assert(model.values["slideSeconds"] == 0.105)
        model.set("slideSeconds", -100)
        assert(model.values["slideSeconds"] == 0.02)
        model.set("spaceDistance", 999)
        assert(model.values["spaceDistance"] == 0.6)
        model.restoreDefaults()
        let reset = try SettingsModel(configURL: config, schemaURL: schema)
        assert(reset.values == reset.schema.mapValues(\.defaultValue))
        // Corrupt and older-version individual fields recover without losing
        // valid fields. Booleans must not become numeric slider values.
        let data = try PropertyListSerialization.data(fromPropertyList: [
            "slideSeconds": 99, "initialSeconds": true, "spaceDistance": "invalid", "futureSetting": 1
        ], format: .binary, options: 0)
        try data.write(to: config, options: .atomic)
        let migrated = try SettingsModel(configURL: config, schemaURL: schema)
        assert(migrated.values["slideSeconds"] == 0.3)
        assert(migrated.values["initialSeconds"] == 0.04)
        assert(migrated.values["spaceDistance"] == 0.16)
        let untouched = try Data(contentsOf: config)
        let preview = try SettingsModel(preview: true, configURL: config, schemaURL: schema)
        preview.set("slideSeconds", 0.2)
        preview.restoreDefaults()
        preview.shutdown()
        let afterPreview = try Data(contentsOf: config)
        assert(afterPreview == untouched)
        model.shutdown()
        print("settings app persistence tests passed")

        let helper = folder.appendingPathComponent("fake-engine")
        try "#!/bin/sh\nprintf '%s' \"$$\" > \"$2.pid\"\necho 'iss: swipe active' >&2\nexec /bin/sleep 30\n".write(to: helper, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: helper.path)
        let domain = "com.instant-swipe.tests.\(UUID().uuidString)"
        let preferences = UserDefaults(suiteName: domain)!
        defer { preferences.removePersistentDomain(forName: domain) }
        let environment = EngineEnvironment(executable: helper,
            legacyAgent: folder.appendingPathComponent("absent.plist"), trusted: { true }, preferences: preferences)
        let runtime = try SettingsModel(configURL: config, schemaURL: schema, environment: environment)
        runtime.start()
        await wait { runtime.isRunning }
        let pidFile = URL(fileURLWithPath: config.path + ".pid")
        let firstPID = Int32(try String(contentsOf: pidFile, encoding: .utf8))!
        runtime.setEnabled(false)
        await wait { runtime.status == "Paused" }
        assert(kill(firstPID, 0) == -1)
        runtime.setEnabled(true)
        await wait { runtime.isRunning }
        let secondPID = Int32(try String(contentsOf: pidFile, encoding: .utf8))!
        assert(firstPID != secondPID)
        // Rapid pause/resume must wait for the previous child to exit.
        runtime.setEnabled(false)
        runtime.setEnabled(true)
        await wait { runtime.isRunning }
        assert(kill(secondPID, 0) == -1)
        let finalPID = Int32(try String(contentsOf: pidFile, encoding: .utf8))!
        runtime.shutdown()
        await wait { kill(finalPID, 0) == -1 }
        print("settings app lifecycle tests passed")
    }
    @MainActor static func wait(until predicate: () -> Bool) async {
        for _ in 0..<80 {
            if predicate() { return }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        fatalError("Timed out waiting for engine lifecycle")
    }
}
