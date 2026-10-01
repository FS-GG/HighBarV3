function gadget:GetInfo()
  return {
    name = "BARC Stock Queue Reader",
    desc = "Bounded team-scoped stock production and rally observation",
    author = "FS-GG",
    version = "1",
    date = "2026-10-01",
    license = "MIT",
    layer = 0,
    enabled = true,
  }
end

if not gadgetHandler:IsSyncedCode() then
  return
end

local REQUEST_ROUTE = "BARC_QUEUE_REQUEST/"
local REQUEST_MAGIC = "BARC_QUEUE_REQUEST/1"
local RESPONSE_MAGIC = "BARC_QUEUE_RESPONSE/1"
local BRIDGE = "barc-stock-queue-reader-v1"
local MAX_BYTES = 8192
local MAX_LINE_BYTES = 512
local MAX_ENTRIES = 64
local PROBE_ENTRIES = 65
local MAX_PARAMS = 16
local MAX_TOTAL_PARAMS = 256
local TWO32 = 4294967296
local TWO31 = 2147483648
local HEX = "0123456789abcdef"

local function u32(value)
  return value % TWO32
end

local function pureBand(a, b)
  local result, place = 0, 1
  a, b = u32(a), u32(b)
  for _ = 1, 32 do
    local aa, bb = a % 2, b % 2
    if aa == 1 and bb == 1 then result = result + place end
    a, b, place = math.floor(a / 2), math.floor(b / 2), place * 2
  end
  return result
end

local function pureBxor(a, b)
  local result, place = 0, 1
  a, b = u32(a), u32(b)
  for _ = 1, 32 do
    local aa, bb = a % 2, b % 2
    if aa ~= bb then result = result + place end
    a, b, place = math.floor(a / 2), math.floor(b / 2), place * 2
  end
  return result
end

local bitlib = rawget(_G, "bit") or rawget(_G, "bit32")
local function band(a, b)
  if bitlib then return u32(bitlib.band(a, b)) end
  return pureBand(a, b)
end
local function bxor2(a, b)
  if bitlib then return u32(bitlib.bxor(a, b)) end
  return pureBxor(a, b)
end
local function bxor(a, b, c)
  local result = bxor2(a, b)
  return c == nil and result or bxor2(result, c)
end
local function bnot(a)
  if bitlib then return u32(bitlib.bnot(a)) end
  return 4294967295 - u32(a)
end
local function rshift(a, count)
  if bitlib then return u32(bitlib.rshift(a, count)) end
  return math.floor(u32(a) / (2 ^ count))
end
local function ror(a, count)
  if bitlib and bitlib.ror then return u32(bitlib.ror(a, count)) end
  if bitlib and bitlib.rrotate then return u32(bitlib.rrotate(a, count)) end
  return u32(rshift(a, count) + (u32(a) % (2 ^ count)) * (2 ^ (32 - count)))
end

local function hex32(value)
  local chars = {}
  value = u32(value)
  for shift = 28, 0, -4 do
    local nibble = math.floor(value / (2 ^ shift)) % 16
    chars[#chars + 1] = HEX:sub(nibble + 1, nibble + 1)
  end
  return table.concat(chars)
end

local SHA_K = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
}

local function sha256(message)
  local bytes = {}
  for index = 1, #message do bytes[index] = message:byte(index) end
  local bitLength = #bytes * 8
  bytes[#bytes + 1] = 0x80
  while (#bytes % 64) ~= 56 do bytes[#bytes + 1] = 0 end
  local high = math.floor(bitLength / TWO32)
  local low = bitLength % TWO32
  for shift = 24, 0, -8 do bytes[#bytes + 1] = math.floor(high / (2 ^ shift)) % 256 end
  for shift = 24, 0, -8 do bytes[#bytes + 1] = math.floor(low / (2 ^ shift)) % 256 end

  local h = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}
  for offset = 1, #bytes, 64 do
    local w = {}
    for index = 0, 15 do
      local base = offset + index * 4
      w[index] = bytes[base] * 16777216 + bytes[base + 1] * 65536 + bytes[base + 2] * 256 + bytes[base + 3]
    end
    for index = 16, 63 do
      local x, y = w[index - 15], w[index - 2]
      local s0 = bxor(ror(x, 7), ror(x, 18), rshift(x, 3))
      local s1 = bxor(ror(y, 17), ror(y, 19), rshift(y, 10))
      w[index] = u32(w[index - 16] + s0 + w[index - 7] + s1)
    end
    local a,b,c,d,e,f,g,hh = h[1],h[2],h[3],h[4],h[5],h[6],h[7],h[8]
    for index = 0, 63 do
      local s1 = bxor(ror(e, 6), ror(e, 11), ror(e, 25))
      local choice = bxor(band(e, f), band(bnot(e), g))
      local t1 = u32(hh + s1 + choice + SHA_K[index + 1] + w[index])
      local s0 = bxor(ror(a, 2), ror(a, 13), ror(a, 22))
      local majority = bxor(band(a, b), band(a, c), band(b, c))
      local t2 = u32(s0 + majority)
      hh,g,f,e,d,c,b,a = g,f,e,u32(d + t1),c,b,a,u32(t1 + t2)
    end
    h[1],h[2],h[3],h[4] = u32(h[1]+a),u32(h[2]+b),u32(h[3]+c),u32(h[4]+d)
    h[5],h[6],h[7],h[8] = u32(h[5]+e),u32(h[6]+f),u32(h[7]+g),u32(h[8]+hh)
  end
  local parts = {}
  for index = 1, 8 do parts[index] = hex32(h[index]) end
  return table.concat(parts)
end

local function integerInRange(value, minimum, maximum)
  return type(value) == "number" and value == math.floor(value) and value >= minimum and value <= maximum
end

local negativeZeroSupported = (1 / (-0.0)) == -math.huge
local function float32Hex(value)
  if type(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then
    return nil, "nonfinite-float"
  end
  local negative = value < 0 or (value == 0 and negativeZeroSupported and (1 / value) == -math.huge)
  local magnitude = math.abs(value)
  if magnitude == 0 then
    if not negativeZeroSupported then return nil, "negative-zero-unsupported" end
    return negative and "80000000" or "00000000"
  end
  local fraction, exponent = math.frexp(magnitude)
  local exponentBits, mantissa
  if exponent > -126 then
    exponentBits = exponent + 126
    mantissa = math.floor(((fraction * 2 - 1) * 8388608) + 0.5)
    if mantissa == 8388608 then exponentBits, mantissa = exponentBits + 1, 0 end
    if exponentBits >= 255 then return nil, "nonfinite-float" end
  else
    exponentBits = 0
    mantissa = math.floor((magnitude * (2 ^ 149)) + 0.5)
    if mantissa <= 0 or mantissa >= 8388608 then return nil, "nonfinite-float" end
  end
  local bits = (negative and TWO31 or 0) + exponentBits * 8388608 + mantissa
  return hex32(bits)
end

local function splitLines(message)
  if message:sub(-1) ~= "\n" then return nil end
  local lines, position = {}, 1
  while position <= #message do
    local newline = message:find("\n", position, true)
    if not newline then return nil end
    if newline - position + 1 > MAX_LINE_BYTES then return nil end
    lines[#lines + 1] = message:sub(position, newline - 1)
    position = newline + 1
  end
  return lines
end

local function parseRequest(message)
  if type(message) ~= "string" or #message > MAX_BYTES or message:find("\0", 1, true) then return nil end
  for index = 1, #message do if message:byte(index) > 127 then return nil end end
  local lines = splitLines(message)
  if not lines or #lines ~= 6 or lines[6] ~= "end" then return nil end
  local length = lines[2]:match("^length=(%d%d%d%d%d%d%d%d)$")
  local bridge = lines[3]:match("^bridge=([%w%-]+)$")
  local domain = lines[4]:match("^domain=([%l]+)$")
  local unitText = lines[5]:match("^unit=(%-?%d+)$")
  local unit = unitText and tonumber(unitText) or nil
  if not length or tonumber(length) ~= #message or not bridge or not domain or not integerInRange(unit, 1, 2147483647) then return nil end
  if domain ~= "production" and domain ~= "rally" then return nil end
  local reason
  if lines[1] ~= REQUEST_MAGIC then reason = lines[1]:sub(1, #REQUEST_ROUTE) == REQUEST_ROUTE and "wrong-version" or "malformed"
  elseif bridge ~= BRIDGE then reason = "missing-bridge" end
  return { bridge=bridge, domain=domain, unit=unit, reason=reason }
end

local function response(requestHash, domain, unit, status, reason, rows)
  local lines = { RESPONSE_MAGIC, "length=00000000", "bridge=" .. BRIDGE, "request-sha256=" .. requestHash,
    "status=" .. status, "domain=" .. domain, "unit=" .. tostring(unit) }
  if reason then lines[#lines + 1] = "reason=" .. reason end
  lines[#lines + 1] = "count=" .. tostring(#rows)
  for _, row in ipairs(rows) do lines[#lines + 1] = "row=" .. row end
  lines[#lines + 1] = "end"
  local document = table.concat(lines, "\n") .. "\n"
  document = document:gsub("length=00000000", string.format("length=%08d", #document), 1)
  if #document > MAX_BYTES then return nil end
  for _, line in ipairs(lines) do if #line + 1 > MAX_LINE_BYTES then return nil end end
  return document
end

local function unavailable(message, request, reason)
  return response(sha256(message), request.domain, request.unit, "unavailable", reason, {})
end

local function encodeRows(domain, commands)
  if type(commands) ~= "table" then return nil, "malformed" end
  if #commands > MAX_ENTRIES then return nil, "overflow" end
  local rows, totalParams = {}, 0
  for index = 1, #commands do
    local command = commands[index]
    local options = type(command) == "table" and command.options or nil
    local params = type(command) == "table" and command.params or nil
    if type(options) ~= "table" or type(params) ~= "table"
      or not integerInRange(command.id, -2147483648, 2147483647)
      or not integerInRange(options.coded, 0, 65535)
      or not integerInRange(command.tag, -2147483648, 2147483647) then return nil, "malformed" end
    if #params > MAX_PARAMS then return nil, "overflow" end
    totalParams = totalParams + #params
    if totalParams > MAX_TOTAL_PARAMS then return nil, "overflow" end
    local encoded = {}
    for paramIndex = 1, #params do
      local bits, reason = float32Hex(params[paramIndex])
      if not bits then return nil, reason end
      encoded[paramIndex] = bits
    end
    local row = table.concat({domain, tostring(command.id), tostring(options.coded), tostring(command.tag), table.concat(encoded, ",")}, "|")
    if #row + 5 > MAX_LINE_BYTES then return nil, "oversize" end
    rows[index] = row
  end
  return rows
end

function gadget:RecvSkirmishAIMessage(aiTeam, message)
  if type(message) ~= "string" or message:sub(1, #REQUEST_ROUTE) ~= REQUEST_ROUTE then return nil end
  local request = parseRequest(message)
  if not request then return nil end
  if request.reason then return unavailable(message, request, request.reason) end
  if not Spring.ValidUnitID(request.unit) or Spring.GetUnitTeam(request.unit) ~= aiTeam then
    return unavailable(message, request, "wrong-team")
  end
  local unitDefID = Spring.GetUnitDefID(request.unit)
  if not unitDefID or not UnitDefs[unitDefID] or UnitDefs[unitDefID].isFactory ~= true then
    return unavailable(message, request, "not-factory")
  end
  local commands
  if request.domain == "production" then commands = Spring.GetFactoryCommands(request.unit, PROBE_ENTRIES)
  else commands = Spring.GetUnitCommands(request.unit, PROBE_ENTRIES) end
  local rows, reason = encodeRows(request.domain, commands)
  if not rows then return unavailable(message, request, reason) end
  local result = response(sha256(message), request.domain, request.unit, "ok", nil, rows)
  if result then return result end
  return unavailable(message, request, "oversize")
end
