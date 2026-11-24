/* Reine HID-Shared-Memory-Ringpuffer-Logik (Pad/Touch/Analogsticks).
 *
 * Referenz: tools/azahar/src/core/hle/service/hid/hid.cpp
 * (UpdatePadCallback/UpdateAccelerometerCallback/UpdateGyroscopeCallback) und
 * hid.h (SharedMem-Layout). Die Funktionen hier schreiben ausschliesslich in
 * einen Host-Schattenpuffer und kennen weder Gast-Lock noch Kernel-Handles;
 * dadurch sind sie ohne Runtime testbar (tests/hid_ring_test.c). Die
 * Ereignis-Signalisierung melden sie als Bitmaske, der Aufrufer signalisiert
 * die OneShot-Events (Azahar signalisiert beide Pad/Touch-Events je Update,
 * Accelerometer/Gyroscope nur bei ihrem eigenen Update).
 *
 * Azahar schreibt nie die ganze 0x1000-Byte-Seite, sondern nur die Felder der
 * vier Ringe. HID_RING_RANGES nennt die Bereiche, die der Aufrufer in den
 * Gastpuffer kopieren darf.
 */
#ifndef HID_RING_H
#define HID_RING_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* Layout (Azahar hid.h): pad 0x000, touch 0x0A8, accel 0x108, gyro 0x158. */
#define HID_SHMEM_SIZE       0x1000u
#define HID_PAD_ENTRIES      8u
#define HID_TOUCH_ENTRIES    8u
#define HID_ACCEL_ENTRIES    8u
#define HID_GYRO_ENTRIES     32u

#define HID_PAD_TICKS        0x000u
#define HID_PAD_TICKS_PREV   0x008u
#define HID_PAD_INDEX        0x010u
#define HID_PAD_SLIDER       0x018u
#define HID_PAD_CURRENT      0x01Cu
#define HID_PAD_ENTRIES_OFF  0x028u   /* 8 x 16 Byte */

#define HID_TOUCH_TICKS       0x0A8u
#define HID_TOUCH_TICKS_PREV  0x0B0u
#define HID_TOUCH_INDEX       0x0B8u
#define HID_TOUCH_RAW         0x0C0u
#define HID_TOUCH_ENTRIES_OFF 0x0C8u  /* 8 x 8 Byte */

#define HID_ACC_TICKS        0x108u
#define HID_ACC_TICKS_PREV   0x110u
#define HID_ACC_INDEX        0x118u
#define HID_ACC_RAW          0x120u
#define HID_ACC_ENTRIES_OFF  0x128u   /* 8 x 6 Byte */

#define HID_GYRO_TICKS       0x158u
#define HID_GYRO_TICKS_PREV  0x160u
#define HID_GYRO_INDEX       0x168u
#define HID_GYRO_RAW         0x170u
#define HID_GYRO_ENTRIES_OFF 0x178u   /* 32 x 6 Byte */

/* Bereiche, die Azahar im Shared Memory aktualisiert (Ringfelder + Eintraege).
 * Nur diese werden zurueck in den Gastpuffer kopiert. */
typedef struct { uint16_t offset, length; } HidRingRange;
static const HidRingRange HID_RING_RANGES[] = {
    {HID_PAD_TICKS,   HID_TOUCH_TICKS - HID_PAD_TICKS},          /* pad   */
    {HID_TOUCH_TICKS, HID_ACC_TICKS - HID_TOUCH_TICKS},          /* touch */
    {HID_ACC_TICKS,   HID_GYRO_TICKS - HID_ACC_TICKS},           /* accel */
    {HID_GYRO_TICKS,
     HID_GYRO_ENTRIES_OFF + HID_GYRO_ENTRIES * 6u - HID_GYRO_TICKS}, /* gyro */
};
#define HID_RING_RANGE_COUNT (sizeof(HID_RING_RANGES) / sizeof(HID_RING_RANGES[0]))

/* Ereignis-Bits, die ein Update ausloest. */
#define HID_RING_EV_PAD0   (1u << 0)
#define HID_RING_EV_PAD1   (1u << 1)
#define HID_RING_EV_ACCEL  (1u << 2)
#define HID_RING_EV_GYRO   (1u << 3)

/* Host-Eingabe (runtime/main.c setzt sie je Frame aus window.c). Bitfelder:
 * 1 = gedrueckt. Circle-Pad in Rohkoordinaten -0x9A..0x9A, Touch in Pixeln
 * des unteren Bildschirms (0..319 / 0..239). */
typedef struct {
    uint32_t a, b, x, y, l, r, start, select;
    uint32_t up, down, left, right;      /* Steuerkreuz */
    int16_t circle_x, circle_y;          /* Circle-Pad */
    int touch_pressed;
    uint16_t touch_x, touch_y;
} HidRingInput;

/* Zustand, den Azahar im Modul haelt (Indizes, Zeitstempel, Pad-Zustand,
 * Circle-Pad-Mittelung). */
typedef struct {
    uint32_t next_pad, next_touch, next_acc, next_gyro;
    int64_t pad_ticks, pad_ticks_prev;
    int64_t touch_ticks, touch_ticks_prev;
    int64_t acc_ticks, acc_ticks_prev;
    int64_t gyro_ticks, gyro_ticks_prev;
    uint32_t pad_state;
    int16_t circle_old_x[2], circle_old_y[2];
} HidRing;

static inline void hid_ring_wr16(uint8_t *s, uint32_t off, uint16_t v) {
    memcpy(s + off, &v, 2);
}
static inline void hid_ring_wr32(uint8_t *s, uint32_t off, uint32_t v) {
    memcpy(s + off, &v, 4);
}
static inline void hid_ring_wr64(uint8_t *s, uint32_t off, uint64_t v) {
    memcpy(s + off, &v, 8);
}
static inline uint32_t hid_ring_rd32(const uint8_t *s, uint32_t off) {
    uint32_t v = 0;
    memcpy(&v, s + off, 4);
    return v;
}

/* Richtungsbits des Circle-Pads nach Azahar GetStickDirectionState:
 * Schwelle 40^2, tan30/tan60; x>0 = rechts, y>0 = oben. */
static inline uint32_t hid_ring_circle_direction(int16_t x, int16_t y) {
    uint32_t bits = 0;
    if ((int32_t)x * x + (int32_t)y * y > 40 * 40) {
        float t = (x != 0) ? fabsf((float)y / (float)x) : 1.0e9f;
        if (x != 0 && t < 1.7320508075688772f) {          /* tan60 */
            bits |= (x > 0) ? (1u << 28) : (1u << 29);
        }
        if (x == 0 || t > 0.5773502691896257f) {          /* tan30 */
            bits |= (y > 0) ? (1u << 30) : (1u << 31);
        }
    }
    return bits;
}

/* Ein Pad-/Touch-Update (Azahar UpdatePadCallback). */
static inline uint32_t hid_ring_write_pad(HidRing *r, uint8_t *shadow,
                                          const HidRingInput *in, int64_t tick) {
    uint32_t hex = 0, idx, last, old, e;
    int32_t nx = in->circle_x, ny = in->circle_y;
    int16_t ax, ay, tx, ty;

    if (in->a) hex |= 1u << 0;
    if (in->b) hex |= 1u << 1;
    if (in->select) hex |= 1u << 2;
    if (in->start) hex |= 1u << 3;
    if (in->right) hex |= 1u << 4;
    if (in->left) hex |= 1u << 5;
    if (in->up) hex |= 1u << 6;
    if (in->down) hex |= 1u << 7;
    if (in->r) hex |= 1u << 8;
    if (in->l) hex |= 1u << 9;
    if (in->x) hex |= 1u << 10;
    if (in->y) hex |= 1u << 11;

    /* Azahar mittelt den neuen Rohwert mit den zwei vorherigen
     * (CIRCLE_PAD_AVERAGING = 3, ganzzahlige Division). */
    ax = (int16_t)((nx + r->circle_old_x[0] + r->circle_old_x[1]) / 3);
    ay = (int16_t)((ny + r->circle_old_y[0] + r->circle_old_y[1]) / 3);
    r->circle_old_x[0] = r->circle_old_x[1]; r->circle_old_x[1] = (int16_t)nx;
    r->circle_old_y[0] = r->circle_old_y[1]; r->circle_old_y[1] = (int16_t)ny;

    hex |= hid_ring_circle_direction(ax, ay);
    r->pad_state = hex;
    hid_ring_wr32(shadow, HID_PAD_CURRENT, hex);

    idx = r->next_pad;
    r->next_pad = (idx + 1u) % HID_PAD_ENTRIES;
    hid_ring_wr32(shadow, HID_PAD_INDEX, idx);
    last = (idx - 1u) % HID_PAD_ENTRIES;      /* u32-Arithmetik wie Azahar */
    old = hid_ring_rd32(shadow, HID_PAD_ENTRIES_OFF + last * 16u);
    e = HID_PAD_ENTRIES_OFF + idx * 16u;
    hid_ring_wr32(shadow, e + 0u, hex);
    hid_ring_wr32(shadow, e + 4u, hex & ~old);          /* delta_additions */
    hid_ring_wr32(shadow, e + 8u, old & ~hex);          /* delta_removals */
    hid_ring_wr16(shadow, e + 12u, (uint16_t)ax);
    hid_ring_wr16(shadow, e + 14u, (uint16_t)ay);
    if (idx == 0u) {
        r->pad_ticks_prev = r->pad_ticks;
        r->pad_ticks = tick;
        hid_ring_wr64(shadow, HID_PAD_TICKS_PREV, (uint64_t)r->pad_ticks_prev);
        hid_ring_wr64(shadow, HID_PAD_TICKS, (uint64_t)r->pad_ticks);
    }

    /* Touch: ohne Druck sind x/y wie bei Azahar 0 und valid 0. */
    idx = r->next_touch;
    r->next_touch = (idx + 1u) % HID_TOUCH_ENTRIES;
    hid_ring_wr32(shadow, HID_TOUCH_INDEX, idx);
    if (in->touch_pressed) {
        tx = (int16_t)(in->touch_x < 320 ? in->touch_x : 319);
        ty = (int16_t)(in->touch_y < 240 ? in->touch_y : 239);
    } else {
        tx = 0;
        ty = 0;
    }
    e = HID_TOUCH_ENTRIES_OFF + idx * 8u;
    hid_ring_wr16(shadow, e + 0u, (uint16_t)tx);
    hid_ring_wr16(shadow, e + 2u, (uint16_t)ty);
    hid_ring_wr32(shadow, e + 4u, in->touch_pressed ? 1u : 0u);
    if (idx == 0u) {
        r->touch_ticks_prev = r->touch_ticks;
        r->touch_ticks = tick;
        hid_ring_wr64(shadow, HID_TOUCH_TICKS_PREV, (uint64_t)r->touch_ticks_prev);
        hid_ring_wr64(shadow, HID_TOUCH_TICKS, (uint64_t)r->touch_ticks);
    }

    /* 3D-Slider: Azahar schreibt factor_3d/100 (Standard 0). */
    hid_ring_wr32(shadow, HID_PAD_SLIDER, 0u);

    /* Azahar signalisiert beide Pad-/Touch-Events bei jedem Update. */
    return HID_RING_EV_PAD0 | HID_RING_EV_PAD1;
}

/* Ein Accelerometer-Update. Azahar: Eintrag = Rohwert, raw.x = -2*x,
 * raw.z = 2*y, raw.y = -2*z. */
static inline uint32_t hid_ring_write_accel(HidRing *r, uint8_t *shadow,
                                            int16_t x, int16_t y, int16_t z,
                                            int64_t tick) {
    uint32_t idx = r->next_acc, e;
    r->next_acc = (idx + 1u) % HID_ACCEL_ENTRIES;
    hid_ring_wr32(shadow, HID_ACC_INDEX, idx);
    e = HID_ACC_ENTRIES_OFF + idx * 6u;
    hid_ring_wr16(shadow, e + 0u, (uint16_t)x);
    hid_ring_wr16(shadow, e + 2u, (uint16_t)y);
    hid_ring_wr16(shadow, e + 4u, (uint16_t)z);
    hid_ring_wr16(shadow, HID_ACC_RAW + 0u, (uint16_t)(int16_t)(-2 * x));
    hid_ring_wr16(shadow, HID_ACC_RAW + 2u, (uint16_t)(int16_t)(-2 * z));
    hid_ring_wr16(shadow, HID_ACC_RAW + 4u, (uint16_t)(int16_t)(2 * y));
    if (idx == 0u) {
        r->acc_ticks_prev = r->acc_ticks;
        r->acc_ticks = tick;
        hid_ring_wr64(shadow, HID_ACC_TICKS_PREV, (uint64_t)r->acc_ticks_prev);
        hid_ring_wr64(shadow, HID_ACC_TICKS, (uint64_t)r->acc_ticks);
    }
    return HID_RING_EV_ACCEL;
}

/* Ein Gyroscope-Update. Azahar: raw.x = x, raw.z = -y, raw.y = z. */
static inline uint32_t hid_ring_write_gyro(HidRing *r, uint8_t *shadow,
                                           int16_t x, int16_t y, int16_t z,
                                           int64_t tick) {
    uint32_t idx = r->next_gyro, e;
    r->next_gyro = (idx + 1u) % HID_GYRO_ENTRIES;
    hid_ring_wr32(shadow, HID_GYRO_INDEX, idx);
    e = HID_GYRO_ENTRIES_OFF + idx * 6u;
    hid_ring_wr16(shadow, e + 0u, (uint16_t)x);
    hid_ring_wr16(shadow, e + 2u, (uint16_t)y);
    hid_ring_wr16(shadow, e + 4u, (uint16_t)z);
    hid_ring_wr16(shadow, HID_GYRO_RAW + 0u, (uint16_t)x);
    hid_ring_wr16(shadow, HID_GYRO_RAW + 2u, (uint16_t)z);
    hid_ring_wr16(shadow, HID_GYRO_RAW + 4u, (uint16_t)(int16_t)(-y));
    if (idx == 0u) {
        r->gyro_ticks_prev = r->gyro_ticks;
        r->gyro_ticks = tick;
        hid_ring_wr64(shadow, HID_GYRO_TICKS_PREV, (uint64_t)r->gyro_ticks_prev);
        hid_ring_wr64(shadow, HID_GYRO_TICKS, (uint64_t)r->gyro_ticks);
    }
    return HID_RING_EV_GYRO;
}

#endif /* HID_RING_H */
