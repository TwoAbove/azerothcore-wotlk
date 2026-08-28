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
StaticPopupDialogs = {}
ACCEPT = "Accept"
CANCEL = "Cancel"
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

receive("V2:I:C:1", "PARTY", "LocalPlayer")
receive("V2:I:C:1", "WHISPER", "OtherPlayer")
receive("V1:I:C:1")
assert(generation() == 0, "untrusted or mismatched protocol messages were accepted")

receive("V2:I:C:5")
receive("V2:I:B:5:0:4:12345:D:17:69:13")
receive("V2:I:E:5:16:54321:D:0:0:11")
receive("V2:I:D:5")
local bags = upvalue(handle, "bagDefinitions")
local equipped = upvalue(handle, "equippedDefinitions")
local combined = bags["0:4"]
assert(combined and combined.points == 17 and combined.fabled == 13
    and combined.rerollCost == 12345,
    "combined ordinary, Fabled, and reroll data did not parse")
assert(#combined.powers == 3 and combined.powers[1] == 1
    and combined.powers[2] == 3 and combined.powers[3] == 7,
    "ordinary mask did not round-trip from the combined record")
assert(equipped[16] and equipped[16].fabled == 11
    and equipped[16].rerollCost == 54321 and #equipped[16].powers == 0,
    "Fabled-only migration exception did not parse")

receive("V2:I:C:4")
receive("V2:I:D:4")
assert(upvalue(handle, "bagDefinitions")["0:4"] == combined,
    "an older generation replaced the committed inventory frame")

receive("V2:I:C:6")
receive("V2:I:B:6:0:4:10000:D:99:2:0")
assert(upvalue(handle, "bagDefinitions")["0:4"] == combined,
    "an incomplete frame partially replaced the committed inventory frame")
receive("V2:I:C:7")
receive("V2:I:B:7:0:4:20000:D:23:16:0")
receive("V2:I:D:6")
assert(upvalue(handle, "bagDefinitions")["0:4"] == combined,
    "a stale Done committed an abandoned inventory frame")
receive("V2:I:D:7")
local replacement = upvalue(handle, "bagDefinitions")["0:4"]
assert(replacement.points == 23 and replacement.powers[1] == 5
    and replacement.rerollCost == 20000,
    "matching Done did not atomically commit the current inventory frame")

receive("V2:L:C:6")
receive("V2:L:R:6:2:D:0:0:4")
receive("V2:L:D:6")
assert(upvalue(handle, "lootDefinitions")[2].fabled == 4,
    "loot frame did not parse")

receive("V2:A:C:7:0")
receive("V2:A:R:7:0:3:D:9:2:0")
receive("V2:A:D:7:0")
local auction = upvalue(handle, "auctionDefinitions")
assert(auction[0][3].points == 9 and auction[0][3].powers[1] == 2,
    "auction frame did not parse")

receive("V2:M:C:8")
receive("V2:M:U:8:1032")
receive("V2:M:S:8:15:4")
receive("V2:M:S:8:16:11")
receive("V2:M:D:8")
local unlocked = upvalue(handle, "unlockedFabled")
local attuned = upvalue(handle, "attunedBySlot")
assert(unlocked[4] and unlocked[11] and attuned[15] == 4 and attuned[16] == 11,
    "Fabled memories did not commit atomically")

receive("V2:M:C:9")
receive("V2:M:U:9:1")
receive("V2:M:C:10")
receive("V2:M:U:10:2")
receive("V2:M:S:10:13:2")
receive("V2:M:D:9")
assert(upvalue(handle, "attunedBySlot")[16] == 11,
    "a stale Fabled Done committed an abandoned frame")
receive("V2:M:D:10")
assert(upvalue(handle, "unlockedFabled")[2]
    and upvalue(handle, "attunedBySlot")[13] == 2,
    "current Fabled frame did not replace the prior frame")

receive("V2:R:C:0")
local reroll = upvalue(handle, "rerollSettings")
assert(not reroll.enabled, "disabled reroll setting did not parse")
receive("V2:R:C:1")
assert(upvalue(handle, "rerollSettings").enabled,
    "enabled reroll setting did not parse")
print("tertiary UI protocol parser: ok")
