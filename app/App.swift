import AppKit
import SwiftUI

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var model: SettingsModel!
    private var window: NSWindow!
    private var statusItem: NSStatusItem!
    private var statusLine: NSMenuItem!
    private var toggleItem: NSMenuItem!
    private var loginItem: NSMenuItem!
    private var renderDirectory: String?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let args = CommandLine.arguments
        if let index = args.firstIndex(of: "--render-preview"), args.count > index + 1 { renderDirectory = args[index + 1] }
        let preview = args.contains("--preview") || renderDirectory != nil
        do { model = try SettingsModel(preview: preview) }
        catch {
            let alert = NSAlert()
            alert.messageText = "ISS couldn’t load its settings"
            alert.informativeText = error.localizedDescription
            alert.runModal()
            NSApp.terminate(nil)
            return
        }
        installApplicationMenu()
        let host = NSHostingView(rootView: SettingsView(model: model))
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 860, height: 760),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "ISS Settings"
        window.minSize = NSSize(width: 840, height: 740)
        window.contentView = host
        window.isReleasedWhenClosed = false
        window.center()
        if !preview { window.setFrameAutosaveName("ISSSettingsWindow") }
        buildStatusItem()
        model.onStatusChange = { [weak self] in self?.updateMenu() }
        model.begin()
        updateMenu()
        // Login launches stay in the menu bar; explicit launches and first run
        // show the settings window. Apple supplies this event for login items.
        let event = NSAppleEventManager.shared().currentAppleEvent
        let launchedAtLogin = event?.eventID == kAEOpenApplication &&
            event?.paramDescriptor(forKeyword: keyAEPropData)?.enumCodeValue == keyAELaunchedAsLogInItem
        if !launchedAtLogin || !model.accessibility || preview { showSettings() }
        if let directory = renderDirectory { renderPages(directory, pages: SettingsPage.allCases) }
    }

    private func installApplicationMenu() {
        let menu = NSMenu()
        let appItem = NSMenuItem()
        let submenu = NSMenu()
        let settings = NSMenuItem(title: "Settings…", action: #selector(showSettings), keyEquivalent: ",")
        settings.target = self; submenu.addItem(settings)
        submenu.addItem(.separator())
        submenu.addItem(withTitle: "Quit ISS", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = submenu; menu.addItem(appItem)
        let edit = NSMenuItem()
        let editMenu = NSMenu(title: "Edit")
        editMenu.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        edit.submenu = editMenu; menu.addItem(edit)
        NSApp.mainMenu = menu
    }
    private func buildStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem.button?.image = NSImage(systemSymbolName: "rectangle.on.rectangle", accessibilityDescription: "ISS")
        statusItem.button?.image?.isTemplate = true
        let menu = NSMenu(); menu.delegate = self
        statusLine = NSMenuItem(title: "ISS", action: nil, keyEquivalent: "")
        statusLine.isEnabled = false; menu.addItem(statusLine)
        menu.addItem(.separator())
        toggleItem = item("Pause ISS", #selector(toggle), menu: menu)
        item("Settings…", #selector(showSettings), key: ",", menu: menu)
        menu.addItem(.separator())
        loginItem = item("Launch at Login", #selector(toggleLogin), menu: menu)
        item("Accessibility Settings…", #selector(accessibility), menu: menu)
        menu.addItem(.separator())
        item("Quit ISS", #selector(quit), key: "q", menu: menu)
        statusItem.menu = menu
    }
    @discardableResult private func item(_ title: String, _ action: Selector, key: String = "", menu: NSMenu) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
        item.target = self; menu.addItem(item); return item
    }
    func menuWillOpen(_ menu: NSMenu) { updateMenu() }
    private func updateMenu() {
        statusLine.title = "ISS · \(model.status)"
        toggleItem.title = model.enabled ? "Pause ISS" : "Resume ISS"
        loginItem.state = model.loginEnabled ? .on : .off
        statusItem.button?.toolTip = "ISS — \(model.status)"
        statusItem.button?.appearsDisabled = !model.isRunning
    }
    @objc func showSettings() {
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
    @objc private func toggle() { model.setEnabled(!model.enabled); updateMenu() }
    @objc private func toggleLogin() { model.setLogin(!model.loginEnabled); updateMenu() }
    @objc private func accessibility() { model.requestAccessibility() }
    @objc private func quit() { NSApp.terminate(nil) }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showSettings(); return true
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationWillTerminate(_ notification: Notification) { model?.shutdown() }

    // Render the actual native settings views for local visual checks, with no
    // helper, permissions prompt, login registration, or settings writes.
    private func renderPages(_ directory: String, pages: [SettingsPage]) {
        guard let page = pages.first else { NSApp.terminate(nil); return }
        model.page = page
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) { [self] in
            guard let view = window.contentView,
                  let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) else {
                fputs("Unable to render settings view\n", stderr); NSApp.terminate(nil); return
            }
            view.cacheDisplay(in: view.bounds, to: bitmap)
            do {
                let url = URL(fileURLWithPath: directory, isDirectory: true)
                try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
                try bitmap.representation(using: .png, properties: [:])?.write(to: url.appendingPathComponent("\(page.id).png"))
            } catch { fputs("Preview failed: \(error)\n", stderr) }
            renderPages(directory, pages: Array(pages.dropFirst()))
        }
    }
}

@main
struct ISSApp {
    @MainActor static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.setActivationPolicy(.accessory)
        app.delegate = delegate
        withExtendedLifetime(delegate) { app.run() }
    }
}
