-- Drive the unmodified shipped hook through real Lua allocation and garbage collection.
local clock, screen = 100, "ScreenSelectMusic"
local playing = false
local swapped = false
local invalid = false
local function index(pn)
    return pn == "PlayerNumber_P2" and 2 or 1
end
local function person(pn)
    return swapped and 3 - index(pn) or index(pn)
end
GetTimeSinceStart = function()
    return clock
end
Def = {
    ActorFrame = function(actor)
        return actor
    end,
}
LoadActor = function()
    return {}
end
SCREENMAN = {
    GetTopScreen = function()
        return {
            GetName = function()
                return screen
            end,
        }
    end,
    SystemMessage = function() end,
}
PROFILEMAN = {
    IsPersistentProfile = function()
        return true
    end,
    GetProfile = function(_, pn)
        return {
            GetDisplayName = function()
                return person(pn) == 1 and "Alice" or "Bob"
            end,
            GetGUID = function()
                return "profile-" .. person(pn)
            end,
        }
    end,
}
GAMESTATE = {
    GetNumPlayersEnabled = function()
        return playing and 2 or 0
    end,
    IsHumanPlayer = function()
        return playing
    end,
    GetCurrentSong = function()
        return {
            GetDisplayMainTitle = function()
                return "Digitalis"
            end,
        }
    end,
    GetCurrentSteps = function(_, pn)
        return {
            GetStepsType = function()
                return "StepsType_Pump_Single"
            end,
            GetMeter = function()
                return person(pn) == 1 and 14 or 16
            end,
            GetDescription = function()
                return person(pn) == 1 and "S14" or "S16"
            end,
        }
    end,
    GetPlayerState = function(_, pn)
        return {
            GetPlayerController = function()
                return "PlayerController_Human"
            end,
            GetPlayerOptionsString = function()
                return invalid and person(pn) == 1 and "EasyJudgement" or "m550, BgaOff"
            end,
        }
    end,
    GetGameMode = function()
        return "Full"
    end,
    IsCourseMode = function()
        return false
    end,
    GetRandomTrainChannel = function()
        return false
    end,
    GetMusicTrainChannel = function()
        return false
    end,
    GetProgressiveChannel = function()
        return false
    end,
    GetSongOptionsObject = function()
        return {
            MusicRate = function()
                return 1
            end,
        }
    end,
}
STATSMAN = {
    GetCurStageStats = function()
        return {
            GetPlayerStageStats = function(_, pn)
                return {
                    GetPhoenixScore = function()
                        return (person(pn) == 1 and 950001 or 980002) + (swapped and 100 or 0)
                    end,
                    GetFailedAux = function()
                        return false
                    end,
                    GetAutoPlay = function()
                        return 0
                    end,
                    IsDisqualified = function()
                        return false
                    end,
                    GetTapNoteScores = function(_, name)
                        if name == "TapNoteScore_W1" then
                            return 90
                        end
                        if name == "TapNoteScore_Miss" then
                            return person(pn) == 1 and 6 or 10
                        end
                        return 0
                    end,
                }
            end,
        }
    end,
}
local mount = dofile(arg[0]:gsub("[^/\\]+$", "actors.lua"))(_G, function()
    local file = io.open(arg[2] .. "/Save/PiuCompanion/status.txt", "rb")
    if not file then
        return nil
    end
    local text = file:read(513)
    file:close()
    return text
end)
local actor = assert(loadfile(arg[1]))()
function actor:SetUpdateFunction(callback)
    self.update = callback
end
mount(actor)
for command in io.lines() do
    if command == "quit" then
        break
    end
    if command == "play" or command == "swap" or command == "invalid" then
        playing = true
        swapped = command ~= "play"
        invalid = command == "invalid"
        screen = "ScreenGameplay"
    elseif command == "finish" then
        screen = "ScreenEvaluation"
    end
    clock = clock + 1
    actor.update(actor, 1)
    local output = assert(io.open(arg[2] .. "/notifications.txt", "wb"))
    for _, card in ipairs(actor[2]) do
        local text = card:GetChild("Message").values.settext
        output:write(card.values.visible[1] and text and text[1] or "hidden", "\n")
    end
    output:close()
    collectgarbage("collect")
end
