#ifndef PAD_H
#define PAD_H
/* gamepad.c <-> pad_fe.c / pad_rumble.c (docs/gamepad.md) */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdint.h>

#define PAD_MAX 4                 /* JOY library joystick slots */
#define PAD_GAME_BUTTONS 10

/* gamepad.c */
SDL_GameController *pad_controller(int slot);               /* NULL if the slot has no pad */
int pad_any_button(SDL_GameControllerButton b);             /* held on any pad */
void pad_post_key(int vk, int up);                          /* WM_KEYDOWN/UP to the game window */
int pad_joystick_for_slot(int slot);                        /* JOY index of an SDL slot */
int pad_frontend(void);                                     /* a menu (or the pause menu) is shown */
int pad_race_drawn(void);                                   /* a race is drawn underneath (pause menu) */
int pad_racing(void);                                       /* a race is running, no menu */

/* pad_fe.c: frontend (menus) with a pad only */
void padfe_install(void);
int padfe_start(void);                                      /* Start pressed in a menu; 1 = handled */
void padfe_frame(void);                                     /* every SDL pump, game thread */
/* adjusts what the fake joystick reports (axes -1..1, buttons 0x80 held) */
void padfe_filter(int slot, double *x, double *y, unsigned char *buttons);

/* pad_rumble.c: force feedback -> SDL_GameControllerRumble */
void padrumble_install(void);
void padrumble_frame(void);
#endif
