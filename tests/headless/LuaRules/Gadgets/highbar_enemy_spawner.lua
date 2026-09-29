function gadget:GetInfo()
  return {
    name = "HighBar Enemy Spawner",
    desc = "Spawns passive enemy fixtures for HighBar live coverage",
    author = "HighBar",
    version = "0.1",
    date = "2026-04-23",
    license = "MIT",
    layer = 1001,
    enabled = true,
  }
end

local MESSAGE_PREFIX = "highbar_spawn_enemy:"
local DAMAGE_PREFIX = "highbar_damage_unit:"
local SPEED_PREFIX = "highbar_admin_speed:"
local SPEED_SYNC_ACTION = "HighBarAdminSetSpeed"

if not gadgetHandler:IsSyncedCode() then
  local function handleSetSpeed(_, speed)
    local value = tonumber(speed)
    if not value or value <= 0 then
      return
    end
    local text = string.format("%g", value)
    Spring.SendCommands({
      "setmaxspeed " .. text,
      "setminspeed " .. text,
      "setmaxspeed " .. text,
    })
  end

  function gadget:Initialize()
    gadgetHandler:AddSyncAction(SPEED_SYNC_ACTION, handleSetSpeed)
  end

  function gadget:Shutdown()
    gadgetHandler:RemoveSyncAction(SPEED_SYNC_ACTION)
  end

  return
end

local spawnedByAiTeam = {}
local adminFixtureSeeded = false
local barcLiveFixtureSeeded = false
local barcLiveActor = nil
local barcLiveTarget = nil
local barcLiveTraceCount = 0
local BARC_LIVE_TRACE_LIMIT = 256

local function adminFixtureEnabled()
  local options = Spring.GetModOptions() or {}
  local value = options.highbar_admin_behavior_fixture
  return value == true or value == "1" or value == 1
end

local function barcLiveFixtureEnabled()
  local options = Spring.GetModOptions() or {}
  local value = options.highbar_barc_live_fixture
  return value == true or value == "1" or value == 1
end

local function seedBarcLiveFixture()
  if barcLiveFixtureSeeded or not barcLiveFixtureEnabled() then
    return
  end
  barcLiveFixtureSeeded = true
  local teams = {}
  local gaiaTeam = Spring.GetGaiaTeamID()
  for _, teamID in ipairs(Spring.GetTeamList()) do
    if teamID ~= gaiaTeam then teams[#teams + 1] = teamID end
  end
  if #teams < 2 then return end
  local x, z = 2048, 2048
  local actor = Spring.CreateUnit("armflea", x, Spring.GetGroundHeight(x, z), z, "east", teams[1])
  local targetX = x + 120
  local target = Spring.CreateUnit("corak", targetX, Spring.GetGroundHeight(targetX, z), z, "west", teams[2])
	barcLiveActor = actor
	barcLiveTarget = target
  for _, unitID in ipairs({actor, target}) do
    if unitID then
      Spring.GiveOrderToUnit(unitID, CMD.STOP, {}, {})
      Spring.GiveOrderToUnit(unitID, CMD.FIRE_STATE, {0}, {})
    end
  end
  Spring.Echo(string.format("highbar_barc_live_fixture actor=%s target=%s", tostring(actor), tostring(target)))
end

local function numberOrNil(value)
  return value and string.format("%.3f", value) or "nil"
end

local function commandTrace(unitID)
  if not unitID or not Spring.ValidUnitID(unitID) then return "invalid" end
  local commands = Spring.GetUnitCommands(unitID, 8) or {}
  local encoded = {}
  for index, command in ipairs(commands) do
    local params = {}
    for paramIndex = 1, math.min(#(command.params or {}), 4) do
      params[#params + 1] = numberOrNil(command.params[paramIndex])
    end
    local coded = command.options and command.options.coded or 0
    encoded[#encoded + 1] = string.format("%d:%s:%s:%s",
      index, tostring(command.id), tostring(coded), table.concat(params, ","))
  end
  return string.format("%d[%s]", Spring.GetUnitCommandCount(unitID) or #commands,
    table.concat(encoded, ";"))
end

local function emitBarcLiveTrace(frame)
  if not barcLiveFixtureSeeded or barcLiveTraceCount >= BARC_LIVE_TRACE_LIMIT then return end
  if frame % 15 ~= 0 then return end
  barcLiveTraceCount = barcLiveTraceCount + 1
  if not barcLiveActor or not barcLiveTarget
      or not Spring.ValidUnitID(barcLiveActor) or not Spring.ValidUnitID(barcLiveTarget) then
    Spring.Echo(string.format(
      "highbar_barc_live_trace frame=%d actor=%s target=%s status=fixture_invalid",
      frame, tostring(barcLiveActor), tostring(barcLiveTarget)))
    return
  end
  local ax, ay, az = Spring.GetUnitPosition(barcLiveActor)
  local tx, ty, tz = Spring.GetUnitPosition(barcLiveTarget)
  local ah = Spring.GetUnitHealth(barcLiveActor)
  local th = Spring.GetUnitHealth(barcLiveTarget)
  Spring.Echo(string.format(
    "highbar_barc_live_trace frame=%d actor=%s actor_pos=%s,%s,%s actor_health=%s actor_commands=%s target=%s target_pos=%s,%s,%s target_health=%s target_commands=%s",
    frame, tostring(barcLiveActor), numberOrNil(ax), numberOrNil(ay), numberOrNil(az),
    numberOrNil(ah), commandTrace(barcLiveActor), tostring(barcLiveTarget),
    numberOrNil(tx), numberOrNil(ty), numberOrNil(tz), numberOrNil(th), commandTrace(barcLiveTarget)))
end

local function teamStartPosition(teamID, fallbackIndex)
  local x, y, z = Spring.GetTeamStartPosition(teamID)
  if x and z and x >= 0 and z >= 0 then
    return x, y or Spring.GetGroundHeight(x, z), z
  end
  x = fallbackIndex == 0 and 1536 or 4608
  z = 4096
  return x, Spring.GetGroundHeight(x, z), z
end

local function seedAdminBehaviorFixture()
  if adminFixtureSeeded or not adminFixtureEnabled() then
    return
  end
  adminFixtureSeeded = true

  local gaiaTeam = Spring.GetGaiaTeamID()
  for index, teamID in ipairs(Spring.GetTeamList()) do
    if teamID ~= gaiaTeam then
      local _, _, _, isDead = Spring.GetTeamInfo(teamID, false)
      if not isDead and #Spring.GetTeamUnits(teamID) == 0 then
        local unitName = teamID == 0 and "armcom" or "corcom"
        local x, y, z = teamStartPosition(teamID, index - 1)
        local unitID = Spring.CreateUnit(unitName, x, y, z, "south", teamID)
        if unitID then
          Spring.GiveOrderToUnit(unitID, CMD.STOP, {}, {})
        end
      end
    end
  end
end

local function splitFields(payload)
  local fields = {}
  for field in payload:gmatch("([^:]+)") do
    fields[#fields + 1] = field
  end
  return fields
end

local function firstEnemyTeam(aiTeam)
  local senderAllyTeam = select(6, Spring.GetTeamInfo(aiTeam, false))
  if senderAllyTeam == nil then
    return nil
  end
  local gaiaTeam = Spring.GetGaiaTeamID()
  for _, teamID in ipairs(Spring.GetTeamList()) do
    if teamID ~= gaiaTeam then
      local _, _, _, isDead, _, allyTeam = Spring.GetTeamInfo(teamID, false)
      if not isDead and allyTeam ~= senderAllyTeam then
        return teamID
      end
    end
  end
  return nil
end

local function destroyTrackedUnit(aiTeam)
  local unitID = spawnedByAiTeam[aiTeam]
  if unitID and Spring.ValidUnitID(unitID) then
    Spring.DestroyUnit(unitID, false, true)
  end
  spawnedByAiTeam[aiTeam] = nil
end

function gadget:RecvSkirmishAIMessage(aiTeam, dataStr)
  if dataStr:sub(1, #SPEED_PREFIX) == SPEED_PREFIX then
    local speed = tonumber(dataStr:sub(#SPEED_PREFIX + 1))
    if not speed or speed <= 0 then
      return "error:invalid_speed"
    end
    SendToUnsynced(SPEED_SYNC_ACTION, speed)
    return "ok"
  end

  if dataStr:sub(1, #DAMAGE_PREFIX) == DAMAGE_PREFIX then
    local fields = splitFields(dataStr:sub(#DAMAGE_PREFIX + 1))
    local unitID = tonumber(fields[1] or "")
    local damage = tonumber(fields[2] or "") or 25
    if not unitID or not Spring.ValidUnitID(unitID) then
      return "error:invalid_unit"
    end
    if Spring.GetUnitTeam(unitID) ~= aiTeam then
      return "error:not_owned"
    end
    local health, maxHealth = Spring.GetUnitHealth(unitID)
    if not health or not maxHealth then
      return "error:health_unavailable"
    end
    if health <= 1 then
      return "error:unit_already_critical"
    end
    local appliedDamage = math.max(1, math.min(damage, health - 1))
    local newHealth = math.max(1, health - appliedDamage)
    Spring.SetUnitHealth(unitID, newHealth)
    return string.format("%.1f", newHealth)
  end

  if dataStr:sub(1, #MESSAGE_PREFIX) ~= MESSAGE_PREFIX then
    return nil
  end

  local fields = splitFields(dataStr:sub(#MESSAGE_PREFIX + 1))
  local unitName = fields[1]
  local x = tonumber(fields[2] or "")
  local z = tonumber(fields[3] or "")
  local facing = fields[4] or "west"
  if not unitName or not x or not z then
    return "error:invalid_args"
  end

  local enemyTeam = firstEnemyTeam(aiTeam)
  if enemyTeam == nil then
    return "error:no_enemy_team"
  end

  destroyTrackedUnit(aiTeam)

  local y = Spring.GetGroundHeight(x, z)
  local unitID = Spring.CreateUnit(unitName, x, y, z, facing, enemyTeam)
  if not unitID then
    return "error:create_failed"
  end

  Spring.GiveOrderToUnit(unitID, CMD.STOP, {}, {})
  spawnedByAiTeam[aiTeam] = unitID
  return tostring(unitID)
end

function gadget:GameStart()
  seedAdminBehaviorFixture()
  seedBarcLiveFixture()
end

function gadget:GameFrame(frame)
  if frame == 0 then
    seedAdminBehaviorFixture()
  end
  if frame == 1 then
    seedBarcLiveFixture()
  end
  emitBarcLiveTrace(frame)
end

function gadget:UnitDestroyed(unitID)
  for aiTeam, trackedUnitID in pairs(spawnedByAiTeam) do
    if trackedUnitID == unitID then
      spawnedByAiTeam[aiTeam] = nil
    end
  end
end
