/* Verhaltenstest der HID-Ringpuffer (runtime/platform/hid_ring.h).
 *
 * Getestet ohne Gast/Runtime: Index-Umlauf aller vier Ringe, Tastenbits,
 * Circle-Pad-Richtungsbits, delta_additions/removals, die
 * Ereignis-Signalisierung (Rueckgabemaske), die Touch-Koordinaten ohne/mit
 * Druck und die Accelerometer-Ruhelage (Erdbeschleunigung nach Azahar).
 *
 *   python tests/hid_ring_test.py
 */
#include <stdio.h>
#include <string.h>

#include "hid_ring.h"

static int g_fail;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fail++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define BUTTON(n) (1u << (n))
#define PAD_A      BUTTON(0)
#define PAD_B      BUTTON(1)
#define PAD_SELECT BUTTON(2)
#define PAD_START  BUTTON(3)
#define PAD_RIGHT  BUTTON(4)
#define PAD_LEFT   BUTTON(5)
#define PAD_UP     BUTTON(6)
#define PAD_DOWN   BUTTON(7)
#define PAD_R      BUTTON(8)
#define PAD_L      BUTTON(9)
#define PAD_X      BUTTON(10)
#define PAD_Y      BUTTON(11)

static uint32_t rd32(const uint8_t *s, uint32_t off) {
    return hid_ring_rd32(s, off);
}
static uint16_t rd16u(const uint8_t *s, uint32_t off) {
    uint16_t v = 0;
    memcpy(&v, s + off, 2);
    return v;
}
static int16_t rd16s(const uint8_t *s, uint32_t off) {
    int16_t v = 0;
    memcpy(&v, s + off, 2);
    return v;
}

static void test_index_wrap(void) {
    HidRing r;
    uint8_t sh[HID_SHMEM_SIZE];
    HidRingInput in;
    int i;

    memset(&r, 0, sizeof(r));
    memset(sh, 0, sizeof(sh));
    memset(&in, 0, sizeof(in));

    /* Pad: 8 Eintraege, Index absolut 0..7, dann Umlauf auf 0. */
    for (i = 0; i < 8; i++) {
        hid_ring_write_pad(&r, sh, &in, 100 + i);
        CHECK(rd32(sh, HID_PAD_INDEX) == (uint32_t)i,
              "pad index nach %d. Update = %u, erwartet %d",
              i + 1, rd32(sh, HID_PAD_INDEX), i);
    }
    CHECK(r.next_pad == 0, "pad next nach 8 Updates = %u, erwartet 0", r.next_pad);
    /* Tick nur beim Schreiben von Index 0 (Azahar). */
    CHECK(r.pad_ticks == 100, "pad tick nach Index 0 = %lld, erwartet 100",
          (long long)r.pad_ticks);
    hid_ring_write_pad(&r, sh, &in, 200);
    CHECK(rd32(sh, HID_PAD_INDEX) == 0, "pad index nach Umlauf = %u, erwartet 0",
          rd32(sh, HID_PAD_INDEX));
    CHECK(r.next_pad == 1, "pad next nach Umlauf = %u, erwartet 1", r.next_pad);
    CHECK(r.pad_ticks_prev == 100 && r.pad_ticks == 200,
          "pad tick previous/aktuell nach Umlauf = %lld/%lld, erwartet 100/200",
          (long long)r.pad_ticks_prev, (long long)r.pad_ticks);

    /* Touch: 8 Eintraege. (Die Pad-Updates oben haben den Touch-Ring schon
     * einmal umlaufen lassen; fuer die Zaehlung hier zuruecksetzen.) */
    r.next_touch = 0;
    for (i = 0; i < 8; i++) {
        hid_ring_write_pad(&r, sh, &in, 0);
        CHECK(rd32(sh, HID_TOUCH_INDEX) == (uint32_t)i,
              "touch index nach %d. Update = %u, erwartet %d",
              i + 1, rd32(sh, HID_TOUCH_INDEX), i);
    }
    CHECK(r.next_touch == 0, "touch next nach 8 Updates = %u, erwartet 0",
          r.next_touch);

    /* Accelerometer: 8 Eintraege. */
    for (i = 0; i < 8; i++) {
        hid_ring_write_accel(&r, sh, 1, 2, 3, 0);
        CHECK(rd32(sh, HID_ACC_INDEX) == (uint32_t)i,
              "acc index nach %d. Update = %u, erwartet %d",
              i + 1, rd32(sh, HID_ACC_INDEX), i);
    }
    CHECK(r.next_acc == 0, "acc next nach 8 Updates = %u, erwartet 0", r.next_acc);

    /* Gyroscope: 32 Eintraege. */
    for (i = 0; i < 32; i++) {
        hid_ring_write_gyro(&r, sh, 1, 2, 3, 0);
        CHECK(rd32(sh, HID_GYRO_INDEX) == (uint32_t)i,
              "gyro index nach %d. Update = %u, erwartet %d",
              i + 1, rd32(sh, HID_GYRO_INDEX), i);
    }
    CHECK(r.next_gyro == 0, "gyro next nach 32 Updates = %u, erwartet 0",
          r.next_gyro);
}

static void test_button_bits(void) {
    HidRing r;
    uint8_t sh[HID_SHMEM_SIZE];
    HidRingInput in;

    memset(&r, 0, sizeof(r));
    memset(sh, 0, sizeof(sh));
    memset(&in, 0, sizeof(in));
    in.a = in.b = in.x = in.y = 1;
    in.l = in.r = in.start = in.select = 1;
    in.up = in.down = in.left = in.right = 1;

    hid_ring_write_pad(&r, sh, &in, 0);
    CHECK(rd32(sh, HID_PAD_CURRENT) == 0x0FFFu,
          "Pad-Status = 0x%03X, erwartet 0x0FFF", rd32(sh, HID_PAD_CURRENT));
    CHECK(rd32(sh, HID_PAD_ENTRIES_OFF) == 0x0FFFu,
          "Pad-Eintrag = 0x%03X, erwartet 0x0FFF",
          rd32(sh, HID_PAD_ENTRIES_OFF));
}

static void test_circle_direction(void) {
    static const struct { int16_t x, y; uint32_t bits; const char *name; } cases[] = {
        { 0x9A, 0,      BUTTON(28), "rechts" },
        { -0x9A, 0,     BUTTON(29), "links" },
        { 0, 0x9A,      BUTTON(30), "oben" },
        { 0, -0x9A,     BUTTON(31), "unten" },
        { 0x6D, 0x6D,   BUTTON(28) | BUTTON(30), "diagonal oben-rechts" },
        { 20, 20,       0u, "unter Schwelle 40" },
    };
    HidRing r;
    uint8_t sh[HID_SHMEM_SIZE];
    HidRingInput in;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&r, 0, sizeof(r));
        memset(sh, 0, sizeof(sh));
        memset(&in, 0, sizeof(in));
        in.circle_x = cases[i].x;
        in.circle_y = cases[i].y;
        /* Mittelung auf den Rohwert setzen, sonst daempft der erste Tick. */
        r.circle_old_x[0] = r.circle_old_x[1] = cases[i].x;
        r.circle_old_y[0] = r.circle_old_y[1] = cases[i].y;
        hid_ring_write_pad(&r, sh, &in, 0);
        CHECK(rd32(sh, HID_PAD_CURRENT) == cases[i].bits,
              "Circle %s: Status = 0x%08X, erwartet 0x%08X", cases[i].name,
              rd32(sh, HID_PAD_CURRENT), cases[i].bits);
    }
}

static void test_deltas(void) {
    HidRing r;
    uint8_t sh[HID_SHMEM_SIZE];
    HidRingInput in;
    uint32_t base;

    memset(&r, 0, sizeof(r));
    memset(sh, 0, sizeof(sh));
    memset(&in, 0, sizeof(in));

    in.a = 1;
    hid_ring_write_pad(&r, sh, &in, 0);
    base = HID_PAD_ENTRIES_OFF + 0 * 16u;
    CHECK(rd32(sh, base + 0) == PAD_A, "A-Status = 0x%X", rd32(sh, base + 0));
    CHECK(rd32(sh, base + 4) == PAD_A, "A delta_additions = 0x%X",
          rd32(sh, base + 4));
    CHECK(rd32(sh, base + 8) == 0, "A delta_removals = 0x%X",
          rd32(sh, base + 8));

    in.b = 1;
    hid_ring_write_pad(&r, sh, &in, 0);
    base = HID_PAD_ENTRIES_OFF + 1 * 16u;
    CHECK(rd32(sh, base + 0) == (PAD_A | PAD_B), "A+B-Status = 0x%X",
          rd32(sh, base + 0));
    CHECK(rd32(sh, base + 4) == PAD_B, "A+B delta_additions = 0x%X",
          rd32(sh, base + 4));
    CHECK(rd32(sh, base + 8) == 0, "A+B delta_removals = 0x%X",
          rd32(sh, base + 8));

    in.a = 0;
    hid_ring_write_pad(&r, sh, &in, 0);
    base = HID_PAD_ENTRIES_OFF + 2 * 16u;
    CHECK(rd32(sh, base + 0) == PAD_B, "B-Status = 0x%X", rd32(sh, base + 0));
    CHECK(rd32(sh, base + 4) == 0, "B delta_additions = 0x%X",
          rd32(sh, base + 4));
    CHECK(rd32(sh, base + 8) == PAD_A, "B delta_removals = 0x%X",
          rd32(sh, base + 8));
}

static void test_events(void) {
    HidRing r;
    uint8_t sh[HID_SHMEM_SIZE];
    HidRingInput in;
    uint32_t ev;

    memset(&r, 0, sizeof(r));
    memset(sh, 0, sizeof(sh));
    memset(&in, 0, sizeof(in));

    ev = hid_ring_write_pad(&r, sh, &in, 0);
    CHECK(ev == (HID_RING_EV_PAD0 | HID_RING_EV_PAD1),
          "Pad-Ereignis = 0x%X, erwartet PAD0|PAD1", ev);
    ev = hid_ring_write_accel(&r, sh, 0, 0, 0, 0);
    CHECK(ev == HID_RING_EV_ACCEL, "Accel-Ereignis = 0x%X", ev);
    ev = hid_ring_write_gyro(&r, sh, 0, 0, 0, 0);
    CHECK(ev == HID_RING_EV_GYRO, "Gyro-Ereignis = 0x%X", ev);
}

static void test_touch(void) {
    HidRing r;
    uint8_t sh[HID_SHMEM_SIZE];
    HidRingInput in;
    uint32_t e;

    memset(&r, 0, sizeof(r));
    memset(sh, 0, sizeof(sh));
    memset(&in, 0, sizeof(in));
    in.touch_x = 123;
    in.touch_y = 45;
    hid_ring_write_pad(&r, sh, &in, 0);
    e = HID_TOUCH_ENTRIES_OFF + 0 * 8u;
    CHECK(rd32(sh, e + 4) == 0, "Touch valid ohne Druck = %u",
          rd32(sh, e + 4));
    CHECK(rd32(sh, e + 0) == 0, "Touch x/y ohne Druck = 0x%X",
          rd32(sh, e + 0));

    in.touch_pressed = 1;
    hid_ring_write_pad(&r, sh, &in, 0);
    e = HID_TOUCH_ENTRIES_OFF + 1 * 8u;
    CHECK(rd32(sh, e + 4) == 1, "Touch valid mit Druck = %u",
          rd32(sh, e + 4));
    CHECK(rd32(sh, e + 0) == (123u | (45u << 16)),
          "Touch x/y mit Druck = 0x%X", rd32(sh, e + 0));

    /* Klemmen am Rand wie Azahar (min(width-1)). */
    in.touch_x = 500;
    in.touch_y = 500;
    hid_ring_write_pad(&r, sh, &in, 0);
    e = HID_TOUCH_ENTRIES_OFF + 2 * 8u;
    CHECK(rd32(sh, e + 0) == (319u | (239u << 16)),
          "Touch-Klemme = 0x%X", rd32(sh, e + 0));
}

static void test_accel_rest(void) {
    HidRing r;
    uint8_t sh[HID_SHMEM_SIZE];

    memset(&r, 0, sizeof(r));
    memset(sh, 0, sizeof(sh));
    hid_ring_write_accel(&r, sh, 0, -512, 0, 0);
    CHECK(rd16s(sh, HID_ACC_ENTRIES_OFF + 0) == 0,
          "Accel-Ruhe x = %d", rd16s(sh, HID_ACC_ENTRIES_OFF + 0));
    CHECK(rd16s(sh, HID_ACC_ENTRIES_OFF + 2) == -512,
          "Accel-Ruhe y = %d, erwartet -512",
          rd16s(sh, HID_ACC_ENTRIES_OFF + 2));
    CHECK(rd16s(sh, HID_ACC_ENTRIES_OFF + 4) == 0,
          "Accel-Ruhe z = %d", rd16s(sh, HID_ACC_ENTRIES_OFF + 4));
    CHECK(rd16s(sh, HID_ACC_RAW + 0) == 0,
          "Accel-Ruhe raw.x = %d", rd16s(sh, HID_ACC_RAW + 0));
    CHECK(rd16s(sh, HID_ACC_RAW + 2) == 0,
          "Accel-Ruhe raw.y = %d", rd16s(sh, HID_ACC_RAW + 2));
    CHECK(rd16s(sh, HID_ACC_RAW + 4) == -1024,
          "Accel-Ruhe raw.z = %d, erwartet -1024", rd16s(sh, HID_ACC_RAW + 4));
}

int main(void) {
    test_index_wrap();
    test_button_bits();
    test_circle_direction();
    test_deltas();
    test_events();
    test_touch();
    test_accel_rest();
    if (g_fail) {
        printf("FAILED %d/%d HID-Ring-Tests\n", g_fail, g_checks);
        return 1;
    }
    printf("OK %d HID-Ring-Tests (Index-Umlauf, Tasten/Circle-Bits, Deltas, "
           "Ereignisse, Touch, Accel-Ruhe)\n", g_checks);
    return 0;
}
