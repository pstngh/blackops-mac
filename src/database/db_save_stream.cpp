#include "db_save.h"
#include "db_registry.h"
#include "db_assetnames.h"
#include "db_file_load.h"

#include <clientscript/cscr_stringlist.h>
#include <qcommon/common.h>
#include <zlib/zlib.h>

#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// The fastfile writer's runtime. The generated Save_* functions (db_save.cpp)
// walk loaded objects in the loader's order; this file keeps what the loader
// keeps while it reads (the stream position of each of the 7 blocks, the
// push/pop stack, db_stream.cpp), records where each source range was written,
// and patches the copies' pointer fields with the values the loader converts
// back (db_stream_load.cpp):
//   0                       null
//   0xFFFFFFFF              the data follows in the stream
//   0xFFFFFFFE              an asset follows; DB_InsertPointer keeps a 4-byte
//                           slot in block 4 that gets the asset's final header
//   ((block << 29) | offset) + 1
//                           DB_ConvertOffsetToPointer: data already in a block;
//                           for asset pointers DB_ConvertOffsetToAlias reads
//                           the header out of such a slot
// File: "IWffu100", version 473, then one zlib stream: XFile (36 bytes: sizes),
// XAssetList (16), the stream, and the delayed blocks 2-3 (Load_DelayStream).

namespace dbsave
{
enum SaveDest
{
    SAVE_DEST_NONE,     // block 1: zeroed by the loader, not in the file
    SAVE_DEST_STREAM,
    SAVE_DEST_DELAYED,  // blocks 2-3: read after the stream
};

struct SaveRange
{
    unsigned int size;
    unsigned int block;
    unsigned int offset;
    SaveDest dest;
    unsigned int destOffset;
};

struct SaveStreamPos
{
    unsigned int index;
    unsigned int pos;
};

static unsigned int s_pos[7];
static unsigned int s_maxPos[7];
static unsigned int s_index;
static SaveStreamPos s_posStack[64];
static unsigned int s_posStackIndex;

static std::vector<unsigned char> s_stream;
static std::vector<unsigned char> s_delayed;
static std::map<uintptr_t, SaveRange> s_ranges;

static std::map<const void *, unsigned int> s_assets;
static std::vector<XAsset> s_written;
static std::set<const void *> s_assetsInProgress;
static unsigned int s_assetCount[ASSET_TYPE_COUNT];

static std::vector<std::string> s_strings;
static std::map<std::string, unsigned int> s_stringIndex;
static bool s_stringsFixed;

struct SaveNameRef
{
    int root;
    int type;
    std::string name;
};

static std::set<std::pair<int, std::string>> s_named;
static std::vector<SaveNameRef> s_nameRefs;
static int s_root;
static std::set<std::string> s_seen;
static DBSaveResolveFn s_resolve;

static bool s_failed;

static void DB_SaveFail(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    if (s_failed)
        return;
    s_failed = true;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    Com_PrintError(16, "writezone: %s\n", msg);
}

static void DB_SaveResetPass()
{
    memset(s_pos, 0, sizeof(s_pos));
    memset(s_maxPos, 0, sizeof(s_maxPos));
    memset(s_assetCount, 0, sizeof(s_assetCount));
    s_index = 0;
    s_posStackIndex = 0;
    s_stream.clear();
    s_delayed.clear();
    s_ranges.clear();
    s_assets.clear();
    s_written.clear();
    s_assetsInProgress.clear();
    s_named.clear();
    s_nameRefs.clear();
    s_root = -1;
    s_seen.clear();
    s_failed = false;
}

static void DB_SaveSetStreamIndex(unsigned int index)
{
    s_index = index;
}

void DB_SavePushStreamPos(unsigned int index)
{
    if (index >= 7 || s_posStackIndex >= ARRAY_COUNT(s_posStack))
    {
        DB_SaveFail("bad stream push %u (depth %u)", index, s_posStackIndex);
        return;
    }
    s_posStack[s_posStackIndex].index = s_index;
    DB_SaveSetStreamIndex(index);
    s_posStack[s_posStackIndex++].pos = s_pos[s_index];
}

void DB_SavePopStreamPos()
{
    if (!s_posStackIndex)
    {
        DB_SaveFail("stream pop without push");
        return;
    }
    --s_posStackIndex;
    // The temp block rewinds: what an asset loaded there was copied out.
    if (!s_index)
        s_pos[0] = s_posStack[s_posStackIndex].pos;
    DB_SaveSetStreamIndex(s_posStack[s_posStackIndex].index);
}

// DB_AllocStreamPos aligns the address; blocks start 4096-aligned
// (DB_MemAlloc), so aligning the offset is the same.
void DB_SaveAllocStreamPos(int alignment)
{
    s_pos[s_index] = (s_pos[s_index] + alignment) & ~(unsigned int)alignment;
    if (s_pos[s_index] > s_maxPos[s_index])
        s_maxPos[s_index] = s_pos[s_index];
}

static void DB_SaveIncStreamPos(unsigned int size)
{
    s_pos[s_index] += size;
    if (s_pos[s_index] > s_maxPos[s_index])
        s_maxPos[s_index] = s_pos[s_index];
}

static void DB_SaveAddRange(const void *src, unsigned int size, SaveDest dest, unsigned int destOffset)
{
    SaveRange range;

    range.size = size;
    range.block = s_index;
    range.offset = s_pos[s_index];
    range.dest = dest;
    range.destOffset = destOffset;
    s_ranges[(uintptr_t)src] = range;
}

void Save_Stream(bool atStreamStart, const void *ptr, int size)
{
    const unsigned char *src = (const unsigned char *)ptr;

    if (!atStreamStart || !size)
        return;
    if (size < 0)
    {
        DB_SaveFail("negative size %d", size);
        return;
    }
    if (s_index == 1)
    {
        DB_SaveAddRange(src, size, SAVE_DEST_NONE, 0);
    }
    else if (s_index == 2 || s_index == 3)
    {
        DB_SaveAddRange(src, size, SAVE_DEST_DELAYED, (unsigned int)s_delayed.size());
        s_delayed.insert(s_delayed.end(), src, src + size);
    }
    else
    {
        DB_SaveAddRange(src, size, SAVE_DEST_STREAM, (unsigned int)s_stream.size());
        s_stream.insert(s_stream.end(), src, src + size);
    }
    DB_SaveIncStreamPos(size);
}

// The range a source address was last written to: the closest range starting
// at or before it. Nested copies are rare and close, so a few steps back do.
static const SaveRange *DB_SaveFindRange(const void *ptr, unsigned int size, bool allowTemp, uintptr_t *start)
{
    uintptr_t addr = (uintptr_t)ptr;
    std::map<uintptr_t, SaveRange>::const_iterator it;
    int steps;

    it = s_ranges.upper_bound(addr);
    for (steps = 0; steps < 8 && it != s_ranges.begin(); ++steps)
    {
        --it;
        if (addr - it->first + size <= it->second.size && (allowTemp || it->second.block))
        {
            *start = it->first;
            return &it->second;
        }
    }
    return NULL;
}

static void DB_SavePatch(const void *field, const void *value, unsigned int size)
{
    const SaveRange *range;
    uintptr_t start;
    unsigned int at;

    range = DB_SaveFindRange(field, size, true, &start);
    if (!range)
    {
        DB_SaveFail("pointer field %p was not written", field);
        return;
    }
    at = range->destOffset + (unsigned int)((uintptr_t)field - start);
    if (range->dest == SAVE_DEST_STREAM)
        memcpy(&s_stream[at], value, size);
    else if (range->dest == SAVE_DEST_DELAYED)
        memcpy(&s_delayed[at], value, size);
}

void DB_SaveSetPointer(const void *field, unsigned int value)
{
    DB_SavePatch(field, &value, 4);
}

static void DB_SaveSetShort(const void *field, unsigned __int16 value)
{
    DB_SavePatch(field, &value, 2);
}

static unsigned int DB_SaveAliasValue(unsigned int block, unsigned int offset)
{
    return ((block << 29) | offset) + 1;
}

bool DB_SaveAlias(const void *field, const void *target, unsigned int size)
{
    const SaveRange *range;
    uintptr_t start;

    range = DB_SaveFindRange(target, size ? size : 1, false, &start);
    if (range)
    {
        DB_SaveSetPointer(field, DB_SaveAliasValue(range->block, range->offset + (unsigned int)((uintptr_t)target - start)));
        return true;
    }
    DB_SaveSetPointer(field, 0xFFFFFFFF);
    return false;
}

const void *DB_SaveResolveAsset(XAssetType type, const void *header)
{
    return s_resolve ? s_resolve(type, header) : header;
}

bool DB_SaveAssetAlias(XAssetType type, const void *field, const void *header)
{
    std::map<const void *, unsigned int>::const_iterator it;

    it = s_assets.find(header);
    if (it != s_assets.end())
    {
        DB_SaveSetPointer(field, it->second);
        return true;
    }
    if (s_assetsInProgress.count(header))
    {
        DB_SaveFail("asset %p of type %s refers to itself", header, DB_GetXAssetTypeName(type));
        DB_SaveSetPointer(field, 0);
        return true;
    }
    s_assetsInProgress.insert(header);
    if ((unsigned int)type < ASSET_TYPE_COUNT)
        ++s_assetCount[type];
    s_written.push_back(XAsset());
    s_written.back().type = type;
    s_written.back().header.data = (void *)header;
    DB_SaveSetPointer(field, 0xFFFFFFFE);
    return false;
}

void DB_SaveAssetWritten(const void *header, unsigned int inserted)
{
    s_assetsInProgress.erase(header);
    s_assets[header] = DB_SaveAliasValue(4, inserted);
}

// DB_InsertPointer: a 4-byte slot in block 4 that is not in the file.
unsigned int DB_SaveInsertPointer()
{
    unsigned int slot;

    DB_SavePushStreamPos(4);
    DB_SaveAllocStreamPos(3);
    slot = s_pos[s_index];
    DB_SaveIncStreamPos(4);
    DB_SavePopStreamPos();
    return slot;
}

// Load_XStringCustom: the bytes up to the terminator come straight from the
// file, whatever the block (also 1-3).
static void DB_SaveStringBytes(const char *s)
{
    unsigned int len;

    len = (unsigned int)strlen(s) + 1;
    DB_SaveAddRange(s, len, SAVE_DEST_STREAM, (unsigned int)s_stream.size());
    s_stream.insert(s_stream.end(), (const unsigned char *)s, (const unsigned char *)s + len);
    DB_SaveIncStreamPos(len);
}

void __cdecl Save_XString(bool atStreamStart)
{
    const char *s;

    Save_Stream(atStreamStart, varXString, 4);
    if (*varXString)
    {
        s = *varXString;
        s_seen.insert(s);
        if (!DB_SaveAlias(varXString, s, (unsigned int)strlen(s) + 1))
        {
            DB_SaveAllocStreamPos(0);
            DB_SaveStringBytes(s);
        }
    }
}

static unsigned int DB_SaveStringIndex(const char *s)
{
    std::map<std::string, unsigned int>::const_iterator it;

    it = s_stringIndex.find(s);
    if (it != s_stringIndex.end())
        return it->second;
    if (s_stringsFixed)
    {
        DB_SaveFail("script string '%s' was not seen in the first pass", s);
        return 0;
    }
    s_strings.push_back(s);
    s_stringIndex[s] = (unsigned int)s_strings.size() - 1;
    return (unsigned int)s_strings.size() - 1;
}

// Load_ScriptStringCustom: the field holds an index into the zone's string list.
void Save_ScriptStringCustom(unsigned __int16 *var)
{
    unsigned int index;
    const char *s;

    index = 0;
    if (*var)
    {
        s = SL_ConvertToString(*var, SCRIPTINSTANCE_SERVER);
        s_seen.insert(s);
        index = DB_SaveStringIndex(s);
    }
    if (index > 0xFFFF)
        DB_SaveFail("too many script strings");
    DB_SaveSetShort(var, (unsigned __int16)index);
}

// Tool mode keeps effect names (Load_FxEffectDefFromName looks them up at load).
void DB_SaveFxEffectDefFromName(Ptr32<const char> *name)
{
    SaveNameRef ref;

    if (*name)
    {
        s_named.insert(std::make_pair((int)ASSET_TYPE_FX, std::string((const char *)*name)));
        ref.root = s_root;
        ref.type = ASSET_TYPE_FX;
        ref.name = (const char *)*name;
        s_nameRefs.push_back(ref);
    }
}

// Load_XAssetListCustom and Load_XAssetArrayCustom (db_file_load.cpp).
static void DB_SaveXAssetList(XAssetList *list, XAsset *roots, int rootCount)
{
    std::vector<unsigned int> slots;
    unsigned int i;
    int assetIndex;

    list->stringList.count = (int)s_strings.size();
    Ptr32_SetRaw(list->stringList.strings, s_strings.size() ? 0xFFFFFFFF : 0);
    list->assetCount = rootCount;
    Ptr32_SetRaw(list->assets, rootCount ? 0xFFFFFFFF : 0);

    DB_SavePushStreamPos(4);
    DB_SavePushStreamPos(4);
    if (s_strings.size())
    {
        // Entry 0 is the null string: index 0 means no string.
        DB_SaveAllocStreamPos(3);
        slots.assign(s_strings.size(), 0xFFFFFFFF);
        slots[0] = 0;
        Save_Stream(1, slots.data(), 4 * (int)slots.size());
        for (i = 1; i < s_strings.size(); ++i)
        {
            DB_SaveAllocStreamPos(0);
            DB_SaveStringBytes(s_strings[i].c_str());
        }
    }
    DB_SavePopStreamPos();
    DB_SavePopStreamPos();

    DB_SavePushStreamPos(4);
    if (rootCount)
    {
        DB_SaveAllocStreamPos(3);
        Save_Stream(1, roots, 8 * rootCount);
        for (assetIndex = 0; assetIndex < rootCount && !s_failed; ++assetIndex)
        {
            varXAsset = &roots[assetIndex];
            s_root = assetIndex;
            Save_XAsset(0);
        }
    }
    DB_SavePopStreamPos();
    if (s_posStackIndex)
        DB_SaveFail("stream stack not empty at the end (%u)", s_posStackIndex);
}
} // namespace dbsave

using namespace dbsave;

static bool DB_SaveWriteFile(const char *path, const XAssetList *list, DBSaveStats *stats)
{
    XFile file;
    std::vector<unsigned char> data;
    std::vector<unsigned char> packed;
    uLongf packedSize;
    unsigned int version;
    FILE *f;
    int i;
    int err;

    memset(&file, 0, sizeof(file));
    file.size = (unsigned int)(sizeof(XAssetList) + s_stream.size() + s_delayed.size());
    file.externalSize = 0;
    for (i = 0; i < 7; ++i)
        file.blockSize[i] = s_maxPos[i];
    static_assert(sizeof(XFile) == 36, "XFile");
    static_assert(sizeof(XAssetList) == 16, "XAssetList");

    data.reserve(sizeof(file) + file.size);
    data.insert(data.end(), (const unsigned char *)&file, (const unsigned char *)&file + sizeof(file));
    data.insert(data.end(), (const unsigned char *)list, (const unsigned char *)list + sizeof(*list));
    data.insert(data.end(), s_stream.begin(), s_stream.end());
    data.insert(data.end(), s_delayed.begin(), s_delayed.end());

    packedSize = compressBound((uLong)data.size());
    packed.resize(packedSize);
    err = compress2(packed.data(), &packedSize, data.data(), (uLong)data.size(), Z_BEST_COMPRESSION);
    if (err != Z_OK)
    {
        Com_PrintError(16, "writezone: compress failed (%i)\n", err);
        return false;
    }

    f = fopen(path, "wb");
    if (!f)
    {
        Com_PrintError(16, "writezone: can't write '%s'\n", path);
        return false;
    }
    version = 473;
    fwrite("IWffu100", 1, 8, f);
    fwrite(&version, 4, 1, f);
    fwrite(packed.data(), 1, packedSize, f);
    if (fclose(f))
    {
        Com_PrintError(16, "writezone: error writing '%s'\n", path);
        return false;
    }

    if (stats)
    {
        stats->fileSize = 12 + (unsigned int)packedSize;
        stats->streamSize = file.size;
        for (i = 0; i < 7; ++i)
            stats->blockSize[i] = file.blockSize[i];
        memcpy(stats->assetCount, s_assetCount, sizeof(stats->assetCount));
        stats->stringCount = (unsigned int)s_strings.size();
    }
    return true;
}

const XAsset *DB_SaveWrittenAssets(int *count)
{
    *count = (int)s_written.size();
    return s_written.data();
}

int DB_SaveNamedAssetCount()
{
    return (int)s_named.size();
}

const char *DB_SaveNamedAsset(int index, XAssetType *type)
{
    std::set<std::pair<int, std::string>>::const_iterator it;

    it = s_named.begin();
    std::advance(it, index);
    *type = (XAssetType)it->first;
    return it->second.c_str();
}

int DB_SaveNameRefCount()
{
    return (int)s_nameRefs.size();
}

const char *DB_SaveNameRef(int index, XAssetType *type, int *root)
{
    *type = (XAssetType)s_nameRefs[index].type;
    *root = s_nameRefs[index].root;
    return s_nameRefs[index].name.c_str();
}

void DB_SaveStringsSeen(void (*fn)(const char *s, void *context), void *context)
{
    std::set<std::string>::const_iterator it;

    for (it = s_seen.begin(); it != s_seen.end(); ++it)
        fn(it->c_str(), context);
}

bool DB_SaveZone(const char *path, XAsset *roots, int *rootCount, int maxRoots, DBSaveResolveFn resolve,
                 DBSaveDryRunFn dryRun, DBSaveStats *stats)
{
    XAssetList list;
    bool ok;
    int pass;

    // Dry runs collect the script strings (their list precedes the assets)
    // and let the caller add what the zone names, until the roots settle.
    s_resolve = resolve;
    s_strings.clear();
    s_stringIndex.clear();
    s_strings.push_back(std::string());
    s_stringsFixed = false;
    for (pass = 0; ; ++pass)
    {
        DB_SaveResetPass();
        DB_SaveXAssetList(&list, roots, *rootCount);
        if (s_failed)
        {
            s_resolve = NULL;
            return false;
        }
        if (!dryRun || !dryRun(roots, rootCount, maxRoots))
            break;
        if (pass > 64)
        {
            Com_PrintError(16, "writezone: the roots keep changing\n");
            s_resolve = NULL;
            return false;
        }
    }

    s_stringsFixed = true;
    DB_SaveResetPass();
    DB_SaveXAssetList(&list, roots, *rootCount);
    ok = !s_failed && DB_SaveWriteFile(path, &list, stats);
    s_resolve = NULL;
    return ok;
}
