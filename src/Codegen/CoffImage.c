#include "Codegen/CoffImage.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) || !(defined(_M_X64) || defined(__x86_64__))

/* Native wrapper images are a Windows x64 mechanism; everywhere else the
   loader is a loud no-op so call sites can compile unconditionally. */

CoffImage* CoffImageLoad(const void* objectData, size_t objectSize,
                         const CoffExtern* externs, size_t externCount,
                         char** errorMessage)
{
    (void)objectData;
    (void)objectSize;
    (void)externs;
    (void)externCount;

    if (errorMessage)
    {
        *errorMessage = strdup("CoffImage: native wrapper images require Windows x64");
    }

    return NULL;
}

void* CoffImageGetSymbol(const CoffImage* image, const char* name)
{
    (void)image;
    (void)name;
    return NULL;
}

void CoffImageDestroy(CoffImage* image)
{
    (void)image;
}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* --- COFF x64 object parsing (byte-level reads; object data may be
   unaligned) ----------------------------------------------------------- */

#define COFF_MACHINE_AMD64 0x8664

#define REL_AMD64_ABSOLUTE  0
#define REL_AMD64_ADDR64    1
#define REL_AMD64_ADDR32    2
#define REL_AMD64_ADDR32NB  3
#define REL_AMD64_REL32     4 /* REL32_N family: 4..8, N = type - 4 */
#define REL_AMD64_REL32_4   8
#define REL_AMD64_SECTION   10
#define REL_AMD64_SECREL    11

#define SCN_CNT_UNINITIALIZED_DATA 0x00000080u
#define SCN_MEM_DISCARDABLE        0x02000000u
#define SCN_LNK_REMOVE             0x00000800u

#define SYM_CLASS_EXTERNAL 2

typedef struct
{
    const char* name;
    size_t offset;
} CoffSymEntry;

struct CoffImage
{
    unsigned char* base;
    size_t size;
    PVOID functionTables[8]; /* RtlAddFunctionTable handles */
    size_t functionTableCount;
    CoffSymEntry* symbols;
    size_t symbolCount;
};

typedef struct
{
    const unsigned char* data;
    size_t size;

    const unsigned char* stringTable;
    size_t stringTableSize;

    size_t* sectionOffsets; /* per section: offset of contents in the image */
    size_t sectionCount;
} CoffObject;

static uint16_t Rd16(const unsigned char* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t Rd32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ntdll's dynamic function table API. Resolved dynamically: MinGW's winnt.h
   declares RtlAddFunctionTable with a BOOLEAN return while the real export
   returns a PVOID handle (needed for removal), so header-based linkage is
   not portable across SDK vintages. */
typedef PVOID (*StrataAddFunctionTableFn)(PVOID, DWORD, ULONG64);
typedef BOOLEAN (*StrataDeleteFunctionTableFn)(PVOID);

static PVOID StrataAddFunctionTable(PVOID table, DWORD count, ULONG64 base)
{
    static StrataAddFunctionTableFn fn;

    if (!fn)
    {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        fn = (StrataAddFunctionTableFn)(void*)GetProcAddress(ntdll, "RtlAddFunctionTable");
    }

    return fn ? fn(table, count, base) : NULL;
}

static void StrataDeleteFunctionTable(PVOID handle)
{
    static StrataDeleteFunctionTableFn fn;

    if (!fn)
    {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        fn = (StrataDeleteFunctionTableFn)(void*)GetProcAddress(ntdll, "RtlDeleteFunctionTable");
    }

    if (fn)
    {
        fn(handle);
    }
}

static void Fail(char** errorMessage, const char* fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);

    if (errorMessage && !*errorMessage)
    {
        *errorMessage = strdup(buf);
    }
}

/* Section header field offsets (40-byte IMAGE_SECTION_HEADER: Name[8],
   VirtualSize@8, VirtualAddress@12, SizeOfRawData@16, PointerToRawData@20,
   PointerToRelocations@24, PointerToLinenumbers@28, NumberOfRelocations@32,
   NumberOfLinenumbers@34, Characteristics@36). */
static const unsigned char* SectionAt(const CoffObject* obj, size_t i)
{
    return obj->data + 20 + i * 40;
}

static uint32_t SectionSize(const unsigned char* sh)
{
    uint32_t raw = Rd32(sh + 16);
    uint32_t virt = Rd32(sh + 8);
    return raw ? raw : virt;
}

static uint32_t SectionAlignment(const unsigned char* sh)
{
    uint32_t exponent = (Rd32(sh + 36) >> 20) & 0xF;
    return 1u << exponent;
}

static bool SectionIsSkippable(const unsigned char* sh)
{
    uint32_t chars = Rd32(sh + 36);
    return (chars & SCN_LNK_REMOVE) != 0 || (chars & SCN_MEM_DISCARDABLE) != 0;
}

/* Section name, honoring the "/offset" long-name convention. */
static const char* SectionName(const CoffObject* obj, const unsigned char* sh, char buf[9])
{
    if (sh[0] == '/')
    {
        uint32_t off = 0;

        for (int i = 1; i < 8 && sh[i] >= '0' && sh[i] <= '9'; i++)
        {
            off = off * 10 + (uint32_t)(sh[i] - '0');
        }

        if (off + 1 < obj->stringTableSize)
        {
            return (const char*)obj->stringTable + off;
        }

        buf[0] = '\0';
        return buf;
    }

    memcpy(buf, sh, 8);
    buf[8] = '\0';
    return buf;
}

/* Symbol entry i (18 bytes each; aux records skipped by the caller). */
static const unsigned char* SymbolAt(const CoffObject* obj, size_t i)
{
    return obj->data + Rd32(obj->data + 8) + i * 18;
}

static const char* SymbolName(const CoffObject* obj, const unsigned char* sym, char buf[9])
{
    if (Rd32(sym) == 0)
    {
        uint32_t off = Rd32(sym + 4);

        if (off + 1 < obj->stringTableSize)
        {
            return (const char*)obj->stringTable + off;
        }

        buf[0] = '\0';
        return buf;
    }

    memcpy(buf, sym, 8); /* 8-byte names are not NUL terminated in the table */
    buf[8] = '\0';
    return buf;
}

/* Resolves symbol `i` to an absolute address; externals come from the
   provided table (and are reported via extIndex so callers can reroute
   rel32 calls through the trampoline area). Returns false on failure. */
static bool ResolveSymbol(const CoffObject* obj, size_t i, const CoffExtern* externs, size_t externCount,
                          unsigned char* imageBase, uintptr_t* out, size_t* extIndex, char** err)
{
    const unsigned char* sym = SymbolAt(obj, i);
    int32_t sectionNumber = (int16_t)Rd16(sym + 12);
    uint32_t value = Rd32(sym + 8);
    uint8_t storageClass = sym[16];

    if (sectionNumber > 0)
    {
        if ((size_t)sectionNumber > obj->sectionCount)
        {
            Fail(err, "CoffImage: symbol references bad section %d", sectionNumber);
            return false;
        }

        *extIndex = (size_t)-1;
        *out = (uintptr_t)(imageBase + obj->sectionOffsets[sectionNumber - 1] + value);
        return true;
    }

    if (sectionNumber == 0 && storageClass == SYM_CLASS_EXTERNAL)
    {
        char nameBuf[9];
        const char* name = SymbolName(obj, sym, nameBuf);

        for (size_t e = 0; e < externCount; e++)
        {
            if (strcmp(externs[e].name, name) == 0)
            {
                *extIndex = e;
                *out = (uintptr_t)externs[e].address;
                return true;
            }
        }

        Fail(err, "CoffImage: unresolved external '%s'", name);
        return false;
    }

    Fail(err, "CoffImage: unsupported symbol (section %d, class %u)", sectionNumber, storageClass);
    return false;
}

CoffImage* CoffImageLoad(const void* objectData, size_t objectSize,
                         const CoffExtern* externs, size_t externCount,
                         char** errorMessage)
{
    if (errorMessage)
    {
        *errorMessage = NULL;
    }

    if (objectSize < 20)
    {
        Fail(errorMessage, "CoffImage: object too small");
        return NULL;
    }

    const unsigned char* data = (const unsigned char*)objectData;

    if (Rd16(data) != COFF_MACHINE_AMD64)
    {
        Fail(errorMessage, "CoffImage: not an AMD64 object (machine 0x%x)", Rd16(data));
        return NULL;
    }

    if (Rd16(data + 16) != 0)
    {
        Fail(errorMessage, "CoffImage: expected a raw object file, not an image");
        return NULL;
    }

    CoffObject obj = {0};
    obj.data = data;
    obj.size = objectSize;
    obj.sectionCount = Rd16(data + 2);
    size_t symbolCount = Rd32(data + 12);

    if (Rd32(data + 8) == 0 || symbolCount == 0)
    {
        Fail(errorMessage, "CoffImage: object has no symbol table");
        return NULL;
    }

    size_t stringTableOff = Rd32(data + 8) + symbolCount * 18;

    if (stringTableOff + 4 > objectSize)
    {
        Fail(errorMessage, "CoffImage: string table out of bounds");
        return NULL;
    }

    obj.stringTable = data + stringTableOff;
    obj.stringTableSize = Rd32(obj.stringTable);

    if (obj.stringTableSize == 0 || stringTableOff + obj.stringTableSize > objectSize)
    {
        obj.stringTableSize = objectSize - stringTableOff;
    }

    /* Lay out the sections inside one executable allocation. */
    obj.sectionOffsets = (size_t*)calloc(obj.sectionCount ? obj.sectionCount : 1, sizeof(size_t));

    if (!obj.sectionOffsets)
    {
        Fail(errorMessage, "CoffImage: out of memory");
        return NULL;
    }

    size_t cursor = 16; /* keep section starts away from the allocation head */

    for (size_t i = 0; i < obj.sectionCount; i++)
    {
        const unsigned char* sh = SectionAt(&obj, i);
        uint32_t size = SectionSize(sh);

        if (SectionIsSkippable(sh) || size == 0)
        {
            continue;
        }

        uint32_t align = SectionAlignment(sh);

        if (align < 16)
        {
            align = 16; /* RUNTIME_FUNCTION arrays must be at least 4-aligned; 16 is safe for all */
        }

        cursor = (cursor + align - 1) & ~(size_t)(align - 1);
        obj.sectionOffsets[i] = cursor;
        cursor += size;
    }

    /* Trampoline area after the sections: one 16-byte slot per external
       (movabs rax, imm64 ; jmp rax). Calls to externals go through these so
       the code never depends on a rel32 reaching across more than 2GB — the
       image can be allocated anywhere relative to the host/JIT modules. */
    size_t trampolineBase = (cursor + 15) & ~(size_t)15;

    CoffImage* image = (CoffImage*)calloc(1, sizeof(CoffImage));

    if (!image)
    {
        Fail(errorMessage, "CoffImage: out of memory");
        free(obj.sectionOffsets);
        return NULL;
    }

    image->size = trampolineBase + externCount * 16 + 16;
    image->base = (unsigned char*)VirtualAlloc(NULL, image->size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    if (!image->base)
    {
        Fail(errorMessage, "CoffImage: VirtualAlloc failed (%lu)", (unsigned long)GetLastError());
        free(obj.sectionOffsets);
        CoffImageDestroy(image);
        return NULL;
    }

    for (size_t e = 0; e < externCount; e++)
    {
        unsigned char* t = image->base + trampolineBase + e * 16;
        t[0] = 0x48;
        t[1] = 0xB8; /* movabs rax, imm64 */
        memcpy(t + 2, &externs[e].address, 8);
        t[10] = 0xFF;
        t[11] = 0xE0; /* jmp rax */
    }

    /* Copy raw data; BSS sections stay zeroed. */
    for (size_t i = 0; i < obj.sectionCount; i++)
    {
        const unsigned char* sh = SectionAt(&obj, i);
        uint32_t rawSize = Rd32(sh + 16);

        if (SectionIsSkippable(sh) || rawSize == 0)
        {
            continue;
        }

        uint32_t rawPtr = Rd32(sh + 20);

        if ((size_t)rawPtr + rawSize > objectSize)
        {
            Fail(errorMessage, "CoffImage: section %zu data out of bounds", i);
            free(obj.sectionOffsets);
            CoffImageDestroy(image);
            return NULL;
        }

        memcpy(image->base + obj.sectionOffsets[i], data + rawPtr, rawSize);
    }

    /* Apply relocations. */
    for (size_t i = 0; i < obj.sectionCount; i++)
    {
        const unsigned char* sh = SectionAt(&obj, i);
        uint32_t relocPtr = Rd32(sh + 24);
        uint16_t relocCount = Rd16(sh + 32);

        if (SectionIsSkippable(sh) || relocPtr == 0)
        {
            continue;
        }

        if ((size_t)relocPtr + (size_t)relocCount * 10u > objectSize)
        {
            Fail(errorMessage, "CoffImage: section %zu relocations out of bounds", i);
            free(obj.sectionOffsets);
            CoffImageDestroy(image);
            return NULL;
        }

        char nameBuf[9];
        const char* secName = SectionName(&obj, sh, nameBuf);
        bool isPdata = strncmp(secName, ".pdata", 6) == 0;

        for (uint16_t r = 0; r < relocCount; r++)
        {
            const unsigned char* rr = data + relocPtr + (size_t)r * 10u;
            uint32_t rva = Rd32(rr);
            uint32_t symIdx = Rd32(rr + 4);
            uint16_t type = Rd16(rr + 8);

            if (type == REL_AMD64_ABSOLUTE)
            {
                continue;
            }

            uintptr_t S = 0;
            size_t extIndex = (size_t)-1;

            if (!ResolveSymbol(&obj, symIdx, externs, externCount, image->base, &S, &extIndex, errorMessage))
            {
                free(obj.sectionOffsets);
                CoffImageDestroy(image);
                return NULL;
            }

            unsigned char* P = image->base + obj.sectionOffsets[i] + rva;

            switch (type)
            {
            case REL_AMD64_ADDR64:
                *(uint64_t*)P = (uint64_t)((int64_t)S + (int64_t)*(uint64_t*)P);
                break;

            case REL_AMD64_ADDR32:
                if (isPdata)
                {
                    /* RUNTIME_FUNCTION fields: RVAs relative to the image
                       base handed to RtlAddFunctionTable. */
                    *(uint32_t*)P = (uint32_t)((int64_t)S + (int32_t)*(uint32_t*)P - (int64_t)(uintptr_t)image->base);
                }
                else
                {
                    *(uint32_t*)P = (uint32_t)((int64_t)S + (int32_t)*(uint32_t*)P);
                }
                break;

            case REL_AMD64_ADDR32NB:
                *(uint32_t*)P = (uint32_t)((int64_t)S + (int32_t)*(uint32_t*)P - (int64_t)(uintptr_t)image->base);
                break;

            case REL_AMD64_REL32:
            case 5:
            case 6:
            case 7:
            case REL_AMD64_REL32_4:
            {
                /* Calls to externals target the in-image trampoline so the
                   32-bit displacement never has to reach the (possibly far
                   away) host or JIT modules. */
                if (extIndex != (size_t)-1)
                {
                    S = (uintptr_t)(image->base + trampolineBase + extIndex * 16);
                }

                unsigned n = (unsigned)(type - REL_AMD64_REL32);
                *(int32_t*)P = (int32_t)((int64_t)S + (int32_t)*(int32_t*)P - ((int64_t)(uintptr_t)P + 4 + (int64_t)n));
                break;
            }

            default:
                Fail(errorMessage, "CoffImage: unsupported relocation type %u in %s", type, secName);
                free(obj.sectionOffsets);
                CoffImageDestroy(image);
                return NULL;
            }
        }
    }

    FlushInstructionCache(GetCurrentProcess(), image->base, image->size);

    /* Register every .pdata section with the OS dynamic function table. */
    bool sawPdata = false;

    for (size_t i = 0; i < obj.sectionCount; i++)
    {
        const unsigned char* sh = SectionAt(&obj, i);
        uint32_t size = SectionSize(sh);

        if (size == 0)
        {
            continue;
        }

        char nameBuf[9];
        const char* secName = SectionName(&obj, sh, nameBuf);

        if (strncmp(secName, ".pdata", 6) != 0)
        {
            continue;
        }

        sawPdata = true;

        PVOID handle = StrataAddFunctionTable((PVOID)(image->base + obj.sectionOffsets[i]), size / 12u,
                                               (ULONG64)(uintptr_t)image->base);

        if (!handle)
        {
            Fail(errorMessage, "CoffImage: RtlAddFunctionTable failed for %s", secName);
            free(obj.sectionOffsets);
            CoffImageDestroy(image);
            return NULL;
        }

        if (image->functionTableCount < 8)
        {
            image->functionTables[image->functionTableCount++] = handle;
        }
    }

    if (!sawPdata)
    {
        Fail(errorMessage, "CoffImage: object carries no unwind info (.pdata)");
        free(obj.sectionOffsets);
        CoffImageDestroy(image);
        return NULL;
    }

    /* Collect defined symbols for CoffImageGetSymbol. */
    size_t defined = 0;

    for (size_t i = 0; i < symbolCount; i++)
    {
        const unsigned char* sym = SymbolAt(&obj, i);

        if ((int16_t)Rd16(sym + 12) > 0 && sym[16] == SYM_CLASS_EXTERNAL)
        {
            defined++;
        }

        i += sym[17]; /* skip auxiliary records */
    }

    image->symbols = (CoffSymEntry*)calloc(defined ? defined : 1, sizeof(CoffSymEntry));
    image->symbolCount = 0;

    if (image->symbols)
    {
        for (size_t i = 0; i < symbolCount && image->symbolCount < defined; i++)
        {
            const unsigned char* sym = SymbolAt(&obj, i);
            int32_t sectionNumber = (int16_t)Rd16(sym + 12);

            if (sectionNumber > 0 && sym[16] == SYM_CLASS_EXTERNAL)
            {
                /* names live in the caller's object buffer, which is freed
                   after load: copy them */
                char nameBuf[9];
                const char* name = SymbolName(&obj, sym, nameBuf);

                image->symbols[image->symbolCount].name = strdup(name);
                image->symbols[image->symbolCount].offset = obj.sectionOffsets[sectionNumber - 1] + Rd32(sym + 8);
                image->symbolCount++;
            }

            i += sym[17];
        }
    }

    free(obj.sectionOffsets);
    return image;
}

void* CoffImageGetSymbol(const CoffImage* image, const char* name)
{
    if (!image || !name)
    {
        return NULL;
    }

    for (size_t i = 0; i < image->symbolCount; i++)
    {
        if (strcmp(image->symbols[i].name, name) == 0)
        {
            return image->base + image->symbols[i].offset;
        }
    }

    return NULL;
}

void CoffImageDestroy(CoffImage* image)
{
    if (!image)
    {
        return;
    }

    for (size_t i = 0; i < image->functionTableCount; i++)
    {
        StrataDeleteFunctionTable(image->functionTables[i]);
    }

    if (image->base)
    {
        VirtualFree(image->base, 0, MEM_RELEASE);
    }

    for (size_t i = 0; i < image->symbolCount; i++)
    {
        free((void*)image->symbols[i].name);
    }

    free(image->symbols);
    free(image);
}

#endif
