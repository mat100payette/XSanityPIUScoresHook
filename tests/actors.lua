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
    -- Match the inspected game API: no FlushDirCache or RageFile:GetFileSize bindings.
    env.FILEMAN = {
        GetFileSizeBytes = function(_, path)
            assert(path == "/Save/PiuCompanion/status.txt")
            local text = read()
            return text and #text or -1
        end,
    }
    env.lua = {
        ReadFile = function(path, mode)
            assert(path == "/Save/PiuCompanion/status.txt" and mode == 9257)
            return read()
        end,
    }
    env.RageFileUtil = {
        CreateRageFile = function()
            error("RageFile is restricted to song folders")
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
