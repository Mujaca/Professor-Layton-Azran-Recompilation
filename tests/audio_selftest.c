/* Selbsttest DSP-HLE-Dekodierung/Resampling (runtime/audio/audio_codec.c).
 *
 * Erwartungswerte sind von Hand aus der Azahar-Formel gerechnet
 * (audio_core/codec.cpp GC-ADPCM, audio_core/interpolate.cpp StepOverSamples),
 * nicht aus der Implementierung abgeleitet:
 *
 *   ADPCM (Koeffizienten 0, Scale 1): y[n] = x[n]
 *   ADPCM (c1 = 1.0 in 11 Bit = 2048): y[n] = x[n] + y[n-1]
 *   Linear, rate 0.5, Eingang 0,100,...,700:
 *      0,0,0,0,0,50,100,150,200,250,300,350,400,450,500,550
 *   Linear, rate 2.0: 0,0,200,400
 *   None (Zero-Order-Hold, rate 1): zwei Vorlauf-Nullen, dann x[0], x[1], ...
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "audio_codec.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        ++failures;
        printf("FAIL %s\n", what);
    }
}

static void check_samples(const int16_t (*got)[2], const int16_t *want,
                          unsigned count, const char *what) {
    for (unsigned i = 0; i < count; i++) {
        if (got[i][0] != want[i] || got[i][1] != want[i]) {
            ++failures;
            printf("FAIL %s[%u]: got %d/%d want %d\n", what, i,
                   got[i][0], got[i][1], want[i]);
            return;
        }
    }
}

static void test_adpcm_zero_coeff(void) {
    /* Rahmen: Header 0x01 (Scale 2, Praediktor 0), Nibbles 0,1,2,3,... 13. */
    static const uint8_t data[8] = {0x01, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD};
    int16_t out[16][2];
    int16_t want[14];
    int16_t coeff[16] = {0};
    int yn1 = 0, yn2 = 0;

    for (int i = 1; i < 14; i++) want[i] = (int16_t)(i * 2);
    want[0] = 0;
    /* Nibble 8..13 sind negativ (-8..-3). */
    for (int i = 8; i < 14; i++) want[i] = (int16_t)((i == 8) ? -16 :
                                                     (i == 9) ? -14 :
                                                     (i == 10) ? -12 :
                                                     (i == 11) ? -10 :
                                                     (i == 12) ? -8 : -6);
    uint32_t n = audio_adpcm_decode(data, 14, coeff, &yn1, &yn2, out, 16);
    check(n == 14, "adpcm_zero_count");
    check_samples(out, want, 14, "adpcm_zero");
    check(yn1 == -6 && yn2 == -8, "adpcm_zero_state");
}

static void test_adpcm_predictor(void) {
    /* c1 = 2048 (1.0), Scale 1: y[n] = x[n] + y[n-1]. */
    static const uint8_t data[8] = {0x10, 0x12, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00};
    int16_t out[16][2];
    /* Nibbles: 1,2,3,4. y: 1, 3, 6, 10. */
    int16_t want[4] = {1, 3, 6, 10};
    int16_t coeff[16] = {0};
    int yn1 = 0, yn2 = 0;

    coeff[2] = 2048;  /* Praediktor 1: coef1 */
    coeff[3] = 0;
    uint32_t n = audio_adpcm_decode(data, 4, coeff, &yn1, &yn2, out, 16);
    check(n == 4, "adpcm_pred_count");
    check_samples(out, want, 4, "adpcm_pred");
    check(yn1 == 10 && yn2 == 6, "adpcm_pred_state");
}

static void test_pcm(void) {
    static const uint8_t p8mono[4] = {0x00, 0x7F, 0x80, 0xFF};
    static const int16_t p8want[4] = {0x0000, 0x7F00, (int16_t)0x8000, (int16_t)0xFF00};
    static const uint8_t p16st[8] = {0x00, 0x00, 0xFF, 0x7F, 0x00, 0x80, 0x34, 0x12};
    int16_t out[8][2];

    audio_pcm8_decode(p8mono, 4, 0, out, 8);
    check_samples(out, p8want, 4, "pcm8_mono");

    audio_pcm16_decode(p16st, 2, 1, out, 8);
    check(out[0][0] == 0x0000 && out[0][1] == 0x7FFF, "pcm16_stereo0");
    check(out[1][0] == (int16_t)0x8000 && out[1][1] == 0x1234, "pcm16_stereo1");
}

static void test_linear_half(void) {
    int16_t in[8][2];
    int16_t out[160][2];
    AudioInterpState st;
    uint32_t len = 8, outi = 0;
    uint32_t consumed;
    static const int16_t want[16] = {0, 0, 0, 0, 0, 50, 100, 150,
                                     200, 250, 300, 350, 400, 450, 500, 550};

    memset(&st, 0, sizeof(st));
    for (int i = 0; i < 8; i++) in[i][0] = in[i][1] = (int16_t)(i * 100);
    consumed = audio_interp_linear(&st, (const int16_t (*)[2])in, &len, 0.5f,
                                   out, 160, &outi);
    check(consumed == 8, "linear_half_consumed");
    check(outi == 16, "linear_half_count");
    check_samples(out, want, 16, "linear_half");
}

static void test_linear_double(void) {
    int16_t in[8][2];
    int16_t out[160][2];
    AudioInterpState st;
    uint32_t len = 8, outi = 0;
    uint32_t consumed;
    static const int16_t want[4] = {0, 0, 200, 400};

    memset(&st, 0, sizeof(st));
    for (int i = 0; i < 8; i++) in[i][0] = in[i][1] = (int16_t)(i * 100);
    consumed = audio_interp_linear(&st, (const int16_t (*)[2])in, &len, 2.0f,
                                   out, 160, &outi);
    check(consumed == 8, "linear_double_consumed");
    check(outi == 4, "linear_double_count");
    check_samples(out, want, 4, "linear_double");
}

static void test_none(void) {
    int16_t in[3][2];
    int16_t out[160][2];
    AudioInterpState st;
    uint32_t len = 3, outi = 0;
    static const int16_t want[3] = {0, 0, 10};

    memset(&st, 0, sizeof(st));
    in[0][0] = in[0][1] = 10;
    in[1][0] = in[1][1] = 20;
    in[2][0] = in[2][1] = 30;
    audio_interp_none(&st, (const int16_t (*)[2])in, &len, 1.0f, out, 160, &outi);
    /* fposition 0 und 1 lesen die Historie, ab 2 kommt in[0]. */
    check(outi >= 3, "none_count");
    check_samples(out, want, 3, "none");
}

int main(void) {
    test_adpcm_zero_coeff();
    test_adpcm_predictor();
    test_pcm();
    test_linear_half();
    test_linear_double();
    test_none();
    if (failures) {
        printf("%d FEHLER\n", failures);
        return 1;
    }
    printf("audio_codec: alle Tests bestanden\n");
    return 0;
}
