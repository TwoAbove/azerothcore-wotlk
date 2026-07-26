#ifndef MOD_TALENT_LOADOUTS_LOADER_H
#define MOD_TALENT_LOADOUTS_LOADER_H

void AddSC_talent_loadouts();

// Called automatically by the core module script loader (generated from the
// module directory name "mod-talent-loadouts" -> "mod_talent_loadouts").
void Addmod_talent_loadoutsScripts()
{
    AddSC_talent_loadouts();
}

#endif /* MOD_TALENT_LOADOUTS_LOADER_H */
