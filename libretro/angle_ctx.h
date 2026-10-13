#ifndef ANGLE_CTX_H
#define ANGLE_CTX_H

/* GLideN64 on ANGLE (OpenGL ES 3.0 on Metal, shipped next to the core in
 * cores/angle/) instead of the system OpenGL. The core owns the context: it
 * renders into an off-screen framebuffer and hands RetroArch finished frames
 * as plain XRGB8888 images, so any RetroArch video driver can show them and
 * RetroArch needs no OpenGL at all. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loads ANGLE and creates the context and a framebuffer of max_w x max_h. */
bool angle_ctx_create(unsigned max_w, unsigned max_h, retro_log_printf_t log);
void angle_ctx_destroy(void);
bool angle_ctx_active(void);

void angle_ctx_make_current(void);
retro_proc_address_t angle_ctx_get_proc_address(const char *sym);
uintptr_t angle_ctx_get_framebuffer(void);

/* Reads the w x h frame (bottom-left origin) and returns it top-down in
 * XRGB8888. The buffer stays valid until the next call. */
const void *angle_ctx_read_frame(unsigned w, unsigned h, size_t *pitch);

#ifdef __cplusplus
}
#endif

#endif
