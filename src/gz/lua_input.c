#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <vector/vector.h>
#include "files.h"
#include "gfx.h"
#include "gz.h"
#include "lua_input.h"
#include "menu.h"
#include "z64.h"
#include "zu.h"

/*
 * Reads BizHawk N64 Lua input scripts of the frame-by-frame kind, e.g.
 *
 *   joypad.setfrommnemonicstr("|..|    0,    0,............u.....|")
 *   emu.frameadvance()
 *
 * This is not a Lua interpreter: only the statements listed in lua_input.h
 * are understood. Each emu.frameadvance() produces one macro frame using the
 * input from the last setfrommnemonicstr() since the previous advance (or no
 * input if there was none), which matches how BizHawk applies Lua input.
 *
 * BizHawk N64 mnemonic layout: "|<power,reset>|<x>,<y>,<18 buttons>|..."
 * Button order: A-Up A-Down A-Left A-Right, D-Up D-Down D-Left D-Right,
 * Start Z B A, C-Up C-Down C-Right C-Left, L R. '.' means not pressed.
 */

#define LINE_MAX_LEN      512
#define N64_BUTTON_COUNT  18
#define ANALOG_DIGITAL    80

static char err_buf[80];

static const char *set_err(int line, const char *msg)
{
  snprintf(err_buf, sizeof(err_buf), "line %i: %s", line, msg);
  return err_buf;
}

static const char *skip_ws(const char *p)
{
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ';')
    ++p;
  return p;
}

static int starts_with(const char *p, const char *word)
{
  return strncmp(p, word, strlen(word)) == 0;
}

static int clamp_axis(long v)
{
  if (v < -128)
    return -128;
  if (v > 127)
    return 127;
  return v;
}

/* parse a BizHawk N64 mnemonic string into a controller state */
static const char *parse_mnemonic(const char *s, z64_controller_t *c)
{
  /* skip console section "|..|" */
  if (*s != '|')
    return "input string must start with '|'";
  s = strchr(s + 1, '|');
  if (!s)
    return "bad input string";
  ++s;

  /* player 1 analog stick */
  char *end;
  long x = strtol(s, &end, 10);
  if (end == s || *end != ',')
    return "bad stick x value";
  s = end + 1;
  long y = strtol(s, &end, 10);
  if (end == s || *end != ',')
    return "bad stick y value";
  s = end + 1;

  /* player 1 buttons */
  _Bool b[N64_BUTTON_COUNT];
  int n = 0;
  while (*s && *s != '|') {
    if (n == N64_BUTTON_COUNT)
      return "too many buttons (not an n64 script?)";
    b[n++] = (*s != '.' && *s != ' ');
    ++s;
  }
  if (n != N64_BUTTON_COUNT)
    return "too few buttons (not an n64 script?)";

  /* digital stick buttons override the analog value */
  if (b[0])
    y = ANALOG_DIGITAL;
  if (b[1])
    y = -ANALOG_DIGITAL;
  if (b[2])
    x = -ANALOG_DIGITAL;
  if (b[3])
    x = ANALOG_DIGITAL;

  c->pad = 0;
  c->x = clamp_axis(x);
  c->y = clamp_axis(y);
  c->du = b[4];
  c->dd = b[5];
  c->dl = b[6];
  c->dr = b[7];
  c->s  = b[8];
  c->z  = b[9];
  c->b  = b[10];
  c->a  = b[11];
  c->cu = b[12];
  c->cd = b[13];
  c->cr = b[14];
  c->cl = b[15];
  c->l  = b[16];
  c->r  = b[17];
  return NULL;
}

/* parse "(" "<string>" ")" and copy the string into buf */
static const char *parse_string_arg(const char **pp, char *buf, size_t size)
{
  const char *p = skip_ws(*pp);
  if (*p != '(')
    return "expected '('";
  p = skip_ws(p + 1);
  char quote = *p;
  if (quote != '"' && quote != '\'')
    return "expected a quoted string";
  ++p;
  const char *q = strchr(p, quote);
  if (!q)
    return "unterminated string";
  size_t len = q - p;
  if (len >= size)
    return "string too long";
  memcpy(buf, p, len);
  buf[len] = 0;
  p = skip_ws(q + 1);
  if (*p != ')')
    return "expected ')'";
  *pp = p + 1;
  return NULL;
}

/* parse "(" ")" */
static const char *parse_no_args(const char **pp)
{
  const char *p = skip_ws(*pp);
  if (*p != '(')
    return "expected '('";
  p = skip_ws(p + 1);
  if (*p != ')')
    return "expected ')'";
  *pp = p + 1;
  return NULL;
}


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

static _Bool same_input(const z64_controller_t *a, const z64_controller_t *b)
{
  return a->pad == b->pad && a->x == b->x && a->y == b->y;
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

/* ---- lua script loading --------------------------------------------------
 *
 * The script is stored as segments: runs of identical input with their
 * length in script frames, plus the note gap that follows. Lua scripts
 * play at their own speed (the midi player has a speed setting).
 */

static struct lua_song  lua_script;

struct emitter
{
  int               fill;
  int               count;      /* segments emitted */
  _Bool             have_run;
  z64_controller_t  run;
  int               run_len;
};

static const char *emit_seg(struct emitter *e)
{
  if (!e->have_run)
    return NULL;
  if (e->fill) {
    struct lua_seg *g = vector_at(&lua_script.segs, e->count);
    if (!g)
      return "file changed while loading";
    g->c = e->run;
    g->len = e->run_len;
    g->gap = lua_note_gap(&e->run);
  }
  ++e->count;
  return NULL;
}

static const char *emit_frame(struct emitter *e, const z64_controller_t *c)
{
  if (e->have_run && same_input(&e->run, c)) {
    ++e->run_len;
    return NULL;
  }
  const char *err = emit_seg(e);
  if (err)
    return err;
  e->run = *c;
  e->run_len = 1;
  e->have_run = 1;
  return NULL;
}

/* Read the script once. If fill is 0, only validate and count segments.
   If fill is 1, write segments into lua_script (already sized). */
static int run_pass(const char *path, int fill, int *n_segs,
                    const char **err_str)
{
  z64_controller_t pending;
  struct emitter em;
  int line_no = 0;
  char *line = NULL;
  char *arg = NULL;
  memset(&em, 0, sizeof(em));
  em.fill = fill;
  memset(&pending, 0, sizeof(pending));

  FILE *f = fopen(path, "r");
  if (!f) {
    *err_str = strerror(errno);
    return -1;
  }
  line = malloc(LINE_MAX_LEN);
  arg = malloc(LINE_MAX_LEN);
  if (!line || !arg) {
    *err_str = "out of memory";
    goto fail;
  }

  while (fgets(line, LINE_MAX_LEN, f)) {
    ++line_no;
    if (!strchr(line, '\n') && !feof(f)) {
      *err_str = set_err(line_no, "line too long");
      goto fail;
    }
    const char *p = line;
    while (1) {
      p = skip_ws(p);
      if (*p == 0 || starts_with(p, "--"))
        break;
      const char *e;
      if (starts_with(p, "joypad.setfrommnemonicstr")) {
        p += strlen("joypad.setfrommnemonicstr");
        e = parse_string_arg(&p, arg, LINE_MAX_LEN);
        if (!e)
          e = parse_mnemonic(arg, &pending);
      }
      else if (starts_with(p, "emu.frameadvance")) {
        p += strlen("emu.frameadvance");
        e = parse_no_args(&p);
        if (!e) {
          e = emit_frame(&em, &pending);
          memset(&pending, 0, sizeof(pending));
        }
      }
      else if (starts_with(p, "emu.yield")) {
        /* yield does not advance a frame in BizHawk; accept and ignore */
        p += strlen("emu.yield");
        e = parse_no_args(&p);
      }
      else
        e = "unsupported lua statement";
      if (e) {
        *err_str = set_err(line_no, e);
        goto fail;
      }
    }
  }
  if (ferror(f)) {
    *err_str = strerror(errno);
    goto fail;
  }
  /* close the last run */
  {
    const char *e = emit_seg(&em);
    if (e) {
      *err_str = e;
      goto fail;
    }
  }

  free(line);
  free(arg);
  fclose(f);
  *n_segs = em.count;
  return 0;

fail:
  if (line)
    free(line);
  if (arg)
    free(arg);
  fclose(f);
  return -1;
}

int lua_input_load(const char *path, const char **err_str)
{
  int n_segs;

  /* first pass: validate the whole script before replacing the old one */
  if (run_pass(path, 0, &n_segs, err_str))
    return -1;
  if (n_segs == 0) {
    *err_str = "script has no emu.frameadvance()";
    return -1;
  }

  lua_song_init(&lua_script, NULL);
  lua_song_clear(&lua_script);
  if (!vector_reserve(&lua_script.segs, n_segs) ||
      !vector_insert(&lua_script.segs, 0, n_segs, NULL))
  {
    lua_song_clear(&lua_script);
    *err_str = "out of memory";
    return -1;
  }

  /* second pass: fill in the segments */
  int n_filled = 0;
  if (run_pass(path, 1, &n_filled, err_str)) {
    lua_song_clear(&lua_script);
    return -1;
  }
  if (n_filled != n_segs) {
    lua_song_clear(&lua_script);
    *err_str = "file changed while loading";
    return -1;
  }
  return 0;
}

void command_playlua(void)
{
  lua_song_toggle(&lua_script);
}

/* ---- lua menu ------------------------------------------------------------ */

static char lua_name[32];

static int do_import_lua(const char *path, void *data)
{
  const char *err_str = NULL;
  if (lua_input_load(path, &err_str)) {
    menu_prompt(gz.menu_main, err_str, "return\0", 0, NULL, NULL);
    return 1;
  }
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  strncpy(lua_name, base, sizeof(lua_name) - 1);
  lua_name[sizeof(lua_name) - 1] = 0;
  gz_log("lua loaded: %i frames", lua_song_frames(&lua_script));
  return 0;
}

static void import_lua_proc(struct menu_item *item, void *data)
{
  menu_get_file(gz.menu_main, GETFILE_LOAD, NULL, ".lua",
                do_import_lua, NULL);
}

static void play_lua_proc(struct menu_item *item, void *data)
{
  lua_song_toggle(&lua_script);
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
  if (lua_song_count(&lua_script) == 0) {
    gfx_printf(font, x, y, "no lua loaded");
    return 1;
  }
  gfx_printf(font, x, y, "%s", lua_name);
  gfx_printf(font, x, y + ch, "%s %i / %i",
             lua_song_playing(&lua_script) ? "playing" : "stopped",
             lua_song_pos(&lua_script), lua_song_frames(&lua_script));
  return 1;
}

struct menu *gz_lua_menu(void)
{
  static struct menu menu;
  struct menu_item *item;
  menu_init(&menu, MENU_NOVALUE, MENU_NOVALUE, MENU_NOVALUE);
  menu.selector = menu_add_submenu(&menu, 0, 0, NULL, "return");
  item = menu_add_button(&menu, 0, 2, "import lua", import_lua_proc, NULL);
  item->tooltip = "load a bizhawk lua file";
  item = menu_add_button(&menu, 0, 3, "play/stop lua", play_lua_proc, NULL);
  item->tooltip = "plays over controller 1";
  menu_add_static_custom(&menu, 0, 5, lua_status_draw_proc, NULL, 0xC0C0C0);
  menu_add_tooltip(&menu, 8, 0, gz.menu_main, 0xC0C0C0);
  return &menu;
}
