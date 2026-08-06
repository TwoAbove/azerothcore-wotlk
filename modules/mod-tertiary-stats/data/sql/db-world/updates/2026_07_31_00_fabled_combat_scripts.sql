DELETE FROM `spell_script_names`
WHERE `ScriptName` IN ('spell_fabled_vengeful_guard', 'spell_fabled_blood_debt');

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(82017, 'spell_fabled_blood_debt'),
(82023, 'spell_fabled_vengeful_guard');
