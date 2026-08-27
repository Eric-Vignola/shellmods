// symgen.exe -- resolves Windhawk-style symbol names to RVAs, offline.
//
// This is the half of Windhawk's symbol engine that we deliberately moved out of
// the target process. Windhawk downloads a PDB and walks it with msdia inside
// explorer.exe; doing that means shipping a COM symbol parser and network I/O
// into the shell. Here it happens in a console program you run by hand (or that
// the loader runs for you), and the only thing that reaches explorer.exe is a
// small text file of offsets.
//
// Input:  symreq\<mod>.symreq -- which modules, and which symbol names.
// Output: dist\offsets\<module>.sym -- "<hex rva>\t<decorated>\t<undecorated>",
//         preceded by the fingerprint of the exact binary it was generated from.
//
// Requires msdia140.dll, which ships with Visual Studio. symgen finds the copy
// already on the machine (see find_msdia.cpp) rather than carrying one, so this
// project redistributes no Microsoft binaries. PDBs are fetched over plain HTTPS
// by pdb_fetch.cpp rather than through symsrv.dll, so nothing else is needed
// either.

#include <windows.h>

#include <dia2.h>
#include <diacreate.h>
#include <tlhelp32.h>

#include "find_msdia.h"
#include "pdb_fetch.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "diaguids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
// diaguids.lib's NoOleCoCreate reads the registry to find a registered msdia.
#pragma comment(lib, "advapi32.lib")

namespace {

constexpr wchar_t kDefaultSymbolServer[] =
    L"https://msdl.microsoft.com/download/symbols";

// Just enough COM plumbing for the DIA calls below. Deliberately not ATL or
// WIL: this way the project needs nothing from the Visual Studio installer
// beyond the C++ toolchain itself.
template <typename T>
class ComPtr {
   public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* operator->() const { return m_p; }
    operator T*() const { return m_p; }
    T** put() {
        reset();
        return &m_p;
    }
    void reset() {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }

   private:
    T* m_p = nullptr;
};

class Bstr {
   public:
    Bstr() = default;
    ~Bstr() { reset(); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;

    BSTR* put() {
        reset();
        return &m_s;
    }
    void reset() {
        if (m_s) {
            SysFreeString(m_s);
            m_s = nullptr;
        }
    }
    std::wstring str() const { return m_s ? std::wstring(m_s) : std::wstring(); }

   private:
    BSTR m_s = nullptr;
};

struct Request {
    std::vector<std::wstring> modules;
    // Every name any code path in the mod might ask for. Over-collection is
    // harmless: a name that belongs to a different module simply will not
    // resolve there, and the mod's own `optional` flags cover absences.
    std::set<std::wstring> names;
};

void Fail(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    fwprintf(stderr, L"symgen: error: ");
    vfwprintf(stderr, format, args);
    fwprintf(stderr, L"\n");
    va_end(args);
}

void Info(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    vfwprintf(stdout, format, args);
    fwprintf(stdout, L"\n");
    va_end(args);
}

std::wstring DirOfThisExe() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring s = path;
    size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

bool DirHasSymreq(const std::wstring& dir) {
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((dir + L"\\*.symreq").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return false;
    }
    FindClose(search);
    return true;
}

// symreq\ and offsets\ sit beside the executable, and this deliberately does not
// traverse "..".
//
// It used to. The defaults were exeDir\..\symreq and exeDir\..\dist\offsets,
// which are correct when symgen.exe is run from dist\ inside a source tree --
// the only way it was ever tested -- and wrong in the released archive, where
// symgen.exe is at the archive root and ".." escapes the extracted folder
// entirely. So: one layout, resolved by looking for the data rather than
// assuming a position relative to it.
//
//   released / staged   <root>\symgen.exe  <root>\symreq\  <root>\offsets\
//   straight from build build\Release\symgen.exe  ->  <repo>\dist\...
//
// The second candidate exists only so that running the freshly linked binary
// out of build\Release still works; everything shipped uses the first.
std::wstring FindDataRoot(const std::wstring& exeDir,
                          std::vector<std::wstring>* candidates) {
    const std::wstring roots[] = {
        exeDir,
        exeDir + L"\\..\\..\\dist",
    };
    for (const std::wstring& root : roots) {
        const std::wstring symreq = root + L"\\symreq";
        candidates->push_back(symreq);
        if (DirHasSymreq(symreq)) {
            return root;
        }
    }
    return std::wstring();
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    int chars = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(chars), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), chars);
    return w;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) {
        return std::string();
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
    std::string u(static_cast<size_t>(bytes), 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        u.data(), bytes, nullptr, nullptr);
    return u;
}

// ---------------------------------------------------------------------------
// .symreq parsing
// ---------------------------------------------------------------------------

bool ReadRequest(const std::wstring& path, Request* request) {
    std::ifstream file(path);
    if (!file) {
        Fail(L"cannot open %s", path.c_str());
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const char* kModule = "!module ";
        if (line.rfind(kModule, 0) == 0) {
            std::string module = line.substr(strlen(kModule));
            // Trailing "# why this module" comments are allowed here. They are
            // not stripped from symbol lines, because a mangled name may
            // legitimately contain almost anything.
            size_t comment = module.find('#');
            if (comment != std::string::npos) {
                module.erase(comment);
            }
            size_t end = module.find_last_not_of(" \t");
            module = (end == std::string::npos) ? std::string()
                                                : module.substr(0, end + 1);
            size_t begin = module.find_first_not_of(" \t");
            if (begin != std::string::npos) {
                request->modules.push_back(Utf8ToWide(module.substr(begin)));
            }
            continue;
        }
        request->names.insert(Utf8ToWide(line));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Module lookup and fingerprinting
// ---------------------------------------------------------------------------

bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::towlower);
    return r;
}

// Base name -> full path for everything currently mapped into the target
// process. Built once, lazily.
//
// This is the authoritative answer to "which file will actually be hooked", and
// it matters here: half the modules the taskbar mod touches are not in System32
// at all. On this build Taskbar.View.dll and SystemTray.dll live under
// SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy and SearchUx.UI.dll
// under ...Client.CBS_cw5n1h2txyewy. Asking the running shell where its own
// modules came from beats maintaining a list of package directories that
// Microsoft reorganises between releases.
const std::map<std::wstring, std::wstring>& LiveModules(
    const std::wstring& processName) {
    static std::map<std::wstring, std::wstring> modules;
    static bool built = false;
    if (built) {
        return modules;
    }
    built = true;

    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        const std::wstring wanted = ToLower(processName);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (ToLower(entry.szExeFile) == wanted) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    if (pid == 0) {
        Info(L"  note: %s is not running, falling back to directory search",
             processName.c_str());
        return modules;
    }

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Info(L"  note: cannot list modules of %s (%lu), falling back to "
             L"directory search",
             processName.c_str(), GetLastError());
        return modules;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            modules.emplace(ToLower(entry.szModule), entry.szExePath);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return modules;
}

// A bare name in a .symreq is resolved first against the live target process,
// then against the usual system locations. An absolute path is taken as given,
// which is how you point symgen at a copy of a binary from another machine.
std::wstring ResolveModulePath(const std::wstring& module,
                              const std::wstring& processName) {
    if (module.find(L'\\') != std::wstring::npos) {
        return FileExists(module) ? module : std::wstring();
    }

    const auto& live = LiveModules(processName);
    auto it = live.find(ToLower(module));
    if (it != live.end() && FileExists(it->second)) {
        return it->second;
    }

    wchar_t windows[MAX_PATH];
    if (!GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) {
        return std::wstring();
    }
    const std::wstring root = windows;

    // Sysnative is not a real directory; System32 from a 64-bit process is the
    // 64-bit directory, which is what we want.
    const std::wstring candidates[] = {
        root + L"\\System32\\" + module,
        root + L"\\" + module,
        root + L"\\SysWOW64\\" + module,
    };
    for (const auto& candidate : candidates) {
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return std::wstring();
}

// The fingerprint the shim checks at load time. Both fields come from the PE
// headers, so reading them from the file on disk gives the same values the
// loader will see for the mapped image.
bool ReadFingerprint(const std::wstring& path,
                     DWORD* timestamp,
                     DWORD* sizeOfImage) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    file.seekg(dos.e_lfanew, std::ios::beg);
    DWORD signature = 0;
    file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    if (!file || signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    IMAGE_FILE_HEADER fileHeader{};
    file.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
    if (!file) {
        return false;
    }
    *timestamp = fileHeader.TimeDateStamp;

    // SizeOfImage sits at the same offset in the 32- and 64-bit optional
    // headers, but read the right one anyway rather than relying on that.
    WORD magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!file) {
        return false;
    }
    file.seekg(-static_cast<int>(sizeof(magic)), std::ios::cur);

    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 opt{};
        file.read(reinterpret_cast<char*>(&opt), sizeof(opt));
        if (!file) {
            return false;
        }
        *sizeOfImage = opt.SizeOfImage;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 opt{};
        file.read(reinterpret_cast<char*>(&opt), sizeof(opt));
        if (!file) {
            return false;
        }
        *sizeOfImage = opt.SizeOfImage;
    } else {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// DIA
// ---------------------------------------------------------------------------

struct ResolvedSymbol {
    DWORD rva;
    std::wstring decorated;
    std::wstring undecorated;
};

// Walks the module's PDB and picks out every symbol whose decorated or
// undecorated name is in `wanted`.
//
// The symbol tags and the choice of get_undecoratedName (rather than
// get_undecoratedNameEx with custom flags) mirror windhawk-symbol-helper's
// SymbolEnum, because the mods' hook strings were written against exactly that
// spelling. Changing either would silently stop matching.
bool CollectSymbols(IDiaDataSource* source,
                    const std::set<std::wstring>& wanted,
                    std::map<std::wstring, ResolvedSymbol>* out) {
    ComPtr<IDiaSession> session;
    if (FAILED(source->openSession(session.put()))) {
        Fail(L"openSession failed");
        return false;
    }

    ComPtr<IDiaSymbol> global;
    if (FAILED(session->get_globalScope(global.put()))) {
        Fail(L"get_globalScope failed");
        return false;
    }

    const enum SymTagEnum kSymTags[] = {
        SymTagPublicSymbol,
        SymTagFunction,
        SymTagData,
    };

    for (enum SymTagEnum tag : kSymTags) {
        ComPtr<IDiaEnumSymbols> symbols;
        if (FAILED(global->findChildren(tag, nullptr, nsNone, symbols.put()))) {
            continue;
        }

        for (;;) {
            ComPtr<IDiaSymbol> symbol;
            ULONG fetched = 0;
            HRESULT hr = symbols->Next(1, symbol.put(), &fetched);
            if (FAILED(hr) || hr == S_FALSE || fetched == 0) {
                break;
            }

            DWORD rva = 0;
            if (symbol->get_relativeVirtualAddress(&rva) != S_OK) {
                continue;
            }

            Bstr decorated;
            symbol->get_name(decorated.put());
            Bstr undecorated;
            symbol->get_undecoratedName(undecorated.put());

            const std::wstring decoratedName = decorated.str();
            const std::wstring undecoratedName = undecorated.str();

            for (const std::wstring* name : {&decoratedName, &undecoratedName}) {
                if (name->empty() || !wanted.count(*name)) {
                    continue;
                }
                // First hit wins, matching how the shim resolves at load time.
                if (out->find(*name) == out->end()) {
                    out->emplace(*name,
                                 ResolvedSymbol{rva, decoratedName,
                                                undecoratedName});
                }
            }
        }
    }

    return true;
}

bool WriteSymFile(const std::wstring& path,
                  const std::wstring& moduleName,
                  const std::wstring& modulePath,
                  DWORD timestamp,
                  DWORD sizeOfImage,
                  const std::map<std::wstring, ResolvedSymbol>& resolved) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        Fail(L"cannot write %s", path.c_str());
        return false;
    }

    // The two fingerprint lines are load-bearing: the shim refuses every symbol
    // hook in this module unless they match the image it finds mapped.
    file << "# shellmods symbol file -- generated by symgen.exe, do not edit\n";
    file << "# module=" << WideToUtf8(moduleName) << "\n";
    char buffer[64];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%08X", timestamp);
    file << "# timestamp=" << buffer << "\n";
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%08X", sizeOfImage);
    file << "# sizeofimage=" << buffer << "\n";
    file << "# source=" << WideToUtf8(modulePath) << "\n";

    // Deduplicate: a symbol requested under both its decorated and undecorated
    // name should appear once.
    std::set<DWORD> written;
    for (const auto& entry : resolved) {
        const ResolvedSymbol& symbol = entry.second;
        if (!written.insert(symbol.rva).second) {
            continue;
        }
        _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%08X", symbol.rva);
        file << buffer << "\t" << WideToUtf8(symbol.decorated) << "\t"
             << WideToUtf8(symbol.undecorated) << "\n";
    }

    return true;
}

bool ProcessModule(const std::wstring& moduleName,
                   const Request& request,
                   const std::wstring& msdiaPath,
                   const std::wstring& symbolCacheDir,
                   const std::wstring& symbolServer,
                   const std::wstring& outputDir,
                   const std::wstring& processName,
                   bool* anyResolved) {
    const std::wstring modulePath = ResolveModulePath(moduleName, processName);
    if (modulePath.empty()) {
        // Expected and fine: the taskbar mod names alternative modules for
        // different Windows builds, and only some exist on any one machine.
        Info(L"  %s: not present on this machine, skipping", moduleName.c_str());
        return true;
    }

    DWORD timestamp = 0;
    DWORD sizeOfImage = 0;
    if (!ReadFingerprint(modulePath, &timestamp, &sizeOfImage)) {
        Fail(L"cannot read PE headers of %s", modulePath.c_str());
        return false;
    }

    // Find out which PDB this exact binary wants, then make sure we have it.
    symgen::PdbInfo pdbInfo;
    if (!symgen::ReadPdbInfo(modulePath, &pdbInfo)) {
        Fail(L"%s has no CodeView debug record", moduleName.c_str());
        return false;
    }

    std::wstring pdbPath;
    std::wstring error;
    if (!symgen::EnsurePdb(symbolCacheDir, symbolServer, pdbInfo, &pdbPath,
                           &error)) {
        Fail(L"%s: %s", moduleName.c_str(), error.c_str());
        return false;
    }
    Info(L"  %s: pdb %s", moduleName.c_str(), pdbPath.c_str());

    ComPtr<IDiaDataSource> source;
    HRESULT hr = NoRegCoCreate(msdiaPath.c_str(), CLSID_DiaSource,
                               IID_IDiaDataSource,
                               reinterpret_cast<void**>(source.put()));
    if (FAILED(hr)) {
        Fail(L"NoRegCoCreate(%s) failed: 0x%08X", msdiaPath.c_str(), hr);
        return false;
    }

    // Validating GUID and age here means a cache entry that somehow belongs to a
    // different build is rejected rather than silently producing wrong offsets.
    hr = source->loadAndValidateDataFromPdb(pdbPath.c_str(), &pdbInfo.guid, 0,
                                            pdbInfo.age);
    if (FAILED(hr)) {
        Fail(L"loadAndValidateDataFromPdb(%s) failed: 0x%08X", pdbPath.c_str(),
             hr);
        return false;
    }

    std::map<std::wstring, ResolvedSymbol> resolved;
    if (!CollectSymbols(source, request.names, &resolved)) {
        return false;
    }

    if (resolved.empty()) {
        Info(L"  %s: no requested symbol found here", moduleName.c_str());
        return true;
    }

    const std::wstring outPath = outputDir + L"\\" + moduleName + L".sym";
    if (!WriteSymFile(outPath, moduleName, modulePath, timestamp, sizeOfImage,
                      resolved)) {
        return false;
    }

    Info(L"  %s: %zu symbol(s) -> %s.sym  [%08X/%08X]", moduleName.c_str(),
         resolved.size(), moduleName.c_str(), timestamp, sizeOfImage);
    *anyResolved = true;
    return true;
}

int Usage() {
    wprintf(
        L"symgen -- resolve Windhawk symbol names to RVAs for shellmods\n"
        L"\n"
        L"usage: symgen.exe [options] [<mod>.symreq ...]\n"
        L"\n"
        L"With no .symreq given, every file in symreq\\ beside this executable\n"
        L"is processed, and .sym files are written to offsets\\ beside it.\n"
        L"\n"
        L"options:\n"
        L"  --symreq <dir>    where to look for .symreq files\n"
        L"  --out <dir>       where to write .sym files\n"
        L"  --cache <dir>     local PDB cache (default: %%TEMP%%\\shellmods-symbols)\n"
        L"  --server <url>    symbol server (default: Microsoft public)\n"
        L"  --msdia <path>    msdia140.dll to use (default: beside symgen.exe)\n");
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring exeDir = DirOfThisExe();

    // Both empty unless overridden; resolved from the data root after parsing.
    std::wstring outputDir;
    std::wstring symreqDir;
    std::wstring symbolCacheDir;
    std::wstring symbolServer = kDefaultSymbolServer;
    // Left empty unless --msdia is given. LocateMsdia treats a non-empty value
    // as an explicit request and will not fall back to searching, so seeding it
    // with a guess here would defeat the whole discovery path.
    std::wstring msdiaPath;
    // Module paths are resolved from whatever this process currently has mapped,
    // which is the only reliable way to find the taskbar's SystemApps DLLs.
    std::wstring processName = L"explorer.exe";
    std::vector<std::wstring> requestFiles;

    for (int i = 1; i < argc; i++) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring {
            return (i + 1 < argc) ? argv[++i] : std::wstring();
        };
        if (arg == L"--out") {
            outputDir = next();
        } else if (arg == L"--symreq") {
            symreqDir = next();
        } else if (arg == L"--cache") {
            symbolCacheDir = next();
        } else if (arg == L"--server") {
            symbolServer = next();
        } else if (arg == L"--msdia") {
            msdiaPath = next();
        } else if (arg == L"--process") {
            processName = next();
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            return Usage();
        } else if (!arg.empty() && arg[0] == L'-') {
            Fail(L"unknown option %s", arg.c_str());
            return Usage();
        } else {
            requestFiles.push_back(arg);
        }
    }

    if (symbolCacheDir.empty()) {
        wchar_t temp[MAX_PATH];
        DWORD n = GetTempPathW(ARRAYSIZE(temp), temp);
        std::wstring base = (n > 0 && n < ARRAYSIZE(temp)) ? temp : L".\\";
        if (!base.empty() && base.back() == L'\\') {
            base.pop_back();
        }
        symbolCacheDir = base + L"\\shellmods-symbols";
    }

    // Resolve the data root even when .symreq files were named explicitly, so
    // that --out still has a sensible default.
    std::vector<std::wstring> searchedForSymreq;
    const std::wstring dataRoot = FindDataRoot(exeDir, &searchedForSymreq);

    if (symreqDir.empty() && !dataRoot.empty()) {
        symreqDir = dataRoot + L"\\symreq";
    }
    if (outputDir.empty()) {
        outputDir = (dataRoot.empty() ? exeDir : dataRoot) + L"\\offsets";
    }

    if (requestFiles.empty()) {
        if (symreqDir.empty()) {
            std::wstring looked;
            for (const std::wstring& candidate : searchedForSymreq) {
                looked += L"\n    " + candidate;
            }
            Fail(L"no .symreq files found. They say which symbols each mod "
                 L"needs, and ship in symreq\\ beside this executable.\n"
                 L"  looked in:%s\n"
                 L"  pass --symreq <dir>, or name .symreq files as arguments.",
                 looked.c_str());
            return 1;
        }

        WIN32_FIND_DATAW found{};
        HANDLE search =
            FindFirstFileW((symreqDir + L"\\*.symreq").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                requestFiles.push_back(symreqDir + L"\\" + found.cFileName);
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
        if (requestFiles.empty()) {
            Fail(L"no .symreq files in %s", symreqDir.c_str());
            return 1;
        }
    }

    std::wstring searched;
    msdiaPath = symgen::LocateMsdia(msdiaPath, exeDir, &searched);
    if (msdiaPath.empty()) {
        Fail(L"cannot find msdia140.dll. It ships with Visual Studio 2015 and "
             L"newer; install any edition, including Build Tools, or pass "
             L"--msdia <path>.\n  looked in:%s",
             searched.c_str());
        return 1;
    }
    Info(L"msdia: %s", msdiaPath.c_str());

    CreateDirectoryW(symbolCacheDir.c_str(), nullptr);
    CreateDirectoryW(outputDir.c_str(), nullptr);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        Fail(L"CoInitializeEx failed: 0x%08X", hr);
        return 1;
    }

    int failures = 0;
    for (const std::wstring& requestFile : requestFiles) {
        Info(L"%s", requestFile.c_str());

        Request request;
        if (!ReadRequest(requestFile, &request)) {
            failures++;
            continue;
        }
        Info(L"  %zu module(s), %zu name(s) requested", request.modules.size(),
             request.names.size());

        bool anyResolved = false;
        for (const std::wstring& module : request.modules) {
            if (!ProcessModule(module, request, msdiaPath, symbolCacheDir,
                               symbolServer, outputDir, processName,
                               &anyResolved)) {
                failures++;
            }
        }

        if (!anyResolved) {
            Fail(L"%s resolved nothing at all -- the mods that need it will "
                 L"refuse to hook",
                 requestFile.c_str());
            failures++;
        }
    }

    CoUninitialize();

    if (failures) {
        Fail(L"%d failure(s)", failures);
        return 1;
    }
    Info(L"done");
    return 0;
}
