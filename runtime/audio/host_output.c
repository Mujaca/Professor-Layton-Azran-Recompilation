/* waveOut-Ausgabe des DSP-HLE. Ein Frame sind 160 Stereo-Samples bei
 * 32728 Hz (Azahar native_sample_rate). waveOut puffert vier Frames; ist
 * keiner frei, wird der Frame verworfen und gezaehlt. Fehler beim Oeffnen
 * deaktivieren die Ausgabe dauerhaft (kein Einfluss auf den Gast). */
#include "host_output.h"

#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#include "dsp_hle.h"

#define OUT_BUFFERS 4

static HWAVEOUT g_dev;
static WAVEHDR g_hdr[OUT_BUFFERS];
static int16_t g_buf[OUT_BUFFERS][AUDIO_DSP_SAMPLES_PER_FRAME * 2];
static int g_queued[OUT_BUFFERS];
static int g_state;   /* 0 = noch nicht versucht, 1 = offen, -1 = aus */
static int g_enabled = 1;
static unsigned g_dropped;
static unsigned g_next;

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
    int slot = -1;

    if (!g_enabled) return;
    if (!host_audio_open()) return;
    if (frames != AUDIO_DSP_SAMPLES_PER_FRAME) return;

    for (int i = 0; i < OUT_BUFFERS; i++) {
        int k = (int)((g_next + (unsigned)i) % OUT_BUFFERS);
        if (!g_queued[k]) { slot = k; break; }
        if (g_hdr[k].dwFlags & WHDR_DONE) { g_queued[k] = 0; slot = k; break; }
    }
    if (slot < 0) {
        g_dropped++;
        return;
    }
    memcpy(g_buf[slot], stereo, frames * 2 * sizeof(int16_t));
    g_hdr[slot].dwBufferLength = frames * 2 * (DWORD)sizeof(int16_t);
    g_hdr[slot].dwFlags &= ~(DWORD)WHDR_DONE;
    if (waveOutWrite(g_dev, &g_hdr[slot], sizeof(g_hdr[slot])) == MMSYSERR_NOERROR) {
        g_queued[slot] = 1;
        g_next = (unsigned)(slot + 1) % OUT_BUFFERS;
    } else {
        g_dropped++;
    }
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
