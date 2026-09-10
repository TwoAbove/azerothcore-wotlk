local addonPath = arg[1] or "../TertiaryStatsUI/TertiaryStatsUI.lua"

-- Only WoW/LibQTip boundaries are mocked. Unimplemented APIs deliberately fail.
local frames, menu, popup, sent, visibleSidecar = {}, nil, nil, nil, nil
local function noop() end
local widget = {}
for _, method in ipairs({ "SetPoint", "SetAllPoints", "SetHeight", "SetWidth",
    "SetFrameStrata", "SetFrameLevel", "SetClampedToScreen", "SetBackdrop",
    "SetBackdropColor", "SetBackdropBorderColor", "EnableMouse", "SetTexture",
    "SetVertexColor", "SetTextColor", "SetJustifyH" }) do
    widget[method] = noop
end
function widget:SetText(text) self.text = text end
function widget:SetScript(event, callback) self.scripts[event] = callback end
function widget:HookScript(event, callback)
    local previous = self.scripts[event]
    self.scripts[event] = function(...)
        if previous then previous(...) end
        callback(...)
    end
end
function widget:RegisterEvent(event) self.events[event] = true end
function widget:IsShown() return self.shown end
function widget:Show() self.shown = true end
function widget:Hide()
    self.shown = false
    if self.scripts.OnHide then self.scripts.OnHide(self) end
end
function widget:Enable() self.enabled = true end
function widget:Disable() self.enabled = false end
function widget:GetFrameLevel() return 1 end
function widget:CreateFontString()
    local region = setmetatable({}, { __index = widget })
    self.regions[#self.regions + 1] = region
    return region
end
widget.CreateTexture = widget.CreateFontString
CreateFrame = function(kind, name, parent)
    local frame = setmetatable({ kind = kind, parent = parent, scripts = {},
        events = {}, regions = {}, shown = true, enabled = true }, { __index = widget })
    frames[#frames + 1] = frame
    if name then _G[name] = frame end
    return frame
end
UIParent = CreateFrame("Frame")
CharacterFrame = CreateFrame("Frame")
CharacterFrame:Hide()
GameTooltip = CreateFrame("GameTooltip")
ItemRefTooltip = CreateFrame("GameTooltip")
local equipLoc = "INVTYPE_FINGER"
function GameTooltip:GetItem() return "Item", self.link end
function GameTooltip:SetBagItem(bag, slot) self.link = GetContainerItemLink(bag, slot) end
function GameTooltip:SetInventoryItem(unit, slot) self.link = GetInventoryItemLink(unit, slot) end
function GameTooltip:SetLootItem(slot) self.link = "loot:" .. slot end
function GameTooltip:SetAuctionItem(list, index) self.link = list .. ":" .. index end
function GameTooltip:SetQuestItem(kind, index) self.link = kind .. ":" .. index end
hooksecurefunc = function(object, method, callback)
    local original = assert(object[method], "missing client method " .. method)
    object[method] = function(self, ...)
        original(self, ...)
        callback(self, ...)
    end
end
local qtip = {}
function qtip:Acquire()
    local tooltip = { lines = {} }
    function tooltip:AddLine(...)
        self.lines[#self.lines + 1] = { ... }
        return #self.lines
    end
    tooltip.AddHeader = tooltip.AddLine
    function tooltip:SetCell(line, column, text) self.lines[line][column] = text end
    function tooltip:GetLineCount() return #self.lines end
    function tooltip:Show() visibleSidecar = self end
    for _, method in ipairs({ "AddSeparator", "SetFrameStrata", "SetFrameLevel",
        "SetFont", "SetHeaderFont", "SmartAnchorTo" }) do
        tooltip[method] = noop
    end
    return tooltip
end
function qtip:Release(tooltip)
    if visibleSidecar == tooltip then visibleSidecar = nil end
end
LibStub = function(name)
    assert(name == "LibQTip-1.0")
    return qtip
end
StaticPopupDialogs = {}
ACCEPT, CANCEL = "Accept", "Cancel"
UnitName = function(unit) return unit == "player" and "LocalPlayer" or nil end
GetTime = function() return 1 end
GetMoney = function() return 2147483647 end
GetCoinTextureString = function(cost) return tostring(cost) end
GetInventoryItemLink = function(_, slot) return "equipped:" .. slot end
GetContainerNumSlots = function(bag) return bag == 0 and 4 or 0 end
GetContainerItemLink = function(bag, slot) return "bag:" .. bag .. ":" .. slot end
GetItemInfo = function(link) return link, link, nil, nil, nil, nil, nil, nil, equipLoc end
EasyMenu = function(items) menu = items end
StaticPopup_Show = function(name, link, cost, data)
    popup = { dialog = assert(StaticPopupDialogs[name]), link = link, cost = cost, data = data }
end
SendAddonMessage = function(prefix, payload, channel, recipient)
    assert(prefix == "TStats" and channel == "WHISPER" and recipient == "LocalPlayer")
    sent = payload
end

local function event(name, ...)
    for _, frame in ipairs(frames) do
        if frame.events[name] then frame.scripts.OnEvent(frame, name, ...) end
    end
end
local function receive(payload, channel, sender, prefix)
    event("CHAT_MSG_ADDON", prefix or "TStats", payload,
        channel or "WHISPER", sender or "LocalPlayer")
end
local function control(text)
    for _, frame in ipairs(frames) do
        if frame.text == text then return frame end
        for _, region in ipairs(frame.regions) do
            if region.text == text then return frame end
        end
    end
    error("missing visible control: " .. text)
end
local function openMenu()
    local button = control("Reroll Tertiary Powers")
    assert(button.enabled, "reroll button is disabled")
    menu = nil
    button.scripts.OnClick(button)
    return assert(menu, "reroll click did not open a menu")
end
local function accept(confirmation)
    sent = nil
    confirmation.dialog.OnAccept(nil, confirmation.data)
    return sent
end
local function plain(text) return (text:gsub("|c%x%x%x%x%x%x%x%x", ""):gsub("|r", "")) end
local function lines(section)
    local result, active = {}, section == nil
    for _, row in ipairs(visibleSidecar and visibleSidecar.lines or {}) do
        local left = plain(row[1])
        if section and (left == "Tertiary Stats" or left:match("^Compared with ")
            or left:match("^Currently equipped: ")) then
            active = left == section
        elseif active then
            result[#result + 1] = left .. (row[2] and "=" .. plain(row[2]) or "")
        end
    end
    return table.concat(result, "\n")
end
local function has(text, expected)
    assert(text:find(expected, 1, true), "missing " .. expected .. " in:\n" .. text)
end
local function lacks(text, unexpected)
    assert(not text:find(unexpected, 1, true), "unexpected " .. unexpected .. " in:\n" .. text)
end
local function bagTooltip()
    GameTooltip.scripts.OnTooltipCleared(GameTooltip)
    GameTooltip:Show()
    GameTooltip:SetBagItem(0, 4)
    GameTooltip.scripts.OnTooltipSetItem(GameTooltip)
end
local function memories(gen, mask, slots)
    receive("V3:M:C:" .. gen)
    receive("V3:M:U:" .. gen .. ":" .. mask)
    for slot, effect in pairs(slots) do receive("V3:M:S:" .. gen .. ":" .. slot .. ":" .. effect) end
    receive("V3:M:D:" .. gen)
end

dofile(addonPath)
event("PLAYER_LOGIN")
assert(sent == "V3:S", "login did not request a snapshot")
assert(not control("Reroll Tertiary Powers").enabled, "empty inventory enabled reroll")
-- Send entire rejected frames: a rejected Clear alone is not observable.
for _, source in ipairs({ { "V3", "PARTY", "LocalPlayer", "TStats" },
    { "V3", "WHISPER", "OtherPlayer", "TStats" },
    { "V3", "WHISPER", "LocalPlayer", "OtherAddon" },
    { "V1", "WHISPER", "LocalPlayer", "TStats" },
    { "V2", "WHISPER", "LocalPlayer", "TStats" } }) do
    for _, body in ipairs({ ":I:C:100", ":I:B:100:0:4:101:100:D:17:69:13", ":I:D:100" }) do
        receive(source[1] .. body, source[2], source[3], source[4])
    end
    bagTooltip()
    assert(not visibleSidecar and not control("Reroll Tertiary Powers").enabled,
        "untrusted or mismatched frame reached the UI")
end
receive("V3:I:C:5")
receive("V3:I:B:5:0:4:101:12345:D:17:69:13")
receive("V3:I:E:5:16:4294967295:54321:D:0:0:11")
receive("V3:I:D:5")
bagTooltip()
has(lines("Tertiary Stats"), "Fabled: Warcaster")
for _, power in ipairs({ "Avoidance", "Siphon", "Opportunity" }) do
    has(lines("Tertiary Stats"), power .. "=+17")
end
assert(#openMenu() == 2, "Fabled-only item became rerollable")
menu[2].func()
assert(popup.cost == "12345" and accept(popup) == "V3:R:B:0:4:101:12345")
GameTooltip:SetInventoryItem("player", 16)
has(lines("Tertiary Stats"), "Fabled: Overkill")
lacks(lines("Tertiary Stats"), "=+")
bagTooltip()
local committed = lines()
receive("V3:I:C:4")
receive("V3:I:D:4")
assert(lines() == committed, "older inventory frame replaced the visible tooltip")
receive("V3:I:C:6")
receive("V3:I:B:6:0:4:102:10000:D:99:2:0")
bagTooltip()
assert(lines() == committed, "incomplete inventory frame leaked")
receive("V3:I:C:7")
receive("V3:I:B:7:0:4:103:20000:D:23:16:0")
receive("V3:I:D:6")
bagTooltip()
assert(lines() == committed, "stale Done published an abandoned frame")
receive("V3:I:D:7")
has(lines("Tertiary Stats"), "Echo=+23")
lacks(lines("Tertiary Stats"), "Avoidance")
openMenu()[2].func()
assert(accept(popup) == "V3:R:B:0:4:103:20000")
receive("V3:L:C:6")
receive("V3:L:R:6:2:D:0:0:4")
receive("V3:L:D:6")
GameTooltip:SetLootItem(2)
has(lines("Tertiary Stats"), "Fabled: Ghostwalk")
receive("V3:A:C:7:0")
receive("V3:A:R:7:0:3:D:9:2:0")
receive("V3:A:D:7:0")
GameTooltip:SetAuctionItem("list", 3)
has(lines("Tertiary Stats"), "Fleetfoot=+9")
receive("V3:R:C:0")
assert(not control("Reroll Tertiary Powers").enabled, "disabled reroll remained clickable")
receive("V3:R:C:1")
assert(control("Reroll Tertiary Powers").enabled, "enabled reroll stayed disabled")

-- Each invalid record gets its own committed frame, so one rejected record cannot
-- hide another record accidentally accepted at the same location.
local generation = 7
local function invalid(record)
    generation = generation + 1
    receive("V3:I:C:" .. generation)
    receive("V3:I:" .. record:gsub("GEN", tostring(generation)))
    receive("V3:I:D:" .. generation)
    assert(not control("Reroll Tertiary Powers").enabled, "malformed target enabled reroll: " .. record)
end
for _, identity in ipairs({ "0", "4294967296", "-1", "1.5", "1e2", "0x10", " 12", "", "abc" }) do
    invalid("B:GEN:0:1:" .. identity .. ":100:D:9:2:0")
    invalid("E:GEN:1:" .. identity .. ":100:D:9:2:0")
end
for _, cost in ipairs({ "-1", "2147483648", "1.5", "1e2", "", "abc" }) do
    invalid("B:GEN:0:2:104:" .. cost .. ":D:9:2:0")
    invalid("E:GEN:2:104:" .. cost .. ":D:9:2:0")
end
for _, record in ipairs({ "B:GEN:0:3:100:D:9:2:0", "E:GEN:2:104:100:D:9:2",
    "E:GEN:3:104:100:D:9:2:0:", "E:GEN:4:104:100:D:9:2:0:extra" }) do invalid(record) end
local function inventory(bag, equipped)
    generation = generation + 1
    receive("V3:I:C:" .. generation)
    receive("V3:I:B:" .. generation .. ":0:4:" .. bag)
    receive("V3:I:E:" .. generation .. ":5:" .. equipped)
    receive("V3:I:D:" .. generation)
end
inventory("105:0:D:9:2:0", "4294967295:2147483647:D:9:2:0")
receive("V3:I:D")
assert(#openMenu() == 3, "valid identity/quote boundaries were rejected")
menu[2].func()
local equippedPopup = popup
menu[3].func()
local bagPopup = popup
assert(equippedPopup.link == "equipped:5" and equippedPopup.cost == "2147483647")
assert(bagPopup.link == "bag:0:4" and bagPopup.cost == "0")
inventory("107:200:D:9:2:0", "106:100:D:9:2:0")
assert(accept(equippedPopup) == "V3:R:E:5:4294967295:2147483647", "equipped confirmation was retargeted")
assert(accept(bagPopup) == "V3:R:B:0:4:105:0", "bag confirmation was retargeted")
openMenu()[3].func()
assert(accept(popup) == "V3:R:B:0:4:107:200", "new confirmation missed the refreshed quote")

-- Observe per-destination deltas, not the item's provenance header: provenance
-- remains visible even when learned/cleared memories must not auto-attune.
inventory("108:100:D:17:5:13", "109:100:D:0:0:0")
memories(8, 0, {})
bagTooltip()
has(lines("Compared with Ring 1"), "Warcaster=+ gained")
lacks(lines("Compared with Ring 1"), "Avoidance")
equipLoc = "INVTYPE_CHEST"
bagTooltip()
has(lines("Compared with Chest"), "Avoidance=+17")
lacks(lines("Compared with Chest"), "Warcaster")
equipLoc = "INVTYPE_FINGER"
memories(9, 4096, {})
bagTooltip()
local cleared = lines("Compared with Ring 1")
has(cleared, "Avoidance=+17")
has(cleared, "Siphon=+17")
lacks(cleared, "Warcaster")
receive("V3:M:C:10")
receive("V3:M:U:10:0")
bagTooltip()
assert(lines("Compared with Ring 1") == cleared, "incomplete memories leaked")
receive("V3:M:C:11")
receive("V3:M:U:11:4224")
receive("V3:M:S:11:11:8")
receive("V3:M:S:11:12:13")
receive("V3:M:D:10")
bagTooltip()
assert(lines("Compared with Ring 1") == cleared, "stale memories Done published")
receive("V3:M:D:11")
bagTooltip()
has(lines("Compared with Ring 1"), "No tertiary change")
lacks(lines("Compared with Ring 1"), "Warcaster")
-- Exercise the installed row/menu hook and its checked destination/request.
local row = control("Warcaster")
assert(row:IsShown(), "learned memory did not appear in character panel")
row.scripts.OnMouseUp(row, "LeftButton")
assert(not menu[2].checked and menu[3].checked, "attunement menu selected the wrong ring")
menu[2].func()
assert(sent == "V3:A:13:11")
menu[4].func()
assert(sent == "V3:A:0:12")
memories(12, 4096, { [12] = 13 })
bagTooltip()
has(lines("Compared with Ring 1"), "Siphon=+17")
lacks(lines("Compared with Ring 1"), "Warcaster")
has(lines("Compared with Ring 2"), "No tertiary change")
memories(13, 0, { [12] = 13 })
bagTooltip()
has(lines("Compared with Ring 1"), "Avoidance=+17")
lacks(lines("Compared with Ring 1"), "Warcaster")
-- Equipped provenance alone is not active: quest comparisons expose the equipped
-- effective powers without introducing a candidate's own auto-attunement.
memories(14, 0, {})
generation = generation + 1
receive("V3:I:C:" .. generation)
receive("V3:I:E:" .. generation .. ":11:109:100:D:17:5:13")
receive("V3:I:D:" .. generation)
GameTooltip:SetQuestItem("reward", 1)
has(lines("Currently equipped: Ring 1"), "Avoidance=+17")
lacks(lines("Currently equipped: Ring 1"), "Fabled:")
GameTooltip:Hide()
assert(not visibleSidecar, "tooltip hide left the sidecar visible")
print("tertiary UI protocol and comparison: ok")
