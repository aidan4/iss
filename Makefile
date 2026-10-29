PREFIX  ?= $(HOME)/.local
BIN      = iss
LABEL    = com.instant-swipe
AGENT_DIR = $(HOME)/Library/LaunchAgents
PLIST     = $(AGENT_DIR)/$(LABEL).plist

# Xcode 26.x's linker (TAPI 21.0.0) cannot parse the arm64e.x1 slices in the
# newer CommandLineTools MacOSX27 SDK, so pin to the newest SDK the active
# toolchain understands. Re-check after updating Xcode: if the default SDK's
# .tbd files link cleanly, this pin can be dropped.
SDKROOT ?= $(shell xcrun --sdk macosx26.5 --show-sdk-path 2>/dev/null || xcrun --sdk macosx --show-sdk-path 2>/dev/null)
CFLAGS  ?= -std=c11 -O3 -march=native -pipe -flto -Wall -Wextra -Wpedantic
ifneq ($(SDKROOT),)
CFLAGS  += -isysroot $(SDKROOT) -mmacosx-version-min=14.0
endif
LDFLAGS  = -framework ApplicationServices -framework CoreFoundation \
           -Wl,-dead_strip -Wl,-dead_strip_dylibs -Wl,-x

$(BIN): iss.c focus_switch.c focus_switch.h focus_event.h swipe.h native_event.h iohid.h slide.h settings.h settings_io.h runtime_watch.h Makefile
	$(CC) $(CFLAGS) -o $@ iss.c focus_switch.c $(LDFLAGS) -framework Carbon

test: tests/focus_layout_test.c focus_switch.c focus_switch.h tests/runtime_watch_test.c tests/focus_event_test.c runtime_watch.h focus_event.h tests/swipe_test.c tests/native_event_test.c tests/settings_test.c swipe.h native_event.h iohid.h slide.h settings.h settings_io.h
	$(CC) $(CFLAGS) -o /tmp/iss-swipe-test tests/swipe_test.c
	/tmp/iss-swipe-test
	$(CC) $(CFLAGS) -o /tmp/iss-native-event-test tests/native_event_test.c $(LDFLAGS)
	/tmp/iss-native-event-test
	$(CC) $(CFLAGS) -o /tmp/iss-settings-test tests/settings_test.c $(LDFLAGS)
	/tmp/iss-settings-test
	$(CC) $(CFLAGS) -o /tmp/iss-runtime-watch-test tests/runtime_watch_test.c $(LDFLAGS)
	/tmp/iss-runtime-watch-test
	$(CC) $(CFLAGS) -o /tmp/iss-focus-event-test tests/focus_event_test.c $(LDFLAGS)
	/tmp/iss-focus-event-test
	$(CC) $(CFLAGS) -o /tmp/iss-focus-layout-test tests/focus_layout_test.c $(LDFLAGS) -framework Carbon
	/tmp/iss-focus-layout-test

lint: iss.c
	cppcheck --enable=warning,style,performance --error-exitcode=1 iss.c

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)
	codesign -fs - $(PREFIX)/bin/$(BIN)
	xattr -d com.apple.quarantine $(PREFIX)/bin/$(BIN) 2>/dev/null || true
	mkdir -p $(AGENT_DIR)
	printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"' \
	  '  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0">' \
	  '<dict>' \
	  '  <key>Label</key>' \
	  '  <string>$(LABEL)</string>' \
	  '  <key>ProgramArguments</key>' \
	  '  <array>' \
	  '    <string>$(PREFIX)/bin/$(BIN)</string>' \
	  '  </array>' \
	  '  <key>RunAtLoad</key>' \
	  '  <true/>' \
	  '  <key>KeepAlive</key>' \
	  '  <true/>' \
	  '</dict>' \
	  '</plist>' > $(PLIST)
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true
	launchctl bootstrap gui/$$(id -u) $(PLIST)

uninstall:
	launchctl bootout gui/$$(id -u) $(PLIST) 2>/dev/null || true
	rm -f $(PREFIX)/bin/$(BIN) $(PLIST)

clean:
	rm -f $(BIN)
	rm -rf build

.PHONY: install uninstall clean lint test

APP = build/ISS.app
APP_INSTALL_DIR ?= $(HOME)/Applications
SWIFTC = xcrun swiftc -sdk $(SDKROOT) -target $(shell uname -m)-apple-macosx14.0 -swift-version 5 -module-cache-path build/module-cache

app: $(APP)/Contents/MacOS/ISS

build/make-icon: app/MakeIcon.swift
	mkdir -p build
	$(SWIFTC) -O -o $@ $< -framework AppKit

build/AppIcon.icns: build/make-icon
	build/make-icon build/AppIcon.iconset
	iconutil -c icns build/AppIcon.iconset -o $@

$(APP)/Contents/MacOS/ISS: app/App.swift app/SettingsView.swift app/SettingsModel.swift app/Info.plist THIRD_PARTY_NOTICES.md build/AppIcon.icns $(BIN) Makefile
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	$(SWIFTC) -O -parse-as-library -o $@ app/App.swift app/SettingsView.swift app/SettingsModel.swift -framework AppKit -framework SwiftUI -framework ServiceManagement -framework ApplicationServices
	cp app/Info.plist $(APP)/Contents/Info.plist
	cp THIRD_PARTY_NOTICES.md $(APP)/Contents/Resources/THIRD_PARTY_NOTICES.md
	cp build/AppIcon.icns $(APP)/Contents/Resources/AppIcon.icns
	cp $(BIN) $(APP)/Contents/MacOS/iss-engine
	./$(BIN) --settings-schema > $(APP)/Contents/Resources/settings-schema.json
	codesign --force --sign - --identifier com.instant-swipe.engine $(APP)/Contents/MacOS/iss-engine
	codesign --force --sign - --identifier com.instant-swipe.app $(APP)

install-app: app
	mkdir -p "$(APP_INSTALL_DIR)"
	ditto "$(APP)" "$(APP_INSTALL_DIR)/ISS.app"
	open "$(APP_INSTALL_DIR)/ISS.app"

.PHONY: app install-app

build/settings-model-test: tests/settings_model_test.swift app/SettingsModel.swift
	mkdir -p build
	$(SWIFTC) -Onone -parse-as-library -o $@ tests/settings_model_test.swift app/SettingsModel.swift -framework AppKit -framework SwiftUI -framework ServiceManagement -framework ApplicationServices

test-app: app build/settings-model-test
	build/settings-model-test $(APP)/Contents/Resources/settings-schema.json
	codesign --verify --deep --strict $(APP)
	plutil -lint $(APP)/Contents/Info.plist

.PHONY: test-app
