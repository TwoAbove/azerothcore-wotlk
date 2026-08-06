local addonPath = arg[1] or "../TertiaryStatsUI/TertiaryStatsUI.lua"

local controller
LibStub = function()
    return {}
end
GameTooltip = {
    IsShown = function()
        return false
    end,
}
ItemRefTooltip = {}
CreateFrame = function(_, name)
    local frame = { scripts = {} }
    function frame:RegisterEvent() end
    function frame:SetScript(script, callback)
        self.scripts[script] = callback
    end
    if name == "TertiaryStatsUIController" then
        controller = frame
    end
    return frame
end
UnitName = function(unit)
    return unit == "player" and "LocalPlayer" or nil
end
GetTime = function()
    return 1
end

dofile(addonPath)
assert(controller and controller.scripts.OnEvent, "addon event handler was not installed")
local onEvent = controller.scripts.OnEvent

local function upvalue(callback, wanted)
    for index = 1, 100 do
        local name, value = debug.getupvalue(callback, index)
        if not name then
            break
        end
        if name == wanted then
            return value
        end
    end
    error("missing upvalue " .. wanted)
end

local handle = upvalue(onEvent, "HandleProtocolMessage")
local function generation()
    return upvalue(handle, "inventoryGeneration")
end
local function receive(payload, channel, sender)
    onEvent(nil, "CHAT_MSG_ADDON", "TStats", payload,
        channel or "WHISPER", sender or "LocalPlayer")
end

receive("V1:I:C:1", "PARTY", "LocalPlayer")
receive("V1:I:C:1", "WHISPER", "OtherPlayer")
receive("V0:I:C:1")
assert(generation() == 0, "untrusted or mismatched protocol messages were accepted")

receive("V1:I:C:5")
receive("V1:I:B:5:0:4:O:17:0:2:6")
receive("V1:I:E:5:16:F:13")
receive("V1:I:D:5")
local bags = upvalue(handle, "bagDefinitions")
local equipped = upvalue(handle, "equippedDefinitions")
local ordinary = bags["0:4"]
assert(ordinary and ordinary.points == 17, "ordinary points did not parse")
assert(#ordinary.powers == 3 and ordinary.powers[1] == 1
    and ordinary.powers[2] == 3 and ordinary.powers[3] == 7,
    "ordinary powers did not round-trip from wire indexes")
assert(equipped[16] and equipped[16].fabled == 13,
    "Fabled effect did not round-trip from its semantic record")

receive("V1:I:C:4")
receive("V1:I:D:4")
assert(upvalue(handle, "bagDefinitions")["0:4"] == ordinary,
    "an older generation replaced the committed inventory frame")

receive("V1:I:C:6")
receive("V1:I:B:6:0:4:O:99:1")
assert(upvalue(handle, "bagDefinitions")["0:4"] == ordinary,
    "an incomplete frame partially replaced the committed inventory frame")
receive("V1:I:C:7")
receive("V1:I:B:7:0:4:O:23:4")
receive("V1:I:D:6")
assert(upvalue(handle, "bagDefinitions")["0:4"] == ordinary,
    "a stale Done committed an abandoned inventory frame")
receive("V1:I:D:7")
local replacement = upvalue(handle, "bagDefinitions")["0:4"]
assert(replacement.points == 23 and replacement.powers[1] == 5,
    "matching Done did not atomically commit the current inventory frame")

receive("V1:L:C:6")
receive("V1:L:R:6:2:F:4")
receive("V1:L:D:6")
assert(upvalue(handle, "lootDefinitions")[2].fabled == 4,
    "loot frame did not parse")

receive("V1:A:C:7:0")
receive("V1:A:R:7:0:3:O:9:1")
receive("V1:A:D:7:0")
local auction = upvalue(handle, "auctionDefinitions")
assert(auction[0][3].points == 9 and auction[0][3].powers[1] == 2,
    "auction frame did not parse")

print("tertiary UI protocol parser: ok")
