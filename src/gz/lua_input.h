#ifndef LUA_INPUT_H
#define LUA_INPUT_H
#include <vector/vector.h>
#include "z64.h"

/*
 * Ocarina timing fix: gz needs a gap after each note so the game registers
 * the next one. After every note ends, empty frames are inserted:
 *   single note (A or a C button alone)              -> NOTE_GAP_SINGLE
 *   double note (note + R, Z, or stick pitch bend)   -> NOTE_GAP_DOUBLE
 */
#define NOTE_GAP_SINGLE   1
#define NOTE_GAP_DOUBLE   3

/* speed choices, in percent, shared by the lua and midi menus */
#define LUA_SPEED_OPTIONS "25%\0""50%\0""75%\0""90%\0""100%\0""110%\0" \
                          "125%\0""150%\0""175%\0""200%\0""300%\0""400%\0"
#define LUA_SPEED_DEFAULT 4
extern const int  lua_speeds[];

/* a run of identical input, followed by gap empty frames */
struct lua_seg
{
  z64_controller_t  c;
  int               len;
  int               gap;
};

/* a playable song. If speed is not NULL, segment lengths are scaled by
   *speed percent while playing; gaps are never scaled. */
struct lua_song
{
  struct vector     segs;
  _Bool             init;
  const int        *speed;
};

int           lua_note_gap(const z64_controller_t *c);

/* song player; only one song plays at a time. Its buttons are added on top
   of controller 1's input (real controller, virtual controller or a playing
   macro). */
void          lua_song_init(struct lua_song *s, const int *speed);
void          lua_song_clear(struct lua_song *s);   /* stops it if playing */
int           lua_song_count(struct lua_song *s);   /* segments */
int           lua_song_frames(struct lua_song *s);  /* total game frames */
int           lua_song_pos(struct lua_song *s);
_Bool         lua_song_playing(struct lua_song *s);
void          lua_song_start(struct lua_song *s);
void          lua_song_stop(void);
void          lua_song_toggle(struct lua_song *s);
void          lua_song_seek(struct lua_song *s, int pos);
void          lua_player_update(void);  /* once per game frame, first */
void          lua_player_apply(z64_input_t *input);  /* merge into input */

/* Load a BizHawk-style Lua input script.
   Supported statements (anything else is an error):
     joypad.setfrommnemonicstr("|..|    0,    0,..................|")
     emu.frameadvance()
     emu.yield()
     -- comments, blank lines, semicolons
   Returns 0 on success. On failure returns -1 and sets *err_str to a
   message (pointing to static storage). */
int           lua_input_load(const char *path, const char **err_str);
void          command_playlua(void);
struct menu  *gz_lua_menu(void);

/* midi player (midi_input.c) */
int           midi_input_load(const char *path, const char **err_str);
struct menu  *gz_midi_menu(void);

#endif
