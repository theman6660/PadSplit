#pragma once

#include "../shared/protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace padsplit::core {

extern HMODULE g_hModule;
extern SharedState* g_shm;
extern HANDLE g_shmHandle;
extern bool g_shmOwnedByCore;
extern HWND g_gameHwnd;
extern DWORD g_gameThreadId;
extern RECT g_gameMonRect;
extern volatile LONG g_detaching;

// Original unhooked user32 pointers (populated by hooks_focus / hooks_cursor / hooks_input)
using FnGetForegroundWindow = HWND(WINAPI*)();
using FnGetActiveWindow = HWND(WINAPI*)();
using FnGetFocus = HWND(WINAPI*)();
using FnClipCursor = BOOL(WINAPI*)(const RECT*);
using FnSetCursorPos = BOOL(WINAPI*)(int, int);
using FnShowCursor = int(WINAPI*)(BOOL);
using FnSetCursor = HCURSOR(WINAPI*)(HCURSOR);
using FnGetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using FnRegisterRawInputDevices = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);

extern FnGetForegroundWindow g_origGetForegroundWindow;
extern FnGetActiveWindow g_origGetActiveWindow;
extern FnGetFocus g_origGetFocus;
extern FnClipCursor g_origClipCursor;
extern FnSetCursorPos g_origSetCursorPos;
extern FnShowCursor g_origShowCursor;
extern FnSetCursor g_origSetCursor;
extern FnGetRawInputData g_origGetRawInputData;
extern FnRegisterRawInputDevices g_origRegisterRawInputDevices;

bool OpenSharedState();
void CloseSharedState();
void CoreLog(const char* fmt, ...);
bool DiscoverGameWindow();
void UpdateGameMonitorRect();
void DetectSteamOverlay();

bool IsCursorOutsideGameMonitor();
bool ShouldSpoofFocus();
bool ShouldBlockKBM();

} // namespace padsplit::core
