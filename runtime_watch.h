#ifndef ISS_RUNTIME_WATCH_H
#define ISS_RUNTIME_WATCH_H
#include <CoreFoundation/CoreFoundation.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

// A single run-loop source waits for edits, termination signals, and parent
// exit. Watching the directory survives the app's atomic file replacements.
typedef struct {
    int queue, directory;
    CFFileDescriptorRef descriptor;
    CFRunLoopSourceRef source;
    bool settings_dirty, stop_requested, signals;
    struct sigaction old_int, old_term;
} RuntimeWatch;

static void runtime_watch_callback(CFFileDescriptorRef descriptor, CFOptionFlags flags, void *context) {
    (void)flags;
    RuntimeWatch *watch = context;
    struct kevent events[8];
    struct timespec immediate = {0};
    int count;
    while ((count = kevent(watch->queue, NULL, 0, events, 8, &immediate)) > 0) {
        for (int i = 0; i < count; ++i) {
            if (events[i].filter == EVFILT_VNODE) watch->settings_dirty = true;
            if (events[i].filter == EVFILT_SIGNAL || events[i].filter == EVFILT_PROC)
                watch->stop_requested = true;
        }
    }
    CFFileDescriptorEnableCallBacks(descriptor, kCFFileDescriptorReadCallBack);
}
static void runtime_watch_close(RuntimeWatch *watch) {
    if (watch->source) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), watch->source, kCFRunLoopCommonModes);
        CFRelease(watch->source);
    }
    if (watch->descriptor) { CFFileDescriptorInvalidate(watch->descriptor); CFRelease(watch->descriptor); }
    if (watch->queue >= 0) close(watch->queue);
    if (watch->directory >= 0) close(watch->directory);
    if (watch->signals) {
        sigaction(SIGINT, &watch->old_int, NULL);
        sigaction(SIGTERM, &watch->old_term, NULL);
    }
    *watch = (RuntimeWatch){ .queue = -1, .directory = -1 };
}
static bool runtime_watch_open(RuntimeWatch *watch, const char *path, pid_t parent, bool signals) {
    *watch = (RuntimeWatch){ .queue = -1, .directory = -1 };
    watch->queue = kqueue();
    if (watch->queue < 0) return false;
    fcntl(watch->queue, F_SETFD, FD_CLOEXEC);
    struct kevent events[4];
    int count = 0;
    if (path) {
        char directory[PATH_MAX];
        if (strlen(path) >= sizeof(directory)) goto fail;
        strcpy(directory, path);
        char *slash = strrchr(directory, '/');
        if (!slash) strcpy(directory, ".");
        else if (slash == directory) slash[1] = '\0';
        else *slash = '\0';
        watch->directory = open(directory, O_EVTONLY | O_CLOEXEC);
        if (watch->directory < 0) goto fail;
        EV_SET(&events[count++], watch->directory, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
        watch->settings_dirty = true;
    }
    if (parent > 0) EV_SET(&events[count++], parent, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (signals) {
        struct sigaction ignore = { .sa_handler = SIG_IGN };
        sigemptyset(&ignore.sa_mask);
        sigaction(SIGINT, &ignore, &watch->old_int);
        sigaction(SIGTERM, &ignore, &watch->old_term);
        watch->signals = true;
        EV_SET(&events[count++], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
        EV_SET(&events[count++], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    }
    if (kevent(watch->queue, events, count, NULL, 0, NULL) < 0) goto fail;
    CFFileDescriptorContext context = { .info = watch };
    watch->descriptor = CFFileDescriptorCreate(NULL, watch->queue, false, runtime_watch_callback, &context);
    if (!watch->descriptor) goto fail;
    watch->source = CFFileDescriptorCreateRunLoopSource(NULL, watch->descriptor, 0);
    if (!watch->source) goto fail;
    CFRunLoopAddSource(CFRunLoopGetMain(), watch->source, kCFRunLoopCommonModes);
    CFFileDescriptorEnableCallBacks(watch->descriptor, kCFFileDescriptorReadCallBack);
    if (parent > 0 && getppid() != parent) watch->stop_requested = true;
    return true;
fail:
    runtime_watch_close(watch);
    return false;
}
#endif
