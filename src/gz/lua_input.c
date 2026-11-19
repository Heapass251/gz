#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector/vector.h>
#include "files.h"
#include "gfx.h"
#include "gz.h"
#include "lua_input.h"
#include "lua_vm.h"
#include "menu.h"
#include "z64.h"
#include "zu.h"

/*
 * Input player shared by the lua and midi menus.
 *
 * Lua scripts are run by the real Lua interpreter in lua_vm.c; while one is
 * running, its input replaces the song player's for that frame. Songs (midi)
 * are stored as segments and played back by the code below.
 */

const int lua_speeds[] = {25, 50, 75, 90, 100, 110, 125, 150, 175, 200, 300,
                          400};

static _Bool is_note(const z64_controller_t *c)
{
  return c->a || c->cu || c->cd || c->cl || c->cr;
}

static _Bool is_double_note(const z64_controller_t *c)
{
  return is_note(c) && (c->r || c->z || c->x != 0 || c->y != 0);
}

int lua_note_gap(const z64_controller_t *c)
{
  if (!is_note(c))
    return 0;
  return is_double_note(c) ? NOTE_GAP_DOUBLE : NOTE_GAP_SINGLE;
}

/* ---- song player ----------------------------------------------------------
 *
 * The player is independent of the macro system. Each game frame it takes
 * the next frame of the song and ORs it on top of controller 1's input.
 * It is added before macro recording (so it gets recorded) and after macro
 * playback (so it plays over a macro), for both the normal game input and
 * the ocarina's own input read.
 */

static struct lua_song *pl_song;      /* song playing or last played */
static _Bool            pl_playing;
static int              pl_seg_idx;
static int              pl_seg_frame;
static int              pl_gap_left;
static _Bool            pl_in_gap;
static int              pl_pos;       /* game frames played */
static _Bool            pl_active;    /* apply pl_cur/pl_prev this frame */
static z64_controller_t pl_cur;
static z64_controller_t pl_prev;

void lua_song_init(struct lua_song *s, const int *speed)
{
  if (!s->init) {
    vector_init(&s->segs, sizeof(struct lua_seg));
    s->init = 1;
  }
  s->speed = speed;
}

void lua_song_clear(struct lua_song *s)
{
  if (pl_song == s) {
    pl_playing = 0;
    pl_pos = 0;
  }
  if (s->init) {
    vector_clear(&s->segs);
    vector_shrink_to_fit(&s->segs);
  }
}

int lua_song_count(struct lua_song *s)
{
  return s->init ? (int)s->segs.size : 0;
}

/* length of a segment in game frames at the song's speed */
static int seg_len(struct lua_song *s, const struct lua_seg *g)
{
  if (!s->speed)
    return g->len;
  int speed = *s->speed;
  int n = (g->len * 100 + speed / 2) / speed;
  return n < 1 ? 1 : n;
}

int lua_song_frames(struct lua_song *s)
{
  int total = 0;
  for (int i = 0; i < lua_song_count(s); ++i) {
    struct lua_seg *g = vector_at(&s->segs, i);
    total += seg_len(s, g) + g->gap;
  }
  return total;
}

int lua_song_pos(struct lua_song *s)
{
  return pl_song == s ? pl_pos : 0;
}

_Bool lua_song_playing(struct lua_song *s)
{
  return pl_playing && pl_song == s;
}

void lua_song_seek(struct lua_song *s, int pos)
{
  int n = lua_song_count(s);
  int f = pos;
  int i;
  pl_in_gap = 0;
  pl_seg_frame = 0;
  pl_gap_left = 0;
  for (i = 0; i < n; ++i) {
    struct lua_seg *g = vector_at(&s->segs, i);
    int len = seg_len(s, g);
    if (f < len) {
      pl_seg_frame = f;
      break;
    }
    f -= len;
    if (f < g->gap) {
      pl_seg_frame = len;
      pl_in_gap = 1;
      pl_gap_left = g->gap - f;
      break;
    }
    f -= g->gap;
  }
  pl_seg_idx = i;
  pl_song = s;
  pl_pos = pos;
}

void lua_song_start(struct lua_song *s)
{
  if (lua_song_count(s) == 0) {
    gz_log("nothing loaded");
    return;
  }
  if (lua_song_playing(s))
    return;
  lua_vm_stop();
  /* if another song was playing, its last input is released next frame by
     the normal transition from pl_prev */
  lua_song_seek(s, 0);
  pl_playing = 1;
}

void lua_song_stop(void)
{
  pl_playing = 0;
}

void lua_song_toggle(struct lua_song *s)
{
  if (lua_song_playing(s))
    lua_song_stop();
  else
    lua_song_start(s);
}

/* produce the next frame into pl_cur; returns 0 when the song is over */
static _Bool next_frame(void)
{
  struct lua_song *s = pl_song;
  while (pl_seg_idx < lua_song_count(s)) {
    struct lua_seg *g = vector_at(&s->segs, pl_seg_idx);
    if (!pl_in_gap) {
      if (pl_seg_frame < seg_len(s, g)) {
        pl_cur = g->c;
        ++pl_seg_frame;
        return 1;
      }
      pl_in_gap = 1;
      pl_gap_left = g->gap;
    }
    if (pl_gap_left > 0) {
      memset(&pl_cur, 0, sizeof(pl_cur));
      --pl_gap_left;
      return 1;
    }
    pl_in_gap = 0;
    pl_seg_frame = 0;
    ++pl_seg_idx;
  }
  return 0;
}

void lua_player_update(void)
{
  pl_prev = pl_cur;
  /* a running lua script provides this frame's input */
  if (lua_vm_frame(&pl_cur)) {
    pl_active = 1;
    return;
  }
  if (pl_playing && next_frame()) {
    ++pl_pos;
    pl_active = 1;
  }
  else {
    pl_playing = 0;
    /* one extra frame with nothing held so the last buttons are released */
    memset(&pl_cur, 0, sizeof(pl_cur));
    pl_active = (pl_prev.pad != 0 || pl_prev.x != 0 || pl_prev.y != 0);
  }
}

void lua_player_apply(z64_input_t *input)
{
  if (!pl_active)
    return;
  uint16_t pad = input->raw.pad | pl_cur.pad;
  uint16_t pad_prev = input->raw_prev.pad | pl_prev.pad;
  input->raw.pad = pad;
  input->raw_prev.pad = pad_prev;
  input->pad_pressed = (input->pad_pressed | (pad & ~pad_prev)) & pad;
  input->pad_released = (input->pad_released | (~pad & pad_prev)) & ~pad;
  if (pl_cur.x != 0 || pl_cur.y != 0 || pl_prev.x != 0 || pl_prev.y != 0) {
    if (pl_cur.x != 0 || pl_cur.y != 0) {
      input->raw.x = pl_cur.x;
      input->raw.y = pl_cur.y;
    }
    input->x_diff = input->raw.x - (pl_prev.x != 0 ? pl_prev.x
                                                   : input->raw_prev.x);
    input->y_diff = input->raw.y - (pl_prev.y != 0 ? pl_prev.y
                                                   : input->raw_prev.y);
    input->adjusted_x = zu_adjust_joystick(input->raw.x);
    input->adjusted_y = zu_adjust_joystick(input->raw.y);
  }
}

/* ---- lua menu ------------------------------------------------------------ */

void command_playlua(void)
{
  lua_vm_toggle();
}

static int do_import_lua(const char *path, void *data)
{
  const char *err_str = NULL;
  if (lua_vm_load(path, &err_str)) {
    gz_log("lua load failed:");
    gz_log("%.38s", err_str);
    if (strlen(err_str) > 38)
      gz_log("%.38s", err_str + 38);
    menu_prompt(gz.menu_main, "lua load failed, see log", "return\0", 0,
                NULL, NULL);
    return 1;
  }
  gz_log("lua loaded: %s", lua_vm_name());
  return 0;
}

static void import_lua_proc(struct menu_item *item, void *data)
{
  menu_get_file(gz.menu_main, GETFILE_LOAD, NULL, ".lua",
                do_import_lua, NULL);
}

static void play_lua_proc(struct menu_item *item, void *data)
{
  lua_vm_toggle();
}

static int lua_status_draw_proc(struct menu_item *item,
                                struct menu_draw_params *draw_params)
{
  struct gfx_font *font = draw_params->font;
  int ch = menu_get_cell_height(item->owner, 1);
  int x = draw_params->x;
  int y = draw_params->y;
  gfx_mode_set(GFX_MODE_COLOR, GPACK_RGB24A8(draw_params->color,
                                             draw_params->alpha));
  if (!lua_vm_loaded()) {
    gfx_printf(font, x, y, "no lua loaded");
    return 1;
  }
  gfx_printf(font, x, y, "%s", lua_vm_name());
  gfx_printf(font, x, y + ch, "%s, frame %i", lua_vm_status(),
             lua_vm_frames());
  /* show the error wrapped over a few lines */
  const char *err = lua_vm_error();
  if (err) {
    const int w = 32;
    for (int i = 0; i < 4 && *err; ++i) {
      gfx_printf(font, x, y + ch * (2 + i), "%.*s", w, err);
      err += strlen(err) < w ? strlen(err) : w;
    }
  }
  return 1;
}

struct menu *gz_lua_menu(void)
{
  static struct menu menu;
  struct menu_item *item;
  menu_init(&menu, MENU_NOVALUE, MENU_NOVALUE, MENU_NOVALUE);
  menu.selector = menu_add_submenu(&menu, 0, 0, NULL, "return");
  item = menu_add_button(&menu, 0, 2, "import lua", import_lua_proc, NULL);
  item->tooltip = "load a bizhawk lua script";
  item = menu_add_button(&menu, 0, 3, "run/stop lua", play_lua_proc, NULL);
  item->tooltip = "adds its input to controller 1";
  menu_add_static_custom(&menu, 0, 5, lua_status_draw_proc, NULL, 0xC0C0C0);
  menu_add_tooltip(&menu, 8, 0, gz.menu_main, 0xC0C0C0);
  return &menu;
}
