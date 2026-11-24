/* DSP-HLE-Audio: Quellen, Mixer und die zwei DSP-RAM-Regionen.
 *
 * Spezifikation: Azahar tools/azahar/src/audio_core/hle/{hle,shared_memory,
 * source,mixers,filter,effects}.cpp sowie audio_core/{codec,interpolate}.cpp.
 * Die Strukturoffsets sind aus shared_memory.h berechnet (die 15 Adressen aus
 * AudioPipeWriteStructAddresses ergeben sich als 0x8000 + offset/2).
 *
 * Das Modul kennt keine Spieladressen: Pufferadressen kommen aus den
 * Konfigurationsstrukturen des Spiels. Die Anbindung an den dsp::DSP-Dienst
 * (Pipe-Puffer, Interrupt-Ereignisse, Gast-Lock) macht runtime/kernel/hle.c. */
#include "dsp_hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#include "audio_codec.h"
#include "host_output.h"

#define SPF AUDIO_DSP_SAMPLES_PER_FRAME

/* SharedMemory-Offsets (shared_memory.h, Groesse 0x8000). */
#define SM_DSP_STATUS        0x0800
#define SM_DSP_DEBUG         0x0820
#define SM_FINAL_SAMPLES     0x0A80
#define SM_SOURCE_STATUS     0x0D00
#define SM_COMPRESSOR        0x0E20
#define SM_DSP_CONFIG        0x2860
#define SM_INTERMEDIATE      0x2924
#define SM_SOURCE_CONFIG     0x3D24
#define SM_ADPCM_COEFFS      0x4F24
#define SM_UNKNOWN10         0x5224
#define SM_UNKNOWN11         0x5424
#define SM_UNKNOWN12         0x55A4
#define SM_UNKNOWN13         0x58A4
#define SM_UNKNOWN14         0x58B8
#define SM_FRAME_COUNTER     0x7FFE

/* SourceConfiguration::Configuration (192 Byte). */
#define CFG_DIRTY            0
#define CFG_GAIN             4
#define CFG_RATE             52
#define CFG_INTERP           56
#define CFG_FILTERS_ENABLED  58
#define CFG_SIMPLE_FILTER    60
#define CFG_BIQUAD_FILTER    64
#define CFG_BUFFERS_DIRTY    74
#define CFG_BUFFERS          76
#define CFG_LOOP_RELATED     156
#define CFG_ENABLE           160
#define CFG_SYNC_COUNT       162
#define CFG_PLAY_POSITION    164
#define CFG_EMB_PHYS         172
#define CFG_EMB_LEN          176
#define CFG_EMB_FLAGS1       180
#define CFG_EMB_PS           182
#define CFG_EMB_YN           184
#define CFG_EMB_FLAGS2       188
#define CFG_EMB_BUFFER_ID    190
#define CFG_SIZE             192

/* SourceConfiguration::Configuration::Buffer (20 Byte). */
#define BUF_PHYS             0
#define BUF_LEN              4
#define BUF_PS               8
#define BUF_YN               10
#define BUF_DIRTY            14
#define BUF_LOOPING          15
#define BUF_ID               16
#define BUF_SIZE             20

/* SourceStatus::Status (12 Byte). */
#define ST_ENABLED           0
#define ST_CURRENT_ID_DIRTY  1
#define ST_SYNC_COUNT        2
#define ST_BUFFER_POSITION   4
#define ST_CURRENT_ID        8
#define ST_LAST_ID           10

/* DspConfiguration (196 Byte); dirty-Bits wie shared_memory.h. */
#define DSPCFG_DIRTY         0
#define DSPCFG_MASTER_VOLUME 4
#define DSPCFG_AUX_RETURN    8
#define DSPCFG_OUTPUT_COUNT  16
#define DSPCFG_OUTPUT_FORMAT 22
#define DSPCFG_CLIPPING      24
#define DSPCFG_HEADPHONES    26
#define DSPCFG_SURROUND_DEPTH 28
#define DSPCFG_SURROUND_POS  30
#define DSPCFG_REAR_RATIO    34
#define DSPCFG_AUX_FRONT     36
#define DSPCFG_AUX_BUS       40
#define DSPCFG_DELAY         44
#define DSPCFG_REVERB        84
#define DSPCFG_SYNC_MODE     188
#define DSPCFG_DIRTY2        192
#define DSPCFG_SIZE          196
#define DELAY_SIZE           20

#define INTERMEDIATE_SIZE    5120
#define MIX_FRAME_BYTES      2560   /* 4 Kanaele * 160 Samples * 4 Byte */
#define ADPCM_COEFF_SIZE     768
#define MAX_QUEUE            64
#define MAX_DECODE_SAMPLES   (1u << 22)  /* Schutz gegen Unterlauf-Laengen */

typedef struct {
    uint32_t physical_address;
    uint32_t length;
    uint8_t adpcm_ps;
    uint16_t adpcm_yn[2];
    uint8_t adpcm_dirty;
    uint8_t is_looping;
    uint16_t buffer_id;
    uint16_t mono_or_stereo;
    uint16_t format;
    uint8_t from_queue;
    uint32_t play_position;
    uint8_t has_played;
} DspBuffer;

typedef struct {
    int enabled;
    uint16_t sync_count;
    float gain[3][4];
    float gain_ramp_start[3][4];
    int gain_ramp_active[3];

    uint16_t mono_or_stereo;
    uint16_t format;

    int16_t *cur;          /* dekodierter Rest des aktuellen Puffers */
    uint32_t cur_len;
    uint32_t cur_cap;
    uint32_t current_sample_number;
    uint32_t current_buffer_physical_address;
    uint32_t current_buffer_length;
    uint16_t current_buffer_mono_or_stereo;
    uint16_t current_buffer_format;
    int current_buffer_is_looping;
    int buffer_update;
    uint16_t last_buffer_id;
    uint16_t current_buffer_id;

    int16_t adpcm_coeffs[16];
    int adpcm_yn1;
    int adpcm_yn2;

    float rate_multiplier;
    int interpolation_mode;
    AudioInterpState interp;

    int simple_enabled;
    int biquad_enabled;
    int simple_b0, simple_a1;
    int simple_y1[2];
    int biquad_a1, biquad_a2, biquad_b0, biquad_b1, biquad_b2;
    int biquad_x1[2], biquad_x2[2], biquad_y1[2], biquad_y2[2];

    DspBuffer queue[MAX_QUEUE];
    int queue_len;

    int16_t frame[SPF][2];
} DspSource;

typedef struct {
    int enabled;
    uint32_t delay_samples;
    float g, a, b;
    float *delay_output[4];
    float last_output[4];
    uint32_t delay_pos;
} DspDelay;

typedef struct {
    float intermediate_mixer_volume[3];
    int aux_bus_enable[2];
    int output_format;
    int32_t mix_buffer[3][SPF][4];
    int16_t current_frame[SPF][2];
    DspDelay delay[2];
} DspMixers;

typedef struct {
    uint8_t *mem;
    uint32_t mem_size;
    uint8_t *region[2];
    int state;
    int sleep_pending;
    uint32_t sleep_reply_frames;
    DspSource sources[AUDIO_DSP_NUM_SOURCES];
    DspMixers mixers;
    void (*lock_enter)(void);
    void (*lock_leave)(void);
    void (*interrupt)(int type, int pipe);
    void (*struct_addresses)(void);
    volatile int running;
    HANDLE thread;
    unsigned long long frame_count;
    unsigned diag_frames;
    int verbose;
    uint8_t region_backup[AUDIO_DSP_REGION_SIZE * 2];
} DspState;

static DspState g_dsp;

/* ---------- Gast-Speicher lesen/schreiben ---------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
/* u32_dsp ist im Gast mittelendian: die beiden 16-Bit-Haelften sind vertauscht
 * (shared_memory.h u32_dsp). */
static uint32_t rd_dsp32(const uint8_t *p) {
    return ((uint32_t)p[2] | ((uint32_t)p[3] << 8) | ((uint32_t)p[0] << 16) |
            ((uint32_t)p[1] << 24));
}
static void wr_dsp32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xFF);
    p[1] = (uint8_t)((v >> 24) & 0xFF);
    p[2] = (uint8_t)(v & 0xFF);
    p[3] = (uint8_t)((v >> 8) & 0xFF);
}
static float rd_f32(const uint8_t *p) {
    float f;
    memcpy(&f, p, 4);
    return f;
}

/* Regionen: read = hoeherer Frame-Counter, write = die andere (Azahar
 * CurrentRegionIndex/ReadRegion/WriteRegion). */
static int current_region_index(DspState *d) {
    uint16_t fc0 = rd16(d->region[0] + SM_FRAME_COUNTER);
    uint16_t fc1 = rd16(d->region[1] + SM_FRAME_COUNTER);
    if (fc0 == 0xFFFFu && fc1 != 0xFFFEu) return 1;
    if (fc1 == 0xFFFFu && fc0 != 0xFFFEu) return 0;
    return (fc0 > fc1) ? 0 : 1;
}

static uint8_t *src_cfg(DspState *d, uint8_t *region, int i) {
    return region + SM_SOURCE_CONFIG + (size_t)i * CFG_SIZE;
}
static uint8_t *src_status(uint8_t *region, int i) {
    return region + SM_SOURCE_STATUS + (size_t)i * 12;
}
static uint8_t *coeff_ptr(uint8_t *region, int i) {
    return region + SM_ADPCM_COEFFS + (size_t)i * 32;
}

static int mem_range_ok(DspState *d, uint32_t addr, uint64_t bytes) {
    uint64_t a = (uint64_t)(addr & 0x1FFFFFFFu);
    return d->mem != NULL && a + bytes <= (uint64_t)d->mem_size;
}

/* ---------- Quellen ---------- */

static void source_reset(DspSource *s) {
    free(s->cur);
    memset(s, 0, sizeof(*s));
    s->mono_or_stereo = 1; /* MonoOrStereo::Mono */
    s->format = 2;         /* Format::ADPCM */
    s->rate_multiplier = 1.0f;
    s->interpolation_mode = 0; /* Polyphase (Azahar nutzt dafuer Linear) */
    s->simple_b0 = 1 << 15;
    s->biquad_b0 = 1 << 14;
}

static int source_ensure_cap(DspState *d, DspSource *s, uint32_t need) {
    int16_t *p;
    if (need > MAX_DECODE_SAMPLES) return 0;
    if (need <= s->cur_cap) return 1;
    p = (int16_t *)realloc(s->cur, (size_t)need * 2 * sizeof(int16_t));
    if (p == NULL) return 0;
    s->cur = p;
    s->cur_cap = need;
    (void)d;
    return 1;
}

static int decode_into(DspState *d, DspSource *s, const DspBuffer *b) {
    uint32_t addr = b->physical_address & 0x1FFFFFFFu;
    uint32_t need = b->length + (b->length & 1u);
    int stereo = b->mono_or_stereo == 2;
    uint64_t bytes;

    if (need == 0 || need > MAX_DECODE_SAMPLES) { s->cur_len = 0; return 0; }
    if (b->format == 2) {
        bytes = ((uint64_t)b->length + 13) / 14 * 8;
    } else if (b->format == 1) {
        bytes = (uint64_t)b->length * (stereo ? 4 : 2);
    } else {
        bytes = (uint64_t)b->length * (stereo ? 2 : 1);
    }
    if (!mem_range_ok(d, addr, bytes) || !source_ensure_cap(d, s, need)) {
        s->cur_len = 0;
        return 0;
    }

    if (b->format == 2) {
        s->cur_len = audio_adpcm_decode(d->mem + addr, b->length, s->adpcm_coeffs,
                                        &s->adpcm_yn1, &s->adpcm_yn2, (int16_t (*)[2])s->cur,
                                        s->cur_cap);
    } else if (b->format == 1) {
        s->cur_len = audio_pcm16_decode(d->mem + addr, b->length, stereo, (int16_t (*)[2])s->cur,
                                        s->cur_cap);
    } else {
        s->cur_len = audio_pcm8_decode(d->mem + addr, b->length, stereo, (int16_t (*)[2])s->cur,
                                       s->cur_cap);
    }
    return 1;
}

static int queue_push(DspSource *s, const DspBuffer *b) {
    if (s->queue_len >= MAX_QUEUE) return 0;
    s->queue[s->queue_len++] = *b;
    return 1;
}

/* Kleinste buffer_id zuerst (Azahar BufferOrder). */
static int queue_pop(DspSource *s, DspBuffer *out) {
    int best = -1;
    if (s->queue_len == 0) return 0;
    for (int i = 0; i < s->queue_len; i++)
        if (best < 0 || s->queue[i].buffer_id < s->queue[best].buffer_id) best = i;
    *out = s->queue[best];
    memmove(&s->queue[best], &s->queue[best + 1],
            (size_t)(s->queue_len - best - 1) * sizeof(DspBuffer));
    s->queue_len--;
    return 1;
}

static int source_dequeue(DspState *d, DspSource *s) {
    DspBuffer b;

    if (s->cur_len != 0) return 0;
    if (!queue_pop(s, &b)) return 0;

    if (b.adpcm_dirty) {
        s->adpcm_yn1 = b.adpcm_yn[0];
        s->adpcm_yn2 = b.adpcm_yn[1];
    }
    if (!decode_into(d, s, &b)) {
        /* Azahar: ungueltige Adresse -> leerer Puffer, aber Buffer gilt als
         * verbraucht (dequeue liefert true). */
        s->cur_len = 0;
    }

    s->current_sample_number = b.has_played ? 0 : b.play_position;
    s->current_buffer_physical_address = b.physical_address;
    s->current_buffer_length = b.length;
    s->current_buffer_mono_or_stereo = b.mono_or_stereo;
    s->current_buffer_format = b.format;
    s->current_buffer_is_looping = b.is_looping;
    s->current_buffer_id = b.buffer_id;
    s->last_buffer_id = 0;
    s->buffer_update = b.from_queue && !b.has_played;

    if (b.is_looping) {
        b.has_played = 1;
        queue_push(s, &b);
    }

    if (s->current_sample_number >= s->cur_len) {
        if (s->current_sample_number > s->cur_len) s->current_sample_number = 0;
        s->cur_len = 0;
    } else {
        uint32_t drop = s->current_sample_number;
        memmove(s->cur, s->cur + drop * 2,
                (size_t)(s->cur_len - drop) * 2 * sizeof(int16_t));
        s->cur_len -= drop;
    }
    return 1;
}

static void biquad_process(DspSource *s) {
    for (int i = 0; i < SPF; i++) {
        for (int c = 0; c < 2; c++) {
            int x0 = s->frame[i][c];
            int64_t tmp = (int64_t)s->biquad_b0 * x0 +
                          (int64_t)s->biquad_b1 * s->biquad_x1[c] +
                          (int64_t)s->biquad_b2 * s->biquad_x2[c] +
                          (int64_t)s->biquad_a1 * s->biquad_y1[c] +
                          (int64_t)s->biquad_a2 * s->biquad_y2[c];
            int y0 = (int)(tmp >> 14);
            if (y0 < -32768) y0 = -32768;
            if (y0 > 32767) y0 = 32767;
            s->biquad_x2[c] = s->biquad_x1[c];
            s->biquad_x1[c] = x0;
            s->biquad_y2[c] = s->biquad_y1[c];
            s->biquad_y1[c] = y0;
            s->frame[i][c] = (int16_t)y0;
        }
    }
}

static void simple_process(DspSource *s) {
    for (int i = 0; i < SPF; i++) {
        for (int c = 0; c < 2; c++) {
            int64_t tmp = (int64_t)s->simple_b0 * s->frame[i][c] +
                          (int64_t)s->simple_a1 * s->simple_y1[c];
            int y0 = (int)(tmp >> 15);
            if (y0 < -32768) y0 = -32768;
            if (y0 > 32767) y0 = 32767;
            s->simple_y1[c] = y0;
            s->frame[i][c] = (int16_t)y0;
        }
    }
}

static void source_generate_frame(DspState *d, DspSource *s) {
    uint32_t pos = 0;

    memset(s->frame, 0, sizeof(s->frame));
    if (s->cur_len == 0) {
        if (source_dequeue(d, s)) return;
        s->enabled = 0;
        s->buffer_update = 1;
        s->last_buffer_id = s->current_buffer_id;
        s->current_buffer_id = 0;
        return;
    }

    while (pos < SPF) {
        uint32_t before;
        uint32_t consumed;
        uint32_t len;
        const int16_t (*in)[2];

        if (s->cur_len == 0 && !source_dequeue(d, s)) break;

        before = s->cur_len;
        len = s->cur_len;
        in = (const int16_t (*)[2])s->cur;
        if (s->interpolation_mode == 1 || s->interpolation_mode == 0) {
            consumed = audio_interp_linear(&s->interp, in, &len,
                                           s->rate_multiplier, s->frame, SPF, &pos);
        } else {
            consumed = audio_interp_none(&s->interp, in, &len,
                                         s->rate_multiplier, s->frame, SPF, &pos);
        }
        s->cur_len = len;
        if (consumed > 0)
            memmove(s->cur, s->cur + consumed * 2,
                    (size_t)len * 2 * sizeof(int16_t));
        s->current_sample_number += consumed;
        (void)before;
    }

    if (s->simple_enabled) simple_process(s);
    if (s->biquad_enabled) biquad_process(s);
}

static void source_parse_config(DspState *d, DspSource *s, uint8_t *cfg,
                                const uint8_t *coeff) {
    uint32_t dirty = rd32(cfg + CFG_DIRTY);
    uint32_t play_position;

    if (dirty == 0) return;

    if (dirty & (1u << 29)) {
        source_reset(s);
        (void)d;
    }
    if (dirty & (1u << 4)) {
        s->queue_len = 0;
    }
    if (dirty & (1u << 16)) {
        s->enabled = cfg[CFG_ENABLE] != 0;
    }
    if (dirty & (1u << 28)) {
        s->sync_count = rd16(cfg + CFG_SYNC_COUNT);
    }
    if (dirty & (1u << 18)) {
        s->rate_multiplier = rd_f32(cfg + CFG_RATE);
        if (!(s->rate_multiplier > 0.0f)) s->rate_multiplier = 1.0f;
    }
    if (dirty & (1u << 2)) {
        for (int i = 0; i < 16; i++)
            s->adpcm_coeffs[i] = (int16_t)rd16(coeff + i * 2);
    }
    for (int g = 0; g < 3; g++) {
        if (dirty & (1u << (25 + g))) {
            for (int c = 0; c < 4; c++) {
                s->gain_ramp_start[g][c] = s->gain[g][c];
                s->gain[g][c] = rd_f32(cfg + CFG_GAIN + (g * 4 + c) * 4);
            }
            s->gain_ramp_active[g] = 1;
        }
    }
    if (dirty & (1u << 22)) {
        uint16_t en = rd16(cfg + CFG_FILTERS_ENABLED);
        s->simple_enabled = en & 1;
        s->biquad_enabled = (en >> 1) & 1;
        if (!s->simple_enabled) {
            memset(s->simple_y1, 0, sizeof(s->simple_y1));
            s->simple_b0 = 1 << 15;
            s->simple_a1 = 0;
        }
        if (!s->biquad_enabled) {
            memset(s->biquad_x1, 0, sizeof(s->biquad_x1));
            memset(s->biquad_x2, 0, sizeof(s->biquad_x2));
            memset(s->biquad_y1, 0, sizeof(s->biquad_y1));
            memset(s->biquad_y2, 0, sizeof(s->biquad_y2));
            s->biquad_b0 = 1 << 14;
            s->biquad_a1 = s->biquad_a2 = s->biquad_b1 = s->biquad_b2 = 0;
        }
    }
    if (dirty & (1u << 23)) {
        s->simple_b0 = (int16_t)rd16(cfg + CFG_SIMPLE_FILTER);
        s->simple_a1 = (int16_t)rd16(cfg + CFG_SIMPLE_FILTER + 2);
    }
    if (dirty & (1u << 24)) {
        s->biquad_a2 = (int16_t)rd16(cfg + CFG_BIQUAD_FILTER + 0);
        s->biquad_a1 = (int16_t)rd16(cfg + CFG_BIQUAD_FILTER + 2);
        s->biquad_b2 = (int16_t)rd16(cfg + CFG_BIQUAD_FILTER + 4);
        s->biquad_b1 = (int16_t)rd16(cfg + CFG_BIQUAD_FILTER + 6);
        s->biquad_b0 = (int16_t)rd16(cfg + CFG_BIQUAD_FILTER + 8);
    }
    if (dirty & (1u << 17)) {
        s->interpolation_mode = cfg[CFG_INTERP];
    }
    if (dirty & (1u << 0)) {
        uint16_t f = rd16(cfg + CFG_EMB_FLAGS1);
        s->format = (uint16_t)((f >> 2) & 3u); /* Bits 2..3 der flags1 */
    }
    if (dirty & (1u << 1)) {
        uint16_t f = rd16(cfg + CFG_EMB_FLAGS1);
        s->mono_or_stereo = f & 3u;
    }
    if (dirty & (1u << 30)) {
        uint16_t f = rd16(cfg + CFG_EMB_FLAGS1);
        s->format = (uint16_t)((f >> 2) & 3u);
        s->mono_or_stereo = f & 3u;
    }

    play_position = 0;
    if (dirty & (1u << 21)) {
        play_position = rd_dsp32(cfg + CFG_PLAY_POSITION);
    }

    if (dirty & (1u << 3)) {
        /* Teilupdate des laufenden Puffers: nur PCM16 (Azahar). */
        if (s->current_buffer_format == 1 && s->current_buffer_physical_address != 0) {
            DspBuffer b;
            memset(&b, 0, sizeof(b));
            b.physical_address = s->current_buffer_physical_address;
            b.length = rd_dsp32(cfg + CFG_EMB_LEN);
            b.mono_or_stereo = s->mono_or_stereo;
            b.format = s->format;
            if (decode_into(d, s, &b) && s->cur_len > 0) {
                if (s->current_sample_number >= s->cur_len) {
                    s->current_sample_number = 0;
                } else {
                    uint32_t drop = s->current_sample_number;
                    memmove(s->cur, s->cur + drop * 2,
                            (size_t)(s->cur_len - drop) * 2 * sizeof(int16_t));
                    s->cur_len -= drop;
                }
            }
        }
    }

    if (dirty & (1u << 30)) {
        DspBuffer b;
        int32_t len = (int32_t)rd_dsp32(cfg + CFG_EMB_LEN);
        if (len < 0) {
            /* Azahar ueberspringt untergelaufene Laengen (Schutz). */
        } else {
            memset(&b, 0, sizeof(b));
            b.physical_address = rd_dsp32(cfg + CFG_EMB_PHYS);
            b.length = (uint32_t)len;
            b.adpcm_ps = (uint8_t)rd16(cfg + CFG_EMB_PS);
            b.adpcm_yn[0] = rd16(cfg + CFG_EMB_YN);
            b.adpcm_yn[1] = rd16(cfg + CFG_EMB_YN + 2);
            b.adpcm_dirty = rd16(cfg + CFG_EMB_FLAGS2) & 1;
            b.is_looping = (rd16(cfg + CFG_EMB_FLAGS2) >> 1) & 1;
            b.buffer_id = rd16(cfg + CFG_EMB_BUFFER_ID);
            b.mono_or_stereo = s->mono_or_stereo;
            b.format = s->format;
            b.from_queue = 0;
            b.play_position = play_position;
            b.has_played = 0;
            queue_push(s, &b);
        }
    }

    if (dirty & (1u << 19)) {
        uint16_t mask = rd16(cfg + CFG_BUFFERS_DIRTY);
        for (int i = 0; i < 4; i++) {
            if (mask & (1u << i)) {
                const uint8_t *bp = cfg + CFG_BUFFERS + i * BUF_SIZE;
                int32_t len = (int32_t)rd_dsp32(bp + BUF_LEN);
                if (len >= 0) {
                    DspBuffer b;
                    memset(&b, 0, sizeof(b));
                    b.physical_address = rd_dsp32(bp + BUF_PHYS);
                    b.length = (uint32_t)len;
                    b.adpcm_ps = (uint8_t)rd16(bp + BUF_PS);
                    b.adpcm_yn[0] = rd16(bp + BUF_YN);
                    b.adpcm_yn[1] = rd16(bp + BUF_YN + 2);
                    b.adpcm_dirty = bp[BUF_DIRTY] != 0;
                    b.is_looping = bp[BUF_LOOPING] != 0;
                    b.buffer_id = rd16(bp + BUF_ID);
                    b.mono_or_stereo = s->mono_or_stereo;
                    b.format = s->format;
                    b.from_queue = 1;
                    b.play_position = 0;
                    b.has_played = 0;
                    queue_push(s, &b);
                }
            }
        }
        wr16(cfg + CFG_BUFFERS_DIRTY, 0);
    }

    wr32(cfg + CFG_DIRTY, 0);
}

static void source_mix_into(DspSource *s, int32_t (*dest)[4], int mix) {
    const float *gains = s->gain[mix];
    int ramp = s->gain_ramp_active[mix];
    const float *start = s->gain_ramp_start[mix];
    const float ramp_scale = 1.0f / (float)(SPF - 1);

    if (!s->enabled) {
        memcpy(s->gain_ramp_start[mix], gains, sizeof(s->gain_ramp_start[mix]));
        s->gain_ramp_active[mix] = 0;
        return;
    }
    for (int i = 0; i < SPF; i++) {
        float progress = (float)i * ramp_scale;
        float g0 = ramp ? start[0] + (gains[0] - start[0]) * progress : gains[0];
        float g1 = ramp ? start[1] + (gains[1] - start[1]) * progress : gains[1];
        float g2 = ramp ? start[2] + (gains[2] - start[2]) * progress : gains[2];
        float g3 = ramp ? start[3] + (gains[3] - start[3]) * progress : gains[3];
        dest[i][0] += (int32_t)(g0 * (float)s->frame[i][0]);
        dest[i][1] += (int32_t)(g1 * (float)s->frame[i][1]);
        dest[i][2] += (int32_t)(g2 * (float)s->frame[i][0]);
        dest[i][3] += (int32_t)(g3 * (float)s->frame[i][1]);
    }
    if (ramp) {
        memcpy(s->gain_ramp_start[mix], gains, sizeof(s->gain_ramp_start[mix]));
        s->gain_ramp_active[mix] = 0;
    }
}

static void source_write_status(DspSource *s, uint8_t *st) {
    st[ST_ENABLED] = (uint8_t)(s->enabled ? 1 : 0);
    st[ST_CURRENT_ID_DIRTY] = (uint8_t)(s->buffer_update ? 1 : 0);
    s->buffer_update = 0;
    wr16(st + ST_SYNC_COUNT, s->sync_count);
    wr_dsp32(st + ST_BUFFER_POSITION, s->current_sample_number);
    wr16(st + ST_CURRENT_ID, s->current_buffer_id);
    wr16(st + ST_LAST_ID, s->last_buffer_id);
}

static void source_tick(DspState *d, DspSource *s, uint8_t *cfg, uint8_t *status,
                        const uint8_t *coeff) {
    source_parse_config(d, s, cfg, coeff);
    if (s->enabled) source_generate_frame(d, s);
    source_write_status(s, status);
}

/* ---------- Mixer ---------- */

static void delay_reset(DspDelay *e) {
    for (int c = 0; c < 4; c++) {
        free(e->delay_output[c]);
        e->delay_output[c] = NULL;
    }
    e->enabled = 0;
    e->delay_samples = 0;
    e->g = e->a = e->b = 0.0f;
    e->delay_pos = 0;
    memset(e->last_output, 0, sizeof(e->last_output));
}

static void delay_parse(DspDelay *e, uint8_t *cfg) {
    uint16_t dirty = rd16(cfg + 0);
    int was_enabled = e->enabled;
    uint32_t samples;

    if (!dirty) return;
    wr16(cfg + 0, 0);
    e->enabled = rd16(cfg + 2) != 0;
    if (!e->enabled) {
        if (was_enabled) delay_reset(e);
        return;
    }
    samples = (uint32_t)rd16(cfg + 12) * SPF;
    if (samples == 0) {
        e->enabled = 0;
        delay_reset(e);
        return;
    }
    if (samples > (uint32_t)(4 * AUDIO_DSP_SAMPLE_RATE))
        samples = (uint32_t)(4 * AUDIO_DSP_SAMPLE_RATE);

    e->g = (float)(int16_t)rd16(cfg + 14) / 128.0f;
    e->a = (float)(int16_t)rd16(cfg + 16) / 128.0f;
    e->b = (float)(int16_t)rd16(cfg + 18) / 128.0f;

    if (samples != e->delay_samples || e->delay_output[0] == NULL) {
        for (int c = 0; c < 4; c++) {
            float *p = (float *)realloc(e->delay_output[c], (size_t)samples * sizeof(float));
            if (p == NULL) {
                e->enabled = 0;
                delay_reset(e);
                return;
            }
            e->delay_output[c] = p;
            memset(p, 0, (size_t)samples * sizeof(float));
        }
        e->delay_samples = samples;
        e->delay_pos = 0;
        memset(e->last_output, 0, sizeof(e->last_output));
    }
}

static void delay_process(DspDelay *e, int32_t (*frame)[4]) {
    if (!e->enabled || e->delay_samples == 0) return;
    for (int i = 0; i < SPF; i++) {
        uint32_t pos = (e->delay_pos + (uint32_t)i) % e->delay_samples;
        for (int c = 0; c < 4; c++) {
            float x = (float)frame[i][c];
            float delayed = e->delay_output[c][pos];
            float y = e->b * e->last_output[c] + e->a * delayed;
            e->delay_output[c][pos] = x - e->g * y;
            e->last_output[c] = y;
            if (y < -2147483648.0f) y = -2147483648.0f;
            if (y > 2147483520.0f) y = 2147483520.0f;
            frame[i][c] = (int32_t)y;
        }
    }
    e->delay_pos = (e->delay_pos + SPF) % e->delay_samples;
}

static void mixers_reset(DspMixers *m) {
    for (int i = 0; i < 2; i++) delay_reset(&m->delay[i]);
    memset(m, 0, sizeof(*m));
    m->output_format = 1; /* Stereo */
}

static void mixers_parse_config(DspState *d, DspMixers *m, uint8_t *cfg) {
    uint32_t dirty = rd32(cfg + DSPCFG_DIRTY);

    if (dirty == 0) return;
    if (dirty & (1u << 8)) m->aux_bus_enable[0] = rd16(cfg + DSPCFG_AUX_BUS) != 0;
    if (dirty & (1u << 9)) m->aux_bus_enable[1] = rd16(cfg + DSPCFG_AUX_BUS + 2) != 0;
    if (dirty & (1u << 16))
        m->intermediate_mixer_volume[0] = rd_f32(cfg + DSPCFG_MASTER_VOLUME);
    if (dirty & (1u << 24))
        m->intermediate_mixer_volume[1] = rd_f32(cfg + DSPCFG_AUX_RETURN);
    if (dirty & (1u << 25))
        m->intermediate_mixer_volume[2] = rd_f32(cfg + DSPCFG_AUX_RETURN + 4);
    if (dirty & (1u << 26))
        m->output_format = rd16(cfg + DSPCFG_OUTPUT_FORMAT);
    if (dirty & (1u << 10)) delay_parse(&m->delay[0], cfg + DSPCFG_DELAY);
    if (dirty & (1u << 11)) delay_parse(&m->delay[1], cfg + DSPCFG_DELAY + DELAY_SIZE);
    /* Reverb ist auch in Azahar nicht implementiert (effects.cpp). */
    wr32(cfg + DSPCFG_DIRTY, 0);
    (void)d;
}

static int16_t clamp_s16(int32_t v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return (int16_t)v;
}

static void mixers_downmix(DspMixers *m, float gain, const int32_t (*samples)[4]) {
    for (int i = 0; i < SPF; i++) {
        int16_t left, right;
        if (m->output_format == 0) {
            int32_t mono = (int32_t)((gain * (float)samples[i][0] +
                                      gain * (float)samples[i][1] +
                                      gain * (float)samples[i][2] +
                                      gain * (float)samples[i][3]) / 2.0f);
            int16_t v = clamp_s16(mono);
            left = right = v;
        } else {
            /* Stereo und Surround (Azahar faellt fuer Surround auf Stereo). */
            left = clamp_s16((int32_t)(gain * (float)samples[i][0] +
                                       gain * (float)samples[i][2]));
            right = clamp_s16((int32_t)(gain * (float)samples[i][1] +
                                        gain * (float)samples[i][3]));
        }
        m->current_frame[i][0] = clamp_s16((int32_t)m->current_frame[i][0] + left);
        m->current_frame[i][1] = clamp_s16((int32_t)m->current_frame[i][1] + right);
    }
}

static void mixers_tick(DspState *d, uint8_t *read, uint8_t *write,
                        int32_t (*intermediate)[SPF][4]) {
    DspMixers *m = &d->mixers;

    mixers_parse_config(d, m, read + SM_DSP_CONFIG);

    /* AuxReturn: vom Spiel bearbeitete Zwischenmixe uebernehmen. */
    for (int bus = 0; bus < 2; bus++) {
        if (m->aux_bus_enable[bus]) {
            const uint8_t *src = read + SM_INTERMEDIATE + (size_t)bus * MIX_FRAME_BYTES;
            for (int i = 0; i < SPF; i++)
                for (int c = 0; c < 4; c++)
                    m->mix_buffer[bus + 1][i][c] =
                        (int32_t)rd32(src + ((size_t)c * SPF + (size_t)i) * 4);
        }
    }

    memcpy(m->mix_buffer[0], intermediate[0], sizeof(m->mix_buffer[0]));

    for (int bus = 0; bus < 2; bus++) {
        if (m->aux_bus_enable[bus]) {
            uint8_t *dst = write + SM_INTERMEDIATE + (size_t)bus * MIX_FRAME_BYTES;
            for (int i = 0; i < SPF; i++)
                for (int c = 0; c < 4; c++)
                    wr32(dst + ((size_t)c * SPF + (size_t)i) * 4,
                         (uint32_t)intermediate[bus + 1][i][c]);
        } else {
            memcpy(m->mix_buffer[bus + 1], intermediate[bus + 1],
                   sizeof(m->mix_buffer[bus + 1]));
        }
    }

    for (int bus = 0; bus < 2; bus++) {
        delay_process(&m->delay[bus], m->mix_buffer[bus + 1]);
    }

    memset(m->current_frame, 0, sizeof(m->current_frame));
    for (int mix = 0; mix < 3; mix++)
        mixers_downmix(m, m->intermediate_mixer_volume[mix], m->mix_buffer[mix]);
}

/* ---------- DSP-Status und Frame ---------- */

static void write_dsp_status(uint8_t *write) {
    wr16(write + SM_DSP_STATUS, 0);      /* unknown */
    wr16(write + SM_DSP_STATUS + 2, 0);  /* dropped_frames */
}

static void generate_frame(DspState *d) {
    int idx = current_region_index(d);
    uint8_t *read = d->region[idx];
    uint8_t *write = d->region[idx ^ 1];
    int32_t intermediate[3][SPF][4];

    if (d->verbose && d->diag_frames < 4) {
        d->diag_frames++;
        int dirty = 0, enabled = 0;
        for (int i = 0; i < AUDIO_DSP_NUM_SOURCES; i++) {
            if (rd32(src_cfg(d, read, i) + CFG_DIRTY) != 0) dirty++;
            if (d->sources[i].enabled) enabled++;
        }
        printf("[audio] frame=%llu region=%d fc=%u/%u dsp_dirty=%08X src_dirty=%d enabled=%d\n",
               d->frame_count, idx, rd16(d->region[0] + SM_FRAME_COUNTER),
               rd16(d->region[1] + SM_FRAME_COUNTER),
               rd32(read + SM_DSP_CONFIG + DSPCFG_DIRTY), dirty, enabled);
    }

    memset(intermediate, 0, sizeof(intermediate));
    for (int i = 0; i < AUDIO_DSP_NUM_SOURCES; i++) {
        DspSource *s = &d->sources[i];
        source_tick(d, s, src_cfg(d, read, i), src_status(write, i),
                    coeff_ptr(read, i));
        for (int mix = 0; mix < 3; mix++)
            source_mix_into(s, intermediate[mix], mix);
    }

    mixers_tick(d, read, write, intermediate);
    write_dsp_status(write);

    for (int i = 0; i < SPF; i++) {
        wr16(write + SM_FINAL_SAMPLES + (size_t)i * 4,
             (uint16_t)d->mixers.current_frame[i][0]);
        wr16(write + SM_FINAL_SAMPLES + (size_t)i * 4 + 2,
             (uint16_t)d->mixers.current_frame[i][1]);
    }

    host_audio_push(&d->mixers.current_frame[0][0], SPF);
}

/* ---------- Ablaufsteuerung ---------- */

static void dsp_initialize(DspState *d) {
    /* Azahar: dsp_memory->raw_memory.fill(0) fuer den gesamten DSP-RAM,
     * mixers.Reset(), sources Reset. Danach schreibt das Spiel die Regionen
     * neu (die Struktur-Adressen kommen ueber Pipe 2). */
    memset(d->region[0], 0, AUDIO_DSP_REGION_SIZE);
    memset(d->region[1], 0, AUDIO_DSP_REGION_SIZE);
    mixers_reset(&d->mixers);
    for (int i = 0; i < AUDIO_DSP_NUM_SOURCES; i++) source_reset(&d->sources[i]);
}

static void dsp_sleep(DspState *d) {
    memcpy(d->region_backup, d->region[0], AUDIO_DSP_REGION_SIZE);
    memcpy(d->region_backup + AUDIO_DSP_REGION_SIZE, d->region[1],
           AUDIO_DSP_REGION_SIZE);
}

static void dsp_wakeup(DspState *d) {
    memcpy(d->region[0], d->region_backup, AUDIO_DSP_REGION_SIZE);
    memcpy(d->region[1], d->region_backup + AUDIO_DSP_REGION_SIZE,
           AUDIO_DSP_REGION_SIZE);
}

static void finish_pending_sleep(DspState *d) {
    if (!d->sleep_pending) return;
    d->sleep_pending = 0;
    dsp_sleep(d);
    d->state = AUDIO_DSP_SLEEPING;
    if (d->struct_addresses) d->struct_addresses();
}

static void audio_tick(DspState *d) {
    int on;

    if (d->sleep_pending) {
        finish_pending_sleep(d);
        d->sleep_reply_frames = 2;
    } else if (d->sleep_reply_frames > 0 && --d->sleep_reply_frames == 0) {
        if (d->interrupt) d->interrupt(AUDIO_DSP_INT_ZERO, 0);
    }

    on = d->state == AUDIO_DSP_ON && d->region[0] != NULL;
    if (on) {
        generate_frame(d);
        if (d->interrupt) d->interrupt(AUDIO_DSP_INT_PIPE, 2);
    }
    d->frame_count++;
}

static DWORD WINAPI audio_thread(LPVOID p) {
    DspState *d = (DspState *)p;
    LARGE_INTEGER f, now, next;
    double period = (double)SPF / (double)AUDIO_DSP_SAMPLE_RATE;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&next);
    while (d->running) {
        QueryPerformanceCounter(&now);
        if ((double)(now.QuadPart - next.QuadPart) / (double)f.QuadPart < 0.0) {
            Sleep(1);
            continue;
        }
        if (d->lock_enter) d->lock_enter();
        audio_tick(d);
        if (d->lock_leave) d->lock_leave();

        next.QuadPart += (LONGLONG)(period * (double)f.QuadPart);
        QueryPerformanceCounter(&now);
        if ((double)(now.QuadPart - next.QuadPart) / (double)f.QuadPart > 0.03) {
            next = now;
        }
    }
    return 0;
}

/* ---------- Oeffentliche Anbindung an hle.c ---------- */

void audio_dsp_init(uint8_t *mem, uint32_t mem_size) {
    DspState *d = &g_dsp;
    memset(d, 0, sizeof(*d));
    d->mem = mem;
    d->mem_size = mem_size;
    d->verbose = getenv("HLE_AUDIO_LOG") != NULL;
    if (mem != NULL && mem_size >= AUDIO_DSP_REGION1_ADDR + AUDIO_DSP_REGION_SIZE) {
        d->region[0] = mem + AUDIO_DSP_REGION0_ADDR;
        d->region[1] = mem + AUDIO_DSP_REGION1_ADDR;
    }
    mixers_reset(&d->mixers);
    for (int i = 0; i < AUDIO_DSP_NUM_SOURCES; i++) source_reset(&d->sources[i]);
    d->state = AUDIO_DSP_OFF;
}

void audio_dsp_set_lock(void (*enter)(void), void (*leave)(void)) {
    g_dsp.lock_enter = enter;
    g_dsp.lock_leave = leave;
}

void audio_dsp_set_interrupt(void (*handler)(int type, int pipe)) {
    g_dsp.interrupt = handler;
}

void audio_dsp_set_struct_addresses(void (*handler)(void)) {
    g_dsp.struct_addresses = handler;
}

void audio_dsp_set_host_output(int enabled) {
    host_audio_set_enabled(enabled);
}

void audio_dsp_pipe_write(const uint8_t *buffer, uint32_t size) {
    DspState *d = &g_dsp;
    if (buffer == NULL || size != 4) return; /* Azahar: genau 4 Byte */
    if (d->verbose) printf("[audio] pipe state=%u\n", buffer[0]);
    /* Azahar DSP_DSP::WriteProcessPipe: Bytes 2/3 sind Muell und werden
     * genullt; der StateChange steht in Byte 0. */
    switch (buffer[0]) {
    case 0: /* Initialize */
        dsp_initialize(d);
        d->state = AUDIO_DSP_ON;
        if (d->struct_addresses) d->struct_addresses();
        break;
    case 1: /* Shutdown */
        d->state = AUDIO_DSP_OFF;
        break;
    case 2: /* Wakeup */
        dsp_wakeup(d);
        d->state = AUDIO_DSP_ON;
        if (d->struct_addresses) d->struct_addresses();
        break;
    case 3: /* Sleep */
        d->sleep_pending = 1;
        break;
    default:
        d->state = AUDIO_DSP_OFF;
        break;
    }
}

uint16_t audio_dsp_recv_data(void) {
    DspState *d = &g_dsp;
    if (d->sleep_pending || d->sleep_reply_frames > 0) return 1;
    return d->state == AUDIO_DSP_ON ? 0 : 1;
}

int audio_dsp_recv_data_ready(void) {
    DspState *d = &g_dsp;
    return !d->sleep_pending && d->sleep_reply_frames == 0;
}

void audio_dsp_start(void) {
    DspState *d = &g_dsp;
    if (d->thread != NULL) return;
    d->running = 1;
    /* 1-ms-Timer fuer die 4,889-ms-Frames; ohne das rundet Sleep(1) je nach
       System auf ~15,6 ms. */
    timeBeginPeriod(1);
    d->thread = CreateThread(NULL, 0, audio_thread, d, 0, NULL);
}

void audio_dsp_stop(void) {
    DspState *d = &g_dsp;
    if (d->thread == NULL) return;
    d->running = 0;
    WaitForSingleObject(d->thread, 2000);
    CloseHandle(d->thread);
    d->thread = NULL;
    timeEndPeriod(1);
    host_audio_close();
}

unsigned long long audio_dsp_frames(void) {
    return g_dsp.frame_count;
}
