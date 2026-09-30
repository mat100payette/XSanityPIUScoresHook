-- PIU Companion: exports live chart + completed result only. No score-history reads.
-- Install as Themes/xsanity/BGAnimations/ScreenSystemLayer aux.lua after approval.
local root = "Save/PiuCompanion/"
local session = tostring(math.floor(GetTimeSinceStart() * 1000)) .. "-" ..
    tostring(math.random(100000000, 999999999)) .. "-" .. tostring(math.random(100000000, 999999999))
local count, active, lastScreen, heartbeat, warned = 0, nil, "", 0, false

local function quote(value)
    local text = tostring(value or "")
    text = text:gsub('[%z\1-\31\\"]', function(c)
        if c == '\\' then return '\\\\' end
        if c == '"' then return '\\"' end
        return string.format("\\u%04x", string.byte(c))
    end)
    return '"' .. text .. '"'
end

local function write(name, text)
    local file = RageFileUtil.CreateRageFile()
    local opened = file:Open(root .. name, 2)
    if opened then file:PutLine(text); file:Close() end
    file:destroy()
    if not opened then error("Cannot write PIU Companion export") end
end

local function chart()
    if GAMESTATE:GetNumPlayersEnabled() ~= 1 then return nil end
    local player = GAMESTATE:GetMasterPlayerNumber()
    if not GAMESTATE:IsHumanPlayer(player) then return nil end
    local song, steps = GAMESTATE:GetCurrentSong(), GAMESTATE:GetCurrentSteps(player)
    if not song or not steps then return nil end
    local kind = tostring(steps:GetStepsType()):lower():gsub("[^a-z]", "")
    if kind:match("pumpsingle$") then kind = "Single"
    elseif kind:match("pumpdouble$") then kind = "Double"
    else return nil end
    local level = steps:GetMeter()
    local description = steps:GetDescription() or ""
    local label = (kind == "Single" and "S" or "D") .. tostring(level)
    local state = GAMESTATE:GetPlayerState(player)
    local mods = state:GetPlayerOptionsString("ModsLevel_Song")
    local human = tostring(state:GetPlayerController()):lower():match("human$") ~= nil
    local rate = GAMESTATE:GetSongOptionsObject("ModsLevel_Song"):MusicRate()
    return {player=player, title=song:GetDisplayMainTitle(), kind=kind, level=level,
        label=description ~= "" and description or label,
        eligible=human and math.abs(rate - 1) < 0.0001 and
            mods:lower():find("normaljudgement", 1, true) ~= nil and
            (description == "" or description:upper() == label)}
end

local function fields(c)
    return '"title":' .. quote(c.title) .. ',"type":' .. quote(c.kind) ..
        ',"level":' .. tostring(c.level) .. ',"difficulty":' .. quote(c.label)
end

local function tick(delta)
    heartbeat = heartbeat + delta
    local top = SCREENMAN:GetTopScreen()
    local screen = top and top:GetName() or ""
    if screen == "ScreenGameplay" then
        if not active or lastScreen ~= screen then
            active = chart()
            if active then count = count + 1; active.id = session .. "-" .. tostring(count) end
            heartbeat = 2
        end
    elseif screen == "ScreenEvaluation" and lastScreen == "ScreenGameplay" and active then
        local stats = STATSMAN:GetCurStageStats():GetPlayerStageStats(active.player)
        local eligible = active.eligible and not stats:IsDisqualified()
        local score, broken = stats:GetPhoenixScore(), stats:GetFailedAux()
        write("result.json", '{"id":' .. quote(active.id) .. ',' .. fields(active) ..
            ',"score":' .. tostring(score) .. ',"broken":' .. tostring(broken) ..
            ',"eligible":' .. tostring(eligible) .. '}')
        active = nil
        heartbeat = 2
    elseif screen ~= "ScreenGameplay" then
        active = nil
        if screen ~= lastScreen then heartbeat = 2 end
    end
    if heartbeat >= 1 then
        write("current.json", active and ('{"playing":true,' .. fields(active) .. '}') or '{"playing":false}')
        heartbeat = 0
    end
    lastScreen = screen
end

return Def.ActorFrame {
    OnCommand=function(self)
        self:SetUpdateFunction(function(_, delta)
            local ok = pcall(tick, delta)
            if not ok and not warned then
                warned = true
                Warn("PIU Companion export unavailable; game continues normally.")
            end
        end)
    end
}
