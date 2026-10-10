#pragma once
#include "protocol.h"
#include <functional>

namespace fpp {
using Callback = std::function<HRESULT(Event, Result &, std::wstring &)>;
HRESULT runPicker(const std::wstring &executable, Request request, HWND owner,
                  const Callback &callback, Result &result, HANDLE *cancellation = nullptr,
                  HWND *dialogWindow = nullptr, Shared **shared = nullptr,
                  bool manageOwner = true, HANDLE externalCancellation = nullptr);
HRESULT controlPicker(Shared *shared, HWND window, Request &request, DWORD changes);
HRESULT shellItem(const std::wstring &path, IShellItem **out);
HRESULT shellArray(const std::vector<std::wstring> &paths, IShellItemArray **out);
HRESULT validate(const Request &request, Result &result, std::wstring &message);
} // namespace fpp
