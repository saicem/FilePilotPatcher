#pragma once
#include <windows.h>
namespace fpp::agent {
// Called after MinHook initialization and before enabling this adapter's hooks.
bool installChildDeliveryHooks(bool (*enabled)());
}
