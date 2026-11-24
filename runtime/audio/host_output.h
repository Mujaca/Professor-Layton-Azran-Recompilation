/* Host-Audioausgabe ueber Windows waveOut (runtime/audio/host_output.c).
 * Die Ausgabe ist optional: ohne Audiogeraet oder bei Fehlern bleibt sie
 * stumm, der DSP-/Gast-Zustand laeuft unveraendert weiter. */
#ifndef AUDIO_HOST_OUTPUT_H
#define AUDIO_HOST_OUTPUT_H

#include <stdint.h>

/* Oeffnet das Geraet beim ersten Push (Stereo, 16 Bit, AUDIO_DSP_SAMPLE_RATE).
 * Liefert 0, wenn die Ausgabe nicht verfuegbar ist. */
int host_audio_open(void);
void host_audio_push(const int16_t *stereo, unsigned frames);
void host_audio_close(void);
void host_audio_set_enabled(int enabled);
unsigned host_audio_dropped(void);
unsigned host_audio_underruns(void);

#endif
