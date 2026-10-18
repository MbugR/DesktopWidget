#pragma once

#include <windows.h>

// Callbacks invoked from the tray thread's message loop.
struct TrayCallbacks {
	// Left-click / "Show" menu item. May be null.
	void (*onShow)();
	// "Close" menu item. Must at least signal the host loop to exit.
	void (*onClose)();
	// Optional extra menu items appended above Show/Close. May be null.
	// `startId` is a per-slot base id — assign items startId, startId+1, …
	// Return non-zero if the message was handled.
	int (*onMenu)(HMENU menu, int startId);
	// Optional: process WM_COMMAND ids that onMenu registered. `id` is the
	// absolute WM_COMMAND value; `localId` is `id - startId` (i.e. the
	// zero-based offset the callee used inside onMenu). Compare against
	// localId when your logic doesn't care about the slot layout.
	void (*onMenuCommand)(UINT id, int localId);
	// Menu text of the onShow item; null: "Show".
	const wchar_t* showLabel;
	// Menu text of the onClose item; null: "Close".
	const wchar_t* closeLabel;
};

// Opaque handle to one tray slot (an icon+wndproc pair sharing the module's
// tray-host window). A slot survives until Tray_Stop(slot) or process exit.
using TraySlot = int;
constexpr TraySlot kTrayInvalidSlot = -1;

// Start a tray icon. Multiple calls in the same process are supported —
// every slot shares one hidden HWND (so several modules can host their
// icons in one process). `uniqueTag` is the slot's Shell_NotifyIcon uID
// on that HWND. The tray positions each icon independently regardless.
//
// The `separate` argument is ignored; kept for old call sites.
//
// Returns the slot handle, or kTrayInvalidSlot on failure.
TraySlot Tray_Start(const wchar_t* tipText, const wchar_t* windowText, UINT uniqueTag, bool separate, const TrayCallbacks& cb);

// Update the tooltip text for the given slot.
void Tray_SetTip(TraySlot slot, const wchar_t* tipText);

// Replace this slot's icon with a freshly-rendered "NN" numeric icon
// (Win10-Tweaker style: big centered digits over a load-colored progress
// bar). `value` is 0..100; anything else is clamped, and -1 puts back the
// host's own icon. The digits take `color` (e.g. a category color), or the
// load color like the bar when it is CLR_INVALID.
void Tray_SetNumericIcon(TraySlot slot, int value, COLORREF color = CLR_INVALID);

// Two stacked short values (up to 7 chars each, e.g. "850K") in their own
// colors — e.g. download over upload.
void Tray_SetDualTextIcon(TraySlot slot, const wchar_t* top, COLORREF topColor, const wchar_t* bottom, COLORREF bottomColor);

// One rate (up to 7 chars, e.g. "850K") as large as it fits, over a small
// arrow pointing down (download) or up (upload).
void Tray_SetRateIcon(TraySlot slot, const wchar_t* text, COLORREF color, bool down);

// A small bar chart: `count` (up to 4) bars of `heightsPct` % of the icon
// height in their own colors, e.g. the desktop widget's icon.
void Tray_SetBarsIcon(TraySlot slot, const COLORREF* colors, const int* heightsPct, int count);
// The same picture as a standalone `size` px icon (e.g. for WM_SETICON);
// the caller destroys it.
HICON Tray_MakeBarsIcon(const COLORREF* colors, const int* heightsPct, int count, int size);

// Legacy no-op — kept only so old call sites compile. See TrayIcon.cpp.
void Tray_SetIconLabel(TraySlot slot, const wchar_t* label);

// Replace this slot's icon with a text-only icon (1-3 chars filling the
// icon with `color`).
void Tray_SetTextIcon(TraySlot slot, const wchar_t* text, COLORREF color);

// Replace this slot's icon with one glyph from the system icon font
// (Segoe Fluent Icons on Win11, Segoe MDL2 Assets on Win10), e.g. 0xE765
// keyboard, 0xE722 camera, 0xE77F paste. Falls back to `fallbackText`
// rendered like Tray_SetTextIcon when neither font is installed.
void Tray_SetGlyphIcon(TraySlot slot, wchar_t glyph, const wchar_t* fallbackText, COLORREF color);

// True when the taskbar uses the light theme (SystemUsesLightTheme).
bool Tray_TaskbarLight();

// A second, clearly different shade of `c` that still reads on the taskbar
// (lighter on a dark taskbar, darker on a light one), e.g. upload under
// download.
COLORREF Tray_SecondShade(COLORREF c);

// Compatibility shim — always true. See the note in Tray_Start about why
// tray positions cannot be shared across processes.
bool Tray_ShouldRunSeparate();

// Remove the icon for this slot.
void Tray_Stop(TraySlot slot);

// Tear down the shared tray-host window and thread if no slots are live.
// Every module calls it after Tray_Stop; only the last one out
// actually stops the host.
void Tray_Shutdown();
