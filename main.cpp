// DesktopWidget -- a standalone desktop hardware widget: NZXT-CAM-style tiles
// with big values (CPU and GPU load / clock / temperature / fan, motherboard
// fans, RAM and VRAM, disk, network), colored per category and freely placed
// on the desktop. Everything the widget needs is in this process: a single
// tray icon with its menu (Settings, Start with Windows, Exit), a settings
// window, and the sensors. Settings live in %APPDATA%\DesktopWidget\settings.ini.
//
// This file is the process entry: it enforces a single instance, then hands
// control to the widget's run loop (DesktopWidget.cpp).

#include "DesktopWidget.h"
#include "HostCommon.h"

#include <windows.h>

namespace {

// Shared with DesktopWidget.cpp. A second launch opens the running widget's
// settings and exits, so there is never a second widget.
constexpr wchar_t kInstanceMutex[] = L"DesktopWidget";
constexpr wchar_t kSettingsEvent[] = L"DesktopWidgetSettings";

// Pokes the already-running instance to show its settings window.
void PokeRunningInstance() {
	HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kSettingsEvent);
	if (ev) {
		SetEvent(ev);
		CloseHandle(ev);
	}
}

// Per-monitor DPI awareness so the tiles stay crisp on high-DPI displays,
// loaded at runtime so the exe still starts on older Windows that lack it.
void EnablePerMonitorDpi() {
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32) {
		using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
		auto setCtx = reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
		if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
			return;
		}
	}
	SetProcessDPIAware(); // system-DPI fallback
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
	// Single instance: take the named mutex. If it already exists, another copy
	// is running -- bring up its settings and quit.
	HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
	if (!mutex) {
		return 1;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		PokeRunningInstance();
		CloseHandle(mutex);
		return 0;
	}

	EnablePerMonitorDpi();

	widget::RunWidget();

	ReleaseMutex(mutex);
	CloseHandle(mutex);
	return 0;
}
