/* Dekodierung und Resampling der DSP-HLE-Quellen.
 *
 * Reine Funktionen ohne Gast-Speicher, damit sie in tests/audio_test.py
 * unabhaengig von der Runtime geprueft werden koennen. Semantik nach Azahar
 * audio_core/codec.cpp und audio_core/interpolate.cpp. */
#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <stdint.h>

#define AUDIO_CODEC_FRAME_SAMPLES 160
#define AUDIO_INTERP_SCALE (1u << 24)
#define AUDIO_INTERP_MASK  (AUDIO_INTERP_SCALE - 1u)

/* Zwei historische Samples und der Bruchteil der aktuellen Position
 * (24 Nachkommabits, Azahar AudioInterp::State). */
typedef struct {
    int16_t xn1[2];
    int16_t xn2[2];
    uint64_t fposition;
} AudioInterpState;

/* GC-ADPCM (4 Bit, 8-Byte-Rahmen mit 14 Samples). coeff sind 16 s16 mit
 * 11 Nachkommabits; yn1/yn2 werden fortgeschrieben. out nimmt
 * sample_count (auf gerade gerundet) Stereo-Samples auf (beide Kanaele
 * gleich, ADPCM ist mono). Rueckgabe: geschriebene Samples. */
uint32_t audio_adpcm_decode(const uint8_t *data, uint32_t sample_count,
                            const int16_t coeff[16], int16_t *yn1, int16_t *yn2,
                            int16_t (*out)[2], uint32_t out_capacity);

/* PCM8: u8 -> s16 (Wert << 8), mono oder stereo. */
uint32_t audio_pcm8_decode(const uint8_t *data, uint32_t sample_count, int stereo,
                           int16_t (*out)[2], uint32_t out_capacity);

/* PCM16 little-endian, mono oder stereo. */
uint32_t audio_pcm16_decode(const uint8_t *data, uint32_t sample_count, int stereo,
                            int16_t (*out)[2], uint32_t out_capacity);

/* Resampling: konsumiert hoechstens *in_len Samples ab *in, schreibt ab
 * *outi in out (Kapazitaet out_capacity) und liefert die Anzahl der
 * konsumierten Eingabesamples. in/in_len werden entsprechend
 * weitergeschoben. rate > 1 dezimiert, rate < 1 interpoliert hoch. */
uint32_t audio_interp_none(AudioInterpState *st, const int16_t (*in)[2],
                           uint32_t *in_len, float rate, int16_t (*out)[2],
                           uint32_t out_capacity, uint32_t *outi);
uint32_t audio_interp_linear(AudioInterpState *st, const int16_t (*in)[2],
                             uint32_t *in_len, float rate, int16_t (*out)[2],
                             uint32_t out_capacity, uint32_t *outi);

#endif
