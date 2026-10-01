local source = assert(arg[1], "hook path is required")
local file = assert(io.open(source, "rb"))
local source_code = file:read("*a")
file:close()
local passed = 0
local actor_fixture = dofile(arg[0]:gsub("[^/\\]+$", "actors.lua"))

local function check(value, message)
    assert(value, message)
    passed = passed + 1
end

local function fixture(options)
    options = options or {}
    local game = { screen = "ScreenSelectMusic", results = 0, messages = {} }
    options.p2 = options.p2 or {}
    local function side(pn)
        return pn == "PlayerNumber_P2" and 2 or 1
    end
    local function opts(pn)
        return side(pn) == 2 and options.p2 or options
    end
    local function make_player(options)
        local song = {
            GetDisplayMainTitle = function()
                return "Digitalis"
            end,
        }

        local steps = {
            GetStepsType = function()
                return options.kind or "StepsType_Pump_Single"
            end,
            GetMeter = function()
                return options.level or 14
            end,
            GetDescription = function()
                return options.description or ("S" .. tostring(options.level or 14))
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

        return { song = song, steps = steps, player = player, stats = stats }
    end
    local people = { make_player(options), make_player(options.p2) }
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
            IsHumanPlayer = function(_, pn)
                local enabled = options.p2_only and side(pn) == 2
                    or (not options.p2_only and side(pn) <= (options.players or 1))
                return enabled and not opts(pn).autoplay
            end,
            GetCurrentSong = function()
                return people[1].song
            end,
            GetCurrentSteps = function(_, pn)
                return people[side(pn)].steps
            end,
            GetPlayerState = function(_, pn)
                return people[side(pn)].player
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
        PROFILEMAN = {
            IsPersistentProfile = function(_, pn)
                return not opts(pn).guest
            end,
            GetProfile = function(_, pn)
                return {
                    GetDisplayName = function()
                        return opts(pn).profile or (side(pn) == 1 and "Test Player" or "Friend")
                    end,
                    GetGUID = function()
                        return opts(pn).guid or ("fixture-" .. tostring(side(pn)))
                    end,
                }
            end,
        },
        STATSMAN = {
            GetCurStageStats = function()
                return {
                    GetPlayerStageStats = function(_, pn)
                        return people[side(pn)].stats
                    end,
                }
            end,
        },
    }, { __index = _G })
    local mount = actor_fixture(env, function()
        if options.file_error then
            error("Feedback unavailable")
        end
        return game.feedback
    end)
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
        local cur = text:match('^{"current":(.-),"players":')
        local result = text:match(',"results":%[(.*)%],"completed":')
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
    mount(actor)
    game.cards = actor[2]
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

for _, options in ipairs({ { autoplay = true } }) do
    local skipped = fixture(options)
    skipped:tick("ScreenGameplay")
    skipped:tick("ScreenEvaluation")
    check(skipped.result == nil, "autoplay does not export results")
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

local two = fixture({
    players = 2,
    profile = "Alice",
    score = 950001,
    p2 = { profile = "Bob", level = 16, score = 980002, counts = { W1 = 90, Miss = 10 } },
})
two:tick("ScreenGameplay")
check(two.current == '{"playing":false}', "two-player gameplay does not change the single-player overlay")
two:tick("ScreenEvaluation")
check(
    two.result:find('"profile":"Alice","side":1', 1, true)
        and two.result:find('"profile":"Bob","side":2', 1, true)
        and two.result:find('"score":950001', 1, true)
        and two.result:find('"score":980002', 1, true)
        and two.result:find('"difficulty":"S16"', 1, true)
        and two.result:find('"plate":"TG"', 1, true),
    "both independent chart results retain profile, side, score and plate"
)

local partial = fixture({ players = 2, missing_counts = true, p2 = { profile = "Bob" } })
partial:tick("ScreenGameplay")
partial:tick("ScreenEvaluation")
check(
    partial.result
        and not partial.result:find('"side":1', 1, true)
        and partial.result:find('"side":2', 1, true),
    "P1 result API failure does not suppress P2"
)

local duplicate = fixture({ players = 2, profile = "Same", p2 = { profile = "Same" } })
duplicate:tick("ScreenGameplay")
duplicate:tick("ScreenEvaluation")
local _, ambiguous = duplicate.result:gsub('"ambiguous":true', "")
check(ambiguous == 2, "duplicate profiles mark both results as ambiguous")

local coop =
    fixture({ players = 2, kind = "StepsType_Pump_Routine", p2 = { kind = "StepsType_Pump_Routine" } })
coop:tick("ScreenGameplay")
coop:tick("ScreenEvaluation")
check(not coop.result, "co-op charts are never exported as individual scores")

local switched_options = { profile = "Before" }
local switched = fixture(switched_options)
switched:tick("ScreenGameplay")
switched_options.profile = "After"
switched:tick("ScreenEvaluation")
check(
    switched.result:find('"profile":"Before"', 1, true) and switched.result:find('"eligible":false', 1, true),
    "profile change cannot redirect a completed attempt"
)

local solo_p2 = fixture({ p2_only = true, p2 = { profile = "Right" } })
solo_p2:tick("ScreenGameplay")
solo_p2:tick("ScreenEvaluation")
check(solo_p2.result:find('"profile":"Right","side":2', 1, true), "one player may use the P2 side")

local clock_failure = fixture({ clock_error = true })
check(
    not clock_failure.update and #clock_failure.messages == 1,
    "clock failure reports once without crashing"
)
game:tick("ScreenSelectMusic", 61)
check(not game.result, "completed payload expires from the transient mailbox")
local feedback_game = fixture({ players = 2 })
feedback_game:tick("ScreenGameplay")
feedback_game:tick("ScreenEvaluation")
local ids = {}
for id in feedback_game.result:gmatch('"id":"(.-)"') do
    ids[#ids + 1] = id
end
local function message(side)
    return feedback_game.cards[side]:GetChild("Message").values.settext[1]
end
check(message(1) == "Checking PIU Scores...", "notification waits for a real companion outcome")
feedback_game.feedback = "PIUCOMPANION 1\n" .. ids[1] .. "\taccepted\n" .. ids[2] .. "\tauth\n"
feedback_game:tick("ScreenEvaluation", 0.5)
check(
    message(1) == "PB submitted" and message(2) == "Check your API key in the companion",
    "each result card receives only its own outcome"
)
check(
    feedback_game.cards[1].values.xy[2] == 110 and feedback_game.cards[2].values.xy[2] == 110,
    "result cards sit beneath the usercards, above the score area"
)
feedback_game:tick("ScreenEvaluation", 6)
check(
    feedback_game.cards[1].values.diffusealpha[1] == 0,
    "unchanged feedback fades out after five seconds and cannot restart the timer"
)
feedback_game.feedback = "PIUCOMPANION 1\nold-event\tretry\n"
feedback_game:tick("ScreenEvaluation", 0.5)
check(message(1) == "PB submitted", "unrelated or delayed feedback cannot replace the current outcome")
feedback_game:tick("ScreenSelectMusic")
check(
    not feedback_game.cards[1].values.visible[1] and not feedback_game.cards[2].values.visible[1],
    "notifications are hidden outside the result screen"
)
feedback_game:tick("ScreenGameplay")
feedback_game:tick("ScreenEvaluation", 1)
check(message(1) == "Checking PIU Scores...", "a new attempt cannot reuse the previous success")
feedback_game.feedback = string.rep("x", 513)
feedback_game:tick("ScreenEvaluation", 9)
check(
    message(1) == "Sync status unavailable - check companion",
    "missing or oversized feedback never claims a saved score"
)
feedback_game.feedback = "PIUCOMPANION 1\n" .. feedback_game.result:match('"id":"(.-)"') .. "\tretry\n"
feedback_game:tick("ScreenEvaluation", 0.5)
check(message(1) == "Saved - will retry", "late feedback replaces the unavailable state")
feedback_game:tick("ScreenEvaluation", 6)
check(feedback_game.cards[1].values.diffusealpha[1] == 0, "retry notices also expire")
local display_failure = fixture({ file_error = true })
display_failure:tick("ScreenGameplay")
display_failure:tick("ScreenEvaluation")
check(
    display_failure.result ~= nil and #display_failure.messages == 0,
    "notification file failures cannot interrupt result capture or spam game errors"
)
local overlay_notifications = fixture({ overlay_only = true })
overlay_notifications:tick("ScreenGameplay")
overlay_notifications:tick("ScreenEvaluation")
check(#overlay_notifications.cards == 0, "overlay-only mode creates no notification cards")
print(passed .. " Lua behavioral assertions passed (fixture invariants also checked). Simulated game only.")
