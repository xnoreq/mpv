mp.UNKNOWN_TYPE.info = "this value is inserted if the C type is not supported"
mp.UNKNOWN_TYPE.type = "UNKNOWN_TYPE"

mp.ARRAY.info = "native array"
mp.ARRAY.type = "ARRAY"

mp.MAP.info = "native map"
mp.MAP.type = "MAP"

function mp.get_script_name()
    return mp.script_name
end

function mp.get_opt(key, def)
    local opts = mp.get_property_native("options/lua-opts")
    local val = opts[key]
    if val == nil then
        val = def
    end
    return val
end

local callbacks = {}
-- each script has its own section, so that they don't conflict
local default_section = "input_dispatch_" .. mp.script_name

-- Set the list of key bindings. These will override the user's bindings, so
-- you should use this sparingly.
-- A call to this function will remove all bindings previously set with this
-- function. For example, set_key_bindings({}) would remove all script defined
-- key bindings.
-- Note: the bindings are not active by default. Use enable_key_bindings().
--
-- list is an array of key bindings, where each entry is an array as follow:
--      {key, callback}
--      {key, callback, callback_down}
-- key is the key string as used in input.conf, like "ctrl+a"
-- callback is a Lua function that is called when the key binding is used.
-- callback_down can be given too, and is called when a mouse button is pressed
-- if the key is a mouse button. (The normal callback will be for mouse button
-- down.)
--
-- callback can be a string too, in which case the following will be added like
-- an input.conf line: key .. " " .. callback
-- (And callback_down is ignored.)
function mp.set_key_bindings(list, section, flags)
    local cfg = ""
    for i = 1, #list do
        local entry = list[i]
        local key = entry[1]
        local cb = entry[2]
        local cb_down = entry[3]
        if type(cb) == "function" then
            callbacks[#callbacks + 1] = {press=cb, before_press=cb_down}
            cfg = cfg .. key .. " script_dispatch " .. mp.script_name
                  .. " " .. #callbacks .. "\n"
        else
            cfg = cfg .. key .. " " .. cb .. "\n"
        end
    end
    mp.input_define_section(section or default_section, cfg, flags)
end

function mp.enable_key_bindings(section, flags)
    mp.input_enable_section(section or default_section, flags)
end

function mp.disable_key_bindings(section)
    mp.input_disable_section(section or default_section)
end

function mp.set_mouse_area(x0, y0, x1, y1, section)
    mp.input_set_section_mouse_area(section or default_section, x0, y0, x1, y1)
end

local function script_dispatch(event)
    local cb = callbacks[event.arg0]
    if cb then
        if event.type == "press" and cb.press then
            cb.press()
        elseif event.type == "keyup_follows" and cb.before_press then
            cb.before_press()
        end
    end
end

-- "Newer" and more convenient API

local key_bindings = {}
local message_id = 1

local function update_key_bindings()
    for i = 1, 2 do
        local section, flags
        local def = i == 1
        if def then
            section = "input_" .. mp.script_name
            flags = "default"
        else
            section = "input_forced_" .. mp.script_name
            flags = "force"
        end
        local cfg = ""
        for k, v in pairs(key_bindings) do
            if v.forced ~= def then
                cfg = cfg .. v.key .. " script_message_to " .. mp.script_name
                      .. " " .. v.name .. "\n"
            end
        end
        mp.input_define_section(section, cfg, flags)
        -- TODO: remove the section if the script is stopped
        mp.input_enable_section(section)
    end
end

local function add_binding(attrs, key, name, fn)
    if (type(name) ~= "string") and (not fn) then
        fn = name
        name = "message" .. tostring(message_id)
        message_id = message_id + 1
    end
    attrs.key = key
    attrs.name = name
    key_bindings[name] = attrs
    update_key_bindings()
    if fn then
        mp.register_script_message(name, fn)
    end
end

function mp.add_key_binding(...)
    add_binding({forced=false}, ...)
end

function mp.add_forced_key_binding(...)
    add_binding({forced=true}, ...)
end

function mp.remove_key_binding(name)
    key_bindings[name] = nil
    update_key_bindings()
    mp.unregister_script_message(name)
end

local timers = {}

function mp.add_timeout(seconds, cb)
    local t = mp.add_periodic_timer(seconds, cb)
    t.oneshot = true
    return t
end

function mp.add_periodic_timer(seconds, cb)
    local t = {
        timeout = seconds,
        cb = cb,
        oneshot = false,
        next_deadline = mp.get_time() + seconds,
    }
    timers[t] = t
    return t
end

function mp.cancel_timer(t)
    if t then
        timers[t] = nil
    end
end

-- Return the timer that expires next.
local function get_next_timer()
    local best = nil
    for t, _ in pairs(timers) do
        if (best == nil) or (t.next_deadline < best.next_deadline) then
            best = t
        end
    end
    return best
end

-- Run timers that have met their deadline.
-- Return: next absolute time a timer expires as number, or nil if no timers
local function process_timers()
    while true do
        local timer = get_next_timer()
        if not timer then
            return
        end
        local wait = timer.next_deadline - mp.get_time()
        if wait > 0 then
            return wait
        else
            if timer.oneshot then
                timers[timer] = nil
            end
            timer.cb()
            if not timer.oneshot then
                timer.next_deadline = mp.get_time() + timer.timeout
            end
        end
    end
end

local messages = {}

function mp.register_script_message(name, fn)
    messages[name] = fn
end

function mp.unregister_script_message(name)
    messages[name] = nil
end

local function message_dispatch(ev)
    if #ev.args > 0 then
        local handler = messages[ev.args[1]]
        if handler then
            handler(unpack(ev.args, 2))
        end
    end
end

-- used by default event loop (mp_event_loop()) to decide when to quit
mp.keep_running = true

local event_handlers = {}

function mp.register_event(name, cb)
    local list = event_handlers[name]
    if not list then
        list = {}
        event_handlers[name] = list
    end
    list[#list + 1] = cb
    return mp.request_event(name, true)
end

-- default handlers
mp.register_event("shutdown", function() mp.keep_running = false end)
mp.register_event("script-input-dispatch", script_dispatch)
mp.register_event("client-message", message_dispatch)

mp.msg = {
    log = mp.log,
    fatal = function(...) return mp.log("fatal", ...) end,
    error = function(...) return mp.log("error", ...) end,
    warn = function(...) return mp.log("warn", ...) end,
    info = function(...) return mp.log("info", ...) end,
    verbose = function(...) return mp.log("v", ...) end,
    debug = function(...) return mp.log("debug", ...) end,
}

_G.print = mp.msg.info

package.loaded["mp"] = mp
package.loaded["mp.msg"] = mp.msg

_G.mp_event_loop = function()
    local more_events = true
    mp.suspend()
    while mp.keep_running do
        local wait = process_timers()
        if wait == nil then
            wait = 1e20 -- infinity for all practical purposes
        end
        if more_events then
            wait = 0
        end
        -- Resume playloop - important especially if an error happened while
        -- suspended, and the error was handled, but no resume was done.
        if wait > 0 then
            mp.resume_all()
        end
        local e = mp.wait_event(wait)
        -- Empty the event queue while suspended; otherwise, each
        -- event will keep us waiting until the core suspends again.
        mp.suspend()
        more_events = (e.event ~= "none")
        if more_events then
            local handlers = event_handlers[e.event]
            if handlers then
                for _, handler in ipairs(handlers) do
                    handler(e)
                end
            end
        end
    end
end

return {}
