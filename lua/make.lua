require("lua/rom_table")

local arg = {...}
local rom = gru.n64rom_load(arg[1])
local rom_info = rom_table[rom:crc32()]
if rom_info == nil then return nil end

local gz_version = rom_info.gz_version
print("rom is " ..
      rom_info.game .. "-" .. rom_info.version .. "-" .. rom_info.region)

print("building gz")
local make = os.getenv("MAKE")
if make == nil or make == "" then make = "make" end
local _,_,make_result = os.execute(make ..
                                   " gz-" .. gz_version ..
                                   " gz-" .. gz_version .. "-hooks")
if make_result ~= 0 then error("failed to build gz", 0) end

print("loading file system")
local fs = gru.z64fs_load_blob(rom)

print("patching files")
local patches = { gru.gsc_load("hooks/gz/" .. gz_version .. "/gz.gsc") }
for _,v in pairs(rom_info.patches or {}) do
  patches[#patches + 1] = gru.gsc_load("gsc/" .. rom_info.data_dir .. "/" .. v .. ".gsc")
end
local do_hooks = loadfile("lua/hooks.lua")
do_hooks(rom_info, fs, patches)

print("reassembling rom")
local patched_rom = fs:assemble_rom()

print("inserting payload")
local bin = gru.blob_load("bin/gz/" .. gz_version .. "/gz.bin")
local gz_start = bin:read32be(0x0)
local gz_size = bin:read32be(0x4)
local ldr_size = bin:read32be(0x8)
local ldr_rom_lui_ori = bin:read32be(0xC)
local gz = bin:copy(gz_start, gz_size)

if gz:size() % 16 ~= 0 then gz:resize(gz:size() + 16 - gz:size() % 16) end
local payload_rom = fs:prom_tail()
local ldr = gz:copy(0, ldr_size)
ldr:write16be(ldr_rom_lui_ori + 0x2, (payload_rom | 0x10000000) >> 16)
ldr:write16be(ldr_rom_lui_ori + 0x6, (payload_rom | 0x10000000))
local old_ldr = patched_rom:copy(0x1000, ldr_size)
gz:write(0, old_ldr)

-- packed payload (wii vc): [yaz0 compressed image][unpacker], loaded at the
-- end of gz's memory area and decompressed in place, see ldr.S
local payload = gz
local unpack_start = 0
local unpack_size = 0
local ldr_size_lui_ori = 0
local ldr_dest_lui_ori = 0
local ldr_jump = 0
local image_ram = 0
if gz_start >= 0x30 then
  unpack_start = bin:read32be(0x10)
  unpack_size = bin:read32be(0x14)
  ldr_size_lui_ori = bin:read32be(0x18)
  ldr_dest_lui_ori = bin:read32be(0x1C)
  ldr_jump = bin:read32be(0x20)
  image_ram = bin:read32be(0x24)
end
if unpack_size ~= 0 then
  print("compressing payload")
  local yaz0 = dofile("lua/yaz0.lua")
  local packed = yaz0.compress(gz)
  -- make sure it unpacks to exactly the image
  local check = yaz0.decompress(packed)
  if check:size() ~= gz:size() then error("payload compression failed", 0) end
  for i = 0, gz:size() - 1 do
    if check:read8(i) ~= gz:read8(i) then
      error("payload compression failed", 0)
    end
  end
  -- place the data so that decompressing forward to image_ram never
  -- overwrites data that hasn't been read yet (plus some slack)
  local margin = yaz0.inplace_margin(packed) + 0x40
  local data_ram = image_ram + margin
  if data_ram % 16 ~= 0 then data_ram = data_ram + 16 - data_ram % 16 end
  local unpack_ram = data_ram + packed:size()
  local unpacker = bin:copy(unpack_start, unpack_size)
  unpacker:write16be(0x2, (data_ram >> 16) & 0xFFFF)
  unpacker:write16be(0x6, data_ram & 0xFFFF)
  payload = gru.blob_create(packed:size() + unpack_size)
  payload:write(0, packed)
  payload:write(packed:size(), unpacker)
  -- stay well inside gz's own memory (it is cleared/reused after boot)
  if unpack_ram + unpack_size > image_ram + 0x200000 then
    error("compressed gz is too large", 0)
  end
  local dest = data_ram & 0x1FFFFFFF
  ldr:write16be(ldr_dest_lui_ori + 0x2, (dest >> 16) & 0xFFFF)
  ldr:write16be(ldr_dest_lui_ori + 0x6, dest & 0xFFFF)
  local len = payload:size() - 1
  ldr:write16be(ldr_size_lui_ori + 0x2, (len >> 16) & 0xFFFF)
  ldr:write16be(ldr_size_lui_ori + 0x6, len & 0xFFFF)
  ldr:write32be(ldr_jump, 0x08000000 | ((unpack_ram & 0x0FFFFFFF) >> 2))
  print(string.format("payload %d bytes, compressed to %d, loaded at %08X",
                      gz:size(), payload:size(), data_ram))
end

-- the wii vc emulator only loads the first 32 MiB of the rom
if wiivc and payload_rom + payload:size() > 0x2000000 then
  error(string.format("gz does not fit in the wii vc rom (%d bytes over)",
                      payload_rom + payload:size() - 0x2000000), 0)
end

patched_rom:write(0x1000, ldr)
patched_rom:write(payload_rom, payload)
patched_rom:crc_update()

return rom_info, rom, patched_rom
