-- Earthcaller guardians use dedicated creature templates so AzerothCore owns
-- kill credit, loot rights, corpse state, inventory handling, and group rules.
-- Each supported mining/herbalism node loot table maps to entry 900000 + loot id.
-- Guardians exclude guaranteed node materials and roll one enriched side-drop.
SET @EARTHCALLER_ENTRY_BASE := 900000;

DROP TEMPORARY TABLE IF EXISTS `mod_easy_gathering_old_guardian`;
CREATE TEMPORARY TABLE `mod_easy_gathering_old_guardian` (
  `entry` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`entry`)
) ENGINE=MEMORY
SELECT `entry`
FROM `creature_template`
WHERE `entry` BETWEEN @EARTHCALLER_ENTRY_BASE AND @EARTHCALLER_ENTRY_BASE + 99999
  AND `name` = 'Earthcaller Guardian';

DELETE `loot`
FROM `creature_loot_template` AS `loot`
INNER JOIN `mod_easy_gathering_old_guardian` AS `guardian`
  ON `guardian`.`entry` = `loot`.`Entry`;
DELETE `model`
FROM `creature_template_model` AS `model`
INNER JOIN `mod_easy_gathering_old_guardian` AS `guardian`
  ON `guardian`.`entry` = `model`.`CreatureID`;
DELETE `template`
FROM `creature_template` AS `template`
INNER JOIN `mod_easy_gathering_old_guardian` AS `guardian`
  ON `guardian`.`entry` = `template`.`entry`;
DROP TEMPORARY TABLE `mod_easy_gathering_old_guardian`;

DROP TEMPORARY TABLE IF EXISTS `mod_easy_gathering_earthcaller_node`;
CREATE TEMPORARY TABLE `mod_easy_gathering_earthcaller_node` (
  `loot_id` INT UNSIGNED NOT NULL,
  `node_name` VARCHAR(100) NOT NULL,
  `item_level` INT UNSIGNED NOT NULL,
  `is_herb` TINYINT UNSIGNED NOT NULL,
  PRIMARY KEY (`loot_id`)
) ENGINE=MEMORY
SELECT
  `gameobject`.`Data1` AS `loot_id`,
  MIN(`gameobject`.`name`) AS `node_name`,
  MAX(`item`.`ItemLevel`) AS `item_level`,
  MAX(`item`.`subclass` = 9) AS `is_herb`
FROM `gameobject_template` AS `gameobject`
INNER JOIN `gameobject_loot_template` AS `loot`
  ON `loot`.`Entry` = `gameobject`.`Data1`
INNER JOIN `item_template` AS `item`
  ON `item`.`entry` = `loot`.`Item`
WHERE `gameobject`.`type` = 3
  AND `gameobject`.`Data4` = 1
  AND `gameobject`.`Data5` = 1
  AND `gameobject`.`Data1` > 0
  AND `item`.`class` = 7
  AND `item`.`subclass` IN (7, 9)
GROUP BY `gameobject`.`Data1`;

INSERT INTO `creature_template` (
  `entry`, `name`, `subname`, `minlevel`, `maxlevel`, `exp`, `faction`,
  `speed_walk`, `speed_run`, `speed_swim`, `speed_flight`, `detection_range`,
  `rank`, `dmgschool`, `DamageModifier`, `BaseAttackTime`, `RangeAttackTime`,
  `BaseVariance`, `RangeVariance`, `unit_class`, `unit_flags`, `unit_flags2`,
  `dynamicflags`, `family`, `type`, `type_flags`, `lootid`, `mingold`, `maxgold`,
  `AIName`, `MovementType`, `HoverHeight`, `HealthModifier`, `ManaModifier`,
  `ArmorModifier`, `ExperienceModifier`, `RegenHealth`, `CreatureImmunitiesId`,
  `flags_extra`, `ScriptName`, `VerifiedBuild`
)
SELECT
  @EARTHCALLER_ENTRY_BASE + `node`.`loot_id`,
  'Earthcaller Guardian',
  `node`.`node_name`,
  1, 80, 2, 14,
  `base`.`speed_walk`, `base`.`speed_run`, `base`.`speed_swim`, `base`.`speed_flight`,
  `base`.`detection_range`, `base`.`rank`, `base`.`dmgschool`, `base`.`DamageModifier`,
  `base`.`BaseAttackTime`, `base`.`RangeAttackTime`, `base`.`BaseVariance`,
  `base`.`RangeVariance`, `base`.`unit_class`, `base`.`unit_flags`, `base`.`unit_flags2`,
  `base`.`dynamicflags`, `base`.`family`, `base`.`type`, `base`.`type_flags`,
  @EARTHCALLER_ENTRY_BASE + `node`.`loot_id`,
  0, 0, '', 0, `base`.`HoverHeight`, `base`.`HealthModifier`, `base`.`ManaModifier`,
  `base`.`ArmorModifier`, 1, `base`.`RegenHealth`, `base`.`CreatureImmunitiesId`,
  `base`.`flags_extra` | 64, '', NULL
FROM `mod_easy_gathering_earthcaller_node` AS `node`
CROSS JOIN `creature_template` AS `base`
WHERE `base`.`entry` = 92;

INSERT INTO `creature_template_model`
  (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT
  @EARTHCALLER_ENTRY_BASE + `node`.`loot_id`,
  `model`.`Idx`, `model`.`CreatureDisplayID`, `model`.`DisplayScale`, `model`.`Probability`, NULL
FROM `mod_easy_gathering_earthcaller_node` AS `node`
CROSS JOIN `creature_template_model` AS `model`
WHERE `model`.`CreatureID` = 92;

DROP TEMPORARY TABLE IF EXISTS `mod_easy_gathering_earthcaller_candidate`;
CREATE TEMPORARY TABLE `mod_easy_gathering_earthcaller_candidate` ENGINE=MEMORY
SELECT
  `ranked`.*,
  SUM(IF(`ranked`.`priority` > 1, `ranked`.`weight`, 0))
    OVER (PARTITION BY `ranked`.`loot_id`) AS `rare_weight`
FROM (
  SELECT
    `source`.*,
    LEAST(100.0, `source`.`chance` * 3.0) AS `weight`,
    ROW_NUMBER() OVER (
      PARTITION BY `source`.`loot_id`
      ORDER BY `source`.`chance` DESC, `source`.`item`
    ) AS `priority`
  FROM (
    SELECT
      `loot`.`Entry` AS `loot_id`,
      `loot`.`Item` AS `item`,
      MAX(`loot`.`Chance`) AS `chance`,
      GREATEST(1, MIN(`loot`.`MinCount`)) AS `min_count`,
      GREATEST(MAX(`loot`.`MaxCount`), MIN(`loot`.`MinCount`)) AS `max_count`
    FROM `gameobject_loot_template` AS `loot`
    INNER JOIN `mod_easy_gathering_earthcaller_node` AS `node`
      ON `node`.`loot_id` = `loot`.`Entry`
    INNER JOIN `item_template` AS `item`
      ON `item`.`entry` = `loot`.`Item`
    WHERE `loot`.`Reference` = 0
      AND `loot`.`QuestRequired` = 0
      AND `loot`.`Chance` > 0
      AND `loot`.`Chance` < 100
      AND `item`.`class` <> 12
      AND `item`.`bonding` <> 4
    GROUP BY `loot`.`Entry`, `loot`.`Item`
  ) AS `source`
) AS `ranked`;

-- One grouped side-drop is guaranteed. The most common secondary item is the
-- zero-chance equal-weight fallback; rarer rows get triple their node odds,
-- normalized so they occupy at most 85% of the group.
INSERT INTO `creature_loot_template`
  (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`,
   `GroupId`, `MinCount`, `MaxCount`, `Comment`)
SELECT
  @EARTHCALLER_ENTRY_BASE + `candidate`.`loot_id`,
  `candidate`.`item`, 0,
  IF(`candidate`.`priority` = 1, 0,
    `candidate`.`weight` * LEAST(`candidate`.`rare_weight`, 85.0) / `candidate`.`rare_weight`),
  0, 1, 1,
  `candidate`.`min_count`,
  LEAST(255, `candidate`.`max_count` + 1),
  'Earthcaller enriched node side-drop'
FROM `mod_easy_gathering_earthcaller_candidate` AS `candidate`;

-- Vanilla herb nodes and a few special deposits have no secondary table rows.
-- Give those nodes one curated, same-tier uncommon material rather than
-- duplicating their guaranteed primary herb or ore.
INSERT INTO `creature_loot_template`
  (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`,
   `GroupId`, `MinCount`, `MaxCount`, `Comment`)
SELECT
  @EARTHCALLER_ENTRY_BASE + `node`.`loot_id`,
  CASE
    WHEN `node`.`is_herb` = 1 AND `node`.`item_level` <= 15 THEN 2452   -- Swiftthistle
    WHEN `node`.`is_herb` = 1 AND `node`.`item_level` <= 30 THEN 8153   -- Wildvine
    WHEN `node`.`is_herb` = 1 AND `node`.`item_level` <= 45 THEN 8845   -- Ghost Mushroom
    WHEN `node`.`is_herb` = 1 AND `node`.`item_level` <= 59 THEN 13468  -- Black Lotus
    WHEN `node`.`is_herb` = 1 AND `node`.`item_level` <= 69 THEN 22794  -- Fel Lotus
    WHEN `node`.`is_herb` = 1 THEN 36908                                -- Frost Lotus
    WHEN `node`.`item_level` <= 15 THEN 774                              -- Malachite
    WHEN `node`.`item_level` <= 30 THEN 1206                             -- Moss Agate
    WHEN `node`.`item_level` <= 45 THEN 3864                             -- Citrine
    WHEN `node`.`item_level` <= 59 THEN 12361                            -- Blue Sapphire
    WHEN `node`.`item_level` <= 69 THEN 23436                            -- Living Ruby
    ELSE 36917                                                           -- Bloodstone
  END,
  0, 100, 0, 1, 1, 1, 1,
  'Earthcaller curated node fallback'
FROM `mod_easy_gathering_earthcaller_node` AS `node`
LEFT JOIN `mod_easy_gathering_earthcaller_candidate` AS `candidate`
  ON `candidate`.`loot_id` = `node`.`loot_id`
WHERE `candidate`.`loot_id` IS NULL;

DROP TEMPORARY TABLE `mod_easy_gathering_earthcaller_candidate`;
DROP TEMPORARY TABLE `mod_easy_gathering_earthcaller_node`;
