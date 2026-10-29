#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../runtime_watch.h"

static void drain(RuntimeWatch *watch) {
    for (int i = 0; i < 20 && !watch->settings_dirty && !watch->stop_requested; i++)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .025, true);
}
int main(void) {
    char folder[] = "/tmp/iss-watch-XXXXXX";
    assert(mkdtemp(folder));
    char path[PATH_MAX], temporary[PATH_MAX];
    snprintf(path, sizeof(path), "%s/settings.plist", folder);
    snprintf(temporary, sizeof(temporary), "%s/new.plist", folder);
    FILE *file = fopen(path, "w"); assert(file); fputs("first", file); fclose(file);
    RuntimeWatch watch;
    assert(runtime_watch_open(&watch, path, 0, false));
    assert(watch.settings_dirty);
    watch.settings_dirty = false;
    // No timer or periodic wake when idle.
    assert(CFRunLoopRunInMode(kCFRunLoopDefaultMode, .04, true) == kCFRunLoopRunTimedOut);
    assert(!watch.settings_dirty && !watch.stop_requested);
    for (int i = 0; i < 3; i++) {
        file = fopen(temporary, "w"); assert(file); fprintf(file, "revision %d", i); fclose(file);
        assert(rename(temporary, path) == 0);
        drain(&watch); assert(watch.settings_dirty);
        watch.settings_dirty = false;
    }
    runtime_watch_close(&watch);
    // Termination must wake an otherwise idle run loop immediately.
    assert(runtime_watch_open(&watch, path, 0, true));
    watch.settings_dirty = false;
    assert(kill(getpid(), SIGTERM) == 0);
    drain(&watch); assert(watch.stop_requested);
    runtime_watch_close(&watch);
    unlink(path); rmdir(folder);
    puts("runtime notification tests passed");
}
