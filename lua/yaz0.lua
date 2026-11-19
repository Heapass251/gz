-- Yaz0 compression for the gz payload (see src/gz/ldr.S, GZ_PACKED).
--
-- yaz0.compress(blob) -> blob
-- yaz0.decompress(blob) -> blob
--
-- Format: "Yaz0", u32 decompressed size, 8 zero bytes, then groups of a code
-- byte followed by 8 chunks (msb first). A 1 bit is a literal byte. A 0 bit
-- is a back reference of 2 or 3 bytes:
--   NR RR      -> length N + 2, distance RRR + 1      (N = 1..15)
--   0R RR NN   -> length NN + 0x12, distance RRR + 1

local yaz0 = {}

local WINDOW = 0x1000
local MIN_LEN = 3
local MAX_LEN = 0xFF + 0x12
local MAX_CHAIN = 48

local function blob_to_string(b)
  local parts = {}
  local n = b:size()
  local chunk = {}
  for i = 0, n - 1 do
    chunk[#chunk + 1] = b:read8(i)
    if #chunk == 4096 then
      parts[#parts + 1] = string.char(table.unpack(chunk))
      chunk = {}
    end
  end
  if #chunk > 0 then parts[#parts + 1] = string.char(table.unpack(chunk)) end
  return table.concat(parts)
end

local function bytes_to_blob(out)
  local parts = {}
  for i = 1, #out, 4096 do
    parts[#parts + 1] = string.char(table.unpack(out, i, math.min(i + 4095, #out)))
  end
  return gru.blob_string(table.concat(parts))
end

function yaz0.compress(blob)
  local s = blob_to_string(blob)
  local n = #s
  local byte = string.byte
  local d = {byte(s, 1, -1)}  -- 1-based bytes; fine for a payload < 1 MiB
  local head = {}             -- 3-byte key -> most recent position
  local prev = {}             -- position -> previous position with same key

  local function key(p) return (d[p] << 16) | (d[p + 1] << 8) | d[p + 2] end
  local function insert(p)
    if p + 2 <= n then
      local k = key(p)
      prev[p] = head[k]
      head[k] = p
    end
  end

  local function find(p)
    if p + 2 > n then return 0, 0 end
    local best_len, best_dist = 0, 0
    local max_len = math.min(MAX_LEN, n - p + 1)
    local q = head[key(p)]
    local chain = 0
    while q and p - q <= WINDOW and chain < MAX_CHAIN do
      local l = 3
      while l < max_len and d[q + l] == d[p + l] do l = l + 1 end
      if l > best_len then
        best_len, best_dist = l, p - q
        if l == max_len then break end
      end
      q = prev[q]
      chain = chain + 1
    end
    return best_len, best_dist
  end

  local out = {0x59, 0x61, 0x7A, 0x30,
               (n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF,
               0, 0, 0, 0, 0, 0, 0, 0}
  local p = 1
  while p <= n do
    local code_pos = #out + 1
    out[code_pos] = 0
    local code = 0
    for bit = 7, 0, -1 do
      if p > n then break end
      local len, dist = find(p)
      -- lazy matching: prefer a longer match starting at the next byte
      if len >= MIN_LEN and len < MAX_LEN then
        insert(p)
        local len2 = find(p + 1)
        if len2 > len + 1 then len = 0 end
      else
        insert(p)
      end
      if len >= MIN_LEN then
        local r = dist - 1
        if len <= 0x11 then
          out[#out + 1] = ((len - 2) << 4) | (r >> 8)
          out[#out + 1] = r & 0xFF
        else
          out[#out + 1] = r >> 8
          out[#out + 1] = r & 0xFF
          out[#out + 1] = len - 0x12
        end
        for i = p + 1, p + len - 1 do insert(i) end
        p = p + len
      else
        code = code | (1 << bit)
        out[#out + 1] = d[p]
        p = p + 1
      end
    end
    out[code_pos] = code
  end
  while #out % 16 ~= 0 do out[#out + 1] = 0 end
  return bytes_to_blob(out)
end

-- For decompressing in place: the data is placed at out_start + margin and
-- decompressed forward to out_start. Returns the smallest margin for which
-- the output never overwrites input that hasn't been read yet.
function yaz0.inplace_margin(blob)
  local s = blob_to_string(blob)
  local byte = string.byte
  local n = (byte(s, 5) << 24) | (byte(s, 6) << 16) | (byte(s, 7) << 8) | byte(s, 8)
  local o = 0     -- bytes written
  local p = 16    -- bytes read (0-based)
  local margin = 0
  while o < n do
    local code = byte(s, p + 1); p = p + 1
    for bit = 7, 0, -1 do
      if o >= n then break end
      if code & (1 << bit) ~= 0 then
        p = p + 1
        o = o + 1
      else
        local b1 = byte(s, p + 1)
        p = p + 2
        local len = b1 >> 4
        if len == 0 then
          len = byte(s, p + 1) + 0x12; p = p + 1
        else
          len = len + 2
        end
        o = o + len
      end
      -- written bytes [0, o) must not reach unread input at margin + p
      if o - p > margin then margin = o - p end
    end
  end
  return margin
end

function yaz0.decompress(blob)
  local s = blob_to_string(blob)
  local byte = string.byte
  assert(s:sub(1, 4) == "Yaz0", "not yaz0 data")
  local n = (byte(s, 5) << 24) | (byte(s, 6) << 16) | (byte(s, 7) << 8) | byte(s, 8)
  local out = {}
  local o = 0
  local p = 17
  while o < n do
    local code = byte(s, p); p = p + 1
    for bit = 7, 0, -1 do
      if o >= n then break end
      if code & (1 << bit) ~= 0 then
        o = o + 1
        out[o] = byte(s, p); p = p + 1
      else
        local b1, b2 = byte(s, p, p + 1); p = p + 2
        local dist = (((b1 & 0x0F) << 8) | b2) + 1
        local len = b1 >> 4
        if len == 0 then
          len = byte(s, p) + 0x12; p = p + 1
        else
          len = len + 2
        end
        for _ = 1, len do
          o = o + 1
          out[o] = out[o - dist]
        end
      end
    end
  end
  return bytes_to_blob(out)
end

return yaz0
