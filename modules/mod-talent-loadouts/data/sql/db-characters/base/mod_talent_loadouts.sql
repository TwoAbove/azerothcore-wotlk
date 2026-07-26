-- DB: characters
-- Talent Loadouts module - per-character named talent/glyph loadouts.
-- Keyed by (guid, name) so a character may store unlimited named loadouts.

CREATE TABLE IF NOT EXISTS `mod_talent_loadouts` (
  `guid` int unsigned NOT NULL,
  `name` varchar(32) NOT NULL,
  `spec` tinyint unsigned NOT NULL DEFAULT '0',
  `class` tinyint unsigned NOT NULL DEFAULT '0',
  `created` int unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`,`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Talent Loadouts - loadout headers';

CREATE TABLE IF NOT EXISTS `mod_talent_loadouts_talents` (
  `guid` int unsigned NOT NULL,
  `name` varchar(32) NOT NULL,
  `spell` int unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`,`name`,`spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Talent Loadouts - stored talent rank-spells';

CREATE TABLE IF NOT EXISTS `mod_talent_loadouts_glyphs` (
  `guid` int unsigned NOT NULL,
  `name` varchar(32) NOT NULL,
  `slot` tinyint unsigned NOT NULL DEFAULT '0',
  `glyph` int unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`guid`,`name`,`slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Talent Loadouts - stored glyphs';
