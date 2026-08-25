// Wh_Disasm, backed by Zydis.
//
// Windhawk's engine uses Zydis for this too, which matters more than it looks:
// the taskbar mod does not inspect the decoded instruction structurally, it runs
// std::regex over the *formatted text* to recover a struct field offset. So the
// formatter's exact spelling is part of the contract. Using Zydis with default
// Intel formatter settings, as Windhawk does, is what keeps a pattern like
//
//     movsd xmm\d+, qword ptr \[rcx\+0x([0-9a-f]+)\]
//
// matching. A formatter that spelled the same instruction differently would
// silently fail to match, and the mod would fall back to its default offset.
//
// On x86-64 exactly one such pattern is live; every other Wh_Disasm site in the
// taskbar mod sits inside #ifdef _M_ARM64.

#include <windows.h>

#include <mutex>

#include "Zydis/Zydis.h"
#include "shim_runtime.h"
#include "windhawk_api.h"

namespace shim {
namespace {

std::once_flag g_initOnce;
ZydisDecoder g_decoder;
ZydisFormatter g_formatter;
bool g_ready = false;

void InitDisassembler() {
#if defined(_M_X64)
    const ZydisMachineMode machineMode = ZYDIS_MACHINE_MODE_LONG_64;
    const ZydisStackWidth stackWidth = ZYDIS_STACK_WIDTH_64;
#elif defined(_M_IX86)
    const ZydisMachineMode machineMode = ZYDIS_MACHINE_MODE_LEGACY_32;
    const ZydisStackWidth stackWidth = ZYDIS_STACK_WIDTH_32;
#else
#error "shim_disasm.cpp supports x86 and x86-64 only"
#endif

    if (ZYAN_FAILED(ZydisDecoderInit(&g_decoder, machineMode, stackWidth))) {
        return;
    }
    if (ZYAN_FAILED(
            ZydisFormatterInit(&g_formatter, ZYDIS_FORMATTER_STYLE_INTEL))) {
        return;
    }
    g_ready = true;
}

}  // namespace
}  // namespace shim

using namespace shim;

extern "C" BOOL InternalWh_Disasm(void* mod,
                                  void* address,
                                  WH_DISASM_RESULT* result) {
    UNREFERENCED_PARAMETER(mod);

    if (!address || !result) {
        return FALSE;
    }

    std::call_once(g_initOnce, InitDisassembler);
    if (!g_ready) {
        return FALSE;
    }

    result->length = 0;
    result->text[0] = 0;

    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    // The caller walks arbitrary code, so the bytes after `address` may not all
    // be readable. Bound the read at the longest possible instruction and let
    // the decoder tell us the real length.
    const ZyanUSize maxLength = ZYDIS_MAX_INSTRUCTION_LENGTH;

    ZyanStatus status;
    __try {
        status = ZydisDecoderDecodeFull(&g_decoder, address, maxLength,
                                        &instruction, operands);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }

    if (ZYAN_FAILED(status)) {
        return FALSE;
    }

    result->length = instruction.length;

    // Runtime address is passed so that relative operands format as absolute
    // targets, matching what Windhawk produces.
    if (ZYAN_FAILED(ZydisFormatterFormatInstruction(
            &g_formatter, &instruction, operands,
            instruction.operand_count_visible, result->text,
            sizeof(result->text), reinterpret_cast<ZyanU64>(address),
            ZYAN_NULL))) {
        result->text[0] = 0;
    }

    return TRUE;
}
