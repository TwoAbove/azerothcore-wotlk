ALTER TABLE `item_instance`
    ADD COLUMN `bonusSeed` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `randomPropertyId`;

ALTER TABLE `item_loot_storage`
    ADD COLUMN `bonusSeed` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `randomSuffix`;

DROP TABLE IF EXISTS `mod_tertiary_item_bonus`;
