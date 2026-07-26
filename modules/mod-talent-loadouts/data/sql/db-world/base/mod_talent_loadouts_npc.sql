-- DB: world
-- Talent Loadouts module - gossip NPC + chat command metadata.
--
-- Creature entry 700000 was verified free in this core
-- (SELECT entry FROM acore_world.creature_template WHERE entry BETWEEN 700000 AND 700010 -> no rows).

SET @ENTRY   := 700000;
SET @MODEL   := 24877; -- humanoid display id (same model used by mod-npc-talent-template's 55009)
SET @NAME    := 'Talent Loadouts';
SET @SUBNAME := 'Save & Load Talent Specs';
SET @TEXT    := 'Greetings! I can store and re-apply your talent and glyph loadouts, or reset your talents free of charge while you are out of combat.';

-- Creature template
DELETE FROM `creature_template` WHERE `entry` = @ENTRY;
INSERT INTO `creature_template`
  (`entry`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `rank`, `unit_class`, `unit_flags`, `type`, `type_flags`, `RegenHealth`, `flags_extra`, `ScriptName`)
VALUES
  (@ENTRY, @NAME, @SUBNAME, 'Speak', 0, 80, 80, 35, 1, 1, 1.14286, 0, 1, 2, 7, 138936390, 1, 2, 'npc_talent_loadouts');

-- Display / model
DELETE FROM `creature_template_model` WHERE `CreatureID` = @ENTRY;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
  (@ENTRY, 0, @MODEL, 1, 1, 0);

-- Gossip greeting text. The script sends the menu with the creature entry as the
-- npc_text id (SendGossipMenuFor(player, creature->GetEntry(), ...)), so the
-- npc_text ID must match the creature entry.
DELETE FROM `npc_text` WHERE `ID` = @ENTRY;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`) VALUES
  (@ENTRY, @TEXT, @TEXT);

-- Movement
DELETE FROM `creature_template_movement` WHERE `CreatureId` = @ENTRY;
INSERT INTO `creature_template_movement` (`CreatureId`, `Ground`, `Swim`, `Flight`, `Rooted`, `Chase`, `Random`, `InteractionPauseTimer`) VALUES
  (@ENTRY, 1, 1, 0, 0, 0, 0, NULL);

-- Chat command metadata. Security 0 keeps the .spec commands available to
-- normal players (the module also gates access at runtime via
-- TalentLoadouts.CommandSecurity).
DELETE FROM `command` WHERE `name` IN ('spec save', 'spec load', 'spec list', 'spec delete', 'spec reset');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
  ('spec save',   0, 'Syntax: .spec save $name\nSaves your current active-spec talents and glyphs as a named loadout.'),
  ('spec load',   0, 'Syntax: .spec load $name\nApplies a saved loadout to your active spec. Only usable out of combat.'),
  ('spec list',   0, 'Syntax: .spec list\nLists your saved talent loadouts.'),
  ('spec delete', 0, 'Syntax: .spec delete $name\nDeletes one of your saved talent loadouts.'),
  ('spec reset',  0, 'Syntax: .spec reset\nResets your talents for free. Only usable out of combat.');
