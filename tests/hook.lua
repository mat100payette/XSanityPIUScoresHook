local source = assert(arg[1], "hook path is required")
local passed = 0

local function check(value, message)
    assert(value, message)
    passed = passed + 1
end

local function fixture(options)
    options = options or {}
    local game = { screen = "ScreenSelectMusic", files = {}, writes = {}, warnings = {} }
    local song = {
        GetDisplayMainTitle = function()
            return "Digitalis"
        end,
    }
    local steps = {
        GetStepsType = function()
            return "StepsType_Pump_Single"
        end,
        GetMeter = function()
            return 14
        end,
        GetDescription = function()
            return "S14"
        end,
    }
    local player = {
        GetPlayerOptionsString = function()
            return "NormalJudgement"
        end,
        GetPlayerController = function()
            return "PlayerController_Human"
        end,
    }
    local stats = {
        GetPhoenixScore = function()
            return 998123
        end,
        GetFailedAux = function()
            return false
        end,
        IsDisqualified = function()
            return options.disqualified or false
        end,
    }
    local fallback = {}
    local env = setmetatable({
        Def = {
            ActorFrame = function(value)
                return value
            end,
        },
        LoadActor = function(path)
            check(
                path == "/Themes/_fallback/BGAnimations/ScreenSystemLayer overlay",
                "retain the built-in system overlay"
            )
            return fallback
        end,
        GetTimeSinceStart = function()
            return 100
        end,
        Warn = function(message)
            game.warnings[#game.warnings + 1] = message
        end,
        SCREENMAN = {
            GetTopScreen = function()
                return {
                    GetName = function()
                        return game.screen
                    end,
                }
            end,
        },
        GAMESTATE = {
            GetNumPlayersEnabled = function()
                return options.players or 1
            end,
            GetMasterPlayerNumber = function()
                return "PlayerNumber_P1"
            end,
            IsHumanPlayer = function()
                return not options.autoplay
            end,
            GetCurrentSong = function()
                return song
            end,
            GetCurrentSteps = function()
                return steps
            end,
            GetPlayerState = function()
                return player
            end,
            GetSongOptionsObject = function()
                return {
                    MusicRate = function()
                        return options.rate or 1
                    end,
                }
            end,
        },
        STATSMAN = {
            GetCurStageStats = function()
                return {
                    GetPlayerStageStats = function()
                        return stats
                    end,
                }
            end,
        },
        RageFileUtil = {
            CreateRageFile = function()
                local file = {}
                function file:Open(path, mode)
                    check(mode == 2, "exporter only opens its output files for writing")
                    self.path = path
                    return not game.fail_writes
                end
                function file:PutLine(text)
                    game.files[self.path] = text
                    game.writes[self.path] = (game.writes[self.path] or 0) + 1
                end
                function file:Close() end
                function file:destroy() end
                return file
            end,
        },
    }, { __index = _G })
    local chunk = assert(loadfile(source))
    setfenv(chunk, env)
    local actor = chunk()
    check(actor[1] == fallback, "system overlay remains a child of the returned actor")
    function actor:SetUpdateFunction(update)
        game.update = function(delta)
            update(self, delta)
        end
    end
    -- ScreenSystemLayer loads actors through Init; it does not dispatch the menu's On command.
    check(type(actor.InitCommand) == "function", "exporter starts when its actor is initialized")
    actor.InitCommand(actor)
    check(type(game.update) == "function", "initialization registers the live update callback")
    function game:tick(screen, delta)
        self.screen = screen
        self.update(delta or 0.1)
    end
    return game
end

local result_path = "Save/PiuCompanion/result.json"
local current_path = "Save/PiuCompanion/current.json"
local game = fixture()
game:tick("ScreenSelectMusic", 1)
check(game.files[current_path] == '{"playing":false}', "startup produces a heartbeat before playing")
game:tick("ScreenGameplay")
local current = game.files[current_path]
check(
    current:find('"playing":true', 1, true)
        and current:find('"title":"Digitalis"', 1, true)
        and current:find('"difficulty":"S14"', 1, true),
    "playing Digitalis S14 exports the current chart"
)
game:tick("ScreenEvaluation")
local result = game.files[result_path]
check(
    result
        and result:find('"score":998123', 1, true)
        and result:find('"eligible":true', 1, true)
        and result:find('"broken":false', 1, true),
    "evaluation captures the exact completed result"
)
check(game.files[current_path] == '{"playing":false}', "evaluation clears the current playing state")
game:tick("ScreenEvaluation", 2)
check(game.writes[result_path] == 1, "a result is exported once")
game:tick("ScreenGameplay")
game:tick("ScreenEvaluation")
check(
    game.writes[result_path] == 2 and game.files[result_path] ~= result,
    "a second attempt has a distinct event identity"
)

for _, options in ipairs({ { autoplay = true }, { players = 2 } }) do
    local skipped = fixture(options)
    skipped:tick("ScreenGameplay")
    skipped:tick("ScreenEvaluation")
    check(skipped.files[result_path] == nil, "autoplay and multiplayer do not export results")
end

for _, options in ipairs({ { rate = 1.5 }, { disqualified = true } }) do
    local skipped = fixture(options)
    skipped:tick("ScreenGameplay")
    skipped:tick("ScreenEvaluation")
    check(
        skipped.files[result_path]:find('"eligible":false', 1, true),
        "modified-rate and disqualified results remain ineligible"
    )
end

local aborted = fixture()
aborted:tick("ScreenGameplay")
aborted:tick("ScreenSelectMusic")
aborted:tick("ScreenEvaluation")
check(aborted.files[result_path] == nil, "an abandoned attempt cannot become a completed result")

local recovery = fixture()
recovery.fail_writes = true
recovery:tick("ScreenSelectMusic", 1)
recovery:tick("ScreenSelectMusic", 1)
check(#recovery.warnings == 1, "export errors do not stop gameplay or repeat warnings every frame")
recovery.fail_writes = false
recovery:tick("ScreenSelectMusic", 1)
check(recovery.files[current_path] == '{"playing":false}', "heartbeat recovers after a write failure")

print(passed .. " Lua exporter checks passed. Simulated game only.")
