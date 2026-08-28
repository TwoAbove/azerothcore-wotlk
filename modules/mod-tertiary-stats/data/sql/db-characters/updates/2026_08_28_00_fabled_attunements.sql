CREATE TABLE IF NOT EXISTS `mod_tertiary_fabled` (
    `guid` INT UNSIGNED NOT NULL,
    `effect` TINYINT UNSIGNED NOT NULL,
    `slot` TINYINT UNSIGNED NULL,
    PRIMARY KEY (`guid`, `effect`),
    UNIQUE KEY `uq_mod_tertiary_fabled_slot` (`guid`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Learned Fabled memories and active equipment slots';

START TRANSACTION;

INSERT IGNORE INTO `mod_tertiary_fabled` (`guid`, `effect`, `slot`)
SELECT `guid`, `effect`,
    MIN(CASE
        WHEN `effect` = 1 AND `slot` = 7 THEN `slot`
        WHEN `effect` IN (2, 7, 9, 14) AND `slot` IN (12, 13) THEN `slot`
        WHEN `effect` IN (3, 8, 12, 13) AND `slot` IN (10, 11) THEN `slot`
        WHEN `effect` = 4 AND `slot` = 14 THEN `slot`
        WHEN `effect` = 5 AND `slot` = 6 THEN `slot`
        WHEN `effect` = 6 AND `slot` = 4 THEN `slot`
        WHEN `effect` = 10 AND `slot` = 0 THEN `slot`
        WHEN `effect` = 11 AND `slot` IN (15, 16, 17) THEN `slot`
    END)
FROM (
    SELECT ci.`guid`, ci.`slot`, ((ii.`bonusSeed` & 65535) >> 7) AS `effect`
    FROM `character_inventory` ci
    INNER JOIN `item_instance` ii ON ii.`guid` = ci.`item`
    WHERE ci.`bag` = 0
      AND ci.`slot` < 19
      AND (ii.`bonusSeed` >> 24) = 1
      AND (ii.`bonusSeed` & 127) = 0
      AND ((ii.`bonusSeed` & 65535) >> 7) BETWEEN 1 AND 14
      AND ((ii.`bonusSeed` >> 20) & 15) = 0
) AS `equipped_fabled`
GROUP BY `guid`, `effect`;

UPDATE `item_instance`
SET `bonusSeed` = (`bonusSeed` & 4278648832)
    | (((( `bonusSeed` & 65535) >> 7) & 15) << 20)
WHERE (`bonusSeed` >> 24) = 1
  AND (`bonusSeed` & 127) = 0
  AND ((`bonusSeed` & 65535) >> 7) BETWEEN 1 AND 14
  AND ((`bonusSeed` >> 20) & 15) = 0;

UPDATE `item_loot_storage`
SET `bonusSeed` = (`bonusSeed` & 4278648832)
    | (((( `bonusSeed` & 65535) >> 7) & 15) << 20)
WHERE (`bonusSeed` >> 24) = 1
  AND (`bonusSeed` & 127) = 0
  AND ((`bonusSeed` & 65535) >> 7) BETWEEN 1 AND 14
  AND ((`bonusSeed` >> 20) & 15) = 0;

COMMIT;
