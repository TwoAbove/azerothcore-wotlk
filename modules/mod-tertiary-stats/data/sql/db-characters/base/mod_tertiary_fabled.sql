CREATE TABLE IF NOT EXISTS `mod_tertiary_fabled` (
    `guid` INT UNSIGNED NOT NULL,
    `effect` TINYINT UNSIGNED NOT NULL,
    `slot` TINYINT UNSIGNED NULL,
    PRIMARY KEY (`guid`, `effect`),
    UNIQUE KEY `uq_mod_tertiary_fabled_slot` (`guid`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Learned Fabled memories and active equipment slots';
