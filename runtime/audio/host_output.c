/* waveOut-Ausgabe des DSP-HLE. Ein Frame sind 160 Stereo-Samples bei
 * 32728 Hz (Azahar native_sample_rate), also 4,9 ms. Frueher puffert
 * waveOut nur vier Frames (~20 ms): jede Verzoegerung des DSP-Threads
 * (z. B. Warten auf die Gast-Sperre waehrend eines Frames) fuehrte zu
 * Unterlaeufen und hoerbarem Knacken. Jetzt bis zu OUT_BUFFERS Frames
 * (~156 ms); nach einem Unterlauf werden PREFILL Stille-Frames vorgelegt,
 * damit der Puffer wieder Reserve hat. Ist keiner frei, wird der Frame
 * verworfen und gezaehlt. Fehler beim Oeffnen deaktivieren die Ausgabe
 * dauerhaft (kein Einfluss auf den Gast).
 * Diagnose: HLE_AUDIO_WAV=<datei> schreibt alle erzeugten Frames
 * zusaetzlich als WAV (veraendert keinen Gastzustand). */
#include "host_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#include "dsp_hle.h"

#define OUT_BUFFERS 32
#define PREFILL 6

static HWAVEOUT g_dev;
static WAVEHDR g_hdr[OUT_BUFFERS];
static int16_t g_buf[OUT_BUFFERS][AUDIO_DSP_SAMPLES_PER_FRAME * 2];
static int g_queued[OUT_BUFFERS];
static int g_state;   /* 0 = noch nicht versucht, 1 = offen, -1 = aus */
static int g_enabled = 1;
static unsigned g_dropped;
static unsigned g_next;
static unsigned g_underruns;
static FILE *g_wav;
static unsigned long g_wav_bytes;
static int g_wav_tried;

static void wav_header(FILE *f, unsigned long data) {
    unsigned char h[44];
    unsigned rate = AUDIO_DSP_SAMPLE_RATE, br = rate * 4;
    memcpy(h, "RIFF", 4);
    h[4] = (unsigned char)(data + 36); h[5] = (unsigned char)((data + 36) >> 8);
    h[6] = (unsigned char)((data + 36) >> 16); h[7] = (unsigned char)((data + 36) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    h[24] = (unsigned char)rate; h[25] = (unsigned char)(rate >> 8);
    h[26] = (unsigned char)(rate >> 16); h[27] = 0;
    h[28] = (unsigned char)br; h[29] = (unsigned char)(br >> 8);
    h[30] = (unsigned char)(br >> 16); h[31] = (unsigned char)(br >> 24);
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (unsigned char)data; h[41] = (unsigned char)(data >> 8);
    h[42] = (unsigned char)(data >> 16); h[43] = (unsigned char)(data >> 24);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

static void wav_write(const int16_t *stereo, unsigned frames) {
    if (!g_wav_tried) {
        const char *p = getenv("HLE_AUDIO_WAV");
        g_wav_tried = 1;
        if (p != NULL && *p) {
            g_wav = fopen(p, "wb");
            if (g_wav) wav_header(g_wav, 0);
        }
    }
    if (!g_wav) return;
    fwrite(stereo, sizeof(int16_t) * 2, frames, g_wav);
    g_wav_bytes += frames * 4ul;
    if ((g_wav_bytes & 0xFFFFul) < frames * 4ul) { /* gelegentlich Kopf aktualisieren */
        wav_header(g_wav, g_wav_bytes);
        fflush(g_wav);
    }
}

static int queue_frame(const int16_t *stereo, unsigned frames) {
    int slot = -1;
    for (int i = 0; i < OUT_BUFFERS; i++) {
        int k = (int)((g_next + (unsigned)i) % OUT_BUFFERS);
        if (!g_queued[k]) { slot = k; break; }
        if (g_hdr[k].dwFlags & WHDR_DONE) { g_queued[k] = 0; slot = k; break; }
    }
    if (slot < 0) return 0;
    if (stereo) memcpy(g_buf[slot], stereo, frames * 2 * sizeof(int16_t));
    else memset(g_buf[slot], 0, frames * 2 * sizeof(int16_t));
    g_hdr[slot].dwBufferLength = frames * 2 * (DWORD)sizeof(int16_t);
    g_hdr[slot].dwFlags &= ~(DWORD)WHDR_DONE;
    if (waveOutWrite(g_dev, &g_hdr[slot], sizeof(g_hdr[slot])) != MMSYSERR_NOERROR) return 0;
    g_queued[slot] = 1;
    g_next = (unsigned)(slot + 1) % OUT_BUFFERS;
    return 1;
}

static int pending_frames(void) {
    int n = 0;
    for (int i = 0; i < OUT_BUFFERS; i++) {
        if (g_queued[i] && (g_hdr[i].dwFlags & WHDR_DONE)) g_queued[i] = 0;
        n += g_queued[i];
    }
    return n;
}

int host_audio_open(void) {
    WAVEFORMATEX wf;

    if (g_state != 0) return g_state == 1;
    g_state = -1;
    if (!g_enabled) return 0;

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = AUDIO_DSP_SAMPLE_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = 0;

    if (waveOutOpen(&g_dev, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return 0;

    for (int i = 0; i < OUT_BUFFERS; i++) {
        memset(&g_hdr[i], 0, sizeof(g_hdr[i]));
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = sizeof(g_buf[i]);
        if (waveOutPrepareHeader(g_dev, &g_hdr[i], sizeof(g_hdr[i])) != MMSYSERR_NOERROR) {
            waveOutClose(g_dev);
            return 0;
        }
        g_queued[i] = 0;
    }
    g_state = 1;
    return 1;
}

void host_audio_push(const int16_t *stereo, unsigned frames) {
    if (frames != AUDIO_DSP_SAMPLES_PER_FRAME) return;
    wav_write(stereo, frames);
    if (!g_enabled) return;
    if (!host_audio_open()) return;

    if (pending_frames() == 0) {
        /* Unterlauf (oder Start): Reserve vorlegen statt Frame fuer Frame
         * am Rand des Abrisses zu spielen. */
        g_underruns++;
        for (int i = 0; i < PREFILL; i++) queue_frame(NULL, frames);
    }
    if (!queue_frame(stereo, frames)) g_dropped++;
}

void host_audio_close(void) {
    if (g_state != 1) return;
    waveOutReset(g_dev);
    for (int i = 0; i < OUT_BUFFERS; i++)
        waveOutUnprepareHeader(g_dev, &g_hdr[i], sizeof(g_hdr[i]));
    waveOutClose(g_dev);
    g_state = 0;
    memset(g_hdr, 0, sizeof(g_hdr));
    memset(g_queued, 0, sizeof(g_queued));
}

void host_audio_set_enabled(int enabled) {
    g_enabled = enabled != 0;
    if (!g_enabled) host_audio_close();
}

unsigned host_audio_dropped(void) {
    return g_dropped;
}

unsigned host_audio_underruns(void) {
    return g_underruns;
}
