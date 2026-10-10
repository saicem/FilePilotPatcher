#pragma once
#include "client.h"
#include "agent_shared.h"

namespace fpp::modern {
// The dialog implementation is shared; delivery and module ownership are not.
struct Runtime {
    std::wstring pickerExecutable;
    bool (*enabled)() = nullptr;
    void (*markRoute)(agent::DialogRoute, DWORD) = nullptr;
    void (*enterShow)() = nullptr;
    void (*leaveShow)() = nullptr;
    void (*prepareRequest)(Request &) = nullptr;
    void (*retainModule)() = nullptr;
    void (*releaseModule)() = nullptr;
};
bool knownInterface(REFIID id, bool save);
HRESULT wrap(IUnknown *object, bool save, REFIID id, void **out, Runtime runtime);
} // namespace fpp::modern
