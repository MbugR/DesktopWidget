#include "HostCommon.h"

#include <shlobj.h>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kSnapPx = 12;

constexpr wchar_t kAppFolder[] = L"DesktopWidget";
constexpr wchar_t kIniFile[] = L"settings.ini";

} // namespace

// %APPDATA%\DesktopWidget\ (created when missing).
bool Host_DataFolder(wchar_t* out, size_t cap) {
	out[0] = 0;
	wchar_t appData[MAX_PATH];
	if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData) != S_OK) {
		return false;
	}
	if (swprintf_s(out, cap, L"%s\\%s\\", appData, kAppFolder) <= 0) {
		return false;
	}
	// A failed create still leaves `out` usable when the folder already exists.
	wchar_t folder[MAX_PATH];
	swprintf_s(folder, L"%s\\%s", appData, kAppFolder);
	CreateDirectoryW(folder, nullptr);
	return true;
}

bool Host_IniPath(wchar_t* out, size_t cap) {
	wchar_t dir[MAX_PATH];
	if (!Host_DataFolder(dir, MAX_PATH)) {
		out[0] = 0;
		return false;
	}
	return swprintf_s(out, cap, L"%s%s", dir, kIniFile) > 0;
}

bool Host_CacheFolder(wchar_t* out, size_t cap) {
	return Host_DataFolder(out, cap);
}

// ---- placement ------------------------------------------------------------

void Place_FitToWorkArea(RECT& r, bool snap) {
	Place_FitToMonitor(r, MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), snap);
}

void Place_FitToCursorMonitor(RECT& r, bool snap) {
	POINT pt;
	GetCursorPos(&pt);
	Place_FitToMonitor(r, MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), snap);
}

void Place_FitToMonitor(RECT& r, HMONITOR mon, bool snap) {
	MONITORINFO mi = {};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(mon, &mi)) {
		return;
	}
	const RECT& wa = mi.rcWork;
	const int w = r.right - r.left;
	const int h = r.bottom - r.top;
	int x = r.left;
	int y = r.top;
	if (snap) {
		if (abs(x - wa.left) <= kSnapPx) {
			x = wa.left;
		}
		if (abs(wa.right - (x + w)) <= kSnapPx) {
			x = wa.right - w;
		}
		if (abs(y - wa.top) <= kSnapPx) {
			y = wa.top;
		}
		if (abs(wa.bottom - (y + h)) <= kSnapPx) {
			y = wa.bottom - h;
		}
	}
	if (x + w > wa.right) {
		x = wa.right - w;
	}
	if (y + h > wa.bottom) {
		y = wa.bottom - h;
	}
	if (x < wa.left) {
		x = wa.left;
	}
	if (y < wa.top) {
		y = wa.top;
	}
	OffsetRect(&r, x - r.left, y - r.top);
}

// GetPrivateProfileInt clamps negatives to 0, but a monitor left of the
// primary one has negative coordinates, so parse the strings ourselves.
bool Place_Load(const wchar_t* section, POINT& pt) {
	wchar_t ini[MAX_PATH];
	if (!Host_IniPath(ini, MAX_PATH)) {
		return false;
	}
	wchar_t x[32];
	wchar_t y[32];
	GetPrivateProfileStringW(section, L"X", L"", x, 32, ini);
	GetPrivateProfileStringW(section, L"Y", L"", y, 32, ini);
	if (!x[0] || !y[0]) {
		return false;
	}
	pt.x = _wtoi(x);
	pt.y = _wtoi(y);
	return true;
}

void Place_Save(const wchar_t* section, HWND hwnd) {
	wchar_t ini[MAX_PATH];
	if (!Host_IniPath(ini, MAX_PATH)) {
		return;
	}
	RECT rc;
	GetWindowRect(hwnd, &rc);
	wchar_t buf[32];
	swprintf_s(buf, L"%d", rc.left);
	WritePrivateProfileStringW(section, L"X", buf, ini);
	swprintf_s(buf, L"%d", rc.top);
	WritePrivateProfileStringW(section, L"Y", buf, ini);
}

void Place_OnWindowPosChanging(HWND hwnd, WINDOWPOS* wp) {
	if ((wp->flags & SWP_NOMOVE) && (wp->flags & SWP_NOSIZE)) {
		return;
	}
	RECT cur;
	GetWindowRect(hwnd, &cur);
	const int x = (wp->flags & SWP_NOMOVE) ? cur.left : wp->x;
	const int y = (wp->flags & SWP_NOMOVE) ? cur.top : wp->y;
	const int w = (wp->flags & SWP_NOSIZE) ? cur.right - cur.left : wp->cx;
	const int h = (wp->flags & SWP_NOSIZE) ? cur.bottom - cur.top : wp->cy;
	RECT r = { x, y, x + w, y + h };
	Place_FitToWorkArea(r, false);
	if (r.left != x || r.top != y) {
		wp->x = r.left;
		wp->y = r.top;
		wp->flags &= ~SWP_NOMOVE;
	}
}

void Place_OnMoving(RECT* proposed) {
	Place_FitToCursorMonitor(*proposed, true);
}

void Place_Reclamp(HWND hwnd) {
	RECT rc;
	GetWindowRect(hwnd, &rc);
	SetWindowPos(hwnd, nullptr, rc.left, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
