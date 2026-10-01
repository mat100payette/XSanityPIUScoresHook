-- Minimal actor/file adapter for exercising the shipped hook's own notification code.
return function(env, read)
    local function actor(definition)
        return definition
    end
    env.Def = { ActorFrame = actor, Quad = actor }
    env.SCREEN_WIDTH, env.SCREEN_HEIGHT = 1280, 720
    env.color = function(value)
        return value
    end
    env.LoadFont = function()
        return setmetatable({}, {
            __concat = function(_, definition)
                return definition
            end,
        })
    end
    env.FILEMAN = { FlushDirCache = function() end }
    env.RageFileUtil = {
        CreateRageFile = function()
            local text
            return {
                Open = function(_, path, mode)
                    assert(path == "/Save/PiuCompanion/status.txt" and mode == 1)
                    text = read()
                    return text ~= nil
                end,
                GetFileSize = function()
                    return #text
                end,
                Read = function()
                    return text
                end,
                destroy = function() end,
            }
        end,
    }
    local methods = {}
    for _, name in ipairs({
        "xy",
        "x",
        "zoomto",
        "diffuse",
        "diffusealpha",
        "zoom",
        "maxwidth",
        "halign",
        "visible",
        "settext",
    }) do
        methods[name] = function(self, ...)
            self.values[name] = { ... }
            return self
        end
    end
    methods.stoptweening = function(self)
        return self
    end
    methods.linear = function(self)
        return self
    end
    methods.GetChild = function(self, name)
        for _, child in ipairs(self) do
            if child.Name == name then
                return child
            end
        end
        error("Missing child " .. name)
    end
    local function mount(definition)
        definition.values = {}
        setmetatable(definition, { __index = methods })
        for _, child in ipairs(definition) do
            mount(child)
        end
        if definition.InitCommand then
            definition.InitCommand(definition)
        end
        return definition
    end
    return mount
end
