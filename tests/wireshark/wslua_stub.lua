-- tests/wireshark/wslua_stub.lua — runs a dissector written by `srdump --wireshark` on a FILE capture (.ser)
-- without Wireshark, under any Lua interpreter (2.1.11). It implements the part of Wireshark's Lua API the
-- generated file uses (Proto, ProtoField, TvbRange, TreeItem, UInt64/Int64, FileHandler, …) and prints the
-- dissection as PDML, the shape `tshark -T pdml` has, so tests/wireshark/parity.py compares it with srdump.
--
-- Why: Wireshark up to 4.4 embeds Lua 5.2, 4.6 and later Lua 5.4, and the two differ where the dissector turns
-- numbers into text (5.4 keeps integers and floats apart: tostring(7.0) is "7.0"). The sandbox and CI have only
-- one tshark; this stub runs the same checks under lua5.2 and lua5.4. As in Wireshark with Lua 5.4,
-- TvbRange:le_uint()/le_int() return integers and UInt64/Int64:tonumber() returns a float.
--
-- usage: lua wslua_stub.lua <dissector.lua> <capture.ser>        (PDML on stdout; exit 1 on a load error)
-- Standard Lua only (5.2, 5.3, 5.4); no Wireshark, no C modules.

local script, capture_path = arg[1], arg[2]
if not script or not capture_path then
  io.stderr:write("usage: lua wslua_stub.lua <dissector.lua> <capture.ser>\n")
  os.exit(2)
end

local TWO32 = 4294967296
local tointeger = math.tointeger -- nil in Lua 5.2
local function int(x) -- a whole number as Wireshark 4.6 (Lua 5.4) hands it out: an integer
  if tointeger then return tointeger(x) or x end
  return x
end
local function float(x) return x + 0.0 end -- UInt64/Int64:tonumber() pushes a lua_Number (a float in 5.4)

-- ---- UInt64 / Int64: two 32-bit halves, exact in every Lua version -----------------------------------------
local U64, I64 = {}, {}
local function make(mt, hi, lo) return setmetatable({ hi = hi, lo = lo }, mt) end
local function from_number(mt, n)
  n = math.floor(n)
  if n < 0 then -- two's complement
    local m = -n
    local hi, lo = math.floor(m / TWO32), m % TWO32
    lo = (TWO32 - lo) % TWO32
    hi = (TWO32 - hi - (lo ~= 0 and 1 or 0)) % TWO32
    return make(mt, hi, lo)
  end
  return make(mt, math.floor(n / TWO32) % TWO32, n % TWO32)
end
local function wide(x, mt) -- operand as a 64-bit value
  if type(x) == "number" then return from_number(mt, x) end
  return x
end
local function divmod(u, d) -- u / d and u % d for 0 < d < 2^21
  local rhi = u.hi % d
  local qhi = (u.hi - rhi) / d
  local cur = rhi * TWO32 + u.lo
  local rem = cur % d
  return qhi, (cur - rem) / d, rem
end
local function negative(v) return getmetatable(v) == I64 and v.hi >= 2147483648 end
local function negate(v)
  local lo = (TWO32 - v.lo) % TWO32
  local hi = (TWO32 - v.hi - (v.lo ~= 0 and 1 or 0)) % TWO32
  return make(getmetatable(v), hi, lo)
end
local function decimal(v)
  if v.hi == 0 and v.lo == 0 then return "0" end
  local digits, u = {}, v
  while u.hi ~= 0 or u.lo ~= 0 do
    local qhi, qlo, r = divmod(u, 10)
    digits[#digits + 1] = string.char(48 + math.floor(r))
    u = make(U64, qhi, qlo)
  end
  return string.reverse(table.concat(digits))
end
local function new_ops(mt)
  mt.__index = mt
  mt.__tostring = function(v)
    if negative(v) then return "-" .. decimal(negate(v)) end
    return decimal(v)
  end
  mt.__eq = function(a, b) return a.hi == b.hi and a.lo == b.lo end
  mt.__lt = function(a, b)
    a, b = wide(a, mt), wide(b, mt)
    local sa, sb = negative(a), negative(b)
    if sa ~= sb then return sa end
    if a.hi ~= b.hi then return a.hi < b.hi end
    return a.lo < b.lo
  end
  mt.__le = function(a, b) return a == b or mt.__lt(a, b) end
  mt.__sub = function(a, b)
    a, b = wide(a, mt), wide(b, mt)
    local lo = a.lo - b.lo
    local borrow = lo < 0 and 1 or 0
    return make(mt, (a.hi - b.hi - borrow) % TWO32, lo % TWO32)
  end
  mt.__add = function(a, b)
    a, b = wide(a, mt), wide(b, mt)
    local lo = a.lo + b.lo
    return make(mt, (a.hi + b.hi + math.floor(lo / TWO32)) % TWO32, lo % TWO32)
  end
  mt.__mod = function(a, d)
    local _, _, r = divmod(wide(a, mt), d)
    return from_number(mt, r)
  end
  mt.__div = function(a, d)
    local qhi, qlo = divmod(wide(a, mt), d)
    return make(mt, qhi, qlo)
  end
  mt.__concat = function(a, b) return tostring(a) .. tostring(b) end
  mt.tohex = function(v) return string.format("%08X%08X", int(v.hi), int(v.lo)) end
  mt.tonumber = function(v)
    if negative(v) then local n = negate(v); return float(-(n.hi * TWO32 + n.lo)) end
    return float(v.hi * TWO32 + v.lo)
  end
end
new_ops(U64)
new_ops(I64)
UInt64 = setmetatable({}, { __call = function(_, n) return from_number(U64, n or 0) end })
Int64 = setmetatable({}, { __call = function(_, n) return from_number(I64, n or 0) end })

-- ---- Tvb and TvbRange ---------------------------------------------------------------------------------------
local Range = {}
Range.__index = Range
local function new_range(data, off, len) return setmetatable({ data = data, off = off, n = len }, Range) end
local function new_tvb(data)
  return setmetatable({ data = data }, {
    __index = { len = function(self) return #self.data end },
    __call = function(self, off, len)
      off = off or 0
      len = len or (#self.data - off)
      if off < 0 or len < 0 or off + len > #self.data then error("Range is out of bounds", 2) end
      return new_range(self.data, off, len)
    end,
  })
end
-- ByteArray.new("2a000000"):tvb(name) — 2.1.15: the dissector shows a constant of the call from its own bytes
ByteArray = {
  new = function(hex)
    local data = (hex:gsub("%s", ""):gsub("%x%x", function(h) return string.char(tonumber(h, 16)) end))
    return { tvb = function(_, _) return new_tvb(data) end }
  end,
}
function Range:len() return self.n end
function Range:byte(i) return self.data:byte(self.off + i + 1) end
function Range:le_uint()
  local v = 0
  for i = self.n - 1, 0, -1 do v = v * 256 + self:byte(i) end
  return int(v)
end
function Range:uint()
  local v = 0
  for i = 0, self.n - 1 do v = v * 256 + self:byte(i) end
  return int(v)
end
function Range:le_int()
  local v, top = self:le_uint(), 2 ^ (8 * self.n)
  if v >= top / 2 then v = v - top end
  return int(v)
end
function Range:int()
  local v, top = self:uint(), 2 ^ (8 * self.n)
  if v >= top / 2 then v = v - top end
  return int(v)
end
local function halves(r)
  local lo, hi = 0, 0
  for i = 3, 0, -1 do lo = lo * 256 + r:byte(i) end
  for i = 7, 4, -1 do hi = hi * 256 + r:byte(i) end
  return hi, lo
end
function Range:le_uint64() local hi, lo = halves(self); return make(U64, hi, lo) end
function Range:le_int64() local hi, lo = halves(self); return make(I64, hi, lo) end
function Range:le_float()
  if self.n == 4 then
    local b1, b2, b3, b4 = self:byte(0), self:byte(1), self:byte(2), self:byte(3)
    local sign = b4 >= 128 and -1 or 1
    local e = (b4 % 128) * 2 + math.floor(b3 / 128)
    local m = (b3 % 128) * 65536 + b2 * 256 + b1
    if e == 255 then return m == 0 and sign * math.huge or (0 / 0) end
    if e == 0 then return sign * m * 2.0 ^ -149 end
    return sign * (1 + m / 2 ^ 23) * 2.0 ^ (e - 127)
  end
  local hi, lo = halves(self)
  local sign = hi >= 2147483648 and -1 or 1
  local e = math.floor(hi / 1048576) % 2048
  local m = (hi % 1048576) * TWO32 + lo
  if e == 2047 then return m == 0 and sign * math.huge or (0 / 0) end
  if e == 0 then return sign * m * 2.0 ^ -1074 end
  return sign * (1 + m / 2 ^ 52) * 2.0 ^ (e - 1023)
end
function Range:raw() return self.data:sub(self.off + 1, self.off + self.n) end
function Range:string() return (self:raw():gsub("%z.*$", "")) end -- up to the first NUL, as lua_pushstring does
function Range:tvb() return new_tvb(self:raw()) end

-- ---- Proto, ProtoField, ProtoExpert, preferences, tables ----------------------------------------------------
local protos = {}
Proto = setmetatable({}, { __call = function(_, name, desc)
  local p = { is_proto = true, name = name, desc = desc, prefs = {}, fields = {}, experts = {} }
  function p.register_heuristic() end
  protos[name] = p
  return p
end })
local function field_ctor(ftype)
  return function(abbr, label, base_, vs) return { is_field = true, abbr = abbr, label = label, ftype = ftype, base = base_, vs = vs } end
end
ProtoField = {}
for _, t in ipairs({ "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "string", "bool",
                     "float", "double", "bytes" }) do
  ProtoField[t] = field_ctor(t)
end
ProtoExpert = { new = function(abbr, text) return { is_expert = true, abbr = abbr, text = text } end }
local function names() return setmetatable({}, { __index = function(_, k) return k end }) end
base, expert = names(), { group = names(), severity = names() }
wtap_encaps, wtap_tsprecs, wtap_rec_types, wtap_presence_flags = names(), names(), names(), names()
Pref = { uint = function(_, default) return default end }
DissectorTable = { get = function() return { add = function() end, remove = function() end } end }
local handlers = {}
FileHandler = { new = function(name) return { name = name } end }
function register_filehandler(h) handlers[#handlers + 1] = h end
function dissect_tcp_pdus() end

-- ---- TreeItem ------------------------------------------------------------------------------------------------
local Item = {}
Item.__index = Item
local function new_item(attrs) return setmetatable({ attrs = attrs, kids = {} }, Item) end
local INTS = { int8 = true, int16 = true, int32 = true, uint8 = true, uint16 = true, uint32 = true }
local function number_show(f, v, width)
  if f.base == "HEX" then return string.format("0x%0" .. (width * 2) .. "x", int(v)) end
  return string.format("%d", int(v))
end
local SIZE = { int8 = 1, uint8 = 1, int16 = 2, uint16 = 2, int32 = 4, uint32 = 4 }
local function field_show(f, r, value, le)
  local t = f.ftype
  if value ~= nil then
    if type(value) == "table" then return tostring(value) end
    if type(value) == "boolean" then return value and "True" or "False" end
    if INTS[t] then return number_show(f, value, SIZE[t]) end
    return tostring(value)
  end
  if INTS[t] then
    local v
    if t:sub(1, 1) == "u" then v = le and r:le_uint() or r:uint() else v = le and r:le_int() or r:int() end
    return number_show(f, v, SIZE[t])
  elseif t == "uint64" or t == "int64" then
    local v = t == "uint64" and r:le_uint64() or r:le_int64()
    if f.base == "HEX" then return "0x" .. v:tohex():lower() end
    return tostring(v)
  elseif t == "bool" then return r:le_uint() ~= 0 and "True" or "False"
  elseif t == "float" or t == "double" then return string.format("%.17g", r:le_float())
  elseif t == "string" then return r:string()
  end
  return r:raw()
end
local function add(self, le, what, r, value, ...)
  local node
  if type(what) == "table" and what.is_field then
    local show = field_show(what, r, value, le)
    node = new_item({ name = what.abbr, show = show, showname = what.label .. ": " .. show })
  elseif type(what) == "table" and what.is_proto then
    node = new_item({ proto = what.name, showname = value or what.desc })
  else -- a text item: add(range, text)
    node = new_item({ name = "", show = r, showname = r })
  end
  self.kids[#self.kids + 1] = node
  return node
end
function Item:add(...) return add(self, false, ...) end
function Item:add_le(...) return add(self, true, ...) end
function Item:append_text(t) self.attrs.showname = (self.attrs.showname or "") .. t end
function Item:add_proto_expert_info(x, text)
  self.kids[#self.kids + 1] = new_item({ name = "_ws.expert", showname = text or x.text, show = x.abbr })
end
function Item:set_generated() end
function Item:set_text(t) self.attrs.showname = t end

-- ---- PDML ----------------------------------------------------------------------------------------------------
local function esc(s)
  return (tostring(s):gsub("[%z\1-\31&<>\"\127-\255]", function(c)
    if c == "&" then return "&amp;" elseif c == "<" then return "&lt;" elseif c == ">" then return "&gt;"
    elseif c == '"' then return "&quot;" end
    return string.format("\\x%02x", c:byte())
  end))
end
local function emit(node, out, depth)
  local pad = string.rep("  ", depth)
  if node.attrs.proto then
    out[#out + 1] = pad .. '<proto name="' .. esc(node.attrs.proto) .. '" showname="' .. esc(node.attrs.showname) .. '">'
    for _, k in ipairs(node.kids) do emit(k, out, depth + 1) end
    out[#out + 1] = pad .. "</proto>"
    return
  end
  out[#out + 1] = pad .. '<field name="' .. esc(node.attrs.name) .. '" show="' .. esc(node.attrs.show or "") ..
                  '" showname="' .. esc(node.attrs.showname or "") .. '"' .. (#node.kids == 0 and "/>" or ">")
  if #node.kids > 0 then
    for _, k in ipairs(node.kids) do emit(k, out, depth + 1) end
    out[#out + 1] = pad .. "</field>"
  end
end

-- ---- driver: the generated FileHandler reads the capture, the serreg dissector dissects each packet ----------
local ok, err = pcall(dofile, script)
if not ok then
  io.stderr:write("wslua_stub: loading " .. script .. ": " .. tostring(err) .. "\n")
  os.exit(1)
end
local reader = handlers[1]
local serreg = protos.serreg
if not reader or not serreg or not serreg.dissector then
  io.stderr:write("wslua_stub: the script registered no file handler or no serreg dissector\n")
  os.exit(1)
end

local fh = assert(io.open(capture_path, "rb"))
local bytes = fh:read("*a")
fh:close()
local pos = 0
local file = {
  seek = function(_, whence, off)
    off = off or 0
    if whence == "set" then pos = off elseif whence == "cur" then pos = pos + off else pos = #bytes + off end
    return pos
  end,
  read = function(_, n)
    if pos >= #bytes then return nil end
    local s = bytes:sub(pos + 1, pos + n)
    pos = pos + #s
    return s
  end,
}
local capture = {}
local out = { '<?xml version="1.0"?>', "<pdml>" }
if not reader.read_open(file, capture) then
  io.stderr:write("wslua_stub: the file handler did not accept " .. capture_path .. "\n")
  os.exit(1)
end
for _ = 1, 1000000 do
  local frame = {}
  local at = reader.read(file, capture, frame)
  if not at then break end
  local root = new_item({ proto = "frame", showname = "frame" })
  local pinfo = { cols = {} }
  local dok, derr = pcall(serreg.dissector, new_tvb(frame.data), pinfo, root)
  if not dok then
    root.kids[#root.kids + 1] = new_item({ name = "_ws.lua.error", showname = "Lua Error: " .. tostring(derr) })
  end
  out[#out + 1] = "<packet>"
  for _, k in ipairs(root.kids) do emit(k, out, 1) end
  out[#out + 1] = "</packet>"
end
out[#out + 1] = "</pdml>"
io.write(table.concat(out, "\n"), "\n")
