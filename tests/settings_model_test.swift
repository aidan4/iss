import Foundation
import Combine

@main
struct SettingsModelTests {
    @MainActor static func main() async throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: folder) }
        let config = folder.appendingPathComponent("settings.plist")
        let schema = URL(fileURLWithPath: CommandLine.arguments[1])
        let model = try SettingsModel(configURL: config, schemaURL: schema)
        assert(model.values["slideSeconds"] == 0.06)
        assert(model.values["gestureAcceleration"] == 0)
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
        assert(model.values["slideSeconds"] == 0.105)
        model.set("spaceDistance", 999)
        assert(model.values["spaceDistance"] == 999)
        let slide = model.schema["slideSeconds"]!
        assert(slide.sliderValue(0.060817314, scale: 1000) == 0.061)
        assert(slide.text(for: 0.061, scale: 1000) == "61")
        let gain = model.schema["gestureMultiplier"]!
        assert(gain.sliderValue(2.374928118) == 2.37)
        let distance = model.schema["initialDistance"]!
        assert(distance.sliderValue(0.062543218) == 0.0625)
        assert(distance.parse("0.062543218") == 0.062543218)
        // Slider formatting must not expose binary artifacts after unit conversion.
        for ms in 20...300 {
            let value = slide.sliderValue(Double(ms)/1000, scale: 1000)
            assert(slide.text(for: value, scale: 1000) == String(ms))
        }
        assert(abs(slide.parse("60.125", scale: 1000)! - 0.060125) < 1e-12)
        assert(abs(slide.parse("60,125", scale: 1000, locale: Locale(identifier: "de_DE"))! - 0.060125) < 1e-12)
        for invalid in ["", "-", "oops", "nan", "inf", "-1", "0"] { assert(slide.parse(invalid) == nil) }
        for duration in [0.060125, 0.550125, 1e308, Double.leastNonzeroMagnitude] {
            assert(slide.parse(slide.text(for: duration, scale: 1000), scale: 1000) == duration)
        }
        assert(slide.parse("1e9999999999999999999999", scale: 1000) == nil)
        assert(model.schema["initialSeconds"]!.parse("0") == 0)
        assert(model.schema["releaseOnly"]!.parse("0.5") == nil)
        model.set("slideSeconds", slide.parse("550.125", scale: 1000)!)
        model.set("gestureMultiplier", 7.125)
        model.set("gestureAcceleration", 1.375)
        model.set("gestureAcceleration", -1)
        assert(model.values["gestureAcceleration"] == 1.375)
        assert(model.schema["gestureAcceleration"]!.parse("0") == 0)
        assert(model.schema["gestureAcceleration"]!.sliderValue(1.374928) == 1.37)
        model.set("initialDistance", 0.06254321)
        model.set("releaseOnly", 1)
        try await Task.sleep(nanoseconds: 350_000_000)
        let decimals = try SettingsModel(configURL: config, schemaURL: schema)
        assert(decimals.values["slideSeconds"] == 0.550125)
        assert(decimals.values["gestureMultiplier"] == 7.125)
        assert(decimals.values["gestureAcceleration"] == 1.375)
        assert(decimals.values["initialDistance"] == 0.06254321)
        assert(decimals.values["releaseOnly"] == 1)
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
        assert(migrated.values["slideSeconds"] == 99)
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
        var publications = 0
        let subscription = runtime.objectWillChange.sink { publications += 1 }
        for _ in 0..<10 { runtime.refreshSystemStatus() }
        assert(publications == 0, "Unchanged status must not invalidate the UI")
        let before = try FileManager.default.attributesOfItem(atPath: config.path)[.modificationDate] as! Date
        runtime.set("slideSeconds", runtime.values["slideSeconds"]!)
        try await Task.sleep(nanoseconds: 350_000_000)
        let after = try FileManager.default.attributesOfItem(atPath: config.path)[.modificationDate] as! Date
        assert(before == after, "Unchanged settings must not rewrite the file")
        subscription.cancel()
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
