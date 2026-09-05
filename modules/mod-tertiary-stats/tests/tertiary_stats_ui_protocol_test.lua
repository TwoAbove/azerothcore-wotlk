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

receive("V3:I:C:1", "PARTY", "LocalPlayer")
receive("V3:I:C:1", "WHISPER", "OtherPlayer")
receive("V1:I:C:1")
receive("V2:I:C:1")
assert(generation() == 0, "untrusted or mismatched protocol messages were accepted")

receive("V3:I:C:5")
receive("V3:I:B:5:0:4:101:12345:D:17:69:13")
receive("V3:I:E:5:16:4294967295:54321:D:0:0:11")
receive("V3:I:D:5")
local bags = upvalue(handle, "bagDefinitions")
local equipped = upvalue(handle, "equippedDefinitions")
local combined = bags["0:4"]
assert(combined and combined.points == 17 and combined.fabled == 13
    and combined.rerollCost == 12345 and combined.guidLow == 101,
    "combined ordinary, Fabled, and reroll data did not parse")
assert(#combined.powers == 3 and combined.powers[1] == 1
    and combined.powers[2] == 3 and combined.powers[3] == 7,
    "ordinary mask did not round-trip from the combined record")
assert(equipped[16] and equipped[16].fabled == 11
    and equipped[16].rerollCost == 54321 and equipped[16].guidLow == 4294967295
    and #equipped[16].powers == 0,
    "Fabled-only migration exception did not parse")

receive("V3:I:C:4")
receive("V3:I:D:4")
assert(upvalue(handle, "bagDefinitions")["0:4"] == combined,
    "an older generation replaced the committed inventory frame")

receive("V3:I:C:6")
receive("V3:I:B:6:0:4:102:10000:D:99:2:0")
assert(upvalue(handle, "bagDefinitions")["0:4"] == combined,
    "an incomplete frame partially replaced the committed inventory frame")
receive("V3:I:C:7")
receive("V3:I:B:7:0:4:103:20000:D:23:16:0")
receive("V3:I:D:6")
assert(upvalue(handle, "bagDefinitions")["0:4"] == combined,
    "a stale Done committed an abandoned inventory frame")
receive("V3:I:D:7")
local replacement = upvalue(handle, "bagDefinitions")["0:4"]
assert(replacement.points == 23 and replacement.powers[1] == 5
    and replacement.rerollCost == 20000,
    "matching Done did not atomically commit the current inventory frame")

receive("V3:L:C:6")
receive("V3:L:R:6:2:D:0:0:4")
receive("V3:L:D:6")
assert(upvalue(handle, "lootDefinitions")[2].fabled == 4,
    "loot frame did not parse")

receive("V3:A:C:7:0")
receive("V3:A:R:7:0:3:D:9:2:0")
receive("V3:A:D:7:0")
local auction = upvalue(handle, "auctionDefinitions")
assert(auction[0][3].points == 9 and auction[0][3].powers[1] == 2,
    "auction frame did not parse")

receive("V3:M:C:8")
receive("V3:M:U:8:1032")
receive("V3:M:S:8:15:4")
receive("V3:M:S:8:16:11")
receive("V3:M:D:8")
local unlocked = upvalue(handle, "unlockedFabled")
local attuned = upvalue(handle, "attunedBySlot")
assert(unlocked[4] and unlocked[11] and attuned[15] == 4 and attuned[16] == 11,
    "Fabled memories did not commit atomically")

receive("V3:M:C:9")
receive("V3:M:U:9:1")
receive("V3:M:C:10")
receive("V3:M:U:10:2")
receive("V3:M:S:10:13:2")
receive("V3:M:D:9")
assert(upvalue(handle, "attunedBySlot")[16] == 11,
    "a stale Fabled Done committed an abandoned frame")
receive("V3:M:D:10")
assert(upvalue(handle, "unlockedFabled")[2]
    and upvalue(handle, "attunedBySlot")[13] == 2,
    "current Fabled frame did not replace the prior frame")

receive("V3:R:C:0")
local reroll = upvalue(handle, "rerollSettings")
assert(not reroll.enabled, "disabled reroll setting did not parse")
receive("V3:R:C:1")
assert(upvalue(handle, "rerollSettings").enabled,
    "enabled reroll setting did not parse")
-- Invalid identities and incomplete records must never become reroll targets.
receive("V3:I:C:8")
for _, identity in ipairs({ "0", "4294967296", "-1", "1.5", "1e2", "0x10", " 12", "", "abc" }) do
    receive("V3:I:B:8:0:1:" .. identity .. ":100:D:9:2:0")
    receive("V3:I:E:8:1:" .. identity .. ":100:D:9:2:0")
end
for _, cost in ipairs({ "-1", "2147483648", "1.5", "1e2", "", "abc" }) do
    receive("V3:I:B:8:0:2:104:" .. cost .. ":D:9:2:0")
end
receive("V3:I:B:8:0:3:100:D:9:2:0") -- obsolete record without GUID
receive("V3:I:E:8:2:104:100:D:9:2") -- incomplete definition
receive("V3:I:E:8:3:104:100:D:9:2:0:")
receive("V3:I:E:8:4:104:100:D:9:2:0:extra")
receive("V3:I:B:8:0:4:105:0:D:9:2:0")
receive("V3:I:E:8:5:4294967295:2147483647:D:9:2:0")
receive("V3:I:D:8")
bags = upvalue(handle, "bagDefinitions")
equipped = upvalue(handle, "equippedDefinitions")
assert(not bags["0:1"] and not bags["0:2"] and not bags["0:3"]
    and not equipped[1] and not equipped[2] and not equipped[3] and not equipped[4],
    "malformed or obsolete inventory records were accepted")
assert(bags["0:4"].rerollCost == 0 and equipped[5].rerollCost == 2147483647,
    "valid identity/quote boundaries were rejected")
receive("V3:I:D") -- missing generation after a completed frame must be harmless

local showMenu = upvalue(upvalue(onEvent, "InstallCharacterPanel"), "ShowRerollMenu")
local menu, popup, sent
GetMoney = function() return 2147483647 end
GetCoinTextureString = function(cost) return tostring(cost) end
GetInventoryItemLink = function(_, slot) return "equipped:" .. slot end
GetContainerNumSlots = function(bag) return bag == 0 and 4 or 0 end
GetContainerItemLink = function(bag, slot) return "bag:" .. bag .. ":" .. slot end
EasyMenu = function(items) menu = items end
StaticPopup_Show = function(_, _, _, data) popup = data end
SendAddonMessage = function(prefix, payload, channel, recipient)
    assert(prefix == "TStats" and channel == "WHISPER" and recipient == "LocalPlayer")
    sent = payload
end
showMenu()
menu[2].func()
local equippedPopup = popup
menu[3].func()
local bagPopup = popup
-- Replacing both targets and changing their prices cannot retarget an open confirmation.
receive("V3:I:C:9")
receive("V3:I:E:9:5:106:100:D:9:2:0")
receive("V3:I:B:9:0:4:107:200:D:9:2:0")
receive("V3:I:D:9")
StaticPopupDialogs.TERTIARY_STATS_CONFIRM_REROLL.OnAccept(nil, equippedPopup)
assert(sent == "V3:R:E:5:4294967295:2147483647", "equipped confirmation lost its GUID or quoted cost")
StaticPopupDialogs.TERTIARY_STATS_CONFIRM_REROLL.OnAccept(nil, bagPopup)
assert(sent == "V3:R:B:0:4:105:0", "bag confirmation was retargeted to the replacement item")
showMenu()
menu[3].func()
StaticPopupDialogs.TERTIARY_STATS_CONFIRM_REROLL.OnAccept(nil, popup)
assert(sent == "V3:R:B:0:4:107:200", "new confirmation did not use the refreshed snapshot")

local installHooks = upvalue(onEvent, "InstallTooltipHooks")
local hookTooltip = upvalue(installHooks, "HookItemTooltip")
local addTooltip = upvalue(hookTooltip, "AddTertiaryTooltip")
local render = upvalue(addTooltip, "RenderTertiarySidecar")
local compare = upvalue(render, "ComparisonDefinition")
local candidate = { fabled = 13, points = 17, powers = { 1, 3 } }
local function memories(gen, mask, slots)
    receive("V3:M:C:" .. gen)
    receive("V3:M:U:" .. gen .. ":" .. mask)
    for slot, effect in pairs(slots) do
        receive("V3:M:S:" .. gen .. ":" .. slot .. ":" .. effect)
    end
    receive("V3:M:D:" .. gen)
end
memories(11, 0, {})
assert(compare(candidate, 11, true).fabled == 13
    and #compare(candidate, 11, true).powers == 0, "first learning did not auto-attune")
assert(not compare(candidate, 5, true).fabled
    and compare(candidate, 5, true).powers[1] == 1, "incompatible provenance auto-attuned")
memories(12, 4096, {})
local cleared = compare(candidate, 11, true)
assert(not cleared.fabled and cleared.points == 17 and cleared.powers[2] == 3,
    "learned and cleared provenance suppressed the candidate's ordinary powers")
memories(13, 4224, { [11] = 8, [12] = 13 })
assert(compare(candidate, 11, true).fabled == 8
    and #compare(candidate, 11, true).powers == 0,
    "learned provenance replaced the destination slot's existing attunement")
memories(14, 4096, { [12] = 13 })
assert(not compare(candidate, 11, true).fabled
    and compare(candidate, 11, true).powers[1] == 1,
    "learned provenance moved an attunement from the other ring slot")
memories(15, 0, { [12] = 13 })
assert(not compare(candidate, 11, true).fabled,
    "an existing same-effect attunement was treated as first learning")
assert(not compare(candidate, 11, false).fabled,
    "equipped item provenance was mistaken for an active attunement")
print("tertiary UI protocol and comparison: ok")
