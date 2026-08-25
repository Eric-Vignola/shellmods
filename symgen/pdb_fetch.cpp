#include "pdb_fetch.h"

#include <winhttp.h>

#include <cstdio>
#include <fstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace symgen {

namespace {

// The CodeView record a PE points at for its PDB. Not in the SDK headers.
struct CvInfoPdb70 {
    DWORD signature;  // 'SDSR'
    GUID guid;
    DWORD age;
    char pdbFileName[1];  // NUL-terminated, variable length
};

constexpr DWORD kRsdsSignature = 0x53445352;  // 'SDSR' little-endian

bool ReadWholeFile(const std::wstring& path, std::vector<BYTE>* out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return false;
    }
    out->resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(out->data()), size);
    return static_cast<bool>(file);
}

std::wstring Widen(const char* s) {
    if (!s || !*s) {
        return std::wstring();
    }
    int chars = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (chars <= 1) {
        return std::wstring();
    }
    std::wstring w(static_cast<size_t>(chars) - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), chars);
    return w;
}

std::wstring BaseName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Symbol server key: the GUID as 32 uppercase hex digits with no separators,
// immediately followed by the age in uppercase hex with no padding.
std::wstring SymbolKey(const PdbInfo& info) {
    wchar_t buffer[64];
    _snwprintf_s(buffer, ARRAYSIZE(buffer), _TRUNCATE,
                 L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
                 info.guid.Data1, info.guid.Data2, info.guid.Data3,
                 info.guid.Data4[0], info.guid.Data4[1], info.guid.Data4[2],
                 info.guid.Data4[3], info.guid.Data4[4], info.guid.Data4[5],
                 info.guid.Data4[6], info.guid.Data4[7], info.age);
    return buffer;
}

// Creates every missing component of a directory path.
bool CreateDirectoryTree(const std::wstring& path) {
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == L'\\') {
            if (i == 0) {
                continue;
            }
            const std::wstring part = path.substr(0, i);
            // Skip a bare drive letter such as "C:".
            if (part.size() == 2 && part[1] == L':') {
                continue;
            }
            if (!CreateDirectoryW(part.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                return false;
            }
        }
    }
    return true;
}

// Splits "https://host/a/b" into host and "/a/b".
bool SplitUrl(const std::wstring& url,
              std::wstring* host,
              std::wstring* path,
              INTERNET_PORT* port,
              bool* secure) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0,
                         &components)) {
        return false;
    }

    *host = std::wstring(components.lpszHostName, components.dwHostNameLength);
    *path = std::wstring(components.lpszUrlPath, components.dwUrlPathLength);
    *port = components.nPort;
    *secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

}  // namespace

bool ReadPdbInfo(const std::wstring& modulePath, PdbInfo* info) {
    std::vector<BYTE> image;
    if (!ReadWholeFile(modulePath, &image)) {
        return false;
    }
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    if (dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) >
            image.size()) {
        return false;
    }

    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() +
                                                          dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    // The debug data directory sits at the same index in both optional header
    // layouts, but the directory array itself is at different offsets.
    const IMAGE_DATA_DIRECTORY* debugDir = nullptr;
    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        auto nt64 = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt);
        if (nt64->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_DEBUG) {
            return false;
        }
        debugDir = &nt64->OptionalHeader
                        .DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    } else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (nt->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_DEBUG) {
            return false;
        }
        debugDir =
            &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    } else {
        return false;
    }

    if (debugDir->VirtualAddress == 0 || debugDir->Size == 0) {
        return false;
    }

    // IMAGE_DEBUG_DIRECTORY carries a file offset as well as an RVA, so once we
    // find the directory itself we can stay in file-offset space. Getting to the
    // directory does require an RVA-to-offset translation through the sections.
    auto sections = IMAGE_FIRST_SECTION(nt);
    auto RvaToOffset = [&](DWORD rva) -> const BYTE* {
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            const IMAGE_SECTION_HEADER& section = sections[i];
            if (rva >= section.VirtualAddress &&
                rva < section.VirtualAddress + section.SizeOfRawData) {
                size_t offset =
                    section.PointerToRawData + (rva - section.VirtualAddress);
                return offset < image.size() ? image.data() + offset : nullptr;
            }
        }
        return nullptr;
    };

    const BYTE* debugData = RvaToOffset(debugDir->VirtualAddress);
    if (!debugData) {
        return false;
    }

    const DWORD count = debugDir->Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    auto entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(debugData);
    for (DWORD i = 0; i < count; i++) {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
            continue;
        }
        const DWORD offset = entries[i].PointerToRawData;
        if (offset == 0 || offset + sizeof(CvInfoPdb70) > image.size()) {
            continue;
        }
        auto cv = reinterpret_cast<const CvInfoPdb70*>(image.data() + offset);
        if (cv->signature != kRsdsSignature) {
            continue;
        }

        // Bound the name read at the end of the debug entry so a malformed file
        // cannot walk us off the buffer.
        const size_t nameMax = entries[i].SizeOfData > sizeof(CvInfoPdb70)
                                   ? entries[i].SizeOfData -
                                         offsetof(CvInfoPdb70, pdbFileName)
                                   : 0;
        if (nameMax == 0 || offset + entries[i].SizeOfData > image.size()) {
            continue;
        }
        std::string name(cv->pdbFileName,
                         strnlen(cv->pdbFileName, nameMax));

        info->guid = cv->guid;
        info->age = cv->age;
        info->name = BaseName(Widen(name.c_str()));
        return !info->name.empty();
    }

    return false;
}

std::wstring CachedPdbPath(const std::wstring& cacheDir, const PdbInfo& info) {
    return cacheDir + L"\\" + info.name + L"\\" + SymbolKey(info) + L"\\" +
           info.name;
}

bool EnsurePdb(const std::wstring& cacheDir,
               const std::wstring& serverUrl,
               const PdbInfo& info,
               std::wstring* outPath,
               std::wstring* error) {
    const std::wstring localPath = CachedPdbPath(cacheDir, info);
    *outPath = localPath;

    if (GetFileAttributesW(localPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;  // Already cached.
    }

    const std::wstring url =
        serverUrl + L"/" + info.name + L"/" + SymbolKey(info) + L"/" + info.name;

    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
    if (!SplitUrl(url, &host, &path, &port, &secure)) {
        *error = L"cannot parse symbol server URL: " + url;
        return false;
    }

    HINTERNET session =
        WinHttpOpen(L"shellmods-symgen/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *error = L"WinHttpOpen failed";
        return false;
    }

    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        *error = L"WinHttpConnect failed for " + host;
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        *error = L"WinHttpOpenRequest failed";
        return false;
    }

    bool ok = false;
    do {
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            *error = L"request failed for " + url;
            break;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(
                request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                WINHTTP_NO_HEADER_INDEX)) {
            *error = L"cannot read response status for " + url;
            break;
        }
        if (status != 200) {
            wchar_t buffer[512];
            _snwprintf_s(buffer, ARRAYSIZE(buffer), _TRUNCATE,
                         L"symbol server returned HTTP %lu for %s", status,
                         url.c_str());
            *error = buffer;
            break;
        }

        // Write to a temporary name and move into place, so an interrupted
        // download cannot leave a truncated PDB that later looks cached.
        std::wstring dir = localPath.substr(0, localPath.find_last_of(L'\\'));
        if (!CreateDirectoryTree(dir)) {
            *error = L"cannot create cache directory " + dir;
            break;
        }
        const std::wstring tempPath = localPath + L".part";

        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = L"cannot write " + tempPath;
            break;
        }

        std::vector<char> buffer(64 * 1024);
        bool readOk = true;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                readOk = false;
                break;
            }
            if (available == 0) {
                break;
            }
            while (available > 0) {
                DWORD chunk = available < buffer.size()
                                  ? available
                                  : static_cast<DWORD>(buffer.size());
                DWORD read = 0;
                if (!WinHttpReadData(request, buffer.data(), chunk, &read) ||
                    read == 0) {
                    readOk = false;
                    break;
                }
                out.write(buffer.data(), read);
                available -= read;
            }
            if (!readOk) {
                break;
            }
        }
        out.close();

        if (!readOk) {
            DeleteFileW(tempPath.c_str());
            *error = L"download interrupted for " + url;
            break;
        }

        DeleteFileW(localPath.c_str());
        if (!MoveFileW(tempPath.c_str(), localPath.c_str())) {
            DeleteFileW(tempPath.c_str());
            *error = L"cannot move downloaded PDB into " + localPath;
            break;
        }

        ok = true;
    } while (false);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

}  // namespace symgen
