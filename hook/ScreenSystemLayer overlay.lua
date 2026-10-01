-- PIU Companion: exports live chart + completed result only. No score-history reads.
-- Extend the system overlay that XSanity actually loads; retain its built-in actors.
local capture_results = true -- configured by setup
local publish_chart = true -- configured by setup
local session
local count, active, lastScreen, heartbeat, warned = 0, nil, "", 0, false

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

    local text = '{"current":' .. current
    if completed_result then
        text = text .. ',"result":' .. completed_result .. ',"completed":' .. tostring(completed_at)
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
    if GAMESTATE:GetNumPlayersEnabled() ~= 1 or not GAMESTATE:IsHumanPlayer(player) then
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

local function chart()
    if GAMESTATE:GetNumPlayersEnabled() ~= 1 then
        return nil
    end

    local player = GAMESTATE:GetMasterPlayerNumber()
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

    local level = steps:GetMeter()
    local description = steps:GetDescription() or ""
    local label = (kind == "Single" and "S" or "D") .. tostring(level)
    return {
        player = player,
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

local function tick(delta)
    heartbeat = heartbeat + delta
    local top = SCREENMAN:GetTopScreen()
    local screen = top and top:GetName() or ""
    if screen == "ScreenGameplay" then
        if not active or lastScreen ~= screen then
            active = chart()
            if active then
                count = count + 1
                active.id = session .. "-" .. tostring(count)
            end

            heartbeat = 2
        end

        -- Keep the attempt identity if a settings API fails on its first update.
        lastScreen = screen
        if capture_results and active and active.eligible then
            -- Latch invalid settings: restoring them before results must not undo the skip.
            active.eligible = eligible_play(active.player)
        end
    elseif capture_results and screen == "ScreenEvaluation" and lastScreen == "ScreenGameplay" and active then
        local stats = STATSMAN:GetCurStageStats():GetPlayerStageStats(active.player)
        local eligible = active.eligible
            and eligible_play(active.player)
            and not stats:IsDisqualified()
            and not stats:GetAutoPlay()
        local score, broken = stats:GetPhoenixScore(), stats:GetFailedAux()
        local award = eligible and plate(stats, broken) or nil
        update_mailbox(
            "result",
            '{"id":'
                .. quote(active.id)
                .. ","
                .. fields(active)
                .. ',"score":'
                .. tostring(score)
                .. ',"broken":'
                .. tostring(broken)
                .. ',"eligible":'
                .. tostring(eligible)
                .. (award and (',"plate":' .. quote(award)) or "")
                .. "}"
        )
        active = nil
        heartbeat = 2
    elseif screen ~= "ScreenGameplay" then
        active = nil
        if screen ~= lastScreen then
            heartbeat = 2
        end
    end

    if heartbeat >= 1 then
        update_mailbox(
            "current",
            publish_chart and active and ('{"playing":true,' .. fields(active) .. "}") or '{"playing":false}'
        )
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
                    if active then
                        active.eligible = false
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
