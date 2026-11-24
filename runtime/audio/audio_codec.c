/* Dekodierung/Resampling der DSP-HLE-Quellen (Azahar audio_core/codec.cpp,
 * audio_core/interpolate.cpp). Keine Gastadressen, keine Spielsonderfaelle. */
#include "audio_codec.h"

#include <string.h>

static const int SIGNED_NIBBLES[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1,
};

uint32_t audio_adpcm_decode(const uint8_t *data, uint32_t sample_count,
                            const int16_t coeff[16], int16_t *yn1, int16_t *yn2,
                            int16_t (*out)[2], uint32_t out_capacity) {
    /* GC-ADPCM: 8-Byte-Rahmen, 14 Samples je Rahmen, ein Nibble je Sample. */
    const uint32_t FRAME_LEN = 8;
    const uint32_t SAMPLES_PER_FRAME = 14;
    uint32_t ret_size = sample_count + (sample_count & 1u);
    uint32_t frames;
    int y1 = *yn1, y2 = *yn2;
    uint32_t outi = 0;

    if (ret_size > out_capacity) ret_size = out_capacity & ~1u;
    if (ret_size == 0) return 0;
    memset(out, 0, ret_size * sizeof(out[0]));

    frames = (sample_count + (SAMPLES_PER_FRAME - 1u)) / SAMPLES_PER_FRAME;
    for (uint32_t framei = 0; framei < frames; framei++) {
        const uint8_t header = data[framei * FRAME_LEN];
        int scale = 1 << (header & 0xFu);
        int idx = (header >> 4) & 0x7;
        int coef1 = coeff[idx * 2 + 0];
        int coef2 = coeff[idx * 2 + 1];
        uint32_t datai = framei * FRAME_LEN + 1;

        for (uint32_t i = 0; i < SAMPLES_PER_FRAME && outi < ret_size; i += 2) {
            int nibble = SIGNED_NIBBLES[data[datai] >> 4];
            int xn = nibble * scale;
            /* y[n] = x[n] + 0.5 + c1*y[n-1] + c2*y[n-2], 11 Bit Festkomma. */
            int val = ((xn << 11) + 0x400 + coef1 * y1 + coef2 * y2) >> 11;
            if (val < -32768) val = -32768;
            if (val > 32767) val = 32767;
            y2 = y1;
            y1 = val;
            out[outi][0] = out[outi][1] = (int16_t)val;
            outi++;

            if (outi >= ret_size) break;
            nibble = SIGNED_NIBBLES[data[datai] & 0xF];
            xn = nibble * scale;
            val = ((xn << 11) + 0x400 + coef1 * y1 + coef2 * y2) >> 11;
            if (val < -32768) val = -32768;
            if (val > 32767) val = 32767;
            y2 = y1;
            y1 = val;
            out[outi][0] = out[outi][1] = (int16_t)val;
            outi++;
            datai++;
        }
    }

    *yn1 = y1;
    *yn2 = y2;
    return outi;
}

uint32_t audio_pcm8_decode(const uint8_t *data, uint32_t sample_count, int stereo,
                           int16_t (*out)[2], uint32_t out_capacity) {
    uint32_t n = sample_count < out_capacity ? sample_count : out_capacity;
    for (uint32_t i = 0; i < n; i++) {
        if (stereo) {
            out[i][0] = (int16_t)((uint16_t)data[i * 2 + 0] << 8);
            out[i][1] = (int16_t)((uint16_t)data[i * 2 + 1] << 8);
        } else {
            int16_t s = (int16_t)((uint16_t)data[i] << 8);
            out[i][0] = out[i][1] = s;
        }
    }
    return n;
}

uint32_t audio_pcm16_decode(const uint8_t *data, uint32_t sample_count, int stereo,
                            int16_t (*out)[2], uint32_t out_capacity) {
    uint32_t n = sample_count < out_capacity ? sample_count : out_capacity;
    for (uint32_t i = 0; i < n; i++) {
        if (stereo) {
            memcpy(&out[i][0], data + i * 4 + 0, 2);
            memcpy(&out[i][1], data + i * 4 + 2, 2);
        } else {
            int16_t s;
            memcpy(&s, data + i * 2, 2);
            out[i][0] = out[i][1] = s;
        }
    }
    return n;
}

/* Kombinierter Puffer wie Azahar: vorne die zwei historischen Samples,
 * danach der aktuelle Puffer. Damit bleibt die Indexrechnung identisch. */
static void combined_get(const AudioInterpState *st, const int16_t (*in)[2],
                         uint32_t k, int16_t out[2]) {
    if (k == 0) {
        out[0] = st->xn2[0];
        out[1] = st->xn2[1];
    } else if (k == 1) {
        out[0] = st->xn1[0];
        out[1] = st->xn1[1];
    } else {
        out[0] = in[k - 2][0];
        out[1] = in[k - 2][1];
    }
}

/* Azahar interpolate.cpp rechnet `fraction * delta / scale_factor` mit
 * uint64-Arithmetik; fuer negative deltas ist das eine Abrundung (floor), nicht
 * die C-Division gegen Null. Nachbildung als arithmetischer Shift >> 24. */
static int64_t interp_scaled(int64_t delta, uint64_t fraction) {
    int64_t prod = delta * (int64_t)fraction;
    if (prod >= 0) return prod / (int64_t)AUDIO_INTERP_SCALE;
    return -(((-prod) + (int64_t)AUDIO_INTERP_SCALE - 1) / (int64_t)AUDIO_INTERP_SCALE);
}

static uint32_t step_over(AudioInterpState *st, const int16_t (*in)[2],
                          uint32_t *in_len, float rate, int16_t (*out)[2],
                          uint32_t out_capacity, uint32_t *outi, int linear) {
    uint32_t len = *in_len;
    uint64_t step_size;
    uint64_t fpos;
    uint32_t inputi = 0;

    if (len == 0) return 0;
    if (!(rate > 0.0f)) rate = 1.0f;
    step_size = (uint64_t)(rate * (float)AUDIO_INTERP_SCALE);
    fpos = st->fposition;

    while (*outi < out_capacity) {
        int16_t x0[2], x1[2], x2[2];
        uint64_t fraction;

        inputi = (uint32_t)(fpos / AUDIO_INTERP_SCALE);
        if (inputi >= len) {
            inputi = len;
            break;
        }
        fraction = fpos & AUDIO_INTERP_MASK;
        combined_get(st, in, inputi, x0);
        combined_get(st, in, inputi + 1, x1);
        combined_get(st, in, inputi + 2, x2);

        if (linear) {
            int64_t d0 = (int64_t)x1[0] - x0[0];
            int64_t d1 = (int64_t)x1[1] - x0[1];
            if (d0 < -32768) d0 = -32768;
            if (d0 > 32767) d0 = 32767;
            if (d1 < -32768) d1 = -32768;
            if (d1 > 32767) d1 = 32767;
            out[*outi][0] = (int16_t)((int64_t)x0[0] + interp_scaled(d0, fraction));
            out[*outi][1] = (int16_t)((int64_t)x0[1] + interp_scaled(d1, fraction));
        } else {
            out[*outi][0] = x0[0];
            out[*outi][1] = x0[1];
        }
        (*outi)++;
        fpos += step_size;
    }

    combined_get(st, in, inputi, st->xn2);
    combined_get(st, in, inputi + 1, st->xn1);
    st->fposition = fpos - (uint64_t)inputi * AUDIO_INTERP_SCALE;
    *in_len = len - inputi;
    return inputi;
}

uint32_t audio_interp_none(AudioInterpState *st, const int16_t (*in)[2],
                           uint32_t *in_len, float rate, int16_t (*out)[2],
                           uint32_t out_capacity, uint32_t *outi) {
    return step_over(st, in, in_len, rate, out, out_capacity, outi, 0);
}

uint32_t audio_interp_linear(AudioInterpState *st, const int16_t (*in)[2],
                             uint32_t *in_len, float rate, int16_t (*out)[2],
                             uint32_t out_capacity, uint32_t *outi) {
    return step_over(st, in, in_len, rate, out, out_capacity, outi, 1);
}
