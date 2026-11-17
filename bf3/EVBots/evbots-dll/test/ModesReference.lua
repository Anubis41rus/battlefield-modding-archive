-- Читаем настоящий GameDirector и Registry, не открывая mod.db и не запуская сервер.
local root = assert(arg[1], 'fun-bots root required')
local function readModule(relative) return assert(loadfile(root .. '/' .. relative))() end
local passed = 0
local function check(ok, name)
  assert(ok, 'FAIL ' .. name)
  passed = passed + 1
  print('PASS ' .. name)
end

function class()
  local c = {}
  c.__index = c
  return setmetatable(c, {__call = function(t, ...)
    local o = setmetatable({}, t)
    if o.__init then o:__init(...) end
    return o
  end})
end
function string:split(separator)
  local out = {}
  for value in self:gmatch('[^' .. separator .. ']+') do out[#out + 1] = value end
  return out
end
local vector = {}
vector.__index = vector
function vector:Distance(other)
  local x, y, z = self.x - other.x, self.y - other.y, self.z - other.z
  return math.sqrt(x*x + y*y + z*z)
end
function vector:Clone() return setmetatable({x=self.x,y=self.y,z=self.z}, vector) end
local function pos(x, z) return setmetatable({x=x,y=10,z=z or 0}, vector) end
Debug = {Server = {GAMEDIRECTOR = false}}
InputDeviceKeys = {IDK_LeftAlt = 0}  -- горячая клавиша в этих сценариях не используется
function Logger() return {Write=function() end} end
TeamId = {TeamNeutral=0, Team1=1, Team2=2}
Globals = {NrOfTeams=2}
CapturePointEntity = function(value) return value end
MathUtils = {GetRandomInt=function(_, first) return first end}
local nodes = {paths={}, known={}}
function nodes:GetKnownObjectives() return self.known end
function nodes:GetPaths() return self.paths end
function nodes:Get(point, path)
  if point == nil then return self.paths[path] end
  return self.paths[path] and self.paths[path][point]
end
local probability = false
local utilities = {
  CheckProbability=function(_, value) return probability and value > 0 end,
  isBot=function() return false end
}
local originalRequire = require
require = function(name)
  if name == 'NodeCollection' then return nodes end
  if name == '__shared/Utilities' then return utilities end
  if name == 'Vehicles' then return {} end
  if name == '__shared/Constants/VersionType' then return readModule('ext/Shared/Constants/VersionType.lua') end
  error('unexpected require: ' .. name)
end
readModule('ext/Shared/Registry/Registry.lua')
local director = readModule('ext/Server/GameDirector.lua')
require = originalRequire
local mates = {}
PlayerManager = {
  GetPlayersBySquad=function() return mates end,
  GetPlayersByTeam=function() return mates end
}
Utilities = {DistanceFast=function(_, a, b) local d=a:Distance(b) return d*d end}

local function setup(rush, squad)
  director:RegisterVars()
  director.m_UpdateTimer = 0
  director.m_AvailableVehicles = {[1]={}, [2]={}}
  Globals.IsRush, Globals.IsConquest, Globals.IsSquadRush = rush, not rush, squad or false
  nodes.paths, nodes.known, mates, probability = {}, {}, {}, false
end
local function objective(name, team, base, x)
  local index = #nodes.paths + 1
  local p = pos(x)
  nodes.paths[index] = {{Position=p, PathIndex=index, PointIndex=1, Data={Objectives={name}}}}
  nodes.known[name] = {index}
  local o = {name=name,team=team,isBase=base,active=true,isAttacked=false,
    canBeCaptured=not base,destroyed=false,subObjective=false,assigned={},position=p}
  director.m_AllObjectives[#director.m_AllObjectives + 1] = o
  return o, index
end

setup(false)
objective('base US', 1, true, 0)
objective('base RU', 2, true, 500)
local a, aPath = objective('A', 1, false, 100)
objective('B', 2, false, 400)
check(director:GetSpawnPath(1, 1, false) == aPath, 'Conquest captured flag is a valid spawn')
local cp = {name='ID_H_US_A',team=2,isAttacked=false,flagLocation=100,isControlled=true,
  transform={trans=a.position}}
director:OnCapturePointCaptured(cp)
check(a.team == 2, 'Conquest ownership translated by position')
cp.isControlled, cp.flagLocation = false, 0
director:OnCapturePointLost(cp)
check(a.team == 0, 'Conquest loss becomes neutral')

local function rushObjectives(squad)
  setup(true, squad)
  local _, us1 = objective('base US 1', 1, true, 0)
  local _, ru1 = objective('base RU 1', 2, true, 300)
  local ma = objective('MCOM 1', 0, false, 100)
  local mb, mbPath = objective('MCOM 2', 0, false, 120)
  local _, us2 = objective('base US 2', 1, true, 350)
  local _, ru2 = objective('base RU 2', 2, true, 600)
  local mc = objective('MCOM 3', 0, false, 450)
  local md = objective('MCOM 4', 0, false, 470)
  director:_UpdateValidObjectives()
  return ma, mb, mc, md, us1, ru1, us2, ru2, mbPath
end

local ma, mb, mc, md, us1, ru1, us2, ru2, mbPath = rushObjectives(false)
check(director:GetSpawnPath(1, 1, true) == us1, 'Rush first attacker spawn is sector base')
check(director:GetSpawnPath(2, 1, true) == ru1, 'Rush first defender spawn is sector base, not A or B')
check(ma.active and mb.active and not mc.active and not md.active, 'Rush starts with sector one MCOMs')
local player = {soldier={worldTransform={trans=ma.position}}}
director:OnMcomArmed(player)
check(ma.isAttacked and ma.team == 1 and director.m_ArmedMcoms['MCOM 1'] ~= nil, 'Rush MCOM armed')
director:OnMcomDisarmed(player)
check(not ma.isAttacked and ma.team == 0 and director.m_ArmedMcoms['MCOM 1'] == nil, 'Rush MCOM disarmed')
director:OnMcomArmed(player)
director:_UpdateTimersOfMcoms(Registry.GAME_DIRECTOR.MCOMS_CHECK_CYCLE)
check(ma.destroyed and not mb.destroyed and mb.active and director.m_RushStageCounter == 1,
  'Rush A destroyed leaves B active without advancing sector')
director:OnLifeCounterBaseDestoyed({}, false)
check(director.m_RushStageCounter == 2 and not mb.active and mc.active and md.active,
  'Rush accepted base event advances exactly one sector')
check(director:GetSpawnPath(1, 1, true) == us2 and director:GetSpawnPath(2, 1, true) == ru2,
  'Rush both teams spawn at their new sector bases')

ma, mb, mc, md, us1, ru1, us2, ru2, mbPath = rushObjectives(false)
mates = {{name='Human',soldier={worldTransform={trans=mb.position}},isAllowedToSpawnOn=true}}
probability = true
check(director:GetSpawnPath(2, 1, true) == mbPath, 'Rush explicitly allowed mate spawn is distinct from initial base spawn')

ma, mb = rushObjectives(true)
check(ma.active and not mb.active, 'SquadRush starts with one MCOM')
director:OnLifeCounterBaseDestoyed({}, false)
check(not ma.active and mb.active, 'SquadRush base event activates the next MCOM')
print('TOTAL ' .. passed .. ' PASS, 0 FAIL (actual GameDirector, no mod.db or game)')
