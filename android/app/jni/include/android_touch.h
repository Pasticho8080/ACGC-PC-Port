/* android_touch.h - virtual on-screen touch controls (Android overlay).
 * Android-only: included from pc_main.c / pc_pad.c under #ifdef __ANDROID__. */
#ifndef ANDROID_TOUCH_H
#define ANDROID_TOUCH_H

#include "pc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

void android_touch_init(void);
void android_touch_shutdown(void);
void android_touch_handle_event(const SDL_Event* ev);
void android_touch_draw(void);
void android_touch_apply(u16* buttons, s8* stick_x, s8* stick_y);

/* Opens the Android system file picker (SAF) so the user can select a ROM.
   The Java side copies the picked file to <external>/rom/ and restarts the
   app (or just exits if nothing was picked). Android only. */
void android_rom_pick(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_TOUCH_H */