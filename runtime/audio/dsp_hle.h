/* HLE-DSP-Audio (Azahar audio_core/hle): 24 Quellen, Mixer und die beiden
 * DSP-RAM-Regionen, die das Spiel ueber Pipe 2 (Audio) gemeldet bekommt.
 *
 * Das Spiel schreibt seine Konfiguration in die Region mit dem hoeheren
 * Frame-Counter (Application-seitig gezaehlt), der DSP verarbeitet sie und
 * schreibt Quellen-/Mischer-Status in die andere Region. Jede Audio-Frame
 * (160 Samples @ 32728 Hz, 160*4096*2 ARM11-Zyklen = 4,889 ms) meldet
 * InterruptType::Pipe fuer DspPipe::Audio.
 *
 * Die Modul-API ist absichtlich schmal: die Anbindung an den dsp::DSP-Dienst
 * (Pipe-Puffer, Interrupt-Ereignisse, Gast-Speicher, Gast-Lock) bleibt in
 * runtime/kernel/hle.c. */
#ifndef AUDIO_DSP_HLE_H
#define AUDIO_DSP_HLE_H

#include <stdint.h>

#define AUDIO_DSP_REGION0_ADDR 0x1FF50000u
#define AUDIO_DSP_REGION1_ADDR 0x1FF70000u
#define AUDIO_DSP_REGION_SIZE  0x8000u
/* DSP-RAM: 0x1FF00000, 0x80000 Byte (Azahar DspMemory::raw_memory). */
#define AUDIO_DSP_RAM_ADDR     0x1FF00000u
#define AUDIO_DSP_RAM_SIZE     0x80000u

/* Physikalische Gastadresse -> virtuelle Gastadresse im Port-Speicher.
 * Der Port bildet den Gastspeicher ueber virtuelle Adressen ab; FCRAM
 * (phys 0x20000000+x) erscheint im Prozess als Linear-Heap 0x14000000+x
 * (3DS-Speicherkarte, gleiche Abbildung wie gpu_pica_phys_to_guest).
 * Frueher wurde nur auf x maskiert -> Samples kamen aus dem Codebereich.
 * DSP-RAM (0x1FF00000) ist virtuell identisch abgebildet. */
#define AUDIO_FCRAM_PADDR  0x20000000u
#define AUDIO_FCRAM_SIZE   0x08000000u
#define AUDIO_LINEAR_VADDR 0x14000000u
static inline uint32_t audio_phys_to_guest(uint32_t phys) {
    if (phys >= AUDIO_FCRAM_PADDR && phys - AUDIO_FCRAM_PADDR < AUDIO_FCRAM_SIZE)
        return AUDIO_LINEAR_VADDR + (phys - AUDIO_FCRAM_PADDR);
    return phys;
}
/* Wie Azahar source.cpp: DSP-DMA ignoriert die unteren zwei Adressbits
 * (GetPhysicalPointer(addr & 0xFFFFFFFC)). */
static inline uint32_t audio_phys_to_guest_aligned(uint32_t phys) {
    return audio_phys_to_guest(phys & 0xFFFFFFFCu);
}

#define AUDIO_DSP_NUM_SOURCES  24
#define AUDIO_DSP_SAMPLES_PER_FRAME 160
#define AUDIO_DSP_SAMPLE_RATE  32728

/* 160 Samples * 4096 Teaklite-Zyklen * 2 ARM11-Zyklen (Azahar hle.cpp). */
#define AUDIO_DSP_FRAME_TICKS  1310720u

/* status: 0 = Off, 1 = On, 2 = Sleeping (Azahar DspState). */
enum {
    AUDIO_DSP_OFF = 0,
    AUDIO_DSP_ON = 1,
    AUDIO_DSP_SLEEPING = 2
};

/* Interrupt-Typen wie Service::DSP::InterruptType. */
enum {
    AUDIO_DSP_INT_ZERO = 0,
    AUDIO_DSP_INT_DMA = 1,
    AUDIO_DSP_INT_PIPE = 2
};

/* Speicher + Gast-Lock. mem ist der Gast-Speicher; mem_size begrenzt die
 * Regionen. Ein Frame (Konfiguration lesen, Dirty-Bits loeschen, Status
 * aller Quellen schreiben) muss atomar zum Gast laufen (Haenger 2: der
 * Sound-Thread las sonst Quelle 0 aus Frame N und Quelle 1 aus N+1). Im Port
 * fuehrt ihn der Gast-Scheduler aus (Takt-Modell, audio_dsp_set_notify);
 * ohne notify laeuft er samt Meldung unter der Gast-Sperre (enter/leave).
 * try_enter: nicht blockierender Versuch, 1 = Sperre erhalten. */
void audio_dsp_init(uint8_t *mem, uint32_t mem_size);
void audio_dsp_set_lock(void (*enter)(void), void (*leave)(void));
/* Takt-Modell: der Audio-Thread zaehlt nur faellige Frames und ruft
 * notify(); der Gast-Scheduler fuehrt sie zwischen Gast-Bloecken mit
 * audio_dsp_run_due() aus (atomar zum Gast, wie Azahars core_timing-Tick).
 * audio_dsp_due(): 1 = ein Frame steht an. run_due liefert 1, solange noch
 * Frames anstehen (z. B. weil der naechste auf die Spielantwort wartet). */
void audio_dsp_set_notify(void (*notify)(void));
int audio_dsp_due(void);
int audio_dsp_run_due(void);
unsigned long long audio_dsp_late_frames(void);
unsigned long long audio_dsp_waited_frames(void);
unsigned long long audio_dsp_dropped_due(void);
void audio_dsp_set_trylock(int (*try_enter)(void));

/* Vorgemerkte DSP-Meldungen (Interrupts, Struct-Adressen) zustellen.
 * Aufrufer MUSS die Gast-Sperre halten. Billig, wenn nichts ansteht. */
void audio_dsp_flush_events(void);
long long audio_dsp_last_flush_delay_us(void);   /* Diagnose */

/* Interrupt-Callback: wird aus dem Audio-Thread gerufen und signalisiert das
 * bei RegisterInterruptEvents gebundene Gast-Ereignis (hle.c). */
void audio_dsp_set_interrupt(void (*handler)(int type, int pipe));

/* Callback, der die 15 DSP-Struktur-Adressen in die Audio-Pipe schreibt und
 * InterruptType::Pipe/Audio signalisiert (hle.c: dsp_audio_struct_addresses).
 * Wird bei Initialize, Wakeup und beim Abschluss von Sleep gerufen, wie
 * Azahar AudioPipeWriteStructAddresses. */
void audio_dsp_set_struct_addresses(void (*handler)(void));

/* Host-Ausgabe (runtime/audio/host_output.c). Darf fehlschlagen; der
 * Gast-Zustand laeuft unabhaengig davon weiter. */
void audio_dsp_set_host_output(int enabled);

/* Pipe 2 (Audio) schreiben: Azahar PipeWrite. buffer[0] ist der
 * StateChange-Wert (0 Initialize, 1 Shutdown, 2 Wakeup, 3 Sleep); weitere
 * Bytes werden wie auf Hardware ignoriert. */
void audio_dsp_pipe_write(const uint8_t *buffer, uint32_t size);

/* dsp::DSP RecvData(0)/RecvDataIsReady(0) (Azahar DspHle::Impl). */
uint16_t audio_dsp_recv_data(void);
int audio_dsp_recv_data_ready(void);

/* Thread starten/stoppen (hle_init / hle_report). */
void audio_dsp_start(void);
void audio_dsp_stop(void);

/* Ein Audio-Frame (160 Samples). Der Audio-Thread ruft das periodisch; Tests
 * treiben damit die Frame-Verarbeitung deterministisch. Guest-Lock wird dabei
 * genommen, falls gesetzt. */
void audio_dsp_tick(void);

/* Diagnosezaehler: erzeugte Frames. */
unsigned long long audio_dsp_frames(void);

#endif
