#include <assert.h>
#include <unistd.h>
#include <string.h>
#include "../settings_io.h"
#include "../swipe.h"
#include "../slide.h"

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    assert(file); assert(fputs(text, file) >= 0); assert(fclose(file) == 0);
}
static void near(double a, double b) { assert(fabs(a - b) < 1e-8); }
int main(void) {
    char path[] = "/tmp/iss-settings-test-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    ISSSettings result = iss_defaults;
    write_file(path, "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>slideSeconds</key><real>0.12</real>"
        "<key>spaceDistance</key><real>0.3</real>"
        "<key>initialSeconds</key><real>0</real>"
        "<key>initialDistance</key><real>0.02</real>"
        "<key>reverseDistance</key><real>0.08</real></dict></plist>");
    assert(settings_read(path, &result));
    near(result.slideSeconds, .12); near(result.spaceDistance, .3);
    near(result.centerSeconds, .15);
    iss_settings = result;
    // Actual animation and reducer behavior use the loaded settings.
    near(slide_position(0, 1.5, .06), .75);
    Swipe s = {0};
    SwipeEvent e = {.phase=SwipeBegin};
    swipe_begin(&s, e, 1, 4, 0);
    e = (SwipeEvent){.phase=SwipeChange, .progress=.019};
    assert(swipe_move(&s, e, .01, 1, true).phase == SwipeChange);
    e.progress = .021;
    assert(swipe_move(&s, e, .02, 1, true).phase == SwipeEnd);
    assert(s.target == 2);
    s = (Swipe){.last_direction=1, .pending_direction=1, .pending=.4};
    swipe_accumulate_pending(&s, -.05);
    assert(s.pending_direction == 1);
    swipe_accumulate_pending(&s, -.04);
    assert(s.pending_direction == -1); near(s.pending, -.09);

    // Invalid domains/types fall back; valid decimals are not slider-clamped.
    write_file(path, "<plist version=\"1.0\"><dict>"
        "<key>slideSeconds</key><real>-1</real>"
        "<key>spaceDistance</key><real>99</real>"
        "<key>reverseDistance</key><string>oops</string>"
        "<key>initialSeconds</key><true/>"
        "<key>flickVelocity</key><real>nan</real>"
        "<key>futureSetting</key><real>123</real></dict></plist>");
    assert(settings_read(path, &result));
    near(result.slideSeconds, .06); near(result.spaceDistance, 99);
    near(result.reverseDistance, .04); near(result.initialSeconds, .04);
    near(result.flickVelocity, .08);
    write_file(path, "<plist><array/></plist>");
    assert(!settings_read(path, &result)); near(result.slideSeconds, .06);
    write_file(path, "broken plist");
    assert(!settings_read(path, &result)); near(result.slideSeconds, .06);
    write_file(path, "<plist><dict>"
        "<key>slideSeconds</key><real>0.060125</real>"
        "<key>gestureMultiplier</key><real>7.125</real>"
        "<key>gestureAcceleration</key><real>1.375</real>"
        "<key>releaseOnly</key><real>1</real>"
        "<key>instantFocus</key><real>0.5</real></dict></plist>");
    assert(settings_read(path, &result));
    near(result.slideSeconds, .060125); near(result.gestureMultiplier, 7.125);
    near(result.gestureAcceleration, 1.375);
    near(result.releaseOnly, 1); near(result.instantFocus, 1);
    assert(!settings_value_valid(INFINITY, ISSPositive));
    assert(!settings_value_valid(0, ISSPositive));
    assert(settings_value_valid(0, ISSNonnegative));
    unlink(path); assert(!settings_read(path, &result));
    iss_settings = iss_defaults;
    near(slide_position(0, 1.5, .06), 1.5);
    puts("settings tests passed");
}
