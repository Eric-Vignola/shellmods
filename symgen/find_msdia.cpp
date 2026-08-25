#include "find_msdia.h"

#include <windows.h>

#include <vector>

namespace symgen {

namespace {

// The DIA binary that matches the toolset this project builds with. Older
// Visual Studio versions ship msdia120/msdia100 and so on; we only want 140.
constexpr wchar_t kMsdia[] = L"msdia140.dll";

// Relative to a Visual Studio installation root. amd64 because symgen is x64 and
// the COM object is loaded in-process.
constexpr wchar_t kDiaSubPath[] = L"DIA SDK\\bin\\amd64\\msdia140.dll";

bool IsReadableFile(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void Note(std::wstring* tried, const std::wstring& what) {
    if (tried) {
        *tried += L"\n    " + what;
    }
}

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buffer[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
    return (n > 0 && n < ARRAYSIZE(buffer)) ? buffer : std::wstring();
}

// Runs vswhere and returns its stdout. vswhere lives at a fixed, documented
// location whenever any Visual Studio 2017 or newer is installed, and it is the
// supported way to enumerate installations -- directory guessing breaks on
// side-by-side installs, which put the version in the path
// (…\2026\Professional\18.6.2).
std::wstring RunVsWhere() {
    std::wstring vswhere = EnvVar(L"ProgramFiles(x86)");
    if (vswhere.empty()) {
        return std::wstring();
    }
    vswhere += L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!IsReadableFile(vswhere)) {
        return std::wstring();
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        return std::wstring();
    }
    // Only the write end should reach the child.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    // -products * so Build Tools installations count too; they carry the DIA SDK
    // just as the full IDE does.
    std::wstring commandLine = L"\"" + vswhere +
                               L"\" -latest -prerelease -products * "
                               L"-property installationPath";

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(),
                                            commandLine.end());
    mutableCommandLine.push_back(0);

    if (!CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr,
                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return std::wstring();
    }

    // Must close our copy of the write end, or the read below never sees EOF.
    CloseHandle(writeEnd);

    std::string output;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) &&
           read > 0) {
        output.append(buffer, read);
    }

    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (output.empty()) {
        return std::wstring();
    }
    int chars = MultiByteToWideChar(CP_ACP, 0, output.data(),
                                    static_cast<int>(output.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(chars), 0);
    MultiByteToWideChar(CP_ACP, 0, output.data(),
                        static_cast<int>(output.size()), wide.data(), chars);
    return wide;
}

std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstring current;
    for (wchar_t c : text) {
        if (c == L'\r' || c == L'\n') {
            if (!current.empty()) {
                lines.push_back(current);
                current.clear();
            }
            continue;
        }
        current += c;
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

// Fallback for when vswhere is absent: look where Visual Studio installs by
// default. Two levels deep covers …\2022\Enterprise; three covers side-by-side
// layouts such as …\2026\Professional\18.6.2.
std::vector<std::wstring> DefaultInstallRoots() {
    std::vector<std::wstring> roots;
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        const std::wstring base = EnvVar(var);
        if (base.empty()) {
            continue;
        }
        const std::wstring vsRoot = base + L"\\Microsoft Visual Studio";

        WIN32_FIND_DATAW year{};
        HANDLE yearSearch = FindFirstFileW((vsRoot + L"\\*").c_str(), &year);
        if (yearSearch == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if (!(year.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                year.cFileName[0] == L'.') {
                continue;
            }
            const std::wstring yearPath = vsRoot + L"\\" + year.cFileName;

            WIN32_FIND_DATAW edition{};
            HANDLE editionSearch =
                FindFirstFileW((yearPath + L"\\*").c_str(), &edition);
            if (editionSearch == INVALID_HANDLE_VALUE) {
                continue;
            }
            do {
                if (!(edition.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    edition.cFileName[0] == L'.') {
                    continue;
                }
                const std::wstring editionPath =
                    yearPath + L"\\" + edition.cFileName;
                roots.push_back(editionPath);

                // One level further, for version-suffixed installs.
                WIN32_FIND_DATAW version{};
                HANDLE versionSearch =
                    FindFirstFileW((editionPath + L"\\*").c_str(), &version);
                if (versionSearch == INVALID_HANDLE_VALUE) {
                    continue;
                }
                do {
                    if ((version.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                        version.cFileName[0] != L'.') {
                        roots.push_back(editionPath + L"\\" +
                                        version.cFileName);
                    }
                } while (FindNextFileW(versionSearch, &version));
                FindClose(versionSearch);
            } while (FindNextFileW(editionSearch, &edition));
            FindClose(editionSearch);
        } while (FindNextFileW(yearSearch, &year));
        FindClose(yearSearch);
    }
    return roots;
}

}  // namespace

std::wstring LocateMsdia(const std::wstring& explicitPath,
                         const std::wstring& exeDir,
                         std::wstring* tried) {
    if (!explicitPath.empty()) {
        if (IsReadableFile(explicitPath)) {
            return explicitPath;
        }
        Note(tried, L"--msdia " + explicitPath + L"  (not found)");
        // Deliberately stop here. If someone names a path explicitly, silently
        // using a different copy would be worse than failing.
        return std::wstring();
    }

    const std::wstring beside = exeDir + L"\\" + kMsdia;
    if (IsReadableFile(beside)) {
        return beside;
    }
    Note(tried, beside);

    for (const std::wstring& root : SplitLines(RunVsWhere())) {
        const std::wstring candidate = root + L"\\" + kDiaSubPath;
        if (IsReadableFile(candidate)) {
            return candidate;
        }
        Note(tried, candidate + L"  (from vswhere)");
    }

    for (const std::wstring& root : DefaultInstallRoots()) {
        const std::wstring candidate = root + L"\\" + kDiaSubPath;
        if (IsReadableFile(candidate)) {
            return candidate;
        }
    }

    Note(tried, L"the default Visual Studio directories");
    return std::wstring();
}

}  // namespace symgen
