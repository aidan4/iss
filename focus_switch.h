#ifndef ISS_FOCUS_SWITCH_H
#define ISS_FOCUS_SWITCH_H
#include <stdbool.h>
#include <stdint.h>
// Everything runs on the same CFRunLoop as the gesture adapter. No idle timer.
bool focus_switch_start(bool augment, int64_t event_tag, bool (*gesture_busy)(void), bool verbose);
void focus_switch_enable(bool enabled);
void focus_switch_cancel(void);
void focus_switch_stop(void);
bool focus_switch_available(void);
#endif
