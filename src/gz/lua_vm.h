#ifndef LUA_VM_H
#define LUA_VM_H
#include "gfx.h"
#include "z64.h"

/*
 * Runs real Lua 5.4 scripts written for BizHawk's N64 core.
 *
 * The script runs as a coroutine. Each game frame it is resumed once and
 * runs until emu.frameadvance(); whatever joypad.set() asked for during that
 * time is the input for the frame. One emu.frameadvance() is one game frame.
 *
 * Supported: the Lua standard library (no io/os/package/debug), and the
 * BizHawk functions joypad.*, mainmemory.*, memory.* (RDRAM), emu.*,
 * event.onframestart/onframeend/onexit, bit.*, gui.text/drawText/pixelText,
 * console.log, print. Anything else is reported as a script error.
 *
 * gz extra: gz.game is the RDRAM offset of the game context (z64_game) for
 * the running game version, e.g. gz.game + 0x103DC is the message state.
 */

/* read and syntax-check a script; it replaces the loaded one (stopping it).
   Returns 0 on success, or -1 and sets *err_str (static storage). */
int           lua_vm_load(const char *path, const char **err_str);
_Bool         lua_vm_loaded(void);
const char   *lua_vm_name(void);

void          lua_vm_start(void);
void          lua_vm_stop(void);
void          lua_vm_toggle(void);
_Bool         lua_vm_running(void);
int           lua_vm_frames(void);       /* frames run since start */
const char   *lua_vm_error(void);        /* last error, or NULL */
const char   *lua_vm_status(void);       /* "running", "stopped", ... */

/* once per game frame: run the script up to its next emu.frameadvance()
   and return its input in *c. Returns 0 if the script is not running. */
_Bool         lua_vm_frame(z64_controller_t *c);

/* draw gui.text() output */
void          lua_vm_draw(struct gfx_font *font, uint8_t alpha);

#endif
