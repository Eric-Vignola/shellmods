// Guards the most fragile assumption in the project.
//
// taskbar-icon-size does not inspect decoded instructions structurally. It runs
// std::regex over the *text* Wh_Disasm produces, to recover the byte offset of a
// field inside an undocumented C++ object:
//
//     movsd xmm\d+, qword ptr \[rcx\+0x([0-9a-f]+)\]
//
// That means the disassembler's exact spelling is part of the contract with the
// mod. Windhawk's engine uses Zydis with default Intel formatter settings, so we
// do too -- but "so we do too" is an assumption, and if a Zydis update ever
// changed the spacing, the operand-size prefix or the immediate format, the
// regex would quietly stop matching and the mod would silently fall back to its
// default offset of 0. Nothing would crash; the taskbar would just be subtly
// wrong in a way that is miserable to trace back to here.
//
// So: assemble the exact instruction the mod looks for, run it through our
// Wh_Disasm, and assert that the mod's own regex still matches and extracts the
// right offset.

#include <windows.h>

#include <cstdio>
#include <regex>
#include <string>
#include <string_view>

#include "windhawk_api.h"

namespace {

int g_failures;

void Check(bool condition, const char* what) {
    printf("%s  %s\n", condition ? "[ ok ]" : "[FAIL]", what);
    if (!condition) {
        g_failures++;
    }
}

// The regex from taskbar-icon-size's GetIconHeightOffset(), verbatim.
const std::regex kIconHeightRegex(
    R"(movsd xmm\d+, qword ptr \[rcx\+0x([0-9a-f]+)\])",
    std::regex_constants::icase);

// Copies `bytes` into executable memory, because Wh_Disasm decodes at an
// address and formats relative operands against it.
void* MakeCode(const BYTE* bytes, size_t size) {
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (page) {
        memcpy(page, bytes, size);
    }
    return page;
}

void TestIconHeightPattern() {
    // movsd xmm0, qword ptr [rcx+0x50]   ->  F2 0F 10 41 50
    //   F2 0F 10 /r  = MOVSD xmm, m64
    //   ModRM 0x41   = mod 01 (disp8), reg 000 (xmm0), rm 001 (rcx)
    const BYTE code[] = {0xF2, 0x0F, 0x10, 0x41, 0x50, 0xC3};
    void* page = MakeCode(code, sizeof(code));
    Check(page != nullptr, "allocated executable page");
    if (!page) {
        return;
    }

    WH_DISASM_RESULT result{};
    Check(Wh_Disasm(page, &result) != FALSE, "Wh_Disasm succeeded");
    printf("       text: \"%s\"  length: %zu\n", result.text, result.length);

    Check(result.length == 5, "instruction length is 5");

    std::string_view text = result.text;
    std::match_results<std::string_view::const_iterator> match;
    const bool matched =
        std::regex_match(text.begin(), text.end(), match, kIconHeightRegex);
    Check(matched, "taskbar-icon-size's iconHeightOffset regex matches");

    if (matched) {
        const unsigned long offset = std::stoul(match[1], nullptr, 16);
        printf("       extracted offset: 0x%lX\n", offset);
        Check(offset == 0x50, "extracted offset is 0x50");
    }

    // The same helper stops scanning when it sees a bare "ret", so that spelling
    // matters too.
    WH_DISASM_RESULT ret{};
    Check(Wh_Disasm(static_cast<BYTE*>(page) + 5, &ret) != FALSE,
          "Wh_Disasm decoded the trailing instruction");
    printf("       text: \"%s\"\n", ret.text);
    Check(std::string_view(ret.text) == "ret",
          "a return formats exactly as \"ret\"");

    VirtualFree(page, 0, MEM_RELEASE);
}

}  // namespace

int main() {
    printf("disasm_test -- Wh_Disasm text contract\n\n");
    TestIconHeightPattern();
    printf("\n%s\n", g_failures ? "FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
