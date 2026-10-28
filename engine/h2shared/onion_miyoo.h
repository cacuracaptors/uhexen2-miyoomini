/* onion_miyoo.h -- OnionOS: the Game Switcher and the save at power-off (Miyoo Mini port) */
#ifndef __ONION_MIYOO_H
#define __ONION_MIYOO_H

void Onion_Init (void);		/* before the screen opens (VID_Init) */
void Onion_BeforeScreen (void);	/* right before SDL opens the screen */
void Onion_AfterScreen (void);	/* right after it */
void Onion_Signals (void);	/* once the framebuffer is set up */
void Onion_Frame (void);	/* every frame, after the input */
void Onion_End (void);		/* the game is closing */

const unsigned int *VID_ScreenPage (void);	/* vid_miyoo.c: the 640x480 page on screen, or NULL */

#endif
