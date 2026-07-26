DELETE FROM `creature` WHERE `id` = 90001;
DELETE FROM `creature_template_model` WHERE `CreatureID` = 90001;
DELETE FROM `creature_template` WHERE `entry` = 90001;
DELETE FROM `npc_text` WHERE `ID` = 90001;

INSERT INTO `creature_template`
    (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `faction`, `npcflag`,
     `speed_walk`, `speed_run`, `unit_class`, `unit_flags`, `type`, `ScriptName`)
VALUES
    (90001, 'Eldric', 'Fabled Reforger', 80, 80, 35, 1,
     1, 1.14286, 1, 512, 7, 'npc_fabled_reforger');

INSERT INTO `creature_template_model`
    (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`)
VALUES
    (90001, 0, 3307, 1, 1);

INSERT INTO `npc_text` (`ID`, `text0_0`, `Probability0`)
VALUES
    (90001,
     'I can bind a Fabled memory to its proper place. Choose the item whose memory will be consumed.',
     1);

INSERT INTO `creature`
    (`id`, `map`, `spawnMask`, `phaseMask`, `position_x`, `position_y`, `position_z`,
     `orientation`, `spawntimesecs`, `curhealth`)
VALUES
    -- Stormwind, Ironforge, Darnassus, Exodar
    (90001,   0, 1, 1, -8419.0,   621.0,   95.50, 3.4, 120, 10635),
    (90001,   0, 1, 1, -4784.0, -1124.0,  498.89, 3.0, 120, 10635),
    (90001,   1, 1, 1,  9915.0,  2315.0, 1330.87, 4.5, 120, 10635),
    (90001, 530, 1, 1, -4239.0,-11710.0, -143.90, 2.2, 120, 10635),
    -- Orgrimmar, Thunder Bluff, Undercity, Silvermoon
    (90001,   1, 1, 1,  2062.0, -4835.0,   24.68, 3.3, 120, 10635),
    (90001,   1, 1, 1, -1229.0,   105.0,  129.50, 3.1, 120, 10635),
    (90001,   0, 1, 1,  1678.0,   290.0,  -62.05, 0.7, 120, 10635),
    (90001, 530, 1, 1,  9846.0, -7366.0,   18.60, 2.4, 120, 10635),
    -- Shattrath and Dalaran
    (90001, 530, 1, 1, -2097.0,  5299.0,  -37.24, 3.0, 120, 10635),
    (90001, 571, 1, 1,  5911.0,   668.5,  643.579, 2.2, 120, 10635);
