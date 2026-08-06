SET @column_exists = (SELECT 1 FROM `INFORMATION_SCHEMA`.`COLUMNS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'item_instance' AND `COLUMN_NAME` = 'bonusSeed' LIMIT 1);
SET @sql = IF(@column_exists IS NULL, 'ALTER TABLE `item_instance` ADD COLUMN `bonusSeed` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `randomPropertyId`', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @column_exists = (SELECT 1 FROM `INFORMATION_SCHEMA`.`COLUMNS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'item_loot_storage' AND `COLUMN_NAME` = 'bonusSeed' LIMIT 1);
SET @sql = IF(@column_exists IS NULL, 'ALTER TABLE `item_loot_storage` ADD COLUMN `bonusSeed` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `randomSuffix`', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
