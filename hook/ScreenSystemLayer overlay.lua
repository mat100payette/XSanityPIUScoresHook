-- PIU Companion: exports live chart + completed result only. No score-history reads.
-- Extend the system overlay that XSanity actually loads; retain its built-in actors.
local capture_results = true -- configured by setup
local publish_chart = true -- configured by setup
local session
local count, active, lastScreen, heartbeat, warned = 0, {}, "", 0, false
local players = { "PlayerNumber_P1", "PlayerNumber_P2" }
local profiles = "[]"
local tick_error

local function quote(value)
    local text = tostring(value or "")
    text = text:gsub('[%z\1-\31\\"]', function(c)
        if c == "\\" then
            return "\\\\"
        end

        if c == '"' then
            return '\\"'
        end

        return string.format("\\u%04x", string.byte(c))
    end)
    return '"' .. text .. '"'
end

-- Fixed numeric Lua array, pinned by the actor's update closure. The companion
-- reads only this mailbox; no game addresses, files, settings, or history are used.
-- Protocol and Lua numeric-slot layout are documented in docs/bridge.md.
local mailbox = {}
for i = 1, 1024 do
    mailbox[i] = 0
end
mailbox[1], mailbox[2], mailbox[3], mailbox[4] = 204081632653, 918273645546, 672345891234, 135792468013
mailbox[5], mailbox[6] = 1, 1024
local current, completed_result, completed_at = '{"playing":false}', nil, 0

local function publish()
    local clock = GetTimeSinceStart()
    if completed_result and clock - completed_at > 60 then
        completed_result = nil
    end

    local text = '{"current":' .. current .. ',"players":' .. profiles
    if completed_result then
        text = text .. ',"results":' .. completed_result .. ',"completed":' .. tostring(completed_at)
    end
    text = text .. "}"
    if #text > 6090 then
        error("Export exceeds the mailbox size limit")
    end

    -- Odd means a write is in progress; publish the even sequence last.
    mailbox[7] = mailbox[7] + 1
    mailbox[8], mailbox[9] = #text, clock
    for i = 1, #text, 6 do
        local value, power = 0, 1
        for j = i, math.min(i + 5, #text) do
            value = value + text:byte(j) * power
            power = power * 256
        end
        mailbox[10 + math.floor((i - 1) / 6)] = value
    end
    for i = 10 + math.ceil(#text / 6), 1024 do
        mailbox[i] = 0
    end
    mailbox[7] = mailbox[7] + 1
end

local function update_mailbox(kind, text)
    if kind == "result" then
        completed_result, completed_at = text, GetTimeSinceStart()
    else
        current = text
    end
    publish()
end

local function report_error(detail)
    tick_error = detail
    current = '{"playing":false,"error":' .. quote(detail) .. "}"
    pcall(publish)
    if not warned then
        warned = true
        pcall(function()
            SCREENMAN:SystemMessage("PIU Companion: " .. tostring(detail))
        end)
    end
end

-- These change the judged notes, rather than how the chart is displayed.
local changed_notes = {
    little = true,
    noholds = true,
    norolls = true,
    nomines = true,
    nojumps = true,
    nohands = true,
    noquads = true,
    nostretch = true,
    nolifts = true,
    nofakes = true,
    planted = true,
    floored = true,
    twister = true,
    holdstorolls = true,
    echo = true,
    stomp = true,
    big = true,
    quick = true,
    skippy = true,
    bmrize = true,
    wide = true,
}

local function normal_modifiers(mods)
    local normal = false
    for token in mods:lower():gmatch("[^,]+") do
        local name = token:match("([a-z]+)%s*$")
        if name == "normaljudgement" then
            normal = true
        elseif
            name and (changed_notes[name] or name:find("judgement", 1, true) or name == "judgereverse")
        then
            return false
        end
    end
    return normal
end

local function eligible_play(player)
    if not GAMESTATE:IsHumanPlayer(player) then
        return false
    end

    local mode = GAMESTATE:GetGameMode()
    if
        (mode ~= "Full" and mode ~= "Basic" and mode ~= "Rank")
        or GAMESTATE:IsCourseMode()
        or GAMESTATE:GetRandomTrainChannel()
        or GAMESTATE:GetMusicTrainChannel()
        or GAMESTATE:GetProgressiveChannel()
    then
        return false
    end

    local state = GAMESTATE:GetPlayerState(player)
    if not tostring(state:GetPlayerController()):lower():match("human$") then
        return false
    end

    for _, level in ipairs({ "ModsLevel_Song", "ModsLevel_Current" }) do
        if
            GAMESTATE:GetSongOptionsObject(level):MusicRate() ~= 1
            or not normal_modifiers(state:GetPlayerOptionsString(level))
        then
            return false
        end
    end
    return true
end

-- Match xsanity's ScreenEvaluation plate rules, including hold checkpoints.
-- PIU Scores calls the submitted plate "award"; failed stages have none.
local function plate(stats, broken)
    if broken then
        return nil
    end

    local perfect = stats:GetTapNoteScores("TapNoteScore_W1")
        + stats:GetTapNoteScores("TapNoteScore_W2")
        + stats:GetTapNoteScores("TapNoteScore_CheckpointHit")
    local great = stats:GetTapNoteScores("TapNoteScore_W3")
    local good = stats:GetTapNoteScores("TapNoteScore_W4")
    local bad = stats:GetTapNoteScores("TapNoteScore_W5")
    local miss = stats:GetTapNoteScores("TapNoteScore_Miss")
        + stats:GetTapNoteScores("TapNoteScore_CheckpointMiss")
    if miss == 0 and perfect > 0 then
        if great == 0 and good == 0 and bad == 0 then
            return "PG"
        elseif great > 0 and good == 0 and bad == 0 then
            return "UG"
        elseif great > 0 and good > 0 and bad == 0 then
            return "EG"
        elseif great > 0 and good > 0 and bad > 0 then
            return "SG"
        end
    end

    if miss <= 5 then
        return "MG"
    elseif miss <= 10 then
        return "TG"
    elseif miss <= 20 then
        return "FG"
    end
    return "RG"
end

local function profile(player)
    if not PROFILEMAN:IsPersistentProfile(player) then
        return "", ""
    end
    local value = PROFILEMAN:GetProfile(player)
    return value:GetDisplayName() or "", value:GetGUID() or ""
end

local function chart(player, side)
    if not GAMESTATE:IsHumanPlayer(player) then
        return nil
    end

    local song, steps = GAMESTATE:GetCurrentSong(), GAMESTATE:GetCurrentSteps(player)
    if not song or not steps then
        return nil
    end

    local kind = tostring(steps:GetStepsType()):lower():gsub("[^a-z]", "")
    if kind:match("pumpsingle$") then
        kind = "Single"
    elseif kind:match("pumpdouble$") then
        kind = "Double"
    else
        return nil
    end

    if GAMESTATE:GetNumPlayersEnabled() > 1 and kind ~= "Single" then
        return nil
    end

    local name, guid = "", ""
    if capture_results then
        name, guid = profile(player)
    end
    local level = steps:GetMeter()
    local description = steps:GetDescription() or ""
    local label = (kind == "Single" and "S" or "D") .. tostring(level)
    return {
        player = player,
        side = side,
        profile = name,
        guid = guid,
        title = song:GetDisplayMainTitle(),
        kind = kind,
        level = level,
        label = description ~= "" and description or label,
        eligible = (description == "" or description:upper() == label),
    }
end

local function fields(c)
    return '"title":'
        .. quote(c.title)
        .. ',"type":'
        .. quote(c.kind)
        .. ',"level":'
        .. tostring(c.level)
        .. ',"difficulty":'
        .. quote(c.label)
end

local function capture(c)
    local stats = STATSMAN:GetCurStageStats():GetPlayerStageStats(c.player)
    local name, guid = profile(c.player)
    local eligible = c.eligible
        and name == c.profile
        and guid == c.guid
        and eligible_play(c.player)
        and not stats:IsDisqualified()
        and not stats:GetAutoPlay()
    local score, broken = stats:GetPhoenixScore(), stats:GetFailedAux()
    local award = eligible and plate(stats, broken) or nil
    return '{"id":'
        .. quote(c.id)
        .. ","
        .. fields(c)
        .. ',"profile":'
        .. quote(c.profile)
        .. ',"side":'
        .. tostring(c.side)
        .. ',"ambiguous":'
        .. tostring(c.ambiguous or false)
        .. ',"score":'
        .. tostring(score)
        .. ',"broken":'
        .. tostring(broken)
        .. ',"eligible":'
        .. tostring(eligible)
        .. (award and (',"plate":' .. quote(award)) or "")
        .. "}"
end

local function update_profiles()
    local list = {}
    if capture_results then
        for side, player in ipairs(players) do
            if GAMESTATE:IsHumanPlayer(player) then
                local ok, name = pcall(profile, player)
                list[#list + 1] = '{"side":'
                    .. tostring(side)
                    .. ',"profile":'
                    .. quote(ok and name or "")
                    .. "}"
            end
        end
    end
    profiles = "[" .. table.concat(list, ",") .. "]"
end

local function tick(delta)
    tick_error = nil
    heartbeat = heartbeat + delta
    local top = SCREENMAN:GetTopScreen()
    local screen = top and top:GetName() or ""
    if screen == "ScreenGameplay" then
        if lastScreen ~= screen then
            active = {}
            count = count + 1
            for side, player in ipairs(players) do
                local ok, value = pcall(chart, player, side)
                if ok and value then
                    value.id = session .. "-" .. tostring(count) .. "-P" .. tostring(side)
                    active[side] = value
                elseif not ok then
                    report_error("P" .. tostring(side) .. ": " .. tostring(value))
                end
            end
            if active[1] and active[2] then
                local duplicate = active[1].profile == active[2].profile
                    or (active[1].guid ~= "" and active[1].guid == active[2].guid)
                active[1].ambiguous, active[2].ambiguous = duplicate, duplicate
            end
            heartbeat = 2
        end

        -- Keep each attempt's identity even if its first settings query fails.
        lastScreen = screen
        if capture_results then
            for side, c in pairs(active) do
                if c.eligible then
                    local ok, eligible = pcall(function()
                        local name, guid = profile(c.player)
                        return name == c.profile and guid == c.guid and eligible_play(c.player)
                    end)
                    c.eligible = ok and eligible
                    if not ok then
                        report_error("P" .. tostring(side) .. ": " .. tostring(eligible))
                    end
                end
            end
        end
    elseif capture_results and screen == "ScreenEvaluation" and lastScreen == "ScreenGameplay" then
        local results = {}
        for side = 1, 2 do
            if active[side] then
                local ok, value = pcall(capture, active[side])
                if ok then
                    results[#results + 1] = value
                else
                    report_error("P" .. tostring(side) .. ": " .. tostring(value))
                end
            end
        end
        if #results > 0 then
            update_mailbox("result", "[" .. table.concat(results, ",") .. "]")
        end
        active = {}
        heartbeat = 2
    elseif screen ~= "ScreenGameplay" then
        active = {}
        if screen ~= lastScreen then
            heartbeat = 2
        end
    end

    if heartbeat >= 1 then
        update_profiles()
        -- Preserve the existing single-player OBS contract. Multiplayer overlay is not supported.
        local c = GAMESTATE:GetNumPlayersEnabled() == 1 and (active[1] or active[2]) or nil
        if tick_error then
            publish()
        else
            update_mailbox(
                "current",
                publish_chart and c and ('{"playing":true,' .. fields(c) .. "}") or '{"playing":false}'
            )
        end
        heartbeat = 0
    end
    lastScreen = screen
end

-- Publish loading separately from initialization so a stopped actor is diagnosable.
local loaded, load_error = pcall(update_mailbox, "current", '{"playing":false,"initializing":true}')
if not loaded then
    report_error(load_error)
end

local fallback_ok, fallback = pcall(LoadActor, "/Themes/_fallback/BGAnimations/ScreenSystemLayer overlay")
if not fallback_ok then
    report_error(fallback)
    error(fallback)
end

return Def.ActorFrame({
    fallback,
    InitCommand = function(self)
        local ok, detail = pcall(function()
            session = tostring(math.floor(GetTimeSinceStart() * 1000))
                .. "-"
                .. tostring(math.random(100000000, 999999999))
                .. "-"
                .. tostring(math.random(100000000, 999999999))
            local retry = 0
            self:SetUpdateFunction(function(_, delta)
                retry = retry - delta
                if retry > 0 then
                    return
                end

                local updated, update_error = pcall(tick, delta)
                if not updated then
                    for _, c in pairs(active) do
                        c.eligible = false
                    end

                    retry = 1
                    report_error(update_error)
                end
            end)
            tick(1)
        end)
        if not ok then
            report_error(detail)
        end
    end,
})
