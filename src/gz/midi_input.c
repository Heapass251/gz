#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <vector/vector.h>
#include "files.h"
#include "gfx.h"
#include "gz.h"
#include "lua_input.h"
#include "menu.h"
#include "sys.h"
#include "z64.h"

/*
 * Plays standard MIDI files (.mid) on the ocarina.
 *
 * The file is read once into a list of notes (times in milliseconds). Each
 * time a setting changes, the notes are turned into an ocarina song:
 *   1. notes from the chosen track (or all tracks) are reduced to a single
 *      melody line by always playing the highest note sounding
 *      (channel 10, drums, is ignored),
 *   2. the melody is transposed to fit the ocarina's range (B3 - F5), and
 *      any note still outside is moved by octaves until it fits,
 *   3. each pitch is mapped to buttons, and timing is converted to game
 *      frames (20 per second) at the chosen speed. A note is cut short when
 *      needed so the 1 / 3 frame gap before the next note fits.
 */

#define MIDI_MAX_SIZE     0x100000
#define MIDI_FPS          20
#define OCA_LOW           59  /* B3 */
#define OCA_HIGH          77  /* F5 */
#define BEND              127 /* stick for a two semitone bend */

struct midi_note
{
  uint32_t  start;    /* ms */
  uint32_t  end;      /* ms */
  uint8_t   pitch;
  uint8_t   track;    /* 1-based */
  uint8_t   chan;
};

struct mono_note
{
  uint32_t  start;
  uint32_t  end;
  uint8_t   pitch;
};

struct note_ev
{
  uint32_t  t;
  uint8_t   on;
  uint8_t   pitch;
};

struct tempo_ev
{
  uint32_t  tick;
  uint32_t  us_per_qn;
  uint64_t  us_at;
};

/* loaded file */
static struct vector    midi_notes;
static _Bool            midi_notes_init;
static int              midi_n_tracks;
static char             midi_name[32];

/* settings */
static int              midi_speed_idx = LUA_SPEED_DEFAULT;
static int              midi_fit;         /* 0 auto, 1 octaves, 2 none */
static int              midi_transpose;
static int              midi_track;       /* 0 = all */
static int              midi_shift;       /* last applied total shift */

static struct lua_song  midi_song;

/* ---- file parsing -------------------------------------------------------- */

struct reader
{
  const uint8_t  *p;
  const uint8_t  *end;
};

static _Bool rd_ok(struct reader *r, size_t n)
{
  return (size_t)(r->end - r->p) >= n;
}

static uint32_t rd_be(struct reader *r, int n)
{
  uint32_t v = 0;
  while (n-- > 0)
    v = (v << 8) | *r->p++;
  return v;
}

static int rd_varlen(struct reader *r, uint32_t *v)
{
  uint32_t x = 0;
  for (int i = 0; i < 4; ++i) {
    if (!rd_ok(r, 1))
      return -1;
    uint8_t b = *r->p++;
    x = (x << 7) | (b & 0x7F);
    if (!(b & 0x80)) {
      *v = x;
      return 0;
    }
  }
  return -1;
}

static int push(struct vector *v, const void *e)
{
  return vector_push_back(v, 1, e) ? 0 : -1;
}

/* parse one MTrk chunk, adding notes (start/end in ticks) and tempos */
static const char *parse_track(struct reader *r, int track,
                               struct vector *notes, struct vector *tempos)
{
  static int32_t on_tick[16][128];
  memset(on_tick, 0xFF, sizeof(on_tick));
  uint32_t tick = 0;
  uint8_t status = 0;
  const char *s_bad = "bad midi track";
  const char *s_mem = "out of memory";

  while (r->p < r->end) {
    uint32_t delta;
    if (rd_varlen(r, &delta))
      return s_bad;
    tick += delta;
    if (!rd_ok(r, 1))
      return s_bad;
    if (*r->p & 0x80)
      status = *r->p++;
    else if (status == 0)
      return s_bad;

    if (status == 0xFF) {
      if (!rd_ok(r, 1))
        return s_bad;
      uint8_t type = *r->p++;
      uint32_t len;
      if (rd_varlen(r, &len) || !rd_ok(r, len))
        return s_bad;
      if (type == 0x51 && len == 3) {
        struct reader t = {r->p, r->p + 3};
        struct tempo_ev te = {tick, rd_be(&t, 3), 0};
        if (push(tempos, &te))
          return s_mem;
      }
      r->p += len;
      status = 0;
      if (type == 0x2F)
        break;
      continue;
    }
    if (status == 0xF0 || status == 0xF7) {
      uint32_t len;
      if (rd_varlen(r, &len) || !rd_ok(r, len))
        return s_bad;
      r->p += len;
      status = 0;
      continue;
    }
    if (status >= 0xF0)
      return s_bad;

    int kind = status & 0xF0;
    int chan = status & 0x0F;
    int n_data = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
    if (!rd_ok(r, n_data))
      return s_bad;
    uint8_t d0 = r->p[0] & 0x7F;
    uint8_t d1 = n_data > 1 ? (r->p[1] & 0x7F) : 0;
    r->p += n_data;

    _Bool is_on = (kind == 0x90 && d1 > 0);
    _Bool is_off = (kind == 0x80 || (kind == 0x90 && d1 == 0));
    if (!is_on && !is_off)
      continue;
    /* close a sounding note of this pitch (also on a repeated note-on) */
    if (on_tick[chan][d0] >= 0) {
      struct midi_note n = {on_tick[chan][d0], tick, d0, track, chan};
      if (push(notes, &n))
        return s_mem;
      on_tick[chan][d0] = -1;
    }
    if (is_on)
      on_tick[chan][d0] = tick;
  }

  /* close notes left on at the end of the track */
  for (int c = 0; c < 16; ++c)
    for (int p = 0; p < 128; ++p)
      if (on_tick[c][p] >= 0) {
        struct midi_note n = {on_tick[c][p], tick, p, track, c};
        if (push(notes, &n))
          return s_mem;
      }
  return NULL;
}

static int tempo_cmp(const void *a, const void *b)
{
  const struct tempo_ev *x = a;
  const struct tempo_ev *y = b;
  if (x->tick != y->tick)
    return x->tick < y->tick ? -1 : 1;
  return 0;
}

static uint32_t tick_to_ms(uint32_t tick, struct vector *tempos,
                           uint32_t division, uint32_t smpte_tps)
{
  if (smpte_tps)
    return (uint64_t)tick * 1000 / smpte_tps;
  /* last tempo at or before tick */
  int lo = 0;
  int hi = tempos->size - 1;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    struct tempo_ev *t = vector_at(tempos, mid);
    if (t->tick <= tick)
      lo = mid;
    else
      hi = mid - 1;
  }
  struct tempo_ev *t = vector_at(tempos, lo);
  uint64_t us = t->us_at +
                (uint64_t)(tick - t->tick) * t->us_per_qn / division;
  return us / 1000;
}

static const char *parse_midi(const uint8_t *buf, size_t size,
                              struct vector *notes, int *n_tracks)
{
  const char *s_mem = "out of memory";
  const char *err = NULL;
  struct reader r = {buf, buf + size};
  struct vector tempos;
  vector_init(&tempos, sizeof(struct tempo_ev));

  if (!rd_ok(&r, 14) || memcmp(r.p, "MThd", 4) != 0)
    return "not a midi file";
  r.p += 4;
  uint32_t hlen = rd_be(&r, 4);
  if (hlen < 6 || !rd_ok(&r, hlen))
    return "bad midi header";
  struct reader h = {r.p, r.p + hlen};
  r.p += hlen;
  uint32_t format = rd_be(&h, 2);
  rd_be(&h, 2); /* track count, recounted below */
  uint32_t division = rd_be(&h, 2);
  uint32_t smpte_tps = 0;
  if (format > 2)
    return "unsupported midi format";
  if (division & 0x8000) {
    int fps = -(int8_t)(division >> 8);
    smpte_tps = fps * (division & 0xFF);
    if (smpte_tps == 0)
      return "bad midi timing";
  }
  else if (division == 0)
    return "bad midi timing";

  /* default tempo, 120 bpm */
  struct tempo_ev te = {0, 500000, 0};
  if (push(&tempos, &te)) {
    err = s_mem;
    goto done;
  }

  int track = 0;
  while (rd_ok(&r, 8)) {
    _Bool is_trk = memcmp(r.p, "MTrk", 4) == 0;
    r.p += 4;
    uint32_t len = rd_be(&r, 4);
    if (!rd_ok(&r, len))
      len = r.end - r.p;  /* truncated file, read what is there */
    if (is_trk) {
      ++track;
      if (track > 255) {
        err = "too many tracks";
        goto done;
      }
      struct reader t = {r.p, r.p + len};
      err = parse_track(&t, track, notes, &tempos);
      if (err)
        goto done;
    }
    r.p += len;
  }
  *n_tracks = track;

  /* tempo map: sort by tick (stable enough: equal ticks keep later value
     since the default is first), then compute the time of each change */
  qsort(tempos.begin, tempos.size, sizeof(struct tempo_ev), tempo_cmp);
  for (size_t i = 1; i < tempos.size; ++i) {
    struct tempo_ev *a = vector_at(&tempos, i - 1);
    struct tempo_ev *b = vector_at(&tempos, i);
    b->us_at = a->us_at +
               (uint64_t)(b->tick - a->tick) * a->us_per_qn / division;
  }
  for (size_t i = 0; i < notes->size; ++i) {
    struct midi_note *n = vector_at(notes, i);
    n->start = tick_to_ms(n->start, &tempos, division, smpte_tps);
    n->end = tick_to_ms(n->end, &tempos, division, smpte_tps);
  }

done:
  vector_destroy(&tempos);
  return err;
}

int midi_input_load(const char *path, const char **err_str)
{
  int f = open(path, O_RDONLY);
  if (f == -1) {
    *err_str = strerror(errno);
    return -1;
  }
  struct stat st;
  if (fstat(f, &st)) {
    *err_str = strerror(errno);
    close(f);
    return -1;
  }
  if (st.st_size > MIDI_MAX_SIZE) {
    *err_str = "midi file too big";
    close(f);
    return -1;
  }
  uint8_t *buf = malloc(st.st_size);
  if (!buf) {
    *err_str = "out of memory";
    close(f);
    return -1;
  }
  errno = 0;
  if (read(f, buf, st.st_size) != st.st_size) {
    *err_str = errno ? strerror(errno) : "unexpected end of file";
    free(buf);
    close(f);
    return -1;
  }
  close(f);

  struct vector notes;
  int n_tracks = 0;
  vector_init(&notes, sizeof(struct midi_note));
  const char *err = parse_midi(buf, st.st_size, &notes, &n_tracks);
  free(buf);
  if (!err && notes.size == 0)
    err = "midi file has no notes";
  if (err) {
    vector_destroy(&notes);
    *err_str = err;
    return -1;
  }
  vector_shrink_to_fit(&notes);

  if (midi_notes_init)
    vector_destroy(&midi_notes);
  midi_notes = notes;
  midi_notes_init = 1;
  midi_n_tracks = n_tracks;
  return 0;
}

/* ---- conversion to an ocarina song --------------------------------------- */

/* buttons for each playable pitch, B3 to F5 */
static z64_controller_t oca_input(int pitch)
{
  z64_controller_t c;
  memset(&c, 0, sizeof(c));
  switch (pitch) {
    case 59: c.a = 1;  c.z = 1; c.y = -BEND;  break;  /* B3 */
    case 60: c.a = 1;  c.y = -BEND;           break;  /* C4 */
    case 61: c.a = 1;  c.z = 1;               break;  /* C#4 */
    case 62: c.a = 1;                         break;  /* D4 */
    case 63: c.a = 1;  c.r = 1;               break;  /* D#4 */
    case 64: c.cd = 1; c.z = 1;               break;  /* E4 */
    case 65: c.cd = 1;                        break;  /* F4 */
    case 66: c.cd = 1; c.r = 1;               break;  /* F#4 */
    case 67: c.cd = 1; c.y = BEND;            break;  /* G4 */
    case 68: c.cr = 1; c.z = 1;               break;  /* G#4 */
    case 69: c.cr = 1;                        break;  /* A4 */
    case 70: c.cr = 1; c.r = 1;               break;  /* A#4 */
    case 71: c.cl = 1;                        break;  /* B4 */
    case 72: c.cl = 1; c.r = 1;               break;  /* C5 */
    case 73: c.cu = 1; c.z = 1;               break;  /* C#5 */
    case 74: c.cu = 1;                        break;  /* D5 */
    case 75: c.cu = 1; c.r = 1;               break;  /* D#5 */
    case 76: c.cu = 1; c.y = BEND;            break;  /* E5 */
    case 77: c.cu = 1; c.r = 1; c.y = BEND;   break;  /* F5 */
  }
  return c;
}

/* how awkward a pitch is to play: 0 plain, 1 with R/Z, 2+ with stick */
static int pitch_cost(int pitch)
{
  z64_controller_t c = oca_input(pitch);
  return (c.r || c.z) + (c.y != 0) * 2;
}

static int ev_cmp(const void *a, const void *b)
{
  const struct note_ev *x = a;
  const struct note_ev *y = b;
  if (x->t != y->t)
    return x->t < y->t ? -1 : 1;
  return (int)x->on - (int)y->on; /* note ends before note starts */
}

/* reduce to one melody line: the highest sounding note */
static int make_melody(struct vector *mono)
{
  int n_ev = 0;
  for (size_t i = 0; i < midi_notes.size; ++i) {
    struct midi_note *n = vector_at(&midi_notes, i);
    if (n->chan != 9 && (midi_track == 0 || n->track == midi_track))
      n_ev += 2;
  }
  if (n_ev == 0)
    return 0;
  struct note_ev *ev = malloc(sizeof(*ev) * n_ev);
  if (!ev)
    return -1;
  int k = 0;
  for (size_t i = 0; i < midi_notes.size; ++i) {
    struct midi_note *n = vector_at(&midi_notes, i);
    if (n->chan != 9 && (midi_track == 0 || n->track == midi_track)) {
      ev[k++] = (struct note_ev){n->start, 1, n->pitch};
      ev[k++] = (struct note_ev){n->end, 0, n->pitch};
    }
  }
  qsort(ev, n_ev, sizeof(*ev), ev_cmp);

  static uint16_t active[128];
  memset(active, 0, sizeof(active));
  int cur = -1;
  uint32_t cur_start = 0;
  int i = 0;
  while (i < n_ev) {
    uint32_t t = ev[i].t;
    int retrig = -1;
    for (; i < n_ev && ev[i].t == t; ++i) {
      if (ev[i].on) {
        ++active[ev[i].pitch];
        if (ev[i].pitch > retrig)
          retrig = ev[i].pitch;
      }
      else if (active[ev[i].pitch] > 0)
        --active[ev[i].pitch];
    }
    int top = -1;
    for (int p = 127; p >= 0; --p)
      if (active[p]) {
        top = p;
        break;
      }
    if (top != cur || (top >= 0 && retrig == top)) {
      if (cur >= 0 && t > cur_start) {
        struct mono_note m = {cur_start, t, cur};
        if (push(mono, &m)) {
          free(ev);
          return -1;
        }
      }
      cur = top;
      cur_start = t;
    }
  }
  free(ev);
  return 0;
}

/* pick the transposition that puts the most notes in range */
static int fit_shift(struct vector *mono)
{
  static uint16_t hist[128];
  memset(hist, 0, sizeof(hist));
  for (size_t i = 0; i < mono->size; ++i) {
    struct mono_note *m = vector_at(mono, i);
    if (hist[m->pitch] < 0xFFFF)
      ++hist[m->pitch];
  }
  int best = 0;
  int best_in = -1;
  int best_cost = 0;
  int step = midi_fit == 0 ? 1 : 12;
  for (int s = -60; s <= 60; s += step) {
    int in = 0;
    int cost = 0;
    for (int p = 0; p < 128; ++p) {
      int q = p + s;
      if (hist[p] && q >= OCA_LOW && q <= OCA_HIGH) {
        in += hist[p];
        cost += hist[p] * pitch_cost(q);
      }
    }
    if (in > best_in || (in == best_in && (cost < best_cost ||
        (cost == best_cost && abs(s) < abs(best)))))
    {
      best = s;
      best_in = in;
      best_cost = cost;
    }
  }
  return best;
}

static int frame_at(uint32_t ms, int speed)
{
  return ((uint64_t)ms * MIDI_FPS * 100 + speed * 500) / (speed * 1000);
}

static int add_seg(const z64_controller_t *c, int len)
{
  struct lua_seg g = {*c, len, 0};
  return push(&midi_song.segs, &g);
}

static const char *midi_build(void)
{
  struct vector mono;
  const char *err = NULL;
  int speed = lua_speeds[midi_speed_idx];
  vector_init(&mono, sizeof(struct mono_note));
  lua_song_init(&midi_song, NULL);
  vector_clear(&midi_song.segs);

  if (!midi_notes_init)
    goto done;
  if (make_melody(&mono)) {
    err = "out of memory";
    goto done;
  }
  if (mono.size == 0)
    goto done;

  int shift = midi_transpose;
  if (midi_fit != 2)
    shift += fit_shift(&mono);
  midi_shift = shift;

  z64_controller_t rest;
  memset(&rest, 0, sizeof(rest));
  struct mono_note *first = vector_at(&mono, 0);
  int base = frame_at(first->start, speed);
  int prev_end = 0;
  int prev_gap = 0;
  for (size_t i = 0; i < mono.size; ++i) {
    struct mono_note *m = vector_at(&mono, i);
    int p = m->pitch + shift;
    while (p < OCA_LOW)
      p += 12;
    while (p > OCA_HIGH)
      p -= 12;
    z64_controller_t c = oca_input(p);
    int gap = lua_note_gap(&c);
    int s = frame_at(m->start, speed) - base;
    int e = frame_at(m->end, speed) - base;
    int min_s = prev_end + prev_gap;
    if (s < min_s)
      s = min_s;
    if (i + 1 < mono.size) {
      struct mono_note *next = vector_at(&mono, i + 1);
      int ns = frame_at(next->start, speed) - base;
      if (e > ns - gap)
        e = ns - gap;
    }
    if (e < s + 1)
      e = s + 1;
    if ((s > prev_end && add_seg(&rest, s - prev_end)) || add_seg(&c, e - s)) {
      err = "out of memory";
      goto done;
    }
    prev_end = e;
    prev_gap = gap;
  }
  if (add_seg(&rest, prev_gap))
    err = "out of memory";

done:
  if (err)
    vector_clear(&midi_song.segs);
  vector_destroy(&mono);
  return err;
}

/* rebuild after a setting change, keeping the place if playing */
static void midi_rebuild(int old_speed)
{
  _Bool playing = lua_song_playing(&midi_song);
  int pos = lua_song_pos(&midi_song);
  const char *err = midi_build();
  if (err) {
    lua_song_clear(&midi_song);
    gz_log("midi: %s", err);
    return;
  }
  if (playing) {
    int new_speed = lua_speeds[midi_speed_idx];
    lua_song_seek(&midi_song, pos * old_speed / new_speed);
  }
}

/* ---- menu ---------------------------------------------------------------- */

static int do_import_midi(const char *path, void *data)
{
  const char *err_str = NULL;
  lua_song_init(&midi_song, NULL);
  if (midi_input_load(path, &err_str)) {
    menu_prompt(gz.menu_main, err_str, "return\0", 0, NULL, NULL);
    return 1;
  }
  lua_song_clear(&midi_song);
  if (midi_track > midi_n_tracks)
    midi_track = 0;
  err_str = midi_build();
  if (err_str) {
    menu_prompt(gz.menu_main, err_str, "return\0", 0, NULL, NULL);
    return 1;
  }
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  strncpy(midi_name, base, sizeof(midi_name) - 1);
  midi_name[sizeof(midi_name) - 1] = 0;
  gz_log("midi loaded: %i notes", (int)midi_notes.size);
  return 0;
}

static void import_midi_proc(struct menu_item *item, void *data)
{
  menu_get_file(gz.menu_main, GETFILE_LOAD, NULL, ".mid",
                do_import_midi, NULL);
}

static void play_midi_proc(struct menu_item *item, void *data)
{
  lua_song_toggle(&midi_song);
}

static int speed_proc(struct menu_item *item,
                      enum menu_callback_reason reason, void *data)
{
  if (reason == MENU_CALLBACK_CHANGED) {
    int old_speed = lua_speeds[midi_speed_idx];
    midi_speed_idx = menu_option_get(item);
    midi_rebuild(old_speed);
  }
  else if (reason == MENU_CALLBACK_THINK) {
    if (menu_option_get(item) != midi_speed_idx)
      menu_option_set(item, midi_speed_idx);
  }
  return 0;
}

static int fit_proc(struct menu_item *item,
                    enum menu_callback_reason reason, void *data)
{
  if (reason == MENU_CALLBACK_CHANGED) {
    midi_fit = menu_option_get(item);
    midi_rebuild(lua_speeds[midi_speed_idx]);
  }
  else if (reason == MENU_CALLBACK_THINK) {
    if (menu_option_get(item) != midi_fit)
      menu_option_set(item, midi_fit);
  }
  return 0;
}

static int transpose_proc(struct menu_item *item,
                          enum menu_callback_reason reason, void *data)
{
  if (reason == MENU_CALLBACK_CHANGED) {
    int v = menu_intinput_gets(item);
    if (v < -48)
      v = -48;
    if (v > 48)
      v = 48;
    midi_transpose = v;
    midi_rebuild(lua_speeds[midi_speed_idx]);
  }
  else if (reason == MENU_CALLBACK_THINK_INACTIVE) {
    if (menu_intinput_gets(item) != midi_transpose)
      menu_intinput_set(item, midi_transpose);
  }
  return 0;
}

static int track_proc(struct menu_item *item,
                      enum menu_callback_reason reason, void *data)
{
  if (reason == MENU_CALLBACK_CHANGED) {
    int v = menu_intinput_get(item);
    if (v > midi_n_tracks)
      v = 0;
    midi_track = v;
    midi_rebuild(lua_speeds[midi_speed_idx]);
  }
  else if (reason == MENU_CALLBACK_THINK_INACTIVE) {
    if ((int)menu_intinput_get(item) != midi_track)
      menu_intinput_set(item, midi_track);
  }
  return 0;
}

static int midi_status_draw_proc(struct menu_item *item,
                                 struct menu_draw_params *draw_params)
{
  struct gfx_font *font = draw_params->font;
  int ch = menu_get_cell_height(item->owner, 1);
  int x = draw_params->x;
  int y = draw_params->y;
  gfx_mode_set(GFX_MODE_COLOR, GPACK_RGB24A8(draw_params->color,
                                             draw_params->alpha));
  if (!midi_notes_init) {
    gfx_printf(font, x, y, "no midi loaded");
    return 1;
  }
  gfx_printf(font, x, y, "%s", midi_name);
  if (lua_song_count(&midi_song) == 0)
    gfx_printf(font, x, y + ch, "no notes on this track");
  else
    gfx_printf(font, x, y + ch, "%s %i / %i",
               lua_song_playing(&midi_song) ? "playing" : "stopped",
               lua_song_pos(&midi_song), lua_song_frames(&midi_song));
  gfx_printf(font, x, y + ch * 2, "%i tracks, shift %+i",
             midi_n_tracks, midi_shift);
  return 1;
}

struct menu *gz_midi_menu(void)
{
  static struct menu menu;
  struct menu_item *item;
  menu_init(&menu, MENU_NOVALUE, MENU_NOVALUE, MENU_NOVALUE);
  menu.selector = menu_add_submenu(&menu, 0, 0, NULL, "return");
  item = menu_add_button(&menu, 0, 2, "import midi", import_midi_proc, NULL);
  item->tooltip = "load a .mid file";
  item = menu_add_button(&menu, 0, 3, "play/stop midi", play_midi_proc, NULL);
  item->tooltip = "take out ocarina first";
  menu_add_static(&menu, 0, 4, "speed", 0xC0C0C0);
  item = menu_add_option(&menu, 14, 4, LUA_SPEED_OPTIONS, speed_proc, NULL);
  menu_option_set(item, LUA_SPEED_DEFAULT);
  item->tooltip = "song speed";
  menu_add_static(&menu, 0, 5, "fit to range", 0xC0C0C0);
  item = menu_add_option(&menu, 14, 5, "auto\0""octaves\0""off\0",
                         fit_proc, NULL);
  item->tooltip = "shift into ocarina range";
  menu_add_static(&menu, 0, 6, "transpose", 0xC0C0C0);
  item = menu_add_intinput(&menu, 14, 6, -10, 3, transpose_proc, NULL);
  item->tooltip = "extra semitones up/down";
  menu_add_static(&menu, 0, 7, "track", 0xC0C0C0);
  item = menu_add_intinput(&menu, 14, 7, 10, 3, track_proc, NULL);
  item->tooltip = "track to play, 0 = all";
  menu_add_static_custom(&menu, 0, 9, midi_status_draw_proc, NULL, 0xC0C0C0);
  menu_add_tooltip(&menu, 8, 0, gz.menu_main, 0xC0C0C0);
  return &menu;
}
