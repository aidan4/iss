#ifndef ISS_IOHID_H
#define ISS_IOHID_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

static inline uint32_t hid_u32(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static inline void hid_put32(uint8_t *p, uint32_t v) { memcpy(p,&v,4); }
static inline int32_t hid_fixed(double v) {
    // IOHID is signed 16.16, unlike the outer CGEvent float. Never overflow
    // a 999999 release into a small or opposite-signed velocity.
    return (int32_t)(fmax(-32767, fmin(32767, v)) * 65536);
}

// Synchronize the existing native queue record, preserving its sender/device
// metadata. Returns a retained replacement only when a raw payload was found.
// A callback returning that replacement transfers ownership to CoreGraphics.
static inline CGEventRef sync_native_payload(CGEventRef ev, double input_progress,
                                             double input_velocity, double *raw_sign) {
    CFDataRef data = CGEventCreateData(NULL, ev);
    if (!data) return NULL;
    const uint8_t *bytes = CFDataGetBytePtr(data);
    size_t length = (size_t)CFDataGetLength(data), offset = 4, payload = 0, size = 0;
    if (length < 4 || memcmp(bytes, "\0\0\0\2", 4)) goto fail;
    while (offset + 4 <= length) {
        size_t count = ((size_t)bytes[offset]<<8) | bytes[offset+1];
        unsigned tag = ((unsigned)bytes[offset+2]<<8) | bytes[offset+3];
        unsigned format = tag & 0xf000;
        size_t unit = format == 0x1000 ? 1 : format == 0 ? 8 :
                      (format == 0x4000 || format == 0xc000) ? 4 : 0;
        if (!unit || count > (length-offset-4)/unit) goto fail;
        size = count * unit;
        if (tag == 4205) { payload = offset + 4; break; }
        offset += 4 + size;
    }
    if (!payload || size < 28) goto fail;
    const uint8_t *queue = bytes + payload;
    uint32_t attributes = hid_u32(queue+20), count = hid_u32(queue+24);
    if (attributes > size-28) goto fail;
    size_t at = 28 + attributes, fluid = 0, velocity = 0;
    for (uint32_t i=0; i<count; i++) {
        if (at > size || size-at < 16) goto fail;
        uint32_t n = hid_u32(queue+at), type = hid_u32(queue+at+4);
        if (n < 16 || n > size-at) goto fail;
        if (type == 23 && n >= 40) fluid = at;
        if (type == 9 && n >= 28) velocity = at;
        at += n;
    }
    if (!fluid || at != size) goto fail;
    int32_t original_progress; memcpy(&original_progress, queue+fluid+36, 4);
    if (original_progress && input_progress)
        *raw_sign = original_progress * input_progress > 0 ? 1 : -1;
    else if (velocity && input_velocity) {
        int32_t original_velocity; memcpy(&original_velocity, queue+velocity+16, 4);
        if (original_velocity) *raw_sign = original_velocity * input_velocity > 0 ? 1 : -1;
    }
    double vx = CGEventGetDoubleValueField(ev, (CGEventField)129);
    double vy = CGEventGetDoubleValueField(ev, (CGEventField)130);
    bool append = !velocity && (vx != 0 || vy != 0 || CGEventGetIntegerValueField(ev, (CGEventField)132) == 4);
    if (append && size > 65535-28) goto fail;
    CFMutableDataRef edited = CFDataCreateMutableCopy(NULL, 0, data);
    if (!edited) goto fail;
    if (append) {
        uint8_t record[28] = {0};
        hid_put32(record,28); hid_put32(record+4,9); record[12]=1;
        CFDataReplaceBytes(edited, CFRangeMake((CFIndex)(payload+size),0), record,28);
        velocity=size; size+=28;
    }
    uint8_t *out = CFDataGetMutableBytePtr(edited), *q = out+payload;
    out[payload-4]=(uint8_t)(size>>8); out[payload-3]=(uint8_t)size;
    uint64_t timestamp = CGEventGetTimestamp(ev);
    memcpy(q, &timestamp, sizeof(timestamp));
    if (append) hid_put32(q+24,count+1);
    uint32_t options=hid_u32(q+fluid+8);
    options=(options & 0x00ffffff) | ((uint32_t)CGEventGetIntegerValueField(ev,(CGEventField)132)<<24);
    hid_put32(q+fluid+8,options);
    hid_put32(q+fluid+36,(uint32_t)hid_fixed(CGEventGetDoubleValueField(ev,(CGEventField)124)* *raw_sign));
    if (velocity) {
        hid_put32(q+velocity+16,(uint32_t)hid_fixed(vx * *raw_sign));
        hid_put32(q+velocity+20,(uint32_t)hid_fixed(vy * *raw_sign));
    }
    CGEventRef result=CGEventCreateFromData(NULL,edited);
    CFRelease(edited); CFRelease(data);
    return result;
fail:
    CFRelease(data);
    return NULL;
}
#endif
