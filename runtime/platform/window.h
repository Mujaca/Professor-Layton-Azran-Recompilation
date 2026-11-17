/* Minimales Win32-Fenster ohne externe Abhaengigkeiten. */
#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

int win_open(int width, int height, const char *title);
int win_pump(void);                              /* 1 = Fenster offen */
void win_blit(const uint8_t *rgb, int w, int h);  /* RGB24 -> Fenster */
void win_status(const char *text, int line);
void win_close(void);
int win_key_take(int vk);
void win_key_push(int vk);

#endif
