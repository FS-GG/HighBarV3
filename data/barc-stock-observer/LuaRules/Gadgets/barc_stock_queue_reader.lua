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

-- Stock AICallback::CallRules selects LuaRules' synced handle.  The generic
-- RecvSkirmishAIMessage comment does not change that explicit dispatch route.
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
local HEX = "0123456789abcdef"

local function byteBand(a, b)
  local result, place = 0, 1
  for _ = 1, 8 do
    if a % 2 == 1 and b % 2 == 1 then result = result + place end
    a, b, place = math.floor(a / 2), math.floor(b / 2), place * 2
  end
  return result
end

local function byteBxor(a, b)
  local result, place = 0, 1
  for _ = 1, 8 do
    if a % 2 ~= b % 2 then result = result + place end
    a, b, place = math.floor(a / 2), math.floor(b / 2), place * 2
  end
  return result
end

local function wordFromHex(value)
  local word = {}
  for index = 1, 8, 2 do word[#word + 1] = assert(tonumber(value:sub(index, index + 1), 16)) end
  return word
end

local function wordXor(a, b, c)
  local result = {}
  for index = 1, 4 do
    local value = byteBxor(a[index], b[index])
    result[index] = c and byteBxor(value, c[index]) or value
  end
  return result
end

local function wordAnd(a, b)
  return {byteBand(a[1], b[1]), byteBand(a[2], b[2]), byteBand(a[3], b[3]), byteBand(a[4], b[4])}
end

local function wordNot(a)
  return {255 - a[1], 255 - a[2], 255 - a[3], 255 - a[4]}
end

local function wordShiftRight(a, count)
  local result, whole, bits = {}, math.floor(count / 8), count % 8
  for index = 1, 4 do
    local source = index - whole
    local value = source >= 1 and math.floor(a[source] / (2 ^ bits)) or 0
    if bits > 0 and source > 1 then value = value + (a[source - 1] % (2 ^ bits)) * (2 ^ (8 - bits)) end
    result[index] = value
  end
  return result
end

local function wordShiftLeft(a, count)
  local result, whole, bits = {}, math.floor(count / 8), count % 8
  for index = 1, 4 do
    local source = index + whole
    local value = source <= 4 and (a[source] * (2 ^ bits)) % 256 or 0
    if bits > 0 and source < 4 then value = value + math.floor(a[source + 1] / (2 ^ (8 - bits))) end
    result[index] = value
  end
  return result
end

local function wordRotateRight(a, count)
  return wordXor(wordShiftRight(a, count), wordShiftLeft(a, 32 - count))
end

local function wordAdd(...)
  local values, result, carry = {...}, {}, 0
  for index = 4, 1, -1 do
    local total = carry
    for valueIndex = 1, #values do total = total + values[valueIndex][index] end
    result[index], carry = total % 256, math.floor(total / 256)
  end
  return result
end

local function wordHex(value)
  local chars = {}
  for index = 1, 4 do
    local byte = value[index]
    chars[#chars + 1] = HEX:sub(math.floor(byte / 16) + 1, math.floor(byte / 16) + 1)
    chars[#chars + 1] = HEX:sub((byte % 16) + 1, (byte % 16) + 1)
  end
  return table.concat(chars)
end

local SHA_K_HEX = {
  "428a2f98","71374491","b5c0fbcf","e9b5dba5","3956c25b","59f111f1","923f82a4","ab1c5ed5",
  "d807aa98","12835b01","243185be","550c7dc3","72be5d74","80deb1fe","9bdc06a7","c19bf174",
  "e49b69c1","efbe4786","0fc19dc6","240ca1cc","2de92c6f","4a7484aa","5cb0a9dc","76f988da",
  "983e5152","a831c66d","b00327c8","bf597fc7","c6e00bf3","d5a79147","06ca6351","14292967",
  "27b70a85","2e1b2138","4d2c6dfc","53380d13","650a7354","766a0abb","81c2c92e","92722c85",
  "a2bfe8a1","a81a664b","c24b8b70","c76c51a3","d192e819","d6990624","f40e3585","106aa070",
  "19a4c116","1e376c08","2748774c","34b0bcb5","391c0cb3","4ed8aa4a","5b9cca4f","682e6ff3",
  "748f82ee","78a5636f","84c87814","8cc70208","90befffa","a4506ceb","bef9a3f7","c67178f2",
}
local SHA_K = {}
for index = 1, #SHA_K_HEX do SHA_K[index] = wordFromHex(SHA_K_HEX[index]) end

local function sha256(message)
  local bytes = {}
  for index = 1, #message do bytes[index] = message:byte(index) end
  local bitLength = #bytes * 8
  bytes[#bytes + 1] = 0x80
  while (#bytes % 64) ~= 56 do bytes[#bytes + 1] = 0 end
  local lengthBytes = {}
  for index = 8, 1, -1 do
    lengthBytes[index] = bitLength % 256
    bitLength = math.floor(bitLength / 256)
  end
  for index = 1, 8 do bytes[#bytes + 1] = lengthBytes[index] end

  local h = {}
  for index, value in ipairs({"6a09e667","bb67ae85","3c6ef372","a54ff53a","510e527f","9b05688c","1f83d9ab","5be0cd19"}) do
    h[index] = wordFromHex(value)
  end
  for offset = 1, #bytes, 64 do
    local w = {}
    for index = 0, 15 do
      local base = offset + index * 4
      w[index] = {bytes[base], bytes[base + 1], bytes[base + 2], bytes[base + 3]}
    end
    for index = 16, 63 do
      local x, y = w[index - 15], w[index - 2]
      local s0 = wordXor(wordRotateRight(x, 7), wordRotateRight(x, 18), wordShiftRight(x, 3))
      local s1 = wordXor(wordRotateRight(y, 17), wordRotateRight(y, 19), wordShiftRight(y, 10))
      w[index] = wordAdd(w[index - 16], s0, w[index - 7], s1)
    end
    local a,b,c,d,e,f,g,hh = h[1],h[2],h[3],h[4],h[5],h[6],h[7],h[8]
    for index = 0, 63 do
      local s1 = wordXor(wordRotateRight(e, 6), wordRotateRight(e, 11), wordRotateRight(e, 25))
      local choice = wordXor(wordAnd(e, f), wordAnd(wordNot(e), g))
      local t1 = wordAdd(hh, s1, choice, SHA_K[index + 1], w[index])
      local s0 = wordXor(wordRotateRight(a, 2), wordRotateRight(a, 13), wordRotateRight(a, 22))
      local majority = wordXor(wordAnd(a, b), wordAnd(a, c), wordAnd(b, c))
      local t2 = wordAdd(s0, majority)
      hh,g,f,e,d,c,b,a = g,f,e,wordAdd(d, t1),c,b,a,wordAdd(t1, t2)
    end
    h[1],h[2],h[3],h[4] = wordAdd(h[1],a),wordAdd(h[2],b),wordAdd(h[3],c),wordAdd(h[4],d)
    h[5],h[6],h[7],h[8] = wordAdd(h[5],e),wordAdd(h[6],f),wordAdd(h[7],g),wordAdd(h[8],hh)
  end
  local parts = {}
  for index = 1, 8 do parts[index] = wordHex(h[index]) end
  return table.concat(parts)
end

local function packedHex(bytes, littleEndian)
  if type(bytes) ~= "string" or #bytes ~= 4 then return nil end
  local chars = {}
  if littleEndian then
    for index = 4, 1, -1 do chars[#chars + 1] = wordHex({0, 0, 0, bytes:byte(index)}):sub(7, 8) end
  else
    for index = 1, 4 do chars[#chars + 1] = wordHex({0, 0, 0, bytes:byte(index)}):sub(7, 8) end
  end
  return table.concat(chars)
end

local onePacked = VFS.PackF32(1.0)
local oneNativeHex = packedHex(onePacked, false)
local PACK_LITTLE_ENDIAN = oneNativeHex == "0000803f"
local PACK_ENDIAN_VALID = PACK_LITTLE_ENDIAN or oneNativeHex == "3f800000"
local PACK_ZERO_VALID = PACK_ENDIAN_VALID
  and packedHex(VFS.PackF32(0.0), PACK_LITTLE_ENDIAN) == "00000000"
  and packedHex(VFS.PackF32(-1 / math.huge), PACK_LITTLE_ENDIAN) == "80000000"

local function float32Hex(value)
  if type(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then
    return nil, "nonfinite-float"
  end
  if not PACK_ENDIAN_VALID then return nil, "float-pack-unsupported" end
  local encoded = packedHex(VFS.PackF32(value), PACK_LITTLE_ENDIAN)
  if not encoded then return nil, "float-pack-unsupported" end
  if value == 0 and not PACK_ZERO_VALID then return nil, "negative-zero-unsupported" end
  return encoded
end

local function integerInRange(value, minimum, maximum)
  return type(value) == "number" and value == math.floor(value) and value >= minimum and value <= maximum
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
  if not length or tonumber(length) ~= #message or not bridge or not domain or not integerInRange(unit, 0, 2147483647) then return nil end
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
