#include "db_save.h"
#include "db_registry.h"
#include "db_assetnames.h"

#include <bgame/bg_weapons.h>
#include <gfx_d3d/r_material.h>
#include <qcommon/cmd.h>
#include <qcommon/common.h>
#include <qcommon/threads.h>
#include <sound/snd_bank.h>
#include <sound/snd_utils.h>
#include <universal/com_memory.h>
#include <universal/q_shared.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <string.h>

// Tool mode: the writezone command loads source zones with their assets kept
// out of the database. DB_AddXAsset gives each asset a private copy of its
// header instead of linking it (no pool limits, no name clashes with the
// loaded zones, the world types never touch the live world globals), textures
// keep their load definitions and pixel data (Load_Texture drops them), and
// effects named by other effects keep the name (Load_FxEffectDefFromName looks
// it up). The writer (DB_SaveZone) then writes the chosen assets and everything
// they reach from those copies, and the zones are unloaded again.
//
// The new zone is meant to load after the multiplayer zones that stay loaded
// (code_pre_gfx_mp, code_post_gfx_mp, patch_mp, common_mp), which the tool
// loads normally to compare against:
//   - an asset they have by name is written as a stub (",name"), which the
//     loader resolves to theirs: the zone never replaces a multiplayer asset;
//   - a stub in the source zone (the zombie maps name what common_zombie
//     holds) is replaced by the real asset from the other source zones when
//     multiplayer lacks it;
//   - effects named by effects become root assets;
//   - the sound aliases the zone names (weapon sounds, notetrack sounds, an
//     alias's secondary) are cut from the source banks into one bank, unless
//     multiplayer has them;
//   - the localized strings it names (display names) come along, the source
//     zones' localized zone's (English here).

volatile bool g_dbSaveToolMode;

struct DBSaveToolAsset
{
    XAssetType type;
    XAssetHeader header;
};

static std::vector<DBSaveToolAsset> s_toolAssets;
static std::map<std::string, unsigned int> s_toolAssetIndex[ASSET_TYPE_COUNT];
static std::vector<char *> s_toolAllocs;
static std::map<std::string, snd_alias_list_t *> s_toolAliases;
static std::map<std::string, const void *> s_toolStubs[ASSET_TYPE_COUNT];

static const int DB_SAVE_MAX_ROOTS = 4096;
static XAsset s_roots[DB_SAVE_MAX_ROOTS];

static char s_bankName[64];
static SndBank *s_bank;
static std::set<std::string> s_bankAliases;
static std::set<std::string> s_wantedAliases;
static std::set<std::string> s_unresolved;
static std::set<std::string> s_wantedLocalize;
static std::set<std::string> s_multiplayerAliases;

static void *DB_SaveTool_Alloc(int size)
{
    char *p;

    p = (char *)Z_Malloc(size, "DB_SaveTool", 12);
    s_toolAllocs.push_back(p);
    return p;
}

static void DB_SaveTool_FreeAll()
{
    unsigned int i;
    int type;

    for (i = 0; i < s_toolAllocs.size(); ++i)
        Z_Free(s_toolAllocs[i], 12);
    s_toolAllocs.clear();
    s_toolAssets.clear();
    for (type = 0; type < ASSET_TYPE_COUNT; ++type)
    {
        s_toolAssetIndex[type].clear();
        s_toolStubs[type].clear();
    }
    s_toolAliases.clear();
    s_bank = NULL;
    s_bankAliases.clear();
    s_wantedAliases.clear();
    s_unresolved.clear();
    s_multiplayerAliases.clear();
    s_wantedLocalize.clear();
}

static bool DB_SaveTool_IsWorldType(XAssetType type)
{
    switch (type)
    {
    case ASSET_TYPE_CLIPMAP:
    case ASSET_TYPE_CLIPMAP_PVS:
    case ASSET_TYPE_COMWORLD:
    case ASSET_TYPE_GAMEWORLD_SP:
    case ASSET_TYPE_GAMEWORLD_MP:
    case ASSET_TYPE_MAP_ENTS:
    case ASSET_TYPE_GFXWORLD:
        return true;
    default:
        return false;
    }
}

// Called by DB_AddXAsset on the database thread while the tool loads a zone.
XAssetHeader DB_SaveTool_AddXAsset(XAssetType type, XAssetHeader header)
{
    DBSaveToolAsset asset;
    int size;

    // The world types are never written; their headers stay in temp memory.
    if (DB_SaveTool_IsWorldType(type))
        return header;
    size = DB_GetXAssetTypeSize(type);
    asset.type = type;
    asset.header.data = DB_SaveTool_Alloc(size);
    memcpy((void *)asset.header.data, (const void *)header.data, size);
    s_toolAssets.push_back(asset);
    return asset.header;
}

// Called by Load_Texture: the load definition sits in temp memory, which the
// next asset reuses, so the image gets a copy of it with its pixel data.
void DB_SaveTool_KeepTexture(GfxImage *image, const GfxImageLoadDef *loadDef)
{
    GfxImageLoadDef *copy;
    int size;

    size = (int)offsetof(GfxImageLoadDef, data) + loadDef->resourceSize;
    copy = (GfxImageLoadDef *)DB_SaveTool_Alloc(size);
    memcpy(copy, loadDef, size);
    image->texture.loadDef = copy;
}

static int DB_SaveTool_TypeForName(const char *name)
{
    int type;

    for (type = 0; type < ASSET_TYPE_COUNT; ++type)
    {
        if (!I_stricmp(g_assetNames[type], name))
            return type;
    }
    return -1;
}

static const char *DB_SaveTool_AssetName(XAssetType type, const void *header)
{
    XAsset asset;

    asset.type = type;
    asset.header.data = (void *)header;
    return DB_GetXAssetName(&asset);
}

static std::string DB_SaveTool_Key(const char *name)
{
    std::string key(name);
    unsigned int i;

    for (i = 0; i < key.size(); ++i)
        key[i] = (char)tolower((unsigned char)key[i]);
    return key;
}

// The loaded assets by type and name; the first zone given wins, stubs only
// when nothing else has the name.
static void DB_SaveTool_IndexAssets()
{
    const char *name;
    SndBank *bank;
    unsigned int i;
    unsigned int aliasIndex;
    std::string key;

    for (i = 0; i < s_toolAssets.size(); ++i)
    {
        name = DB_SaveTool_AssetName(s_toolAssets[i].type, (const void *)s_toolAssets[i].header.data);
        if (*name == ',')
        {
            s_toolStubs[s_toolAssets[i].type][DB_SaveTool_Key(name + 1)] = (const void *)s_toolAssets[i].header.data;
            continue;
        }
        key = DB_SaveTool_Key(name);
        if (!s_toolAssetIndex[s_toolAssets[i].type].count(key))
            s_toolAssetIndex[s_toolAssets[i].type][key] = i;
        if (s_toolAssets[i].type == ASSET_TYPE_SOUND)
        {
            bank = s_toolAssets[i].header.sound;
            for (aliasIndex = 0; aliasIndex < bank->aliasCount; ++aliasIndex)
            {
                key = DB_SaveTool_Key(bank->alias[aliasIndex].name);
                if (!s_toolAliases.count(key))
                    s_toolAliases[key] = &bank->alias[aliasIndex];
            }
        }
    }
}

static DBSaveToolAsset *DB_SaveTool_Find(XAssetType type, const char *name)
{
    std::map<std::string, unsigned int>::const_iterator it;

    it = s_toolAssetIndex[type].find(DB_SaveTool_Key(name));
    if (it == s_toolAssetIndex[type].end())
        return NULL;
    return &s_toolAssets[it->second];
}

// The multiplayer zones loaded normally (default entries don't count).
static bool DB_SaveTool_MultiplayerHas(XAssetType type, const char *name)
{
    XAssetEntryPoolEntry *entry;
    bool has;

    Sys_EnterCriticalSection(CRITSECT_DBHASH);
    entry = DB_FindXAssetEntry(type, name);
    has = entry && entry->entry.zoneIndex;
    Sys_LeaveCriticalSection(CRITSECT_DBHASH);
    return has;
}

struct DBSaveToolAliasSearch
{
    unsigned int id;
    bool found;
};

static void DB_SaveTool_FindAliasInBank(XAssetHeader header, void *data)
{
    DBSaveToolAliasSearch *search;
    snd_alias_list_t *list;

    search = (DBSaveToolAliasSearch *)data;
    if (!search->found && SND_FindInIndex(search->id, header.sound, &list))
        search->found = true;
}

static bool DB_SaveTool_MultiplayerHasAlias(const char *name)
{
    DBSaveToolAliasSearch search;

    search.id = SND_HashName(name);
    search.found = false;
    DB_EnumXAssets(ASSET_TYPE_SOUND, DB_SaveTool_FindAliasInBank, &search, 1);
    return search.found;
}

// A stub for an asset multiplayer has: the loader takes theirs.
static const void *DB_SaveTool_MakeStub(XAssetType type, const char *name)
{
    std::map<std::string, const void *>::const_iterator it;
    XAsset asset;
    char *stubName;
    std::string key;

    key = DB_SaveTool_Key(name);
    it = s_toolStubs[type].find(key);
    if (it != s_toolStubs[type].end())
        return it->second;
    stubName = (char *)DB_SaveTool_Alloc((int)strlen(name) + 2);
    stubName[0] = ',';
    strcpy(stubName + 1, name);
    asset.type = type;
    asset.header.data = DB_SaveTool_Alloc(DB_GetXAssetTypeSize(type));
    memset((void *)asset.header.data, 0, DB_GetXAssetTypeSize(type));
    DB_SetXAssetName(&asset, stubName);
    s_toolStubs[type][key] = (const void *)asset.header.data;
    return (const void *)asset.header.data;
}

static const void *DB_SaveTool_Resolve(XAssetType type, const void *header)
{
    DBSaveToolAsset *real;
    const char *name;

    // The cut bank and sound patches are never shared.
    if (type == ASSET_TYPE_SOUND || type == ASSET_TYPE_SOUND_PATCH)
        return header;
    name = DB_SaveTool_AssetName(type, header);
    if (*name == ',')
    {
        if (DB_SaveTool_MultiplayerHas(type, name + 1))
            return header;
        real = DB_SaveTool_Find(type, name + 1);
        if (!real)
        {
            s_unresolved.insert(std::string(g_assetNames[type]) + " " + (name + 1));
            return header;
        }
        return (const void *)real->header.data;
    }
    if (DB_SaveTool_MultiplayerHas(type, name))
        return DB_SaveTool_MakeStub(type, name);
    return header;
}

static bool DB_SaveTool_AddRoot(XAssetType type, XAssetHeader header, XAsset *roots, int *rootCount, int maxRoots)
{
    int i;

    for (i = 0; i < *rootCount; ++i)
    {
        if (roots[i].header.data == header.data)
            return false;
    }
    if (*rootCount >= maxRoots)
    {
        Com_PrintError(16, "writezone: more than %i root assets\n", maxRoots);
        return false;
    }
    roots[*rootCount].type = type;
    roots[*rootCount].header = header;
    ++*rootCount;
    return true;
}

static bool DB_SaveTool_AddNamed(XAssetType type, const char *name, XAsset *roots, int *rootCount, int maxRoots)
{
    DBSaveToolAsset *asset;

    if (DB_SaveTool_MultiplayerHas(type, name))
        return false;
    asset = DB_SaveTool_Find(type, name);
    if (!asset)
    {
        Com_Printf(16, "writezone: %s '%s' is in no source zone and not in multiplayer\n", g_assetNames[type], name);
        return false;
    }
    return DB_SaveTool_AddRoot(type, asset->header, roots, rootCount, maxRoots);
}

// A weapon's animations are named (szXAnims), not pointed to.
static void DB_SaveTool_AddWeaponRoots(WeaponVariantDef *weapon, XAsset *roots, int *rootCount)
{
    int i;
    const char *name;

    if (!weapon->szXAnims)
        return;
    for (i = 0; i < 66; ++i)
    {
        name = weapon->szXAnims[i];
        if (name && *name)
            DB_SaveTool_AddNamed(ASSET_TYPE_XANIMPARTS, name, roots, rootCount, DB_SAVE_MAX_ROOTS);
    }
}

static bool DB_SaveTool_RootBefore(const XAsset &a, const XAsset &b)
{
    if (a.type != b.type)
        return a.type < b.type;
    return I_stricmp(DB_GetXAssetName(&a), DB_GetXAssetName(&b)) < 0;
}

// The writer's coverage test: every asset of the source zones but the sound
// banks (the cut bank holds what the zone names) and what multiplayer has, in
// a fixed order, so writing the written zone again gives the same file.
static void DB_SaveTool_AddAllRoots(XAsset *roots, int *rootCount)
{
    std::map<std::string, unsigned int>::const_iterator it;
    int type;

    for (type = 0; type < ASSET_TYPE_COUNT; ++type)
    {
        if (type == ASSET_TYPE_SOUND)
            continue;
        for (it = s_toolAssetIndex[type].begin(); it != s_toolAssetIndex[type].end(); ++it)
        {
            if (!DB_SaveTool_MultiplayerHas((XAssetType)type, it->first.c_str()))
                DB_SaveTool_AddRoot((XAssetType)type, s_toolAssets[it->second].header, roots, rootCount, DB_SAVE_MAX_ROOTS);
        }
    }
    std::sort(roots, roots + *rootCount, DB_SaveTool_RootBefore);
}

// A string of the zone that names a sound alias or a localized string (a
// weapon's display name) of the source zones.
static void DB_SaveTool_WantString(const char *s, void *context)
{
    std::string key;

    key = DB_SaveTool_Key(s);
    if (s_toolAssetIndex[ASSET_TYPE_LOCALIZE_ENTRY].count(key)
        && !DB_SaveTool_MultiplayerHas(ASSET_TYPE_LOCALIZE_ENTRY, s))
    {
        s_wantedLocalize.insert(key);
    }
    if (!s_toolAliases.count(key))
        return;
    if (DB_SaveTool_MultiplayerHasAlias(s))
        s_multiplayerAliases.insert(key);
    else
        s_wantedAliases.insert(key);
}

// SND_FindInIndex: the alias's id modulo the count is the first slot, the rest
// of a chain sits in free slots (coalesced; every slot holds one alias).
static SndBank *DB_SaveTool_BuildBank(const std::set<std::string> &aliases)
{
    SndBank *bank;
    unsigned int count;
    unsigned int i;
    unsigned int slot;
    unsigned int freeSlot;
    std::vector<bool> placed;
    std::set<std::string>::const_iterator it;

    count = (unsigned int)aliases.size();
    bank = (SndBank *)DB_SaveTool_Alloc(sizeof(SndBank));
    memset(bank, 0, sizeof(SndBank));
    bank->name = s_bankName;
    bank->aliasCount = count;
    bank->alias = (snd_alias_list_t *)DB_SaveTool_Alloc(count * sizeof(snd_alias_list_t));
    bank->aliasIndex = (SndIndexEntry *)DB_SaveTool_Alloc(count * sizeof(SndIndexEntry));
    for (i = 0, it = aliases.begin(); it != aliases.end(); ++i, ++it)
        memcpy(&bank->alias[i], s_toolAliases[*it], sizeof(snd_alias_list_t));
    for (i = 0; i < count; ++i)
    {
        bank->aliasIndex[i].value = SND_BANK_INVALID_VALUE;
        bank->aliasIndex[i].next = SND_BANK_INVALID_VALUE;
    }
    placed.assign(count, false);
    for (i = 0; i < count; ++i)
    {
        slot = bank->alias[i].id % count;
        if (bank->aliasIndex[slot].value == SND_BANK_INVALID_VALUE)
        {
            bank->aliasIndex[slot].value = (unsigned __int16)i;
            placed[i] = true;
        }
    }
    freeSlot = 0;
    for (i = 0; i < count; ++i)
    {
        if (placed[i])
            continue;
        slot = bank->alias[i].id % count;
        while (bank->aliasIndex[slot].next != SND_BANK_INVALID_VALUE)
            slot = bank->aliasIndex[slot].next;
        while (bank->aliasIndex[freeSlot].value != SND_BANK_INVALID_VALUE)
            ++freeSlot;
        bank->aliasIndex[slot].next = (unsigned __int16)freeSlot;
        bank->aliasIndex[freeSlot].value = (unsigned __int16)i;
    }
    return bank;
}

static bool DB_SaveTool_DryRun(XAsset *roots, int *rootCount, int maxRoots)
{
    XAssetType type;
    const char *name;
    bool changed;
    int i;

    changed = false;
    for (i = 0; i < DB_SaveNamedAssetCount(); ++i)
    {
        name = DB_SaveNamedAsset(i, &type);
        if (DB_SaveTool_AddNamed(type, name, roots, rootCount, maxRoots))
            changed = true;
    }

    DB_SaveStringsSeen(DB_SaveTool_WantString, NULL);
    for (std::set<std::string>::const_iterator it = s_wantedLocalize.begin(); it != s_wantedLocalize.end(); ++it)
    {
        if (DB_SaveTool_AddRoot(ASSET_TYPE_LOCALIZE_ENTRY, DB_SaveTool_Find(ASSET_TYPE_LOCALIZE_ENTRY, it->c_str())->header,
                                roots, rootCount, maxRoots))
            changed = true;
    }
    if (s_wantedAliases.size() > 0xFFFE)
    {
        Com_PrintError(16, "writezone: too many sound aliases\n");
        return changed;
    }
    if (s_wantedAliases != s_bankAliases)
    {
        s_bankAliases = s_wantedAliases;
        s_bank = DB_SaveTool_BuildBank(s_bankAliases);
        for (i = 0; i < *rootCount && roots[i].type != ASSET_TYPE_SOUND; ++i)
            ;
        if (i < *rootCount)
            roots[i].header.sound = s_bank;
        else
            DB_SaveTool_AddRoot(ASSET_TYPE_SOUND, XAssetHeader(s_bank), roots, rootCount, maxRoots);
        changed = true;
    }
    return changed;
}

// Each source zone gets its own flag pair (DB_LoadXAssets adds the localized
// zone with the pair's lower flag), so the zones free in the reverse of their
// load order (physical memory is a stack): 0x4000000 first, unloaded last.
static const int s_toolZoneFlags[3] = { 0x4000000, 0x1000000, 0x400000 };

static bool DB_SaveTool_LoadZones(const char *zones)
{
    XZoneInfo zoneInfo[3];
    char names[3][64];
    const char *start;
    const char *end;
    unsigned int count;
    int len;

    count = 0;
    for (start = zones; *start; start = *end ? end + 1 : end)
    {
        if (count == 3)
        {
            Com_PrintError(16, "writezone: at most 3 source zones\n");
            return false;
        }
        end = strchr(start, ',');
        if (!end)
            end = start + strlen(start);
        len = (int)(end - start) + 1;
        I_strncpyz(names[count], start, len < 64 ? len : 64);
        zoneInfo[count].name = names[count];
        zoneInfo[count].allocFlags = s_toolZoneFlags[count];
        zoneInfo[count].freeFlags = 0;
        ++count;
    }
    DB_LoadXAssets(zoneInfo, count, 1);
    return true;
}

static void DB_SaveTool_UnloadZones()
{
    XZoneInfo zoneInfo;

    zoneInfo.name = 0;
    zoneInfo.allocFlags = 0;
    zoneInfo.freeFlags = s_toolZoneFlags[0];
    DB_LoadXAssets(&zoneInfo, 1, 1);
}

// The zones a multiplayer game keeps loaded: the tool compares against them.
static void DB_SaveTool_LoadCommon()
{
    XZoneInfo zoneInfo;

    if (DB_IsZoneLoaded("common_mp"))
        return;
    zoneInfo.name = "common_mp";
    zoneInfo.allocFlags = 256;
    zoneInfo.freeFlags = 0;
    DB_LoadXAssets(&zoneInfo, 1, 1);
}

static void DB_SaveTool_PrintZone(const char *path, const DBSaveStats *stats, int rootCount)
{
    const XAsset *written;
    const GfxImage *image;
    const SndBank *bank;
    int writtenCount;
    int i;

    Com_Printf(0, "writezone: wrote '%s': %u bytes (stream %u), %i roots, %u script strings\n",
               path, stats->fileSize, stats->streamSize, rootCount, stats->stringCount);
    Com_Printf(0, "writezone: blocks %u %u %u %u %u %u %u\n", stats->blockSize[0], stats->blockSize[1],
               stats->blockSize[2], stats->blockSize[3], stats->blockSize[4], stats->blockSize[5], stats->blockSize[6]);
    for (i = 0; i < ASSET_TYPE_COUNT; ++i)
    {
        if (stats->assetCount[i])
            Com_Printf(0, "writezone:   %-16s %u\n", g_assetNames[i], stats->assetCount[i]);
    }
    written = DB_SaveWrittenAssets(&writtenCount);
    for (i = 0; i < writtenCount; ++i)
    {
        if (written[i].type == ASSET_TYPE_IMAGE && written[i].header.image->name[0] != ',')
        {
            image = written[i].header.image;
            Com_Printf(16, "writezone: image %s %ix%i, %i bytes in the zone%s\n", (const char *)image->name,
                       image->width, image->height,
                       image->texture.loadDef ? image->texture.loadDef->resourceSize : -1,
                       image->streaming ? ", streamed" : "");
        }
        else if (written[i].type == ASSET_TYPE_SOUND)
        {
            bank = written[i].header.sound;
            Com_Printf(16, "writezone: sound bank %s, %u aliases\n", (const char *)bank->name, bank->aliasCount);
            for (unsigned int a = 0; a < bank->aliasCount; ++a)
                Com_Printf(16, "writezone:   alias %s (%i variants)\n", (const char *)bank->alias[a].name,
                           bank->alias[a].count);
        }
        else
        {
            Com_Printf(16, "writezone: %s %s\n", g_assetNames[written[i].type], DB_GetXAssetName(&written[i]));
        }
    }
    for (std::set<std::string>::const_iterator it = s_multiplayerAliases.begin(); it != s_multiplayerAliases.end(); ++it)
        Com_Printf(16, "writezone: sound alias %s: multiplayer's\n", it->c_str());
    for (std::set<std::string>::const_iterator it = s_unresolved.begin(); it != s_unresolved.end(); ++it)
        Com_PrintWarning(16, "writezone: stub %s is in no source zone and not in multiplayer (a default asset)\n",
                         it->c_str());
}

// writezone <output .ff> <source zone>[,<zone>...] <type> <name> [<type> <name> ...]
// Loads the source zones (up to 3, with their localized zones) in tool mode,
// writes the named assets with everything they reach into a new zone file,
// and unloads them. Types are the asset type names (weapon, xmodel, xanim,
// material, fx, ...); a weapon brings its animations. "all" in place of the
// assets writes every asset of the source zones (the writer's coverage test).
static void DB_SaveTool_WriteZone_f()
{
    DBSaveStats stats;
    DBSaveToolAsset *asset;
    char zones[256];
    char path[256];
    const char *base;
    int rootCount;
    int argIndex;
    int type;
    bool ok;

    if (Cmd_Argc() != 4 && (Cmd_Argc() < 5 || (Cmd_Argc() & 1) == 0))
    {
        Com_Printf(0, "usage: writezone <output .ff> <source zone>[,<zone>...] <type> <name> [<type> <name> ...]\n");
        Com_Printf(0, "       writezone <output .ff> <source zone>[,<zone>...] all\n");
        return;
    }
    // A client would create the source zones' shaders and world vertex buffers.
    if (!IsDedicatedServer())
    {
        Com_Printf(0, "writezone runs on a dedicated server (+set dedicated 1)\n");
        return;
    }
    if (g_dbSaveToolMode)
        return;
    I_strncpyz(path, Cmd_Argv(1), sizeof(path));
    I_strncpyz(zones, Cmd_Argv(2), sizeof(zones));
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    I_strncpyz(s_bankName, base, sizeof(s_bankName));
    if (strstr(s_bankName, ".ff"))
        *strstr(s_bankName, ".ff") = 0;
    I_strncat(s_bankName, sizeof(s_bankName), ".all");

    DB_SaveTool_LoadCommon();
    DB_SaveTool_FreeAll();
    g_dbSaveToolMode = 1;
    ok = DB_SaveTool_LoadZones(zones);
    g_dbSaveToolMode = 0;
    DB_SaveTool_IndexAssets();
    Com_Printf(0, "writezone: '%s' has %i assets, %i sound aliases\n", zones, (int)s_toolAssets.size(),
               (int)s_toolAliases.size());

    rootCount = 0;
    if (ok && Cmd_Argc() == 4 && !I_stricmp(Cmd_Argv(3), "all"))
        DB_SaveTool_AddAllRoots(s_roots, &rootCount);
    for (argIndex = 3; ok && argIndex + 1 < Cmd_Argc(); argIndex += 2)
    {
        type = DB_SaveTool_TypeForName(Cmd_Argv(argIndex));
        if (type < 0)
        {
            Com_PrintError(16, "writezone: unknown asset type '%s'\n", Cmd_Argv(argIndex));
            ok = false;
            break;
        }
        asset = DB_SaveTool_Find((XAssetType)type, Cmd_Argv(argIndex + 1));
        if (!asset)
        {
            Com_PrintError(16, "writezone: no %s '%s' in '%s'\n", g_assetNames[type], Cmd_Argv(argIndex + 1), zones);
            ok = false;
            break;
        }
        DB_SaveTool_AddRoot(asset->type, asset->header, s_roots, &rootCount, DB_SAVE_MAX_ROOTS);
        if (type == ASSET_TYPE_WEAPON)
            DB_SaveTool_AddWeaponRoots(asset->header.weapon, s_roots, &rootCount);
    }

    if (ok)
    {
        memset(&stats, 0, sizeof(stats));
        ok = DB_SaveZone(path, s_roots, &rootCount, DB_SAVE_MAX_ROOTS, DB_SaveTool_Resolve, DB_SaveTool_DryRun, &stats);
        if (ok)
            DB_SaveTool_PrintZone(path, &stats, rootCount);
    }

    DB_SaveTool_UnloadZones();
    DB_SaveTool_FreeAll();
}

static cmd_function_s DB_SaveTool_WriteZone_f_VAR;

void DB_SaveTool_RegisterCommands()
{
    Cmd_AddCommandInternal("writezone", DB_SaveTool_WriteZone_f, &DB_SaveTool_WriteZone_f_VAR);
}
