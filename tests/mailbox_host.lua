-- Real Lua allocator + shipped hook, driven through an isolated child process.
local clock, screen = 100, "ScreenSelectMusic"
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
}
GAMESTATE = {
    GetNumPlayersEnabled = function()
        return 0
    end,
}
local actor = assert(loadfile(arg[1]))()
function actor:SetUpdateFunction(callback)
    self.update = callback
end
actor.InitCommand(actor)
for command in io.lines() do
    if command == "quit" then
        break
    end
    clock = clock + 1
    actor.update(actor, 1)
    collectgarbage("collect")
end
