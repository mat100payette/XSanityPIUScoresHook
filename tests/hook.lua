local source = assert(arg[1], "hook path is required")
local file = assert(io.open(source, "rb"))
local source_code = file:read("*a")
file:close()
local passed = 0

local function check(value, message)
    assert(value, message)
    passed = passed + 1
end

local function fixture(options)
    options = options or {}
    local game = { screen = "ScreenSelectMusic", results = 0, messages = {} }
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
        GetPlayerOptionsString = function(_, level)
            if options.mods_error then
                error("options unavailable")
            end

            return (level == "ModsLevel_Current" and options.current_mods)
                or options.mods
                or "NormalJudgement"
        end,
        GetPlayerController = function()
            return options.controller or "PlayerController_Human"
        end,
    }

    local stats = {
        GetPhoenixScore = function()
            return options.score or 998123
        end,
        GetFailedAux = function()
            return options.broken or false
        end,
        GetAutoPlay = function()
            return options.used_autoplay or false
        end,
        GetTapNoteScores = function(_, name)
            if options.missing_counts then
                error("judgements unavailable")
            end

            local counts = options.counts or { W1 = 100, W3 = 3, W4 = 2, W5 = 1 }
            return counts[name:gsub("TapNoteScore_", "")] or 0
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
            assert(
                path == "/Themes/_fallback/BGAnimations/ScreenSystemLayer overlay",
                "retain the built-in system overlay"
            )
            return fallback
        end,
        GetTimeSinceStart = function()
            if options.clock_error then
                error("clock unavailable")
            end

            return game.clock or 100
        end,
        SCREENMAN = {
            SystemMessage = function(_, message)
                game.messages[#game.messages + 1] = message
            end,
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
            GetGameMode = function()
                return options.mode or "Full"
            end,
            IsCourseMode = function()
                return options.course or false
            end,
            GetRandomTrainChannel = function()
                return options.random_train or false
            end,
            GetMusicTrainChannel = function()
                return options.music_train or false
            end,
            GetProgressiveChannel = function()
                return options.progressive or false
            end,
            GetSongOptionsObject = function(_, level)
                return {
                    MusicRate = function()
                        return (level == "ModsLevel_Current" and options.current_rate) or options.rate or 1
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
    }, { __index = _G })
    local code = source_code
    if options.overlay_only then
        code = code:gsub("local capture_results = true", "local capture_results = false")
    end

    if options.sync_only then
        code = code:gsub("local publish_chart = true", "local publish_chart = false")
    end

    -- Expose the actor-owned array to this test only; production needs no global.
    code = code:gsub(
        "local current, completed_result",
        "TEST_MAILBOX = mailbox\nlocal current, completed_result"
    )
    local function decode()
        local box = env.TEST_MAILBOX
        if not box or box[8] == 0 then
            return
        end

        assert(#box == 1024 and box[7] % 2 == 0, "bounded complete mailbox")
        local bytes = {}
        for i = 0, box[8] - 1 do
            bytes[#bytes + 1] = string.char(math.floor(box[10 + math.floor(i / 6)] / 256 ^ (i % 6)) % 256)
        end

        local text = table.concat(bytes)
        local cur = text:match('^{"current":(.-),"result":') or text:match('^{"current":(.*)}$')
        local result = text:match(',"result":(.-),"completed":')
        game.current = cur
        if result and result ~= game.result then
            game.results = game.results + 1
        end

        game.result = result
    end

    local chunk = assert(loadstring(code))
    setfenv(chunk, env)
    local actor = chunk()
    decode()
    assert(actor[1] == fallback, "system overlay remains a child of the returned actor")
    function actor:SetUpdateFunction(update)
        game.update = function(delta)
            update(self, delta)
        end
    end

    -- ScreenSystemLayer loads actors through Init; it does not dispatch the menu's On command.
    assert(type(actor.InitCommand) == "function", "exporter starts when its actor is initialized")
    assert(
        options.clock_error or game.current:find('"initializing":true', 1, true),
        "loading is observable before actor initialization"
    )
    actor.InitCommand(actor)
    decode()
    if not options.clock_error then
        assert(type(game.update) == "function", "initialization registers the live update callback")
    end

    function game:tick(screen, delta)
        self.screen = screen
        self.clock = (self.clock or 100) + (delta or 0.1)
        self.update(delta or 0.1)
        decode()
    end

    return game
end

local game = fixture()
game:tick("ScreenSelectMusic", 1)
check(game.current == '{"playing":false}', "startup produces a heartbeat before playing")
game:tick("ScreenGameplay")
local current = game.current
check(
    current:find('"playing":true', 1, true)
        and current:find('"title":"Digitalis"', 1, true)
        and current:find('"difficulty":"S14"', 1, true),
    "playing Digitalis S14 exports the current chart"
)
game:tick("ScreenEvaluation")
local result = game.result
check(
    result
        and result:find('"score":998123', 1, true)
        and result:find('"eligible":true', 1, true)
        and result:find('"broken":false', 1, true),
    "evaluation captures the exact completed result"
)
check(game.current == '{"playing":false}', "evaluation clears the current playing state")
game:tick("ScreenEvaluation", 2)
check(game.results == 1, "a result is exported once")
game:tick("ScreenGameplay")
game:tick("ScreenEvaluation")
check(game.results == 2 and game.result ~= result, "a second attempt has a distinct event identity")

for _, options in ipairs({ { autoplay = true }, { players = 2 } }) do
    local skipped = fixture(options)
    skipped:tick("ScreenGameplay")
    skipped:tick("ScreenEvaluation")
    check(skipped.result == nil, "autoplay and multiplayer do not export results")
end

for _, options in ipairs({
    { rate = 0.6 },
    { rate = 1.5 },
    { current_rate = 0.8 },
    { disqualified = true },
    { used_autoplay = true },
    { controller = "PlayerController_Autoplay" },
    { mods = "EasyJudgement" },
    { mods = "NormalJudgement, HardJudgement" },
    { current_mods = "VeryHardJudgement" },
    { mods = "ExtraJudgement" },
    { mods = "UltraHardJudgement" },
    { mods = "NormalJudgement, JudgeReverse" },
    { mods = "NormalJudgement, NoHolds" },
    { mods = "NormalJudgement, 50% Little" },
    { mods = "NormalJudgement, NoJumps" },
    { mods = "NormalJudgement, Planted" },
    { course = true },
    { random_train = true },
    { music_train = true },
    { progressive = true },
    { mode = "Quest" },
    { mode = "WorldMax" },
    { mode = "Infinity" },
    { mode = "QuestWorld" },
}) do
    local skipped = fixture(options)
    skipped:tick("ScreenGameplay")
    skipped:tick("ScreenEvaluation")
    check(
        skipped.result:find('"eligible":false', 1, true),
        "unsupported settings and disqualified results remain ineligible"
    )
end

-- Check the plate boundaries and checkpoint judgements against the installed theme.
for _, case in ipairs({
    { "PG", { W1 = 50, W2 = 25, CheckpointHit = 25 } },
    { "UG", { W1 = 90, W3 = 10 } },
    { "EG", { W1 = 85, W3 = 10, W4 = 5 } },
    { "SG", { W1 = 80, W3 = 10, W4 = 5, W5 = 5 } },
    { "MG", { W1 = 99, Miss = 1 } },
    { "MG", { W1 = 95, Miss = 3, CheckpointMiss = 2 } },
    { "TG", { W1 = 94, Miss = 5, CheckpointMiss = 1 } },
    { "TG", { W1 = 90, Miss = 10 } },
    { "FG", { W1 = 89, Miss = 11 } },
    { "FG", { W1 = 80, Miss = 20 } },
    { "RG", { W1 = 79, Miss = 21 } },
    -- Preserve the theme's unusual zero-great/zero-good cases as displayed.
    { "MG", { W1 = 99, W5 = 1 } },
}) do
    local play = fixture({ counts = case[2], score = case[1] == "PG" and 1000000 or 950000 })
    play:tick("ScreenGameplay")
    play:tick("ScreenEvaluation")
    check(play.result:find('"plate":"' .. case[1] .. '"', 1, true), "exact evaluation plate " .. case[1])
end

local broken = fixture({ broken = true })
broken:tick("ScreenGameplay")
broken:tick("ScreenEvaluation")
check(not broken.result:find('"plate"', 1, true), "a failed stage carries no plate")

for _, mods in ipairs({
    "NormalJudgement, 5x",
    "NormalJudgement, m550, Mini, Dark, Hidden",
    "NormalJudgement, Mirror, Reverse",
}) do
    local normal = fixture({ mods = mods })
    normal:tick("ScreenGameplay")
    normal:tick("ScreenEvaluation")
    check(normal.result:find('"eligible":true', 1, true), "display and scroll modifiers remain eligible")
end

for _, change in ipairs({
    { rate = 0.8 },
    { current_mods = "EasyJudgement" },
    { controller = "PlayerController_Autoplay" },
    { mods_error = true },
}) do
    local options = {}
    local changed = fixture(options)
    changed:tick("ScreenGameplay")
    for key, value in pairs(change) do
        options[key] = value
    end

    changed:tick("ScreenGameplay")
    for key in pairs(change) do
        options[key] = nil
    end

    changed:tick("ScreenGameplay", 2)
    changed:tick("ScreenEvaluation")
    check(
        changed.result:find('"eligible":false', 1, true),
        "invalid setting is latched until the attempt ends"
    )
end

local initial_error = { mods_error = true }
local recovered = fixture(initial_error)
recovered:tick("ScreenGameplay")
initial_error.mods_error = false
recovered:tick("ScreenGameplay", 2)
recovered:tick("ScreenEvaluation")
check(
    recovered.result:find('"eligible":false', 1, true),
    "first-update API failure cannot reset the attempt's validity"
)

local at_finish = {}
local finish = fixture(at_finish)
finish:tick("ScreenGameplay")
at_finish.current_rate = 0.9
finish:tick("ScreenEvaluation")
check(finish.result:find('"eligible":false', 1, true), "evaluation checks the final settings too")

local unknown_plate = fixture({ missing_counts = true })
unknown_plate:tick("ScreenGameplay")
unknown_plate:tick("ScreenEvaluation")
check(not unknown_plate.result, "missing result judgements never invent a plate")
check(unknown_plate.current:find('"error":', 1, true), "result API failure reaches companion diagnostics")
unknown_plate:tick("ScreenEvaluation", 2)
check(#unknown_plate.messages == 1, "repeated errors do not flood the game with messages")

local aborted = fixture()
aborted:tick("ScreenGameplay")
aborted:tick("ScreenSelectMusic")
aborted:tick("ScreenEvaluation")
check(aborted.result == nil, "an abandoned attempt cannot become a completed result")

local overlay = fixture({ overlay_only = true, missing_counts = true, mods_error = true })
overlay:tick("ScreenGameplay")
check(overlay.current:find('"playing":true', 1, true), "overlay-only publishes the chart")
overlay:tick("ScreenEvaluation")
check(not overlay.result and #overlay.messages == 0, "overlay-only never accesses result or modifier APIs")

local sync = fixture({ sync_only = true })
sync:tick("ScreenGameplay")
check(sync.current == '{"playing":false}', "sync-only heartbeat omits the current chart")
sync:tick("ScreenEvaluation")
check(sync.result and sync.result:find('"eligible":true', 1, true), "sync-only still captures results")

local clock_failure = fixture({ clock_error = true })
check(
    not clock_failure.update and #clock_failure.messages == 1,
    "clock failure reports once without crashing"
)
game:tick("ScreenSelectMusic", 61)
check(not game.result, "completed payload expires from the transient mailbox")
check(RageFileUtil == nil and lua == nil, "hook runs with no file or logging APIs")
print(passed .. " Lua behavioral assertions passed (fixture invariants also checked). Simulated game only.")
