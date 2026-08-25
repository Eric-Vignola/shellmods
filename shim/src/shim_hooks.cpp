// Wh_SetFunctionHook and friends, backed by MinHook.
//
// Windhawk uses MinHook too, so the semantics line up closely. The one thing we
// deliberately reproduce is the register-then-apply split: Wh_SetFunctionHook
// only records the intent, and nothing is patched until ApplyHookOperations
// runs a single MH_ApplyQueued. That matters because MinHook suspends every
// other thread for the duration of an apply, and doing it once for forty hooks
// instead of forty times is the difference between an imperceptible hitch and a
// visibly frozen shell.
//
// Known divergence from Windhawk: Windhawk can chain several mods onto one
// target function and can unhook safely while other threads are executing
// inside a detour (it scans thread call stacks to decide when that is safe).
// MinHook does neither. Our three mods hook disjoint targets, so chaining never
// arises; unload safety is handled by not unhooking except on explicit request.

#include <windows.h>

#include <mutex>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "shim_runtime.h"

namespace shim {

namespace {

std::mutex g_mutex;
bool g_minHookReady = false;

// Targets this DLL has successfully created a MinHook hook for. Kept so that
// HookShutdownAll can retire exactly what we installed.
std::vector<void*> g_installed;

bool EnsureMinHook() {
    if (g_minHookReady) {
        return true;
    }
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Logf(L"MH_Initialize failed: %d", static_cast<int>(status));
        return false;
    }
    g_minHookReady = true;
    return true;
}

}  // namespace

bool HookRegister(void* target, void* detour, void** original) {
    if (!target || !detour) {
        Logf(L"HookRegister refused: target=%p detour=%p", target, detour);
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!EnsureMinHook()) {
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        Logf(L"MH_CreateHook(%p) failed: %d", target, static_cast<int>(status));
        return false;
    }

    status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        Logf(L"MH_QueueEnableHook(%p) failed: %d", target,
             static_cast<int>(status));
        MH_RemoveHook(target);
        return false;
    }

    g_installed.push_back(target);
    return true;
}

bool HookUnregister(void* target) {
    if (!target) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_minHookReady) {
        return false;
    }

    MH_STATUS status = MH_QueueDisableHook(target);
    if (status != MH_OK) {
        Logf(L"MH_QueueDisableHook(%p) failed: %d", target,
             static_cast<int>(status));
        return false;
    }
    return true;
}

bool ApplyHookOperations() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_minHookReady) {
        // No hook was ever registered. Windhawk treats an empty apply as
        // success, and so do we, otherwise a mod that only reads settings would
        // report a spurious failure.
        return true;
    }

    MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) {
        Logf(L"MH_ApplyQueued failed: %d", static_cast<int>(status));
        return false;
    }
    return true;
}

void HookShutdownAll() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_minHookReady) {
        return;
    }

    for (void* target : g_installed) {
        MH_QueueDisableHook(target);
    }
    MH_ApplyQueued();

    for (void* target : g_installed) {
        MH_RemoveHook(target);
    }
    g_installed.clear();

    MH_Uninitialize();
    g_minHookReady = false;
}

}  // namespace shim

using namespace shim;

extern "C" {

BOOL InternalWh_SetFunctionHook(void* mod,
                                void* targetFunction,
                                void* hookFunction,
                                void** originalFunction) {
    UNREFERENCED_PARAMETER(mod);
    return HookRegister(targetFunction, hookFunction, originalFunction) ? TRUE
                                                                        : FALSE;
}

BOOL InternalWh_RemoveFunctionHook(void* mod, void* targetFunction) {
    UNREFERENCED_PARAMETER(mod);
    return HookUnregister(targetFunction) ? TRUE : FALSE;
}

BOOL InternalWh_ApplyHookOperations(void* mod) {
    UNREFERENCED_PARAMETER(mod);
    return ApplyHookOperations() ? TRUE : FALSE;
}

}  // extern "C"
