#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"
#include "gfx.h"
#include "gz.h"
#include "input.h"
#include "lua_input.h"
#include "lua_vm.h"
#include "z64.h"

/*
 * BizHawk-compatible Lua runtime. See lua_vm.h for what is supported.
 *
 * Each game frame, lua_vm_frame():
 *   1. calls event.onframeend callbacks (for the frame that just ended)
 *   2. resumes the script until its next emu.frameadvance()
 *   3. calls event.onframestart callbacks
 * and returns the input collected from joypad.set() during all of that.
 * The script keeps running after its main chunk returns if it registered
 * frame callbacks, until it is stopped.
 *
 * Addresses for mainmemory/memory are RDRAM offsets like in BizHawk
 * (0x1D887C); 0x80xxxxxx/0xA0xxxxxx addresses are accepted too.
 */

#define VM_SRC_MAX      (256 * 1024)  /* script file size limit */
#define VM_MEM_LIMIT    (1024 * 1024) /* lua heap limit, keeps room for
                                         savestates */
#define VM_HOOK_COUNT   1000          /* instructions per hook call */
#define VM_HOOK_LIMIT   1000          /* hook calls per frame */
#define RDRAM_SIZE      0x800000
#define STICK_DIGITAL   80
#define GUI_MAX         16
#define GUI_TEXT_MAX    48
#define LOG_WIDTH       38
#define LOG_LINES       6

enum vm_state
{
  VM_STOPPED,
  VM_RUNNING,
  VM_FINISHED,
  VM_ERROR,
};

struct gui_text
{
  int       x;
  int       y;
  uint32_t  rgba;
  char      s[GUI_TEXT_MAX];
};

static char            *vm_src;
static size_t           vm_src_len;
static char             vm_name[32];
static char             vm_chunk[40];
static lua_State       *vm_L;
static lua_State       *vm_co;
static int              vm_disp_ref = LUA_NOREF;
static _Bool            vm_co_alive;
static enum vm_state    vm_state;
static int              vm_frame_n;
static int              vm_budget;
static size_t           vm_mem;
static char             vm_err[160];
static z64_controller_t vm_in;
static struct gui_text  vm_gui[GUI_MAX];
static int              vm_gui_n;

/* ---- helpers ------------------------------------------------------------ */

static void vm_log(const char *s)
{
  for (int lines = 0; *s && lines < LOG_LINES; ++lines) {
    int n = 0;
    while (s[n] && s[n] != '\n' && n < LOG_WIDTH)
      ++n;
    gz_log("%.*s", n, s);
    s += n;
    if (*s == '\n')
      ++s;
  }
}

static void *vm_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  size_t old = ptr ? osize : 0;
  if (nsize == 0) {
    free(ptr);
    vm_mem -= old;
    return NULL;
  }
  if (nsize > old && vm_mem + (nsize - old) > VM_MEM_LIMIT)
    return NULL;
  void *p = realloc(ptr, nsize);
  if (p)
    vm_mem = vm_mem - old + nsize;
  return p;
}

static void *plain_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, nsize);
}

static int vm_panic(lua_State *L)
{
  const char *msg = lua_tostring(L, -1);
  gz_log("lua panic: %s", msg ? msg : "?");
  return 0;
}

/* stop runaway scripts that never call emu.frameadvance() */
static void vm_hook(lua_State *L, lua_Debug *ar)
{
  if (++vm_budget > VM_HOOK_LIMIT)
    luaL_error(L, "script ran too long without emu.frameadvance()");
}

/* accept integers and floats (truncated), like BizHawk does */
static lua_Integer get_int(lua_State *L, int idx)
{
  if (lua_isinteger(L, idx))
    return lua_tointeger(L, idx);
  return (lua_Integer)luaL_checknumber(L, idx);
}

static const char *str_ieq_prefix(const char *s, const char *word)
{
  while (*word) {
    char a = *s, b = *word;
    if (a >= 'A' && a <= 'Z')
      a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z')
      b += 'a' - 'A';
    if (a != b)
      return NULL;
    ++s;
    ++word;
  }
  return s;
}

static _Bool str_ieq(const char *a, const char *b)
{
  const char *e = str_ieq_prefix(a, b);
  return e && *e == 0;
}

/* ---- memory ------------------------------------------------------------- */

static uint8_t *ram_at(lua_State *L, lua_Integer addr, int size)
{
  if (addr >= 0 && addr <= 0xFFFFFFFF) {
    uint32_t a = addr;
    if (a >= 0x80000000 && a < 0xC0000000)
      a &= 0x1FFFFFFF;
    if (a <= (uint32_t)(RDRAM_SIZE - size))
      return (uint8_t *)(0x80000000 | a);
  }
  char b[64];
  snprintf(b, sizeof(b), "address 0x%llX is outside RDRAM",
           (unsigned long long)addr);
  luaL_error(L, "%s", b);
  return NULL;
}

static void check_domain(lua_State *L, int idx)
{
  if (idx == 0 || lua_isnoneornil(L, idx))
    return;
  const char *d = luaL_checkstring(L, idx);
  if (str_ieq(d, "RDRAM") || str_ieq(d, "System Bus"))
    return;
  luaL_error(L, "memory domain '%s' is not supported (only RDRAM)", d);
}

static uint32_t ram_read(uint8_t *p, int size, _Bool big)
{
  uint32_t v = 0;
  for (int i = 0; i < size; ++i)
    v = (v << 8) | p[big ? i : size - 1 - i];
  return v;
}

static void ram_write(uint8_t *p, int size, _Bool big, uint32_t v)
{
  for (int i = 0; i < size; ++i)
    p[big ? i : size - 1 - i] = v >> (8 * (size - 1 - i));
}

/* upvalues: size, big endian, signed, index of domain argument (0 = none) */
static int m_read(lua_State *L)
{
  int size = lua_tointeger(L, lua_upvalueindex(1));
  _Bool big = lua_toboolean(L, lua_upvalueindex(2));
  _Bool sgn = lua_toboolean(L, lua_upvalueindex(3));
  check_domain(L, lua_tointeger(L, lua_upvalueindex(4)));
  uint32_t v = ram_read(ram_at(L, get_int(L, 1), size), size, big);
  lua_Integer r = v;
  if (sgn && (v & (1u << (size * 8 - 1))))
    r -= (lua_Integer)1 << (size * 8);
  lua_pushinteger(L, r);
  return 1;
}

static int m_write(lua_State *L)
{
  int size = lua_tointeger(L, lua_upvalueindex(1));
  _Bool big = lua_toboolean(L, lua_upvalueindex(2));
  check_domain(L, lua_tointeger(L, lua_upvalueindex(4)));
  uint8_t *p = ram_at(L, get_int(L, 1), size);
  ram_write(p, size, big, get_int(L, 2));
  return 0;
}

/* readfloat(addr, bigendian [, domain]) */
static int m_readfloat(lua_State *L)
{
  check_domain(L, lua_tointeger(L, lua_upvalueindex(1)));
  union { uint32_t u; float f; } v;
  v.u = ram_read(ram_at(L, get_int(L, 1), 4), 4, lua_toboolean(L, 2));
  lua_pushnumber(L, v.f);
  return 1;
}

/* writefloat(addr, value, bigendian [, domain]) */
static int m_writefloat(lua_State *L)
{
  check_domain(L, lua_tointeger(L, lua_upvalueindex(1)));
  uint8_t *p = ram_at(L, get_int(L, 1), 4);
  union { uint32_t u; float f; } v;
  v.f = luaL_checknumber(L, 2);
  ram_write(p, 4, lua_toboolean(L, 3), v.u);
  return 0;
}

/* readbyterange(addr, length [, domain]) -> 0-indexed table
   read_bytes_as_array(addr, length [, domain]) -> 1-indexed table */
static int m_readrange(lua_State *L)
{
  check_domain(L, lua_tointeger(L, lua_upvalueindex(1)));
  int base = lua_tointeger(L, lua_upvalueindex(2));
  lua_Integer addr = get_int(L, 1);
  lua_Integer len = get_int(L, 2);
  if (len < 0 || len > RDRAM_SIZE)
    return luaL_error(L, "bad length");
  uint8_t *p = len > 0 ? ram_at(L, addr, len) : NULL;
  lua_createtable(L, len, 0);
  for (lua_Integer i = 0; i < len; ++i) {
    lua_pushinteger(L, p[i]);
    lua_rawseti(L, -2, i + base);
  }
  return 1;
}

/* writebyterange({[addr] = value, ...} [, domain]) */
static int m_writerange(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  check_domain(L, lua_tointeger(L, lua_upvalueindex(1)) ? 2 : 0);
  lua_pushnil(L);
  while (lua_next(L, 1)) {
    *ram_at(L, get_int(L, -2), 1) = get_int(L, -1);
    lua_pop(L, 1);
  }
  return 0;
}

/* write_bytes_as_array(addr, {v1, v2, ...} [, domain]) */
static int m_writearray(lua_State *L)
{
  check_domain(L, lua_tointeger(L, lua_upvalueindex(1)) ? 3 : 0);
  lua_Integer addr = get_int(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);
  lua_Integer n = luaL_len(L, 2);
  for (lua_Integer i = 0; i < n; ++i) {
    lua_rawgeti(L, 2, i + 1);
    *ram_at(L, addr + i, 1) = get_int(L, -1);
    lua_pop(L, 1);
  }
  return 0;
}

static int m_name(lua_State *L)
{
  lua_pushliteral(L, "RDRAM");
  return 1;
}

static int m_size(lua_State *L)
{
  lua_pushinteger(L, RDRAM_SIZE);
  return 1;
}

static int m_usedomain(lua_State *L)
{
  const char *d = luaL_checkstring(L, 1);
  lua_pushboolean(L, str_ieq(d, "RDRAM") || str_ieq(d, "System Bus"));
  return 1;
}

static int m_domainlist(lua_State *L)
{
  lua_createtable(L, 1, 0);
  lua_pushliteral(L, "RDRAM");
  lua_rawseti(L, -2, 1);
  return 1;
}

struct mem_fn
{
  const char *name;
  uint8_t     size;
  uint8_t     big;
  uint8_t     sgn;
  uint8_t     write;
};

static const struct mem_fn mem_fns[] =
{
  {"readbyte",     1, 1, 0, 0}, {"writebyte",    1, 1, 0, 1},
  {"read_u8",      1, 1, 0, 0}, {"read_s8",      1, 1, 1, 0},
  {"write_u8",     1, 1, 0, 1}, {"write_s8",     1, 1, 1, 1},
  {"read_u16_be",  2, 1, 0, 0}, {"read_s16_be",  2, 1, 1, 0},
  {"read_u24_be",  3, 1, 0, 0}, {"read_s24_be",  3, 1, 1, 0},
  {"read_u32_be",  4, 1, 0, 0}, {"read_s32_be",  4, 1, 1, 0},
  {"read_u16_le",  2, 0, 0, 0}, {"read_s16_le",  2, 0, 1, 0},
  {"read_u24_le",  3, 0, 0, 0}, {"read_s24_le",  3, 0, 1, 0},
  {"read_u32_le",  4, 0, 0, 0}, {"read_s32_le",  4, 0, 1, 0},
  {"write_u16_be", 2, 1, 0, 1}, {"write_s16_be", 2, 1, 1, 1},
  {"write_u24_be", 3, 1, 0, 1}, {"write_s24_be", 3, 1, 1, 1},
  {"write_u32_be", 4, 1, 0, 1}, {"write_s32_be", 4, 1, 1, 1},
  {"write_u16_le", 2, 0, 0, 1}, {"write_s16_le", 2, 0, 1, 1},
  {"write_u24_le", 3, 0, 0, 1}, {"write_s24_le", 3, 0, 1, 1},
  {"write_u32_le", 4, 0, 0, 1}, {"write_s32_le", 4, 0, 1, 1},
};

/* push a mainmemory (domains = 0) or memory (domains = 1) table */
static void push_memory_lib(lua_State *L, _Bool domains)
{
  lua_newtable(L);
  for (size_t i = 0; i < sizeof(mem_fns) / sizeof(*mem_fns); ++i) {
    const struct mem_fn *f = &mem_fns[i];
    lua_pushinteger(L, f->size);
    lua_pushboolean(L, f->big);
    lua_pushboolean(L, f->sgn);
    lua_pushinteger(L, domains ? (f->write ? 3 : 2) : 0);
    lua_pushcclosure(L, f->write ? m_write : m_read, 4);
    lua_setfield(L, -2, f->name);
  }
  lua_pushinteger(L, domains ? 3 : 0);
  lua_pushcclosure(L, m_readfloat, 1);
  lua_setfield(L, -2, "readfloat");
  lua_pushinteger(L, domains ? 4 : 0);
  lua_pushcclosure(L, m_writefloat, 1);
  lua_setfield(L, -2, "writefloat");
  lua_pushinteger(L, domains ? 3 : 0);
  lua_pushinteger(L, 0);
  lua_pushcclosure(L, m_readrange, 2);
  lua_setfield(L, -2, "readbyterange");
  lua_pushinteger(L, domains ? 3 : 0);
  lua_pushinteger(L, 1);
  lua_pushcclosure(L, m_readrange, 2);
  lua_setfield(L, -2, "read_bytes_as_array");
  lua_pushinteger(L, domains);
  lua_pushcclosure(L, m_writerange, 1);
  lua_setfield(L, -2, "writebyterange");
  lua_pushinteger(L, domains);
  lua_pushcclosure(L, m_writearray, 1);
  lua_setfield(L, -2, "write_bytes_as_array");
  lua_pushcfunction(L, m_name);
  lua_setfield(L, -2, domains ? "getcurrentmemorydomain" : "getname");
  lua_pushcfunction(L, m_size);
  lua_setfield(L, -2, "getcurrentmemorydomainsize");
  if (domains) {
    lua_pushcfunction(L, m_size);
    lua_setfield(L, -2, "getmemorydomainsize");
    lua_pushcfunction(L, m_usedomain);
    lua_setfield(L, -2, "usememorydomain");
    lua_pushcfunction(L, m_domainlist);
    lua_setfield(L, -2, "getmemorydomainlist");
  }
}

/* ---- joypad ------------------------------------------------------------- */

struct button
{
  const char *name;   /* BizHawk name */
  uint16_t    bit;
};

static const struct button buttons[] =
{
  {"A",       BUTTON_A},       {"B",       BUTTON_B},
  {"Z",       BUTTON_Z},       {"Start",   BUTTON_START},
  {"L",       BUTTON_L},       {"R",       BUTTON_R},
  {"C Up",    BUTTON_C_UP},    {"C Down",  BUTTON_C_DOWN},
  {"C Left",  BUTTON_C_LEFT},  {"C Right", BUTTON_C_RIGHT},
  {"DPad U",  BUTTON_D_UP},    {"DPad D",  BUTTON_D_DOWN},
  {"DPad L",  BUTTON_D_LEFT},  {"DPad R",  BUTTON_D_RIGHT},
};

static const struct button button_aliases[] =
{
  {"DPad Up",   BUTTON_D_UP},   {"DPad Down",  BUTTON_D_DOWN},
  {"DPad Left", BUTTON_D_LEFT}, {"DPad Right", BUTTON_D_RIGHT},
};

static int8_t clamp_axis(lua_Integer v)
{
  if (v < -128)
    return -128;
  if (v > 127)
    return 127;
  return v;
}

/* strip a "P1 " style prefix; returns NULL for other players */
static const char *player1_key(const char *k)
{
  if ((k[0] == 'P' || k[0] == 'p') && k[1] >= '0' && k[1] <= '9') {
    const char *p = k + 1;
    int n = 0;
    while (*p >= '0' && *p <= '9')
      n = n * 10 + (*p++ - '0');
    if (*p == ' ') {
      if (n != 1)
        return NULL;
      return p + 1;
    }
  }
  return k;
}

/* is controller argument idx missing or 1 */
static _Bool is_player1(lua_State *L, int idx)
{
  return lua_isnoneornil(L, idx) || get_int(L, idx) == 1;
}

static _Bool value_pressed(lua_State *L, int idx)
{
  switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:  return lua_toboolean(L, idx);
    case LUA_TNUMBER:   return lua_tonumber(L, idx) != 0;
    case LUA_TSTRING:   return str_ieq(lua_tostring(L, idx), "true");
    default:            return 0;
  }
}

/* apply one key = value pair to vm_in */
static void set_key(lua_State *L, const char *k, int vidx)
{
  k = player1_key(k);
  if (!k)
    return;
  if (str_ieq(k, "X Axis")) {
    if (lua_isnumber(L, vidx))
      vm_in.x = clamp_axis(get_int(L, vidx));
    return;
  }
  if (str_ieq(k, "Y Axis")) {
    if (lua_isnumber(L, vidx))
      vm_in.y = clamp_axis(get_int(L, vidx));
    return;
  }
  if (!value_pressed(L, vidx))
    return;
  for (size_t i = 0; i < sizeof(buttons) / sizeof(*buttons); ++i)
    if (str_ieq(k, buttons[i].name)) {
      vm_in.pad |= buttons[i].bit;
      return;
    }
  for (size_t i = 0; i < sizeof(button_aliases) / sizeof(*button_aliases); ++i)
    if (str_ieq(k, button_aliases[i].name)) {
      vm_in.pad |= button_aliases[i].bit;
      return;
    }
  /* analog stick as digital directions */
  if (str_ieq(k, "A Up"))
    vm_in.y = STICK_DIGITAL;
  else if (str_ieq(k, "A Down"))
    vm_in.y = -STICK_DIGITAL;
  else if (str_ieq(k, "A Left"))
    vm_in.x = -STICK_DIGITAL;
  else if (str_ieq(k, "A Right"))
    vm_in.x = STICK_DIGITAL;
  /* Power, Reset and unknown names are ignored, like BizHawk */
}

/* joypad.set(buttons [, controller]) */
static int j_set(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  if (!is_player1(L, 2))
    return 0;
  lua_pushnil(L);
  while (lua_next(L, 1)) {
    if (lua_type(L, -2) == LUA_TSTRING)
      set_key(L, lua_tostring(L, -2), lua_gettop(L));
    lua_pop(L, 1);
  }
  return 0;
}

/* joypad.setanalog({["X Axis"] = x, ["Y Axis"] = y} [, controller]) */
static int j_setanalog(lua_State *L)
{
  return j_set(L);
}

/* BizHawk N64 mnemonic: "|<power,reset>|<x>,<y>,<18 buttons>|..."
   Buttons: A-Up A-Down A-Left A-Right, D-Up D-Down D-Left D-Right,
   Start Z B A, C-Up C-Down C-Right C-Left, L R. '.' is not pressed. */
static int j_setfrommnemonicstr(lua_State *L)
{
  static const uint16_t bits[] =
  {
    0, 0, 0, 0,
    BUTTON_D_UP, BUTTON_D_DOWN, BUTTON_D_LEFT, BUTTON_D_RIGHT,
    BUTTON_START, BUTTON_Z, BUTTON_B, BUTTON_A,
    BUTTON_C_UP, BUTTON_C_DOWN, BUTTON_C_RIGHT, BUTTON_C_LEFT,
    BUTTON_L, BUTTON_R,
  };
  const int n_buttons = sizeof(bits) / sizeof(*bits);
  const char *s = luaL_checkstring(L, 1);
  if (*s != '|' || !(s = strchr(s + 1, '|')))
    return luaL_error(L, "bad input string");
  ++s;
  char *end;
  long x = strtol(s, &end, 10);
  if (end == s || *end != ',')
    return luaL_error(L, "bad stick x value in input string");
  s = end + 1;
  long y = strtol(s, &end, 10);
  if (end == s || *end != ',')
    return luaL_error(L, "bad stick y value in input string");
  s = end + 1;
  uint16_t pad = 0;
  int n = 0;
  for (; *s && *s != '|'; ++s, ++n) {
    if (n == n_buttons)
      return luaL_error(L, "too many buttons in input string (not n64?)");
    if (*s == '.' || *s == ' ')
      continue;
    pad |= bits[n];
    if (n == 0)
      y = STICK_DIGITAL;
    else if (n == 1)
      y = -STICK_DIGITAL;
    else if (n == 2)
      x = -STICK_DIGITAL;
    else if (n == 3)
      x = STICK_DIGITAL;
  }
  if (n != n_buttons)
    return luaL_error(L, "too few buttons in input string (not n64?)");
  vm_in.pad |= pad;
  if (x != 0)
    vm_in.x = clamp_axis(x);
  if (y != 0)
    vm_in.y = clamp_axis(y);
  return 0;
}

/* joypad.get([controller]): with no controller, names are "P1 A" etc. */
static int j_get(lua_State *L)
{
  _Bool p1 = is_player1(L, 1);
  const char *prefix = lua_isnoneornil(L, 1) ? "P1 " : "";
  z64_controller_t c = z64_ctxt.input[0].raw;
  if (!p1)
    memset(&c, 0, sizeof(c));
  lua_newtable(L);
  for (size_t i = 0; i < sizeof(buttons) / sizeof(*buttons); ++i) {
    lua_pushfstring(L, "%s%s", prefix, buttons[i].name);
    lua_pushboolean(L, (c.pad & buttons[i].bit) != 0);
    lua_rawset(L, -3);
  }
  static const char *dirs[] = {"A Up", "A Down", "A Left", "A Right"};
  for (int i = 0; i < 4; ++i) {
    lua_pushfstring(L, "%s%s", prefix, dirs[i]);
    lua_pushboolean(L, 0);
    lua_rawset(L, -3);
  }
  lua_pushfstring(L, "%sX Axis", prefix);
  lua_pushinteger(L, c.x);
  lua_rawset(L, -3);
  lua_pushfstring(L, "%sY Axis", prefix);
  lua_pushinteger(L, c.y);
  lua_rawset(L, -3);
  return 1;
}

static const luaL_Reg joypad_lib[] =
{
  {"set",                 j_set},
  {"setanalog",           j_setanalog},
  {"setfrommnemonicstr",  j_setfrommnemonicstr},
  {"get",                 j_get},
  {"getimmediate",        j_get},
  {"getwithmovie",        j_get},
  {NULL, NULL},
};

/* ---- emu ---------------------------------------------------------------- */

static int e_frameadvance(lua_State *L)
{
  if (!lua_isyieldable(L))
    return luaL_error(L, "emu.frameadvance() can't be called from an event "
                      "callback");
  return lua_yield(L, 0);
}

static int e_framecount(lua_State *L)
{
  lua_pushinteger(L, vm_frame_n);
  return 1;
}

static int e_systemid(lua_State *L)
{
  lua_pushliteral(L, "N64");
  return 1;
}

static int ret_false(lua_State *L)
{
  lua_pushboolean(L, 0);
  return 1;
}

static int ret_zero(lua_State *L)
{
  lua_pushinteger(L, 0);
  return 1;
}

static int noop(lua_State *L)
{
  return 0;
}

static const luaL_Reg emu_lib[] =
{
  {"frameadvance",        e_frameadvance},
  {"yield",               e_frameadvance},
  {"framecount",          e_framecount},
  {"getsystemid",         e_systemid},
  {"islagged",            ret_false},
  {"lagcount",            ret_zero},
  /* display / speed settings that have no meaning here */
  {"limitframerate",      noop},
  {"minimizeframeskip",   noop},
  {"setrenderplanes",     noop},
  {"displayvsync",        noop},
  {"frameskip",           noop},
  {"enablerewind",        noop},
  {"setislagged",         noop},
  {"setlagcount",         noop},
  {NULL, NULL},
};

/* ---- console / print ---------------------------------------------------- */

static int c_log(lua_State *L)
{
  int n = lua_gettop(L);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (int i = 1; i <= n; ++i) {
    if (i > 1)
      luaL_addchar(&b, ' ');
    luaL_tolstring(L, i, NULL);
    luaL_addvalue(&b);
  }
  luaL_pushresult(&b);
  vm_log(lua_tostring(L, -1));
  return 0;
}

static const luaL_Reg console_lib[] =
{
  {"log",                 c_log},
  {"writeline",           c_log},
  {"write",               c_log},
  {"clear",               noop},
  {NULL, NULL},
};

/* ---- gui ---------------------------------------------------------------- */

/* BizHawk colors are 0xAARRGGBB numbers or names; returns 0xRRGGBBAA */
static uint32_t get_color(lua_State *L, int idx, uint32_t def)
{
  if (lua_type(L, idx) == LUA_TNUMBER) {
    uint32_t v = get_int(L, idx);
    uint32_t a = v >> 24;
    if (a == 0)
      a = 0xFF;   /* 0xRRGGBB: treat as opaque */
    return (v << 8) | a;
  }
  if (lua_type(L, idx) == LUA_TSTRING) {
    static const struct { const char *name; uint32_t rgba; } names[] =
    {
      {"white",   0xFFFFFFFF}, {"black",   0x000000FF},
      {"red",     0xFF0000FF}, {"green",   0x00FF00FF},
      {"blue",    0x0000FFFF}, {"yellow",  0xFFFF00FF},
      {"cyan",    0x00FFFFFF}, {"magenta", 0xFF00FFFF},
      {"orange",  0xFFA500FF}, {"purple",  0x800080FF},
      {"gray",    0x808080FF}, {"grey",    0x808080FF},
    };
    const char *s = lua_tostring(L, idx);
    if (s[0] == '#') {
      char *end;
      unsigned long v = strtoul(s + 1, &end, 16);
      size_t len = end - (s + 1);
      if (*end == 0 && len == 6)
        return (v << 8) | 0xFF;
      if (*end == 0 && len == 8)
        return (v << 8) | (v >> 24);
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
      if (str_ieq(s, names[i].name))
        return names[i].rgba;
  }
  return def;
}

/* gui.text(x, y, message [, color]), also drawText / pixelText */
static int g_text(lua_State *L)
{
  int x = luaL_checknumber(L, 1);
  int y = luaL_checknumber(L, 2);
  const char *s = luaL_tolstring(L, 3, NULL);
  if (vm_gui_n < GUI_MAX) {
    struct gui_text *t = &vm_gui[vm_gui_n++];
    t->x = x;
    t->y = y;
    t->rgba = get_color(L, 4, 0xFFFFFFFF);
    strncpy(t->s, s, sizeof(t->s) - 1);
    t->s[sizeof(t->s) - 1] = 0;
  }
  return 0;
}

static int g_clear(lua_State *L)
{
  vm_gui_n = 0;
  return 0;
}

/* other gui functions (drawing shapes, images...) are ignored */
static int g_index(lua_State *L)
{
  const char *k = lua_tostring(L, 2);
  if (k) {
    char b[64];
    snprintf(b, sizeof(b), "gui.%s is not supported, ignored", k);
    vm_log(b);
  }
  lua_pushcfunction(L, noop);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, -2);
  lua_rawset(L, 1);   /* only warn once */
  return 1;
}

static const luaL_Reg gui_lib[] =
{
  {"text",                g_text},
  {"drawText",            g_text},
  {"drawString",          g_text},
  {"pixelText",           g_text},
  {"clearGraphics",       g_clear},
  {"cleartext",           g_clear},
  {NULL, NULL},
};

/* ---- client ------------------------------------------------------------- */

static int cl_width(lua_State *L)
{
  lua_pushinteger(L, Z64_SCREEN_WIDTH);
  return 1;
}

static int cl_height(lua_State *L)
{
  lua_pushinteger(L, Z64_SCREEN_HEIGHT);
  return 1;
}

static const luaL_Reg client_lib[] =
{
  {"screenwidth",         cl_width},
  {"screenheight",        cl_height},
  {"bufferwidth",         cl_width},
  {"bufferheight",        cl_height},
  {NULL, NULL},
};

/* ---- lua-side part: event and bit libraries ------------------------------
 * Returns the event dispatcher: dispatch(kind) calls the callbacks of that
 * kind (0 = frame start, 1 = frame end, 2 = exit) and returns the number of
 * frame callbacks still registered.
 */

static const char vm_prelude[] =
  "local cbs, nextid = {}, 0\n"
  "local function reg(kind, fn, name)\n"
  "  if type(fn) ~= 'function' then\n"
  "    error('bad argument #1 (function expected, got ' .. type(fn) .. ')', 3)\n"
  "  end\n"
  "  nextid = nextid + 1\n"
  "  local id = 'gz-' .. nextid\n"
  "  cbs[#cbs + 1] = {kind = kind, fn = fn, name = name, id = id}\n"
  "  return id\n"
  "end\n"
  "local function unreg(field, v)\n"
  "  local found = false\n"
  "  for i = #cbs, 1, -1 do\n"
  "    if cbs[i][field] == v then table.remove(cbs, i); found = true end\n"
  "  end\n"
  "  return found\n"
  "end\n"
  "event = {\n"
  "  onframestart = function(fn, name) return reg(0, fn, name) end,\n"
  "  onframeend = function(fn, name) return reg(1, fn, name) end,\n"
  "  onexit = function(fn, name) return reg(2, fn, name) end,\n"
  "  unregisterbyid = function(id) return unreg('id', id) end,\n"
  "  unregisterbyname = function(name) return unreg('name', name) end,\n"
  "}\n"
  "local M = 0xFFFFFFFF\n"
  "local function fold(op)\n"
  "  return function(a, ...)\n"
  "    for _, b in ipairs({...}) do a = op(a, b) end\n"
  "    return a & M\n"
  "  end\n"
  "end\n"
  "local function s32(a) a = a & M; return a >= 0x80000000 and a - 0x100000000 or a end\n"
  "bit = {\n"
  "  band = fold(function(a, b) return a & b end),\n"
  "  bor = fold(function(a, b) return a | b end),\n"
  "  bxor = fold(function(a, b) return a ~ b end),\n"
  "  bnot = function(a) return ~a & M end,\n"
  "  lshift = function(a, n) return (a << n) & M end,\n"
  "  rshift = function(a, n) return (a & M) >> n end,\n"
  "  arshift = function(a, n) return (s32(a) // (1 << n)) & M end,\n"
  "  rol = function(a, n) a = a & M; n = n % 32\n"
  "    return ((a << n) | (a >> (32 - n))) & M end,\n"
  "  ror = function(a, n) a = a & M; n = n % 32\n"
  "    return ((a >> n) | (a << (32 - n))) & M end,\n"
  "  check = function(a, pos) return (a >> pos) & 1 == 1 end,\n"
  "  set = function(a, pos) return (a | (1 << pos)) & M end,\n"
  "  clear = function(a, pos) return (a & ~(1 << pos)) & M end,\n"
  "  byteswap_16 = function(a) return ((a & 0xFF) << 8) | ((a >> 8) & 0xFF) end,\n"
  "  byteswap_32 = function(a)\n"
  "    return ((a & 0xFF) << 24) | ((a & 0xFF00) << 8)\n"
  "         | ((a >> 8) & 0xFF00) | ((a >> 24) & 0xFF) end,\n"
  "}\n"
  "return function(kind)\n"
  "  for _, c in ipairs({table.unpack(cbs)}) do\n"
  "    if c.kind == kind then c.fn() end\n"
  "  end\n"
  "  local n = 0\n"
  "  for _, c in ipairs(cbs) do if c.kind ~= 2 then n = n + 1 end end\n"
  "  return n\n"
  "end\n";

/* ---- state setup -------------------------------------------------------- */

static void set_lib(lua_State *L, const char *name, const luaL_Reg *fns)
{
  lua_newtable(L);
  luaL_setfuncs(L, fns, 0);
  lua_setglobal(L, name);
}

static int vm_setup(lua_State *L)
{
  static const luaL_Reg std_libs[] =
  {
    {LUA_GNAME,       luaopen_base},
    {LUA_COLIBNAME,   luaopen_coroutine},
    {LUA_TABLIBNAME,  luaopen_table},
    {LUA_STRLIBNAME,  luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {NULL, NULL},
  };
  for (const luaL_Reg *l = std_libs; l->name; ++l) {
    luaL_requiref(L, l->name, l->func, 1);
    lua_pop(L, 1);
  }
  /* no file system access from scripts */
  lua_pushnil(L);
  lua_setglobal(L, "dofile");
  lua_pushnil(L);
  lua_setglobal(L, "loadfile");
  lua_pushcfunction(L, c_log);
  lua_setglobal(L, "print");

  set_lib(L, "joypad", joypad_lib);
  set_lib(L, "emu", emu_lib);
  set_lib(L, "console", console_lib);
  set_lib(L, "client", client_lib);
  set_lib(L, "gui", gui_lib);
  lua_getglobal(L, "gui");
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, g_index);
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
  push_memory_lib(L, 0);
  lua_setglobal(L, "mainmemory");
  push_memory_lib(L, 1);
  lua_setglobal(L, "memory");
  /* gz extras: gz.game is the RDRAM offset of the game context for this
     game version, so scripts can avoid version-specific addresses */
  lua_createtable(L, 0, 1);
  lua_pushinteger(L, (uintptr_t)&z64_game & 0x1FFFFFFF);
  lua_setfield(L, -2, "game");
  lua_setglobal(L, "gz");

  if (luaL_loadbufferx(L, vm_prelude, sizeof(vm_prelude) - 1, "=gz", "t"))
    return lua_error(L);
  lua_call(L, 0, 1);
  vm_disp_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_State *co = lua_newthread(L);
  luaL_ref(L, LUA_REGISTRYINDEX);
  if (luaL_loadbufferx(co, vm_src, vm_src_len, vm_chunk, "t")) {
    lua_xmove(co, L, 1);
    return lua_error(L);
  }
  vm_co = co;
  return 0;
}

/* ---- control ------------------------------------------------------------ */

/* call event callbacks of a kind; returns the number of frame callbacks, or
   -1 on error with the message on top of the stack */
static int vm_dispatch(int kind)
{
  lua_State *L = vm_L;
  vm_budget = 0;
  lua_rawgeti(L, LUA_REGISTRYINDEX, vm_disp_ref);
  lua_pushinteger(L, kind);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK)
    return -1;
  int n = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return n;
}

static void vm_close(void)
{
  if (vm_L) {
    if (vm_disp_ref != LUA_NOREF) {
      if (vm_dispatch(2) < 0)
        lua_pop(vm_L, 1);
    }
    lua_close(vm_L);
  }
  vm_L = NULL;
  vm_co = NULL;
  vm_disp_ref = LUA_NOREF;
  vm_gui_n = 0;
}

/* stop with the error message on top of vm_L's stack */
static void vm_fail(void)
{
  const char *msg = lua_tostring(vm_L, -1);
  if (!msg)
    msg = "(error object is not a string)";
  strncpy(vm_err, msg, sizeof(vm_err) - 1);
  vm_err[sizeof(vm_err) - 1] = 0;
  lua_pop(vm_L, 1);
  vm_close();
  vm_state = VM_ERROR;
  gz_log("lua error:");
  vm_log(vm_err);
}

int lua_vm_load(const char *path, const char **err_str)
{
  static char load_err[sizeof(vm_err)];
  FILE *f = fopen(path, "rb");
  if (!f) {
    *err_str = strerror(errno);
    return -1;
  }
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  while (buf) {
    if (len == cap) {
      if (cap >= VM_SRC_MAX) {
        free(buf);
        fclose(f);
        *err_str = "file too large";
        return -1;
      }
      cap *= 2;
      char *nbuf = realloc(buf, cap);
      if (!nbuf) {
        free(buf);
        buf = NULL;
        break;
      }
      buf = nbuf;
    }
    size_t n = fread(buf + len, 1, cap - len, f);
    len += n;
    if (n == 0)
      break;
  }
  _Bool read_err = ferror(f);
  fclose(f);
  if (!buf) {
    *err_str = "out of memory";
    return -1;
  }
  if (read_err) {
    free(buf);
    *err_str = "read error";
    return -1;
  }

  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  char chunk[sizeof(vm_chunk)];
  snprintf(chunk, sizeof(chunk), "=%s", base);

  /* syntax check */
  lua_State *L = lua_newstate(plain_alloc, NULL);
  if (!L) {
    free(buf);
    *err_str = "out of memory";
    return -1;
  }
  lua_atpanic(L, vm_panic);
  if (luaL_loadbufferx(L, buf, len, chunk, "t") != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    strncpy(load_err, msg ? msg : "syntax error", sizeof(load_err) - 1);
    load_err[sizeof(load_err) - 1] = 0;
    lua_close(L);
    free(buf);
    *err_str = load_err;
    return -1;
  }
  lua_close(L);

  lua_vm_stop();
  free(vm_src);
  vm_src = buf;
  vm_src_len = len;
  strcpy(vm_chunk, chunk);
  strncpy(vm_name, base, sizeof(vm_name) - 1);
  vm_name[sizeof(vm_name) - 1] = 0;
  vm_state = VM_STOPPED;
  vm_err[0] = 0;
  return 0;
}

_Bool lua_vm_loaded(void)
{
  return vm_src != NULL;
}

const char *lua_vm_name(void)
{
  return vm_name;
}

void lua_vm_start(void)
{
  if (!vm_src) {
    gz_log("no lua loaded");
    return;
  }
  lua_vm_stop();
  lua_song_stop();
  vm_mem = 0;
  vm_err[0] = 0;
  memset(&vm_in, 0, sizeof(vm_in));
  lua_State *L = lua_newstate(vm_alloc, NULL);
  if (!L) {
    strcpy(vm_err, "not enough memory");
    vm_state = VM_ERROR;
    gz_log("lua error: %s", vm_err);
    return;
  }
  lua_atpanic(L, vm_panic);
  lua_sethook(L, vm_hook, LUA_MASKCOUNT, VM_HOOK_COUNT);
  vm_L = L;
  vm_budget = 0;
  lua_pushcfunction(L, vm_setup);
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    vm_fail();
    return;
  }
  vm_co_alive = 1;
  vm_frame_n = 0;
  vm_gui_n = 0;
  vm_state = VM_RUNNING;
}

void lua_vm_stop(void)
{
  if (vm_state != VM_RUNNING)
    return;
  vm_close();
  vm_state = VM_STOPPED;
}

void lua_vm_toggle(void)
{
  if (lua_vm_running())
    lua_vm_stop();
  else
    lua_vm_start();
}

_Bool lua_vm_running(void)
{
  return vm_state == VM_RUNNING;
}

int lua_vm_frames(void)
{
  return vm_frame_n;
}

const char *lua_vm_error(void)
{
  return vm_state == VM_ERROR ? vm_err : NULL;
}

const char *lua_vm_status(void)
{
  switch (vm_state) {
    case VM_RUNNING:  return "running";
    case VM_FINISHED: return "finished";
    case VM_ERROR:    return "error";
    default:          return "stopped";
  }
}

_Bool lua_vm_frame(z64_controller_t *c)
{
  if (vm_state != VM_RUNNING)
    return 0;
  memset(&vm_in, 0, sizeof(vm_in));
  vm_gui_n = 0;

  if (vm_frame_n > 0 && vm_dispatch(1) < 0) {
    vm_fail();
    return 0;
  }
  if (vm_co_alive) {
    int nres;
    vm_budget = 0;
    int r = lua_resume(vm_co, vm_L, 0, &nres);
    if (r == LUA_YIELD)
      lua_pop(vm_co, nres);
    else if (r == LUA_OK) {
      lua_pop(vm_co, nres);
      vm_co_alive = 0;
    }
    else {
      lua_xmove(vm_co, vm_L, 1);
      vm_fail();
      return 0;
    }
  }
  int n_cbs = vm_dispatch(0);
  if (n_cbs < 0) {
    vm_fail();
    return 0;
  }

  *c = vm_in;
  ++vm_frame_n;
  if (!vm_co_alive && n_cbs == 0) {
    /* done; this frame's input is still used */
    vm_close();
    vm_state = VM_FINISHED;
    gz_log("lua finished");
  }
  return 1;
}

void lua_vm_draw(struct gfx_font *font, uint8_t alpha)
{
  if (vm_state != VM_RUNNING)
    return;
  for (int i = 0; i < vm_gui_n; ++i) {
    struct gui_text *t = &vm_gui[i];
    uint8_t a = (t->rgba & 0xFF) * alpha / 0xFF;
    gfx_mode_set(GFX_MODE_COLOR, GPACK_RGB24A8(t->rgba >> 8, a));
    gfx_printf(font, t->x, t->y + font->baseline, "%s", t->s);
  }
}
