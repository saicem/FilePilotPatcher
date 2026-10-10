#pragma once
#include "agent_shared.h"
namespace fpp::agent {
struct AttachmentResult { ProcessState state; DWORD error; };
AttachmentResult attachProcess(HANDLE process, DWORD pid, const std::wstring &adapter);
enum class DeliveryStage : DWORD { Identity = 1, Mitigations, Loader, Adapter, Resume };
AttachmentResult attachSuspendedProcess(HANDLE process, DWORD pid, const std::wstring &adapter,
                                       const std::wstring &expectedImage, DeliveryStage &stage);
} // namespace fpp::agent
