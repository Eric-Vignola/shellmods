// Symbol resolution: the piece that actually replaces Windhawk's engine.
//
// Windhawk resolves mangled names to addresses inside the target process, by
// downloading the module's PDB from Microsoft's symbol server and walking it
// with the DIA SDK. Doing that in-process means shipping msdia140 into
// explorer.exe and doing network I/O from a shell hook, so we split it in two:
// symgen.exe resolves names to RVAs offline and writes a .sym file, and this
// file does nothing but read that table back.
//
// The critical safety property is the fingerprint. Every .sym records the
// TimeDateStamp and SizeOfImage of the module it was generated against. If the
// module currently mapped in this process does not match, the table is stale --
// a Windows update replaced the binary and every RVA in it now points at the
// wrong instruction. Hooking a stale address inside explorer.exe is how you get
// a login loop, so a mismatch refuses every symbol hook rather than guessing.

#include <windows.h>

#include <shlwapi.h>

#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "shim_runtime.h"
#include "windhawk_api.h"

namespace shim {

namespace {

struct SymbolTable {
    bool usable = false;
    // Keyed by both the decorated and the undecorated name, because mods match
    // on either. Anonymous data symbols such as __real@4048000000000000 only
    // have a decorated form, and the taskbar mod hooks one of those.
    std::unordered_map<std::wstring, DWORD> rvaByName;
    // Insertion-ordered view, for Wh_FindFirstSymbol enumeration.
    struct Entry {
        DWORD rva;
        std::wstring decorated;
        std::wstring undecorated;
    };
    std::vector<Entry> entries;
};

std::mutex g_mutex;
std::unordered_map<HMODULE, std::shared_ptr<SymbolTable>> g_tables;

std::wstring ModuleBaseName(HMODULE module) {
    WCHAR path[MAX_PATH];
    DWORD len = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path)) {
        return std::wstring();
    }
    return PathFindFileNameW(path);
}

bool ModuleFingerprint(HMODULE module, DWORD* timestamp, DWORD* sizeOfImage) {
    __try {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }
        *timestamp = nt->FileHeader.TimeDateStamp;
        *sizeOfImage = nt->OptionalHeader.SizeOfImage;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    int chars = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (chars <= 0) {
        return std::wstring();
    }
    std::wstring w(static_cast<size_t>(chars), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), chars);
    return w;
}

// One .sym line is "<hex rva>\t<decorated>\t<undecorated>". Either name may be
// empty; a line with neither is skipped.
void ParseDataLine(const std::string& line, SymbolTable* table) {
    size_t tab1 = line.find('\t');
    if (tab1 == std::string::npos) {
        return;
    }
    size_t tab2 = line.find('\t', tab1 + 1);

    DWORD rva = 0;
    try {
        rva = static_cast<DWORD>(
            std::stoul(line.substr(0, tab1), nullptr, 16));
    } catch (...) {
        return;
    }
    if (rva == 0) {
        return;
    }

    SymbolTable::Entry entry;
    entry.rva = rva;
    if (tab2 == std::string::npos) {
        entry.decorated = Utf8ToWide(line.substr(tab1 + 1));
    } else {
        entry.decorated = Utf8ToWide(line.substr(tab1 + 1, tab2 - tab1 - 1));
        entry.undecorated = Utf8ToWide(line.substr(tab2 + 1));
    }

    if (entry.decorated.empty() && entry.undecorated.empty()) {
        return;
    }

    // First writer wins, matching Windhawk's "the first symbol that matches one
    // of the names will be used".
    if (!entry.decorated.empty()) {
        table->rvaByName.emplace(entry.decorated, rva);
    }
    if (!entry.undecorated.empty()) {
        table->rvaByName.emplace(entry.undecorated, rva);
    }
    table->entries.push_back(std::move(entry));
}

std::shared_ptr<SymbolTable> LoadTable(HMODULE module) {
    auto table = std::make_shared<SymbolTable>();

    const std::wstring baseName = ModuleBaseName(module);
    if (baseName.empty()) {
        Logf(L"symbols: cannot determine base name for module %p", module);
        return table;
    }

    DWORD liveTimestamp = 0;
    DWORD liveSizeOfImage = 0;
    if (!ModuleFingerprint(module, &liveTimestamp, &liveSizeOfImage)) {
        Logf(L"symbols: cannot read PE headers of %s", baseName.c_str());
        return table;
    }

    const std::wstring path = OffsetsDir() + L"\\" + baseName + L".sym";
    std::ifstream file(path);
    if (!file) {
        Logf(L"symbols: missing %s -- run symgen.exe to generate it", path.c_str());
        return table;
    }

    DWORD fileTimestamp = 0;
    DWORD fileSizeOfImage = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            const char* kTs = "# timestamp=";
            const char* kSi = "# sizeofimage=";
            if (line.rfind(kTs, 0) == 0) {
                fileTimestamp = static_cast<DWORD>(
                    strtoul(line.c_str() + strlen(kTs), nullptr, 16));
            } else if (line.rfind(kSi, 0) == 0) {
                fileSizeOfImage = static_cast<DWORD>(
                    strtoul(line.c_str() + strlen(kSi), nullptr, 16));
            }
            continue;
        }
        ParseDataLine(line, table.get());
    }

    if (fileTimestamp != liveTimestamp || fileSizeOfImage != liveSizeOfImage) {
        Logf(L"symbols: %s is STALE (file %08X/%08X, loaded %08X/%08X). "
             L"Refusing all symbol hooks. Re-run symgen.exe.",
             baseName.c_str(), fileTimestamp, fileSizeOfImage, liveTimestamp,
             liveSizeOfImage);
        table->rvaByName.clear();
        table->entries.clear();
        return table;
    }

    if (table->entries.empty()) {
        Logf(L"symbols: %s resolved no symbols", baseName.c_str());
        return table;
    }

    Logf(L"symbols: %s loaded, %zu entries", baseName.c_str(),
         table->entries.size());
    table->usable = true;
    return table;
}

std::shared_ptr<SymbolTable> GetTable(HMODULE module) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_tables.find(module);
    if (it != g_tables.end()) {
        return it->second;
    }
    auto table = LoadTable(module);
    g_tables.emplace(module, table);
    return table;
}

}  // namespace

bool SymbolsUsable(HMODULE module) {
    return GetTable(module)->usable;
}

void* ResolveSymbol(HMODULE module, const WCHAR* name, size_t nameLength) {
    auto table = GetTable(module);
    if (!table->usable) {
        return nullptr;
    }
    // SYMBOL_HOOK stores names without a terminator, carrying the length
    // separately, so build the key explicitly rather than treating it as a
    // C string.
    auto it = table->rvaByName.find(std::wstring(name, nameLength));
    if (it == table->rvaByName.end()) {
        return nullptr;
    }
    return reinterpret_cast<BYTE*>(module) + it->second;
}

// Enumeration support for Wh_FindFirstSymbol. Tables are cached for the life of
// the process and never evicted, so the strings we hand out stay valid without
// the search handle needing to own anything.
bool SymbolAt(HMODULE module,
              size_t index,
              void** address,
              const WCHAR** decorated,
              const WCHAR** undecorated) {
    auto table = GetTable(module);
    if (!table->usable || index >= table->entries.size()) {
        return false;
    }
    const auto& entry = table->entries[index];
    *address = reinterpret_cast<BYTE*>(module) + entry.rva;
    *decorated = entry.decorated.c_str();
    *undecorated = entry.undecorated.c_str();
    return true;
}

}  // namespace shim

using namespace shim;

namespace {

// Backing object for the HANDLE returned by Wh_FindFirstSymbol.
struct SymbolSearch {
    HMODULE module = nullptr;
    size_t index = 0;
};

}  // namespace

extern "C" {

BOOL InternalWh_HookSymbols(void* mod,
                            HMODULE module,
                            const WH_SYMBOL_HOOK* symbolHooks,
                            size_t symbolHooksCount,
                            const WH_HOOK_SYMBOLS_OPTIONS* options) {
    UNREFERENCED_PARAMETER(mod);
    UNREFERENCED_PARAMETER(options);

    if (!module) {
        Logf(L"HookSymbols called with a null module");
        return FALSE;
    }

    if (!SymbolsUsable(module)) {
        // Already logged in detail by LoadTable. Failing here makes the mod
        // abort its own init, which is what we want: no symbols means no
        // guarantee about any address in this module.
        return FALSE;
    }

    for (size_t i = 0; i < symbolHooksCount; i++) {
        const WH_SYMBOL_HOOK& hook = symbolHooks[i];

        void* address = nullptr;
        for (size_t j = 0; j < hook.symbolsCount && !address; j++) {
            address = ResolveSymbol(module, hook.symbols[j].string,
                                    hook.symbols[j].length);
        }

        if (!address) {
            if (hook.optional) {
                // Upstream contract: leave pOriginalFunction untouched and
                // carry on. Several hooks are version-specific and are expected
                // to be absent on some builds.
                Logf(L"HookSymbols: optional symbol %d not found, skipping",
                     static_cast<int>(i));
                continue;
            }
            Logf(L"HookSymbols: required symbol not found: %.*s",
                 hook.symbolsCount ? static_cast<int>(hook.symbols[0].length) : 0,
                 hook.symbolsCount ? hook.symbols[0].string : L"");
            return FALSE;
        }

        if (!hook.hookFunction) {
            // Address-only lookup.
            if (hook.pOriginalFunction) {
                *hook.pOriginalFunction = address;
            }
            continue;
        }

        if (!HookRegister(address, hook.hookFunction, hook.pOriginalFunction)) {
            Logf(L"HookSymbols: failed to hook symbol %d at %p",
                 static_cast<int>(i), address);
            return FALSE;
        }
    }

    return TRUE;
}

HANDLE InternalWh_FindFirstSymbol4(void* mod,
                                   HMODULE hModule,
                                   const WH_FIND_SYMBOL_OPTIONS* options,
                                   WH_FIND_SYMBOL* findData) {
    UNREFERENCED_PARAMETER(mod);
    UNREFERENCED_PARAMETER(options);

    if (!hModule) {
        hModule = GetModuleHandleW(nullptr);
    }
    if (!SymbolsUsable(hModule)) {
        return nullptr;
    }

    auto search = new (std::nothrow) SymbolSearch();
    if (!search) {
        return nullptr;
    }
    search->module = hModule;
    search->index = 0;

    HANDLE handle = reinterpret_cast<HANDLE>(search);
    if (!InternalWh_FindNextSymbol2(mod, handle, findData)) {
        delete search;
        return nullptr;
    }
    return handle;
}

BOOL InternalWh_FindNextSymbol2(void* mod,
                                HANDLE symSearch,
                                WH_FIND_SYMBOL* findData) {
    UNREFERENCED_PARAMETER(mod);
    if (!symSearch || !findData) {
        return FALSE;
    }
    auto search = reinterpret_cast<SymbolSearch*>(symSearch);

    void* address = nullptr;
    const WCHAR* decorated = nullptr;
    const WCHAR* undecorated = nullptr;
    if (!SymbolAt(search->module, search->index, &address, &decorated,
                  &undecorated)) {
        return FALSE;
    }
    search->index++;

    findData->address = address;
    findData->symbol = undecorated;
    findData->symbolDecorated = decorated;
    return TRUE;
}

void InternalWh_FindCloseSymbol(void* mod, HANDLE symSearch) {
    UNREFERENCED_PARAMETER(mod);
    if (symSearch) {
        delete reinterpret_cast<SymbolSearch*>(symSearch);
    }
}

}  // extern "C"
