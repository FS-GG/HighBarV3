local requestPath, caseName = arg[1], arg[2]
local file = assert(io.open(requestPath, "rb"))
local request = file:read("*a")
file:close()

gadget = {}
gadgetHandler = { IsSyncedCode = function() return true end }
UnitDefs = { [7] = { isFactory = caseName ~= "not-factory" } }

local function command(index, paramCount)
  local params = {}
  for i = 1, paramCount or 0 do params[i] = 1 end
  return { id = -700 - index, options = { coded = index == 1 and 32 or 65535 }, tag = 40 + index, params = params }
end

local function commands()
  if caseName == "empty" or caseName == "wrong-team" or caseName == "not-factory" then return {} end
  if caseName == "negative-zero" then return {{id=-710,options={coded=32},tag=41,params={1,-0.0}}} end
  if caseName == "nonfinite" then return {{id=-710,options={coded=32},tag=41,params={math.huge}}} end
  if caseName == "overflow65" then local out={} for i=1,65 do out[i]=command(i,0) end return out end
  if caseName == "entries64" then local out={} for i=1,64 do out[i]=command(i,0) end return out end
  if caseName == "params17" then return {command(1,17)} end
  if caseName == "params16" then return {command(1,16)} end
  if caseName == "total257" then local out={} for i=1,16 do out[i]=command(i,16) end out[17]=command(17,1) return out end
  if caseName == "total256" then local out={} for i=1,16 do out[i]=command(i,16) end return out end
  if caseName == "bad-command" then return {{id=-710,options={coded=-1},tag=41,params={}}} end
  if caseName == "nil-commands" then return nil end
  if caseName == "rally" then return {command(1,1)} end
  return {command(1,0)}
end

Spring = {
  ValidUnitID = function(unit) return unit == 42 end,
  GetUnitTeam = function() return caseName == "wrong-team" and 2 or 1 end,
  GetUnitDefID = function() return 7 end,
  GetFactoryCommands = function(_, count)
    assert(caseName ~= "rally", "production getter used for rally")
    assert(count == 65, "missing overflow probe")
    return commands()
  end,
  GetUnitCommands = function(_, count)
    assert(caseName == "rally", "rally getter used for production")
    assert(count == 65, "missing overflow probe")
    return commands()
  end,
}

assert(loadfile("data/barc-stock-observer/LuaRules/Gadgets/barc_stock_queue_reader.lua"))()
local result = gadget:RecvSkirmishAIMessage(1, request)
if result == nil then io.write("__NIL__") else io.write(result) end
