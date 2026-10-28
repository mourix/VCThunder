/* pe_host.c -- map an ordinary PE image and bind its imports ourselves.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * WHY THE SYSTEM LOADER MAPS AND WE BIND. Windows dispatches an SEH frame only
 * to a handler inside a mapped image; a handler in VirtualAlloc'd memory is
 * refused and the refusal ends the process. A hand-rolled
 * mapper would put every one of the game's handlers there. So the file is
 * mapped by LoadLibraryEx with DONT_RESOLVE_DLL_REFERENCES: a real image,
 * relocated by the loader, with no import resolved and no DllMain run. Every
 * import is then bound here, through the caller's resolver, which is where the
 * boundaries are decided.
 */
#include "vcthunder.h"
#include "pe_host.h"

#include <stdio.h>
#include <string.h>

static IMAGE_NT_HEADERS32 *nt_of(const void *base)
{
    const IMAGE_DOS_HEADER *dos = base;
    IMAGE_NT_HEADERS32 *nt;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    nt = (IMAGE_NT_HEADERS32 *)((const BYTE *)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return NULL;
    return nt;
}

/* The file offset of an RVA, from the FILE's section table. */
static long file_off(const IMAGE_NT_HEADERS32 *nt, uint32_t rva)
{
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    unsigned i;

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (rva >= s->VirtualAddress &&
            rva < s->VirtualAddress + s->SizeOfRawData)
            return (long)(rva - s->VirtualAddress + s->PointerToRawData);
    return -1;
}

/* The first HIGHLOW fixup of the FILE's relocation table: its RVA and the
 * dword the file holds there. 0 if there is none. */
static int first_fixup(const BYTE *file, size_t len, const IMAGE_NT_HEADERS32 *fnt,
                       uint32_t *rva, uint32_t *value)
{
    const IMAGE_DATA_DIRECTORY *rd =
        &fnt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    long off = rd->Size ? file_off(fnt, rd->VirtualAddress) : -1;
    const IMAGE_BASE_RELOCATION *b;
    const WORD *e;
    unsigned n, i;

    if (off < 0 || (size_t)off + 8 > len)
        return 0;
    b = (const void *)(file + off);
    e = (const WORD *)(b + 1);
    n = (b->SizeOfBlock - sizeof *b) / 2;
    for (i = 0; i < n; i++) {
        long fo;

        if ((e[i] >> 12) != IMAGE_REL_BASED_HIGHLOW)
            continue;
        *rva = b->VirtualAddress + (e[i] & 0xfff);
        fo = file_off(fnt, *rva);
        if (fo < 0 || (size_t)fo + 4 > len)
            return 0;
        memcpy(value, file + fo, 4);
        return 1;
    }
    return 0;
}

/* Apply every fixup in the MAPPED image's relocation table, one page at a
 * time, each page's protection restored after. Writing into an image mapping
 * makes its pages private copies; the region stays MEM_IMAGE, which is what the
 * exception dispatcher asks of a handler's address. */
static int apply_fixups(pe_image_t *im, uint32_t delta)
{
    BYTE *base = (BYTE *)im->mod;
    IMAGE_NT_HEADERS32 *nt = nt_of(base);
    IMAGE_DATA_DIRECTORY *rd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    BYTE *p = base + rd->VirtualAddress, *end = p + rd->Size;
    unsigned applied = 0;

    while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
        IMAGE_BASE_RELOCATION *b = (IMAGE_BASE_RELOCATION *)p;
        WORD *e = (WORD *)(b + 1);
        BYTE *page = base + b->VirtualAddress;
        unsigned n, i, pages = 1;
        DWORD old[2];

        if (b->SizeOfBlock < sizeof *b)
            break;
        n = (b->SizeOfBlock - sizeof *b) / 2;
        /* A block covers one page; its last fixup may straddle into the next,
         * which is then unprotected too. Each page keeps its own protection:
         * the next one may be another section's. */
        for (i = 0; i < n; i++)
            if (e[i] >> 12 && (e[i] & 0xfff) > 0x1000 - 4)
                pages = 2;
        for (i = 0; i < pages; i++)
            if (!VirtualProtect(page + i * 0x1000, 1, PAGE_EXECUTE_READWRITE,
                                &old[i])) {
                LOGE("pe: %s: cannot unprotect RVA %#lx for its fixups (error"
                     " %lu)", im->name, (unsigned long)b->VirtualAddress
                     + i * 0x1000, GetLastError());
                return 0;
            }
        for (i = 0; i < n; i++) {
            unsigned type = e[i] >> 12;

            if (type == IMAGE_REL_BASED_ABSOLUTE)
                continue;
            if (type != IMAGE_REL_BASED_HIGHLOW) {
                LOGE("pe: %s: relocation type %u is not one a 32-bit image"
                     " uses", im->name, type);
                return 0;
            }
            *(uint32_t *)(page + (e[i] & 0xfff)) += delta;
            applied++;
        }
        while (pages--)
            VirtualProtect(page + pages * 0x1000, 1, old[pages], &old[pages]);
        p += b->SizeOfBlock;
    }
    FlushInstructionCache(GetCurrentProcess(), base, nt->OptionalHeader.SizeOfImage);
    LOGI("pe: %s: %u fixups applied by the host", im->name, applied);
    return 1;
}

/* A moved image is only safe with its fixups applied. Decide on the first
 * HIGHLOW one: the loader applied them all (memory holds file + delta), or
 * none (memory holds the file's value), and then this host applies them and
 * checks again. Anything else is an image nobody can vouch for. */
static int check_relocated(pe_image_t *im, const BYTE *file, size_t len,
                           const IMAGE_NT_HEADERS32 *fnt)
{
    uint32_t delta = (uint32_t)(uintptr_t)im->mod - im->preferred;
    uint32_t rva, value, have;

    if (!first_fixup(file, len, fnt, &rva, &value)) {
        LOGE("pe: %s was moved from %#lx and has no relocations to apply",
             im->name, (unsigned long)im->preferred);
        return 0;
    }
    memcpy(&have, (const BYTE *)im->mod + rva, 4);
    if (have == value && !apply_fixups(im, delta))
        return 0;
    memcpy(&have, (const BYTE *)im->mod + rva, 4);
    if (have != value + delta) {
        LOGE("pe: %s moved by %#lx and its fixup at RVA %#lx holds %#lx, not"
             " %#lx", im->name, (unsigned long)delta, (unsigned long)rva,
             (unsigned long)have, (unsigned long)(value + delta));
        return 0;
    }
    im->relocated = 1;
    return 1;
}

int pe_map(pe_image_t *im, const char *path, const void *file, size_t len)
{
    const IMAGE_NT_HEADERS32 *fnt = len > 0x40 ? nt_of(file) : NULL;
    const char *slash;

    memset(im, 0, sizeof *im);
    snprintf(im->path, sizeof im->path, "%s", path);
    slash = strrchr(im->path, '\\');
    im->name = slash ? slash + 1 : im->path;
    if (!fnt) {
        LOGE("pe: %s is not a 32-bit x86 PE image", path);
        return 0;
    }
    im->preferred = fnt->OptionalHeader.ImageBase;
    im->is_dll = (fnt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;

    im->mod = LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!im->mod) {
        LOGE("pe: cannot map %s (error %lu)", path, GetLastError());
        return 0;
    }
    if (!nt_of(im->mod)) {
        LOGE("pe: %s mapped, but its headers do not read back", path);
        return 0;
    }
    if ((uint32_t)(uintptr_t)im->mod != im->preferred &&
        !check_relocated(im, file, len, fnt))
        return 0;
    LOGI("pe: %s mapped at %p (%s %#lx)", im->name, (void *)im->mod,
         im->relocated ? "relocated from" : "its preferred base",
         (unsigned long)im->preferred);
    return 1;
}

/* Section characteristics to a page protection. */
static DWORD protection(DWORD c)
{
    int x = (c & IMAGE_SCN_MEM_EXECUTE) != 0, w = (c & IMAGE_SCN_MEM_WRITE) != 0;

    return x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
             : (w ? PAGE_READWRITE : PAGE_READONLY);
}

int pe_place(pe_image_t *im, const char *path, const void *file, size_t len)
{
    const IMAGE_NT_HEADERS32 *fnt = len > 0x40 ? nt_of(file) : NULL;
    const IMAGE_SECTION_HEADER *sec;
    MEMORY_BASIC_INFORMATION lo, hi;
    HMODULE self = GetModuleHandleA(NULL);
    const char *slash;
    BYTE *base;
    uint32_t size;
    DWORD old;
    unsigned i;

    memset(im, 0, sizeof *im);
    snprintf(im->path, sizeof im->path, "%s", path);
    slash = strrchr(im->path, '\\');
    im->name = slash ? slash + 1 : im->path;
    if (!fnt) {
        LOGE("pe: %s is not a 32-bit x86 PE image", path);
        return 0;
    }
    if (!(fnt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) ||
        fnt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        LOGE("pe: %s: only an image with no relocations and no TLS is placed",
             im->name);
        return 0;
    }
    base = (BYTE *)(uintptr_t)fnt->OptionalHeader.ImageBase;
    size = fnt->OptionalHeader.SizeOfImage;
    if (!VirtualQuery(base, &lo, sizeof lo) ||
        !VirtualQuery(base + size - 1, &hi, sizeof hi) ||
        lo.Type != MEM_IMAGE || lo.AllocationBase != (void *)self ||
        hi.Type != MEM_IMAGE || hi.AllocationBase != (void *)self) {
        LOGE("pe: %s needs %p..%p inside this executable's own image, and it"
             " is not there", im->name, (void *)base, (void *)(base + size));
        return 0;
    }
    if (!VirtualProtect(base, size, PAGE_READWRITE, &old)) {
        LOGE("pe: %s: cannot open %p..%p (error %lu)", im->name, (void *)base,
             (void *)(base + size), GetLastError());
        return 0;
    }
    memset(base, 0, size);
    memcpy(base, file, fnt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(fnt);
    for (i = 0; i < fnt->FileHeader.NumberOfSections; i++) {
        uint32_t n = sec[i].SizeOfRawData < sec[i].Misc.VirtualSize
                   ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize;

        if (!sec[i].Misc.VirtualSize)
            n = sec[i].SizeOfRawData;
        if (n && sec[i].PointerToRawData + n <= len)
            memcpy(base + sec[i].VirtualAddress,
                   (const BYTE *)file + sec[i].PointerToRawData, n);
    }
    /* The import table stays writable for pe_bind; everything else takes its
     * section's protection, the headers read-only. */
    VirtualProtect(base, fnt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
    for (i = 0; i < fnt->FileHeader.NumberOfSections; i++) {
        uint32_t vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize
                                              : sec[i].SizeOfRawData;

        if (vs)
            VirtualProtect(base + sec[i].VirtualAddress, vs,
                           protection(sec[i].Characteristics), &old);
    }
    FlushInstructionCache(GetCurrentProcess(), base, size);
    im->mod = (HMODULE)base;
    im->preferred = fnt->OptionalHeader.ImageBase;
    im->is_dll = (fnt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
    im->placed = 1;
    LOGI("pe: %s placed at its base %p, inside this executable's span",
         im->name, (void *)base);
    return 1;
}

int pe_bind(pe_image_t *im, pe_resolver_fn fn, pe_bind_stats_t *st)
{
    BYTE *base = (BYTE *)im->mod;
    IMAGE_NT_HEADERS32 *nt = nt_of(base);
    IMAGE_DATA_DIRECTORY *id =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d;
    unsigned missing0 = st->missing;

    if (!id->Size)
        return 1;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(base + id->VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)(base + d->Name);
        IMAGE_THUNK_DATA32 *names = (IMAGE_THUNK_DATA32 *)(base +
            (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32 *slots = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk);

        for (; names->u1.AddressOfData; names++, slots++) {
            const char *name = NULL;
            unsigned ord = 0;
            pe_bind_kind_t kind = PE_BIND_REAL;
            void *to;
            DWORD old;

            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))
                ord = IMAGE_ORDINAL32(names->u1.Ordinal);
            else
                name = (const char *)((IMAGE_IMPORT_BY_NAME *)
                                      (base + names->u1.AddressOfData))->Name;
            st->total++;
            to = fn(im->name, dll, name, ord, &kind);
            if (!to) {
                st->missing++;
                if (name)
                    LOGE("pe: %s: %s!%s does not resolve", im->name, dll, name);
                else
                    LOGE("pe: %s: %s!#%u does not resolve", im->name, dll, ord);
                continue;
            }
            st->kind[kind]++;
            if (!VirtualProtect(&slots->u1.Function, 4, PAGE_READWRITE, &old)) {
                LOGE("pe: %s: cannot write its import table", im->name);
                return 0;
            }
            slots->u1.Function = (DWORD)(uintptr_t)to;
            VirtualProtect(&slots->u1.Function, 4, old, &old);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    return st->missing == missing0;
}

void *pe_entry(const pe_image_t *im)
{
    const IMAGE_NT_HEADERS32 *nt = nt_of(im->mod);

    return nt->OptionalHeader.AddressOfEntryPoint
        ? (BYTE *)im->mod + nt->OptionalHeader.AddressOfEntryPoint : NULL;
}

int pe_attach(pe_image_t *im)
{
    typedef BOOL (WINAPI *dllmain_fn)(HINSTANCE, DWORD, LPVOID);
    dllmain_fn entry = (dllmain_fn)pe_entry(im);

    if (!im->is_dll || !entry)
        return 1;
    if (!entry(im->mod, DLL_PROCESS_ATTACH, NULL)) {
        LOGE("pe: %s refused DLL_PROCESS_ATTACH", im->name);
        return 0;
    }
    LOGI("pe: %s attached", im->name);
    return 1;
}

/* The loader's answer for a mapped image; a placed one is not in its list, so
 * its export table is read directly (no forwarders: refused). */
void *pe_export(const pe_image_t *im, const char *name)
{
    const BYTE *base = (const BYTE *)im->mod;
    const IMAGE_NT_HEADERS32 *nt;
    const IMAGE_DATA_DIRECTORY *ed;
    const IMAGE_EXPORT_DIRECTORY *e;
    const DWORD *names, *funcs;
    const WORD *ords;
    DWORD i;

    if (!im->placed)
        return (void *)GetProcAddress(im->mod, name);
    nt = nt_of(base);
    ed = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!name || !ed->Size)
        return NULL;
    e = (const IMAGE_EXPORT_DIRECTORY *)(base + ed->VirtualAddress);
    names = (const DWORD *)(base + e->AddressOfNames);
    funcs = (const DWORD *)(base + e->AddressOfFunctions);
    ords = (const WORD *)(base + e->AddressOfNameOrdinals);
    for (i = 0; i < e->NumberOfNames; i++)
        if (!strcmp((const char *)(base + names[i]), name)) {
            DWORD rva = funcs[ords[i]];

            if (rva >= ed->VirtualAddress && rva < ed->VirtualAddress + ed->Size)
                return NULL;
            return (void *)(base + rva);
        }
    return NULL;
}
