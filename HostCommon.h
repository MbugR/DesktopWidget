#pragma once

// Helpers shared by the widget window and its settings window: the app's
// data folder / settings file, and the on-screen placement math that keeps
// the widget fully inside a monitor's work area.

#include <windows.h>

// %APPDATA%\DesktopWidget (created on first use), with a trailing backslash.
bool Host_DataFolder(wchar_t* out, size_t cap);

// %APPDATA%\DesktopWidget\settings.ini.
bool Host_IniPath(wchar_t* out, size_t cap);

// Folder that holds files the app downloads or caches (the PawnIO modules):
// the data folder. Kept as its own name because PawnIo.cpp asks for it.
bool Host_CacheFolder(wchar_t* out, size_t cap);

// ---- window placement for the widget -------------------------------------
// X/Y persist in the ini under the widget's own section and the window can
// never end up off screen, even partly.

// Moves `r` fully inside the work area of `mon`. `snap` also pulls edges
// lying within a few pixels onto the work-area edges.
void Place_FitToMonitor(RECT& r, HMONITOR mon, bool snap);
// ... of the monitor `r` mostly covers.
void Place_FitToWorkArea(RECT& r, bool snap);
// ... of the monitor under the mouse. For drags: the window follows the
// cursor onto the next monitor instead of sticking at the shared edge
// until most of it has crossed.
void Place_FitToCursorMonitor(RECT& r, bool snap);

// X/Y saved under [section]; false when never saved.
bool Place_Load(const wchar_t* section, POINT& pt);
void Place_Save(const wchar_t* section, HWND hwnd);

// WM_WINDOWPOSCHANGING: clamps every move or resize, whatever caused it.
void Place_OnWindowPosChanging(HWND hwnd, WINDOWPOS* wp);

// WM_MOVING inside a system move loop (HTCAPTION drags): clamp + snap.
void Place_OnMoving(RECT* proposed);

// WM_DISPLAYCHANGE / WM_SETTINGCHANGE(SPI_SETWORKAREA): re-clamp in place.
void Place_Reclamp(HWND hwnd);
