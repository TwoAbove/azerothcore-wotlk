local ADDON_PREFIX = "TStats"
local PROTOCOL_VERSION = "V2"
local floor = math.floor
local QTip = LibStub("LibQTip-1.0")

local POWER_NAMES = {
    "Avoidance",
    "Fleetfoot",
    "Siphon",
    "Tempo",
    "Echo",
    "Unbroken",
    "Opportunity",
}

local POWER_DESCRIPTIONS = {
    "Your total Avoidance reduces damage taken from area attacks, ground hazards, and environmental effects.",
    "Your total Fleetfoot increases running, swimming, Travel Form, ground mount, and flying mount speed.",
    "Your total Siphon heals you for a portion of effective damage and healing you and your pets produce.",
    "Your total Tempo can grant a burst of movement, attack, casting, and damaging-area tick speed and reduce one eligible class cooldown.",
    "Your total Echo can store effective damage and healing, then release separate damage and healing pools.",
    "Your total Unbroken can answer danger with a shield, healing over time, and movement speed.",
    "Your total Opportunity can make your next eligible class ability critically strike.",
}

local FIRST_EXOTIC_POWER = 4
local COLOR_END = "|r"
local COMPARISON_INCREASE_COLOR = "|cff20ff20"
local COMPARISON_DECREASE_COLOR = "|cffff2020"
local ORDINARY_POWER_COLOR = "|cff40ff40"
local EXOTIC_POWER_COLOR = "|cff4099ff"
local FABLED_POWER_COLOR = "|cffa335ee"

local FABLED_NAMES = {
    "Impact",
    "Premonition",
    "Momentum",
    "Ghostwalk",
    "Cavalier",
    "Keeper",
    "Vengeful Ghost",
    "Crossfire",
    "Alchemist's Gut",
    "Leviathan's Gift",
    "Overkill",
    "Blood Magic",
    "Warcaster",
    "Indomitable",
}

local FABLED_COMPATIBLE_SLOTS = {
    [1] = { 8 },
    [2] = { 13, 14 },
    [3] = { 11, 12 },
    [4] = { 15 },
    [5] = { 7 },
    [6] = { 5 },
    [7] = { 13, 14 },
    [8] = { 11, 12 },
    [9] = { 13, 14 },
    [10] = { 1 },
    [11] = { 16, 17, 18 },
    [12] = { 11, 12 },
    [13] = { 11, 12 },
    [14] = { 13, 14 },
}



local snapshot = {}
local characterPanel
local rows = {}
local lastProtocolRequest = 0
local pendingProtocolRequest
local bagDefinitions = {}
local equippedDefinitions = {}
local lootDefinitions = {}
local auctionDefinitions = { [0] = {}, [1] = {}, [2] = {} }
local inventoryGeneration = 0
local lootGeneration = 0
local auctionGenerations = { [0] = 0, [1] = 0, [2] = 0 }
local fabledSettings = {
    [1] = { 20 },
    [2] = { 4000 },
    [3] = { 1, 40, 15000 },
    [4] = { 6000, 40 },
    [7] = { 10000, 600000, 30 },
    [8] = { 30, 40 },
    [9] = { 5, 30000 },
    [10] = { 60 },
    [11] = { 15000 },
    [12] = { 4 },
}
local fabledRows = {}
local UpdateFabledRows
local unlockedFabled = {}
local attunedBySlot = {}
local attunementGeneration = 0
local rerollSettings = { enabled = true }
local actionMenu
local rerollButton
local UpdateRerollButton



local function TertiaryItemLine(powerIndex, points)
    return "+" .. points .. " " .. POWER_NAMES[powerIndex]
end

local function PowerLineColor(powerIndex)
    if powerIndex >= FIRST_EXOTIC_POWER then
        return 0.25, 0.6, 1.0
    end
    return 0.25, 1.0, 0.25
end

local function PowerLineColorCode(powerIndex)
    if powerIndex >= FIRST_EXOTIC_POWER then
        return EXOTIC_POWER_COLOR
    end
    return ORDINARY_POWER_COLOR
end

local function BagKey(bag, slot)
    return tostring(bag) .. ":" .. tostring(slot)
end

local AUCTION_LIST_TYPES = { list = 0, owner = 1, bidder = 2 }

local EQUIP_LOC_SLOTS = {
    INVTYPE_HEAD = { 1 },
    INVTYPE_NECK = { 2 },
    INVTYPE_SHOULDER = { 3 },
    INVTYPE_BODY = { 4 },
    INVTYPE_CHEST = { 5 },
    INVTYPE_ROBE = { 5 },
    INVTYPE_WAIST = { 6 },
    INVTYPE_LEGS = { 7 },
    INVTYPE_FEET = { 8 },
    INVTYPE_WRIST = { 9 },
    INVTYPE_HAND = { 10 },
    INVTYPE_FINGER = { 11, 12 },
    INVTYPE_TRINKET = { 13, 14 },
    INVTYPE_CLOAK = { 15 },
    INVTYPE_WEAPON = { 16, 17 },
    INVTYPE_2HWEAPON = { 16 },
    INVTYPE_WEAPONMAINHAND = { 16 },
    INVTYPE_WEAPONOFFHAND = { 17 },
    INVTYPE_SHIELD = { 17 },
    INVTYPE_HOLDABLE = { 17 },
    INVTYPE_RANGED = { 18 },
    INVTYPE_THROWN = { 18 },
    INVTYPE_RANGEDRIGHT = { 18 },
    INVTYPE_RELIC = { 18 },
}

local EQUIPMENT_SLOT_LABELS = {
    [1] = "Head",
    [2] = "Neck",
    [3] = "Shoulders",
    [4] = "Shirt",
    [5] = "Chest",
    [6] = "Waist",
    [7] = "Legs",
    [8] = "Feet",
    [9] = "Wrists",
    [10] = "Hands",
    [11] = "Ring 1",
    [12] = "Ring 2",
    [13] = "Trinket 1",
    [14] = "Trinket 2",
    [15] = "Back",
    [16] = "Main Hand",
    [17] = "Off Hand",
    [18] = "Ranged",
}

local function AddSpanningLine(tooltip, text, header)
    local line = header and tooltip:AddHeader(text) or tooltip:AddLine(text)
    tooltip:SetCell(line, 1, text, nil, nil, 2)
    return line
end

local function AddDefinitionRows(tooltip, definition)
    if not definition then
        return
    end
    if definition.fabled then
        AddSpanningLine(
            tooltip, FABLED_POWER_COLOR .. "Fabled: " ..
                FABLED_NAMES[definition.fabled] .. COLOR_END)
    end
    for _, powerIndex in ipairs(definition.powers) do
        tooltip:AddLine(
            PowerLineColorCode(powerIndex) .. POWER_NAMES[powerIndex] .. COLOR_END,
            PowerLineColorCode(powerIndex) .. "+" .. definition.points .. COLOR_END)
    end
end

local function PointsByPower(definition)
    local points = {}
    if definition then
        for _, powerIndex in ipairs(definition.powers) do
            points[powerIndex] = definition.points
        end
    end
    return points
end

local function HasDefinition(definition)
    return definition and (definition.fabled or #definition.powers > 0)
end

local function ComparisonDefinition(definition, slot, candidate)
    local activeFabled = attunedBySlot[slot]
    local memory = candidate and definition and definition.fabled
    if memory and not unlockedFabled[memory] then
        local alreadyAttuned = false
        for _, effect in pairs(attunedBySlot) do
            if effect == memory then
                alreadyAttuned = true
                break
            end
        end
        if not alreadyAttuned then
            for _, compatibleSlot in ipairs(FABLED_COMPATIBLE_SLOTS[memory]) do
                if slot == compatibleSlot then
                    activeFabled = memory
                    break
                end
            end
        end
    end
    return {
        fabled = activeFabled,
        points = definition and definition.points or 0,
        powers = activeFabled and {} or (definition and definition.powers or {}),
    }
end

local function AddComparisonRows(tooltip, candidateDefinition, equippedDefinition)
    local changed = false
    local candidateFabled = candidateDefinition and candidateDefinition.fabled
    local equippedFabled = equippedDefinition and equippedDefinition.fabled
    if candidateFabled ~= equippedFabled then
        if candidateFabled then
            tooltip:AddLine(
                FABLED_POWER_COLOR .. FABLED_NAMES[candidateFabled] .. COLOR_END,
                COMPARISON_INCREASE_COLOR .. "+ gained" .. COLOR_END)
            changed = true
        end
        if equippedFabled then
            tooltip:AddLine(
                FABLED_POWER_COLOR .. FABLED_NAMES[equippedFabled] .. COLOR_END,
                COMPARISON_DECREASE_COLOR .. "- lost" .. COLOR_END)
            changed = true
        end
    end

    local candidatePoints = PointsByPower(candidateDefinition)
    local equippedPoints = PointsByPower(equippedDefinition)
    for powerIndex = 1, #POWER_NAMES do
        local difference = (candidatePoints[powerIndex] or 0) -
            (equippedPoints[powerIndex] or 0)
        if difference ~= 0 then
            local valueColor = difference > 0
                and COMPARISON_INCREASE_COLOR or COMPARISON_DECREASE_COLOR
            local sign = difference > 0 and "+" or ""
            tooltip:AddLine(
                PowerLineColorCode(powerIndex) .. POWER_NAMES[powerIndex] .. COLOR_END,
                valueColor .. sign .. difference .. COLOR_END)
            changed = true
        end
    end

    if not changed then
        AddSpanningLine(tooltip, "|cffaaaaaaNo tertiary change" .. COLOR_END)
    end
end

local function CandidateSlots(tooltip)
    local _, link = tooltip:GetItem()
    local equipLoc = link and select(9, GetItemInfo(link))
    return equipLoc and EQUIP_LOC_SLOTS[equipLoc]
end

local function ReleaseTertiarySidecar(tooltip)
    local sidecar = tooltip and tooltip.__tertiaryStatsSidecar
    if not sidecar then
        return
    end
    tooltip.__tertiaryStatsSidecar = nil
    QTip:Release(sidecar)
end

local function RenderTertiarySidecar(tooltip)
    ReleaseTertiarySidecar(tooltip)
    if not tooltip or not tooltip:IsShown() or
        not tooltip.__tertiaryStatsDefinitionKnown then
        return
    end

    local candidateDefinition = tooltip.__tertiaryStatsDefinition
    local source = tooltip.__tertiaryStatsSource
    local candidateSlots = source and source[1] ~= "E" and CandidateSlots(tooltip)
    local pendingQuestReward = source and source[1] == "Q"
    local hasComparison = false
    if candidateSlots then
        for _, slot in ipairs(candidateSlots) do
            if HasDefinition(candidateDefinition) or
                    HasDefinition(equippedDefinitions[slot]) or attunedBySlot[slot] then
                hasComparison = true
                break
            end
        end
    end
    if not HasDefinition(candidateDefinition) and not hasComparison and not pendingQuestReward then
        return
    end

    local sidecar = QTip:Acquire(tooltip, 2, "LEFT", "RIGHT")
    tooltip.__tertiaryStatsSidecar = sidecar
    sidecar:SetFrameStrata("TOOLTIP")
    sidecar:SetFrameLevel(tooltip:GetFrameLevel() + 1)
    local tooltipFont = GameTooltipTextSmall or GameTooltipText
    sidecar:SetFont(tooltipFont)
    sidecar:SetHeaderFont(tooltipFont)
    sidecar:SmartAnchorTo(tooltip)
    if pendingQuestReward then
        AddSpanningLine(sidecar, "Tertiary Stats", true)
        AddSpanningLine(sidecar, "|cffb0b0b0Rolls when claimed." .. COLOR_END)
        if candidateSlots then
            for _, slot in ipairs(candidateSlots) do
                local equippedDefinition = equippedDefinitions[slot]
                local activeDefinition = ComparisonDefinition(
                    equippedDefinition, slot, false)
                if HasDefinition(activeDefinition) then
                    sidecar:AddSeparator(1, 0.35, 0.35, 0.35, 1)
                    AddSpanningLine(
                        sidecar, "Currently equipped: " ..
                            (EQUIPMENT_SLOT_LABELS[slot] or ("slot " .. slot)), true)
                    AddDefinitionRows(sidecar, activeDefinition)
                end
            end
        end
        sidecar:Show()
        return
    end


    if HasDefinition(candidateDefinition) then
        AddSpanningLine(sidecar, "Tertiary Stats", true)
        AddDefinitionRows(sidecar, candidateDefinition)
    end

    if candidateSlots then
        for _, slot in ipairs(candidateSlots) do
            local equippedDefinition = equippedDefinitions[slot]
            local candidateComparison = ComparisonDefinition(
                candidateDefinition, slot, true)
            local equippedComparison = ComparisonDefinition(
                equippedDefinition, slot, false)
            if HasDefinition(candidateComparison) or HasDefinition(equippedComparison) then
                if sidecar:GetLineCount() > 0 then
                    sidecar:AddSeparator(1, 0.35, 0.35, 0.35, 1)
                end
                AddSpanningLine(
                    sidecar, "Compared with " ..
                        (EQUIPMENT_SLOT_LABELS[slot] or ("slot " .. slot)), true)
                AddComparisonRows(sidecar, candidateComparison, equippedComparison)
            end
        end
    end

    sidecar:Show()
end

local function SetTooltipDefinition(tooltip, source, definition)
    local _, link = tooltip:GetItem()
    tooltip.__tertiaryStatsLink = link
    tooltip.__tertiaryStatsSource = source
    tooltip.__tertiaryStatsDefinition = definition
    tooltip.__tertiaryStatsDefinitionKnown = true
    RenderTertiarySidecar(tooltip)
end

local function AddCachedBagTooltip(tooltip, bag, slot)
    SetTooltipDefinition(
        tooltip, { "B", bag, slot },
        bagDefinitions[BagKey(bag, slot)])
end

local function AddCachedEquippedTooltip(tooltip, unit, slot)
    if unit == "player" then
        SetTooltipDefinition(
            tooltip, { "E", slot },
            equippedDefinitions[slot])
    end
end

local function AddCachedLootTooltip(tooltip, slot)
    SetTooltipDefinition(
        tooltip, { "L", slot },
        lootDefinitions[slot])
end

local function AddCachedAuctionTooltip(tooltip, listName, index)
    local listType = AUCTION_LIST_TYPES[listName]
    if listType ~= nil then
        SetTooltipDefinition(
            tooltip, { "A", listType, index },
            auctionDefinitions[listType][index])
    end
end

local function AddQuestRewardTooltip(tooltip, questItemType, index)
    if questItemType == "reward" or questItemType == "choice" then
        SetTooltipDefinition(tooltip, { "Q", questItemType, index }, nil)
    end
end

local function AddTertiaryTooltip(tooltip)
    local _, link = tooltip:GetItem()
    if tooltip.__tertiaryStatsLink ~= link then
        ReleaseTertiarySidecar(tooltip)
        tooltip.__tertiaryStatsLink = link
        tooltip.__tertiaryStatsSource = nil
        tooltip.__tertiaryStatsDefinition = nil
        tooltip.__tertiaryStatsDefinitionKnown = nil
        return
    end
    RenderTertiarySidecar(tooltip)
end

local function RefreshVisibleCachedTooltip()
    local source = GameTooltip.__tertiaryStatsSource
    if not source or not GameTooltip:IsShown() then
        return
    end
    if source[1] == "B" then
        AddCachedBagTooltip(GameTooltip, source[2], source[3])
    elseif source[1] == "E" then
        AddCachedEquippedTooltip(GameTooltip, "player", source[2])
    elseif source[1] == "L" then
        AddCachedLootTooltip(GameTooltip, source[2])
    elseif source[1] == "A" then
        local listName = source[2] == 0 and "list" or
            (source[2] == 1 and "owner" or "bidder")
        AddCachedAuctionTooltip(GameTooltip, listName, source[3])
    elseif source[1] == "Q" then
        AddQuestRewardTooltip(GameTooltip, source[2], source[3])
    end
end

local function ClearTertiaryTooltip(tooltip)
    ReleaseTertiarySidecar(tooltip)
    tooltip.__tertiaryStatsLink = nil
    tooltip.__tertiaryStatsSource = nil
    tooltip.__tertiaryStatsDefinition = nil
    tooltip.__tertiaryStatsDefinitionKnown = nil
end

local function HookItemTooltip(tooltip)
    if not tooltip or tooltip.__tertiaryStatsHooked then
        return
    end
    tooltip.__tertiaryStatsHooked = true
    tooltip:HookScript("OnTooltipSetItem", AddTertiaryTooltip)
    tooltip:HookScript("OnTooltipCleared", ClearTertiaryTooltip)
    tooltip:HookScript("OnHide", ReleaseTertiarySidecar)
end

local function InstallTooltipHooks()
    HookItemTooltip(GameTooltip)
    HookItemTooltip(ItemRefTooltip)

    if not GameTooltip.__tertiaryStatsSourceHooks then
        GameTooltip.__tertiaryStatsSourceHooks = true
        hooksecurefunc(GameTooltip, "SetBagItem", AddCachedBagTooltip)
        hooksecurefunc(GameTooltip, "SetInventoryItem", AddCachedEquippedTooltip)
        hooksecurefunc(GameTooltip, "SetLootItem", AddCachedLootTooltip)
        hooksecurefunc(GameTooltip, "SetAuctionItem", AddCachedAuctionTooltip)
        hooksecurefunc(GameTooltip, "SetQuestItem", AddQuestRewardTooltip)
    end
end

local function TrimmedNumber(value, decimals)
    value = tonumber(value) or 0
    local text = string.format("%." .. (decimals or 1) .. "f", value)
    text = string.gsub(text, "(%..-)0+$", "%1")
    text = string.gsub(text, "%.$", "")
    return text
end

local function Seconds(milliseconds)
    return TrimmedNumber((tonumber(milliseconds) or 0) / 1000, 1)
end

local function Minutes(milliseconds)
    return TrimmedNumber((tonumber(milliseconds) or 0) / 60000, 1)
end

local function FabledDescription(index)
    local values = fabledSettings[index] or {}
    if index == 1 then
        return "You take no falling damage. When you land, all enemies within "
            .. TrimmedNumber(values[1], 1)
            .. " yd take Physical damage equal to the damage prevented."
    elseif index == 2 then
        return "Killing an enemy that yields experience causes all abilities to cost no resources for "
            .. Seconds(values[1]) .. " sec."
    elseif index == 3 then
        return "Killing an enemy increases movement, melee and ranged attack, and casting speed by "
            .. TrimmedNumber(values[1], 1) .. "% for " .. Seconds(values[3])
            .. " sec. Stacks up to " .. TrimmedNumber(values[2], 0)
            .. " times. Additional kills refresh the duration."
    elseif index == 4 then
        return "After " .. Seconds(values[1]) .. " sec outside combat, you enter stealth and gain "
            .. TrimmedNumber(values[2], 1)
            .. "% movement speed. Dealing damage, casting any ability, or entering combat ends Ghostwalk."
    elseif index == 5 then
        return "You can attack, cast, gather, craft, use consumables, and interact while riding a ground mount outdoors. Flying mounts and taxi flight are excluded."
    elseif index == 6 then
        return "Elixir, flask, Well Fed, and scroll effect timers pause while Keeper is equipped. Drum effects are excluded. Timers resume when Keeper is unequipped or you die."
    elseif index == 7 then
        return "Damage that would kill you instead reduces you to 1 health and grants Vengeful Ghost for "
            .. Seconds(values[1]) .. " sec. Killing an enemy restores "
            .. TrimmedNumber(values[3], 1)
            .. "% maximum health; otherwise, you die. " .. Minutes(values[2]) .. " min cooldown."
    elseif index == 8 then
        return "Non-periodic damage also deals " .. TrimmedNumber(values[1], 1)
            .. "% of its full amount using the same damage school against the previous enemy you damaged within "
            .. TrimmedNumber(values[2], 1) .. " yd."
    elseif index == 9 then
        return "Potions have no cooldown. Drinking a potion inflicts Toxicity for "
            .. Seconds(values[2]) .. " sec, dealing " .. TrimmedNumber(values[1], 1)
            .. "% of maximum health every 3 sec per stack. This damage can kill you."
    elseif index == 10 then
        return "You can breathe underwater and swim " .. TrimmedNumber(values[1], 1) .. "% faster. This bonus stacks additively with other swim-speed increases."
    elseif index == 11 then
        return "Excess damage from killing an enemy that yields experience or honor is released using the killing blow's damage school on your next non-periodic hit within "
            .. Seconds(values[1]) .. " sec."
    elseif index == 12 then
        return "Mana spells can be cast without sufficient mana. Each missing point of mana adds "
            .. TrimmedNumber(values[1], 1) .. " damage to Blood Debt, dealt over "
            .. Seconds(values[2]) .. " sec. Additional casts add to the remaining damage and refresh the duration."
    elseif index == 13 then
        return "You can cast and channel while moving."
    end
    return "The first hostile effect that would take control of you each combat fails. Roots, snares, silences, disarms, and knockbacks are excluded."
end

local function SnapshotDescription(index, data)
    local values = data.values or {}
    if index == 1 then
        return "Reduces damage taken from area attacks, ground hazards, and environmental effects other than falling into the void by "
            .. TrimmedNumber(values[1], 1) .. "%."
    elseif index == 2 then
        return "Increases running, swimming, Travel Form, ground mount, and flying mount speed by "
            .. TrimmedNumber(values[1], 1) .. "%. Does not affect taxis or scripted vehicles."
    elseif index == 3 then
        return "Heals you for " .. TrimmedNumber(values[1], 1)
            .. "% of effective damage and healing you and your pets produce."
    elseif index == 4 then
        return "Damage and effective healing by you or your pets can refresh two Tempo effects for " .. Seconds(values[4])
            .. " sec. The first increases movement speed, melee and ranged attack speed, casting speed, and "
            .. "damaging-area tick speed by " .. TrimmedNumber(values[1], 1)
            .. "%. The second is consumed by the first non-item class ability you use with a "
            .. Seconds(values[5]) .. "-" .. Seconds(values[6])
            .. " sec cooldown, reducing that cooldown by " .. Seconds(values[3])
            .. " sec. Approximately " .. TrimmedNumber(values[2], 2) .. " times per minute."
    elseif index == 5 then
        return "Damage and effective healing by you or your pets can awaken Echo for " .. Seconds(values[3])
            .. " sec. Echo stores " .. TrimmedNumber(values[1], 1)
            .. "% of effective native and tertiary damage and healing in separate pools. Tertiary output is stored but cannot initiate proc chains. When Echo ends, stored damage is divided among enemies engaged with you within "
            .. TrimmedNumber(values[4], 1)
            .. " yd of your current or most recent target; stored healing restores the most injured nearby group members within "
            .. TrimmedNumber(values[5], 1) .. " yd without overhealing. Approximately "
            .. TrimmedNumber(values[2], 2) .. " times per minute."
    elseif index == 6 then
        return "When damage lowers you below " .. TrimmedNumber(values[4], 1)
            .. "% health, or when you become Dazed, Unbroken removes Daze, absorbs "
            .. TrimmedNumber(values[1], 1) .. "% of your maximum health, restores "
            .. TrimmedNumber(values[2], 1) .. "% of your maximum health over " .. Seconds(values[5])
            .. " sec, and increases movement speed by " .. TrimmedNumber(values[3], 1)
            .. "%. Items bearing Unbroken do not lose durability. This effect can occur once every "
            .. Seconds(values[6]) .. " sec."
    elseif index == 7 then
        return "Damage and effective healing by you or your pets can grant Opportunity for "
            .. Seconds(values[2])
            .. " sec. Your next eligible class ability critically strikes every immediate target and every tick of an active channel. Approximately "
            .. TrimmedNumber(values[1], 2) .. " times per minute."
    end
    return POWER_DESCRIPTIONS[index]
end

local AURA_TOOLTIP_HINTS = {
    [82005] = "Movement, attack, and casting speed increased by",
    [82008] = "Absorbs up to",
    [82010] = "Storing ",
}

local function RoundedAuraAmount(value)
    return floor((tonumber(value) or 0) + 0.5)
end

local function AuraTooltipDescription(spellId, unit)
    if spellId == 82005 then
        local data = snapshot[4]
        local values = data and data.values
        if not values or #values == 0 then
            return
        end
        return "Movement, attack, and casting speed increased by "
            .. RoundedAuraAmount(values[1]) .. "%. Your next eligible "
            .. Seconds(values[5]) .. "-" .. Seconds(values[6])
            .. " sec class ability has its cooldown reduced by " .. Seconds(values[3]) .. " sec."
    elseif spellId == 82008 then
        local data = snapshot[6]
        local values = data and data.values
        if not values or #values == 0 then
            return
        end
        local maxHealth = tonumber(UnitHealthMax(unit)) or 0
        local ticks = math.max(1, floor((tonumber(values[5]) or 0) / 1000))
        local shield = math.ceil(maxHealth * (tonumber(values[1]) or 0) / 100)
        local totalHealing = math.ceil(maxHealth * (tonumber(values[2]) or 0) / 100)
        local healingPerTick = math.max(1, floor(totalHealing / ticks))
        return "Absorbs up to " .. shield .. " damage, restores " .. healingPerTick
            .. " health every sec, and increases movement speed by "
            .. RoundedAuraAmount(values[3]) .. "%."
    elseif spellId == 82010 then
        local data = snapshot[5]
        local values = data and data.values
        if not values or #values == 0 then
            return
        end
        return "Storing " .. TrimmedNumber(values[1], 1)
            .. "% of your effective damage and healing for " .. Seconds(values[3]) .. " sec."
    end
end

local function RefreshAuraTooltip(tooltip, unit, auraIndex, filter)
    if not UnitBuff or not tooltip or not tooltip.GetName then
        return
    end
    local spellId = select(11, UnitBuff(unit, auraIndex, filter))
    local hint = AURA_TOOLTIP_HINTS[spellId]
    local description = hint and AuraTooltipDescription(spellId, unit)
    local tooltipName = description and tooltip:GetName()
    if not tooltipName then
        return
    end

    for lineIndex = 2, tooltip:NumLines() do
        local line = _G[tooltipName .. "TextLeft" .. lineIndex]
        local text = line and line:GetText()
        if text and string.find(text, hint, 1, true) then
            line:SetText(description)
            tooltip:Show()
            return
        end
    end
end

local function InstallAuraTooltipHook()
    if GameTooltip and GameTooltip.SetUnitBuff and not GameTooltip.__tertiaryStatsAuraHooked then
        GameTooltip.__tertiaryStatsAuraHooked = true
        hooksecurefunc(GameTooltip, "SetUnitBuff", RefreshAuraTooltip)
    end
end

local function CharacterValue(index, data)
    local raw = data and data.raw or 0
    local values = data and data.values or {}
    if #values == 0 then
        return raw > 0 and raw .. " pts" or "0"
    end
    if index == 7 then
        return TrimmedNumber(values[1], 2) .. " PPM"
    end
    return TrimmedNumber(values[1], 1) .. "%"
end

local function UpdateCharacterRows()
    for index, row in ipairs(rows) do
        local data = snapshot[index]
        local raw = data and data.raw or 0
        row.value:SetText(CharacterValue(index, data))
        if raw > 0 then
            row.label:SetTextColor(1.0, 0.82, 0.25)
            row.value:SetTextColor(0.25, 1.0, 0.25)
        else
            row.label:SetTextColor(0.62, 0.62, 0.62)
            row.value:SetTextColor(0.62, 0.62, 0.62)
        end
    end
end

local function RefreshLocalRawPoints()
    for index = 1, #POWER_NAMES do
        if not snapshot[index] then
            snapshot[index] = { raw = 0, values = {} }
        else
            snapshot[index].raw = 0
        end
    end

    for slot = 1, 19 do
        local definition = equippedDefinitions[slot]
        if definition and not attunedBySlot[slot] then
            for _, powerIndex in ipairs(definition.powers) do
                snapshot[powerIndex].raw = snapshot[powerIndex].raw + definition.points
            end
        end
    end
    UpdateCharacterRows()
    if UpdateFabledRows then
        UpdateFabledRows()
    end
end

local function SendProtocolRequest(request)
    lastProtocolRequest = GetTime()
    pendingProtocolRequest = nil
    local playerName = UnitName("player")
    if playerName and SendAddonMessage then
        SendAddonMessage(
            ADDON_PREFIX, PROTOCOL_VERSION .. ":" .. request, "WHISPER", playerName)
    end
end

local function QueueProtocolRequest(request)
    local now = GetTime()
    if now - lastProtocolRequest < 0.2 then
        if request == "S" or not pendingProtocolRequest then
            pendingProtocolRequest = request
        end
        return
    end
    SendProtocolRequest(request)
end

local function RequestSnapshot()
    RefreshLocalRawPoints()
    QueueProtocolRequest("S")
end

local function RequestInventorySnapshot()
    QueueProtocolRequest("I")
end

local function CurrentFabledSlot(effect)
    for slot = 1, 19 do
        if attunedBySlot[slot] == effect then
            return slot
        end
    end
end

local function ShowFabledAttunementMenu(row)
    local effect = row.fabledIndex
    if not effect or not unlockedFabled[effect] then
        return
    end
    if not actionMenu then
        actionMenu = CreateFrame(
            "Frame", "TertiaryStatsActionMenu", UIParent, "UIDropDownMenuTemplate")
    end

    local menu = {
        {
            text = FABLED_NAMES[effect],
            isTitle = true,
            notCheckable = true,
        },
    }
    for _, slot in ipairs(FABLED_COMPATIBLE_SLOTS[effect] or {}) do
        local selectedEffect = effect
        local selectedSlot = slot
        menu[#menu + 1] = {
            text = EQUIPMENT_SLOT_LABELS[selectedSlot] or ("Slot " .. selectedSlot),
            checked = attunedBySlot[selectedSlot] == selectedEffect,
            func = function()
                SendProtocolRequest(
                    "A:" .. tostring(selectedEffect) .. ":" .. tostring(selectedSlot))
            end,
        }
    end

    local currentSlot = CurrentFabledSlot(effect)
    if currentSlot then
        local selectedSlot = currentSlot
        menu[#menu + 1] = {
            text = "Clear attunement",
            notCheckable = true,
            func = function()
                SendProtocolRequest("A:0:" .. tostring(selectedSlot))
            end,
        }
    end
    EasyMenu(menu, actionMenu, "cursor", 0, 0, "MENU")
end


StaticPopupDialogs["TERTIARY_STATS_CONFIRM_REROLL"] = {
    text = "Reroll the ordinary tertiary powers on %s for %s?\n\n"
        .. "The number of lines and any Fabled provenance will be preserved.",
    button1 = ACCEPT,
    button2 = CANCEL,
    OnAccept = function(_, data)
        if data and data.request then
            SendProtocolRequest("R:" .. data.request)
        end
    end,
    timeout = 0,
    whileDead = 0,
    hideOnEscape = 1,
    exclusive = 1,
    preferredIndex = 3,
}

local function ConfirmReroll(request, itemLink, cost)
    StaticPopup_Show(
        "TERTIARY_STATS_CONFIRM_REROLL",
        itemLink or "this item",
        GetCoinTextureString(cost),
        { request = request })
end

local function HasRerollableItems()
    for _, definition in pairs(equippedDefinitions) do
        if definition and #definition.powers > 0 then
            return true
        end
    end
    for _, definition in pairs(bagDefinitions) do
        if definition and #definition.powers > 0 then
            return true
        end
    end
    return false
end

UpdateRerollButton = function()
    if not rerollButton then
        return
    end
    if rerollSettings.enabled and HasRerollableItems() then
        rerollButton:Enable()
    else
        rerollButton:Disable()
    end
end

local function AddRerollMenuItem(menu, label, request, itemLink, cost)
    if not itemLink or type(cost) ~= "number" then
        return
    end
    local selectedRequest = request
    local selectedLink = itemLink
    local selectedCost = cost
    menu[#menu + 1] = {
        text = label .. ": " .. itemLink .. "  " .. GetCoinTextureString(cost),
        notCheckable = true,
        disabled = GetMoney() < cost,
        func = function()
            ConfirmReroll(selectedRequest, selectedLink, selectedCost)
        end,
    }
end

local function ShowRerollMenu()
    if not rerollSettings.enabled then
        return
    end
    if not actionMenu then
        actionMenu = CreateFrame(
            "Frame", "TertiaryStatsActionMenu", UIParent, "UIDropDownMenuTemplate")
    end

    local menu = {
        {
            text = "Reroll ordinary tertiary powers",
            isTitle = true,
            notCheckable = true,
        },
    }
    for slot = 1, 19 do
        local definition = equippedDefinitions[slot]
        if definition and #definition.powers > 0 then
            AddRerollMenuItem(
                menu,
                EQUIPMENT_SLOT_LABELS[slot] or ("Slot " .. slot),
                "E:" .. tostring(slot),
                GetInventoryItemLink("player", slot),
                definition.rerollCost)
        end
    end
    for bag = 0, 4 do
        for slot = 1, GetContainerNumSlots(bag) do
            local definition = bagDefinitions[BagKey(bag, slot)]
            if definition and #definition.powers > 0 then
                AddRerollMenuItem(
                    menu,
                    "Bag " .. tostring(bag) .. ", slot " .. tostring(slot),
                    "B:" .. tostring(bag) .. ":" .. tostring(slot),
                    GetContainerItemLink(bag, slot),
                    definition.rerollCost)
            end
        end
    end
    if #menu == 1 then
        menu[#menu + 1] = {
            text = "No rerollable items",
            disabled = true,
            notCheckable = true,
        }
    end
    EasyMenu(menu, actionMenu, "cursor", 0, 0, "MENU")
end

local PANE_TOOLTIP_WIDTH = 320
local PANE_TOOLTIP_TEXT_WIDTH = 296
local paneTooltip

local function GetPaneTooltip()
    if paneTooltip then
        return paneTooltip
    end

    paneTooltip = CreateFrame("Frame", "TertiaryStatsPaneTooltip", UIParent)
    paneTooltip:SetWidth(PANE_TOOLTIP_WIDTH)
    paneTooltip:SetFrameStrata("TOOLTIP")
    paneTooltip:SetClampedToScreen(true)
    paneTooltip:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true,
        tileSize = 16,
        edgeSize = 16,
        insets = { left = 5, right = 5, top = 5, bottom = 5 },
    })
    paneTooltip:SetBackdropColor(TOOLTIP_DEFAULT_BACKGROUND_COLOR.r, TOOLTIP_DEFAULT_BACKGROUND_COLOR.g,
        TOOLTIP_DEFAULT_BACKGROUND_COLOR.b)
    paneTooltip:SetBackdropBorderColor(TOOLTIP_DEFAULT_COLOR.r, TOOLTIP_DEFAULT_COLOR.g,
        TOOLTIP_DEFAULT_COLOR.b)

    paneTooltip.title = paneTooltip:CreateFontString(nil, "ARTWORK", "GameTooltipHeaderText")
    paneTooltip.title:SetPoint("TOPLEFT", 12, -10)
    paneTooltip.title:SetWidth(PANE_TOOLTIP_TEXT_WIDTH)
    paneTooltip.title:SetJustifyH("LEFT")
    paneTooltip.title:SetTextColor(1.0, 0.82, 0.25)

    paneTooltip.description = paneTooltip:CreateFontString(nil, "ARTWORK", "GameTooltipText")
    paneTooltip.description:SetPoint("TOPLEFT", paneTooltip.title, "BOTTOMLEFT", 0, -6)
    paneTooltip.description:SetWidth(PANE_TOOLTIP_TEXT_WIDTH)
    paneTooltip.description:SetJustifyH("LEFT")
    paneTooltip.description:SetJustifyV("TOP")
    paneTooltip.description:SetTextColor(0.9, 0.9, 0.9)
    paneTooltip:Hide()
    return paneTooltip
end

local function ShowRowTooltip(row)
    local index = row.powerIndex
    local data = snapshot[index] or { raw = 0, values = {} }
    local tooltip = GetPaneTooltip()
    tooltip.title:SetText(POWER_NAMES[index] .. " " .. tostring(data.raw or 0))
    tooltip.title:SetTextColor(1.0, 0.82, 0.25)
    tooltip.description:SetText(SnapshotDescription(index, data))
    tooltip:SetHeight(28 + tooltip.title:GetHeight() + tooltip.description:GetHeight())
    tooltip:ClearAllPoints()
    tooltip:SetPoint("TOPLEFT", row, "TOPRIGHT", 8, 0)
    tooltip:Show()
end

local function CreateCharacterRow(parent, index)
    local row = CreateFrame("Frame", nil, parent)
    row:SetHeight(22)
    row:SetPoint("TOPLEFT", 12, -32 - (index - 1) * 24)
    row:SetPoint("TOPRIGHT", -12, -32 - (index - 1) * 24)
    row:EnableMouse(true)
    row.powerIndex = index

    local background = row:CreateTexture(nil, "BACKGROUND")
    background:SetAllPoints()
    background:SetTexture("Interface\\ChatFrame\\ChatFrameBackground")
    background:SetVertexColor(index % 2 == 0 and 0.09 or 0.04, index % 2 == 0 and 0.09 or 0.04,
        index % 2 == 0 and 0.12 or 0.07, 0.65)

    row.label = row:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    row.label:SetPoint("LEFT", 7, 0)
    row.label:SetText(POWER_NAMES[index])

    row.value = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    row.value:SetPoint("RIGHT", -7, 0)
    row.value:SetText("0")

    row:SetScript("OnEnter", ShowRowTooltip)
    row:SetScript("OnLeave", function() GetPaneTooltip():Hide() end)
    return row
end

local function ShowFabledRowTooltip(row)
    local index = row.fabledIndex
    local currentSlot = CurrentFabledSlot(index)
    local status
    if currentSlot then
        local label = EQUIPMENT_SLOT_LABELS[currentSlot] or ("Slot " .. currentSlot)
        status = GetInventoryItemLink("player", currentSlot)
            and ("Active in " .. label .. ".")
            or ("Attuned to " .. label .. "; inactive while that slot is empty.")
    else
        status = "Learned, but not attuned."
    end

    local tooltip = GetPaneTooltip()
    tooltip.title:SetText(FABLED_NAMES[index])
    tooltip.title:SetTextColor(0.76, 0.38, 1.0)
    tooltip.description:SetText(
        FabledDescription(index) .. "\n\n" .. status .. " Click to change attunement.")
    tooltip:SetHeight(28 + tooltip.title:GetHeight() + tooltip.description:GetHeight())
    tooltip:ClearAllPoints()
    tooltip:SetPoint("TOPLEFT", row, "TOPRIGHT", 8, 0)
    tooltip:Show()
end

local function CreateFabledRow(parent, position)
    local row = CreateFrame("Frame", nil, parent)
    row:SetHeight(20)
    row:SetPoint("TOPLEFT", 12, -238 - (position - 1) * 22)
    row:SetPoint("TOPRIGHT", -12, -238 - (position - 1) * 22)
    row:EnableMouse(true)

    local background = row:CreateTexture(nil, "BACKGROUND")
    background:SetAllPoints()
    background:SetTexture("Interface\\ChatFrame\\ChatFrameBackground")
    background:SetVertexColor(position % 2 == 0 and 0.09 or 0.04, 0.04,
        position % 2 == 0 and 0.12 or 0.08, 0.65)

    row.label = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    row.label:SetPoint("LEFT", 7, 0)
    row.label:SetPoint("RIGHT", -88, 0)
    row.label:SetJustifyH("LEFT")

    row.value = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    row.value:SetPoint("RIGHT", -7, 0)
    row.value:SetJustifyH("RIGHT")

    row:SetScript("OnEnter", ShowFabledRowTooltip)
    row:SetScript("OnLeave", function() GetPaneTooltip():Hide() end)
    row:SetScript("OnMouseUp", function(self, button)
        if button == "LeftButton" then
            ShowFabledAttunementMenu(self)
        end
    end)
    row:Hide()
    return row
end

UpdateFabledRows = function()
    if not characterPanel then
        return
    end

    local visible = 0
    for fabledIndex = 1, #FABLED_NAMES do
        if unlockedFabled[fabledIndex] then
            visible = visible + 1
            local row = fabledRows[visible]
            local currentSlot = CurrentFabledSlot(fabledIndex)
            local active = currentSlot
                and GetInventoryItemLink("player", currentSlot) ~= nil
            row.fabledIndex = fabledIndex
            row.label:SetText(FABLED_NAMES[fabledIndex])
            row.value:SetText(currentSlot
                and (EQUIPMENT_SLOT_LABELS[currentSlot] or ("Slot " .. currentSlot))
                or "Not attuned")
            if active then
                row.label:SetTextColor(0.76, 0.38, 1.0)
                row.value:SetTextColor(0.76, 0.38, 1.0)
            else
                row.label:SetTextColor(0.48, 0.36, 0.58)
                row.value:SetTextColor(0.55, 0.55, 0.55)
            end
            row:Show()
        end
    end
    for position = visible + 1, #fabledRows do
        fabledRows[position]:Hide()
    end

    characterPanel:SetHeight(246 + visible * 22)
end

local function InstallCharacterPanel()
    if characterPanel or not CharacterFrame then
        return
    end

    characterPanel = CreateFrame("Frame", "TertiaryStatsCharacterPanel", CharacterFrame)
    characterPanel:SetWidth(250)
    characterPanel:SetHeight(246)
    characterPanel:SetPoint("TOPLEFT", CharacterFrame, "TOPRIGHT", -18, -62)
    characterPanel:SetFrameStrata("HIGH")
    characterPanel:SetClampedToScreen(true)
    characterPanel:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true,
        tileSize = 16,
        edgeSize = 14,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    characterPanel:SetBackdropColor(0.025, 0.025, 0.04, 0.96)
    characterPanel:SetBackdropBorderColor(0.72, 0.52, 0.24, 0.95)

    local title = characterPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    title:SetPoint("TOP", 0, -11)
    title:SetText("Tertiary Stats & Fabled Memories")


    for position = 1, #FABLED_NAMES do
        fabledRows[position] = CreateFabledRow(characterPanel, position)
    end

    for index = 1, #POWER_NAMES do
        rows[index] = CreateCharacterRow(characterPanel, index)
    end

    rerollButton = CreateFrame(
        "Button", "TertiaryStatsRerollButton", characterPanel, "UIPanelButtonTemplate")
    rerollButton:SetHeight(22)
    rerollButton:SetPoint("TOPLEFT", 12, -204)
    rerollButton:SetPoint("TOPRIGHT", -12, -204)
    rerollButton:SetText("Reroll Tertiary Powers")
    rerollButton:SetScript("OnClick", ShowRerollMenu)
    rerollButton:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetText("Reroll ordinary tertiary powers", 1.0, 0.82, 0.25)
        GameTooltip:AddLine(
            "Choose an equipped or carried item. The number of power lines and "
                .. "any Fabled provenance are preserved.",
            0.9, 0.9, 0.9, true)
        GameTooltip:Show()
    end)
    rerollButton:SetScript("OnLeave", GameTooltip_Hide)
    UpdateRerollButton()

    characterPanel:SetScript("OnShow", RequestSnapshot)
    characterPanel:SetScript("OnHide", function()
        if paneTooltip then
            paneTooltip:Hide()
        end
    end)
    CharacterFrame:HookScript("OnShow", RequestSnapshot)
    if CharacterFrame:IsShown() then
        RequestSnapshot()
    else
        RefreshLocalRawPoints()
    end
end

local function SplitProtocol(message)
    local fields = {}
    for value in string.gmatch(message or "", "[^:]+") do
        fields[#fields + 1] = value
    end
    return fields
end

local function ProtocolInteger(value, minimum, maximum)
    local number = tonumber(value)
    if not number or number ~= floor(number) or number < minimum or number > maximum then
        return nil
    end
    return number
end

local function ParseItemDefinition(fields, firstField)
    if fields[firstField] ~= "D" or firstField + 3 ~= #fields then
        return nil
    end

    local points = ProtocolInteger(fields[firstField + 1], 0, 1000000)
    local mask = ProtocolInteger(fields[firstField + 2], 0, 127)
    local effect = ProtocolInteger(fields[firstField + 3], 0, #FABLED_NAMES)
    if not points or not mask or not effect
            or (mask == 0 and points ~= 0)
            or (mask ~= 0 and points == 0) then
        return nil
    end

    local powers = {}
    for wireIndex = 0, #POWER_NAMES - 1 do
        if floor(mask / (2 ^ wireIndex)) % 2 == 1 then
            powers[#powers + 1] = wireIndex + 1
        end
    end
    if #powers > 3 or (#powers == 0 and effect == 0) then
        return nil
    end

    return {
        fabled = effect > 0 and effect or nil,
        points = points,
        powers = powers,
    }
end

local inventoryFrameGeneration
local inventoryFrameBagDefinitions
local inventoryFrameEquippedDefinitions
local lootFrameGeneration
local lootFrameDefinitions
local auctionFrameGenerations = { [0] = nil, [1] = nil, [2] = nil }
local auctionFrameDefinitions = { [0] = nil, [1] = nil, [2] = nil }
local attunementFrameGeneration
local attunementFrameUnlocked
local attunementFrameSlots

local function HandleProtocolMessage(message)
    local fields = SplitProtocol(message)
    if fields[1] ~= PROTOCOL_VERSION then
        return
    end

    if fields[2] == "F" then
        local wireIndex = ProtocolInteger(fields[3], 0, #FABLED_NAMES - 1)
        if wireIndex then
            local values = {}
            for fieldIndex = 4, #fields do
                values[#values + 1] = tonumber(fields[fieldIndex]) or 0
            end
            fabledSettings[wireIndex + 1] = values
        end
        return
    end

    if fields[2] == "R" and fields[3] == "C" and #fields == 4 then
        local enabled = ProtocolInteger(fields[4], 0, 1)
        if enabled then
            rerollSettings.enabled = enabled == 1
            if UpdateRerollButton then
                UpdateRerollButton()
            end
        end
        return
    end

    if fields[2] == "M" then
        local action = fields[3]
        local generation = ProtocolInteger(fields[4], 1, 4294967295)
        if action == "C" and generation and #fields == 4 then
            if generation > attunementGeneration
                    and (not attunementFrameGeneration
                        or generation > attunementFrameGeneration) then
                attunementFrameGeneration = generation
                attunementFrameUnlocked = {}
                attunementFrameSlots = {}
            end
        elseif action == "U" and generation == attunementFrameGeneration
                and #fields == 5 then
            local mask = ProtocolInteger(fields[5], 0, 65535)
            if mask then
                for effect = 1, #FABLED_NAMES do
                    if floor(mask / (2 ^ (effect - 1))) % 2 == 1 then
                        attunementFrameUnlocked[effect] = true
                    end
                end
            end
        elseif action == "S" and generation == attunementFrameGeneration
                and #fields == 6 then
            local slot = ProtocolInteger(fields[5], 1, 19)
            local effect = ProtocolInteger(fields[6], 1, #FABLED_NAMES)
            if slot and effect then
                attunementFrameSlots[slot] = effect
            end
        elseif action == "D" and generation == attunementFrameGeneration
                and #fields == 4 then
            attunementGeneration = generation
            unlockedFabled = attunementFrameUnlocked
            attunedBySlot = attunementFrameSlots
            attunementFrameGeneration = nil
            attunementFrameUnlocked = nil
            attunementFrameSlots = nil
            RefreshLocalRawPoints()
        end
        return
    end

    if fields[2] == "I" then
        local action = fields[3]
        local generation = ProtocolInteger(fields[4], 1, 4294967295)
        if action == "C" and generation and #fields == 4 then
            if generation > inventoryGeneration then
                inventoryGeneration = generation
                inventoryFrameGeneration = generation
                inventoryFrameBagDefinitions = {}
                inventoryFrameEquippedDefinitions = {}
            end
        elseif action == "E" and generation == inventoryFrameGeneration then
            local slot = ProtocolInteger(fields[5], 1, 19)
            local cost = ProtocolInteger(fields[6], 0, 2147483647)
            local definition = slot and cost and ParseItemDefinition(fields, 7)
            if definition then
                definition.rerollCost = cost
                inventoryFrameEquippedDefinitions[slot] = definition
            end
        elseif action == "B" and generation == inventoryFrameGeneration then
            local bag = ProtocolInteger(fields[5], 0, 4)
            local slot = ProtocolInteger(fields[6], 1, 255)
            local cost = ProtocolInteger(fields[7], 0, 2147483647)
            local definition = bag and slot and cost and ParseItemDefinition(fields, 8)
            if definition then
                definition.rerollCost = cost
                inventoryFrameBagDefinitions[BagKey(bag, slot)] = definition
            end
        elseif action == "D" and generation == inventoryFrameGeneration and #fields == 4 then
            bagDefinitions = inventoryFrameBagDefinitions
            equippedDefinitions = inventoryFrameEquippedDefinitions
            inventoryFrameGeneration = nil
            inventoryFrameBagDefinitions = nil
            inventoryFrameEquippedDefinitions = nil
            RefreshLocalRawPoints()
            RefreshVisibleCachedTooltip()
            if UpdateRerollButton then
                UpdateRerollButton()
            end
        end
        return
    end

    if fields[2] == "L" then
        local action = fields[3]
        local generation = ProtocolInteger(fields[4], 1, 4294967295)
        if action == "C" and generation and #fields == 4 then
            if generation > lootGeneration then
                lootGeneration = generation
                lootFrameGeneration = generation
                lootFrameDefinitions = {}
            end
        elseif action == "R" and generation == lootFrameGeneration then
            local slot = ProtocolInteger(fields[5], 1, 255)
            local definition = slot and ParseItemDefinition(fields, 6)
            if definition then
                lootFrameDefinitions[slot] = definition
            end
        elseif action == "D" and generation == lootFrameGeneration and #fields == 4 then
            lootDefinitions = lootFrameDefinitions
            lootFrameGeneration = nil
            lootFrameDefinitions = nil
            RefreshVisibleCachedTooltip()
        end
        return
    end

    if fields[2] == "A" then
        local action = fields[3]
        local generation = ProtocolInteger(fields[4], 1, 4294967295)
        local listType = ProtocolInteger(fields[5], 0, 2)
        if action == "C" and generation and listType and #fields == 5 then
            if generation > auctionGenerations[listType] then
                auctionGenerations[listType] = generation
                auctionFrameGenerations[listType] = generation
                auctionFrameDefinitions[listType] = {}
            end
        elseif action == "R" and generation and listType
                and generation == auctionFrameGenerations[listType] then
            local index = ProtocolInteger(fields[6], 1, 1000000)
            local definition = index and ParseItemDefinition(fields, 7)
            if definition then
                auctionFrameDefinitions[listType][index] = definition
            end
        elseif action == "D" and generation and listType
                and generation == auctionFrameGenerations[listType] and #fields == 5 then
            auctionDefinitions[listType] = auctionFrameDefinitions[listType]
            auctionFrameGenerations[listType] = nil
            auctionFrameDefinitions[listType] = nil
            RefreshVisibleCachedTooltip()
        end
        return
    end

    if fields[2] ~= "S" then
        return
    end

    local wireIndex = ProtocolInteger(fields[3], 0, #POWER_NAMES - 1)
    if not wireIndex then
        return
    end

    local index = wireIndex + 1
    local data = { raw = tonumber(fields[4]) or 0, values = {} }
    for fieldIndex = 5, #fields do
        data.values[#data.values + 1] = tonumber(fields[fieldIndex]) or 0
    end
    snapshot[index] = data
    UpdateCharacterRows()
end

local controller = CreateFrame("Frame", "TertiaryStatsUIController")
controller:RegisterEvent("ADDON_LOADED")
controller:RegisterEvent("PLAYER_LOGIN")
controller:RegisterEvent("PLAYER_ENTERING_WORLD")
controller:RegisterEvent("PLAYER_EQUIPMENT_CHANGED")
controller:RegisterEvent("PLAYER_LEVEL_UP")
controller:RegisterEvent("BAG_UPDATE")
controller:RegisterEvent("CHAT_MSG_ADDON")
controller:SetScript("OnEvent", function(_, event, ...)
    if event == "ADDON_LOADED" then
        local loadedAddon = ...
        if loadedAddon == "Blizzard_CharacterUI" then
            InstallCharacterPanel()
        end
    elseif event == "PLAYER_LOGIN" then
        if RegisterAddonMessagePrefix then
            RegisterAddonMessagePrefix(ADDON_PREFIX)
        end
        InstallTooltipHooks()
        InstallAuraTooltipHook()
        InstallCharacterPanel()
        RequestSnapshot()
    elseif event == "PLAYER_ENTERING_WORLD" then
        RequestSnapshot()
    elseif event == "PLAYER_EQUIPMENT_CHANGED" or event == "PLAYER_LEVEL_UP" then
        inventoryFrameGeneration = nil
        inventoryFrameBagDefinitions = nil
        inventoryFrameEquippedDefinitions = nil
        bagDefinitions = {}
        equippedDefinitions = {}
        RefreshLocalRawPoints()
        RefreshVisibleCachedTooltip()
        RequestSnapshot()
    elseif event == "BAG_UPDATE" then
        inventoryFrameGeneration = nil
        inventoryFrameBagDefinitions = nil
        inventoryFrameEquippedDefinitions = nil
        bagDefinitions = {}
        RefreshVisibleCachedTooltip()
        RequestInventorySnapshot()
    elseif event == "CHAT_MSG_ADDON" then
        local prefix, message, channel, sender = ...
        local playerName = UnitName("player")
        if prefix == ADDON_PREFIX and channel == "WHISPER"
                and playerName and sender == playerName then
            HandleProtocolMessage(message)
        end
    end
end)
controller:SetScript("OnUpdate", function()
    if pendingProtocolRequest and GetTime() >= lastProtocolRequest + 0.2 then
        SendProtocolRequest(pendingProtocolRequest)
    end
end)
