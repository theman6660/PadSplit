#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace padsplit::core {

bool InstallFocusHooks();
void AttachWndProc(HWND hwnd);
void EnsureWndProcAttached(HWND hwnd);
void DetachWndProc();

} // namespace padsplit::core
