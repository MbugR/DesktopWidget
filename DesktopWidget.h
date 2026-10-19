#pragma once

// Shared between DesktopWidget.cpp (the tile window) and WidgetSettings.cpp
// (its settings window). Both run on the widget host's thread.

#include <windows.h>

namespace widget {

enum Metric {
	kCpuLoad,
	kCpuClock,
	kCpuTemp,
	kGpuLoad,
	kGpuTemp,
	kGpuClock,
	kGpuFan,
	kRamLoad,
	kDiskActive,
	kNetDown,
	kNetUp,
	// Appended rather than grouped: the ini stores the choice as a bitmask.
	kRamUsed,
	kVramUsed,
	kVramLoad,
	kCpuFan,
	kSysFan1,
	kSysFan2,
	kMetricCount
};

// Groups for per-category value colors and the "group by category" layout.
enum Category { kCatCpu, kCatGpu, kCatMemory, kCatDisk, kCatNetwork, kCatBoard, kCategoryCount };
extern const wchar_t* const kCategoryNames[kCategoryCount];

struct MetricInfo {
	const wchar_t* label; // under the value
	const wchar_t* menu;
	int category;
};

extern const MetricInfo kMetrics[kMetricCount];
extern const int kOrder[kMetricCount]; // tile and menu order; categories are contiguous

enum Layout { kLayoutRow, kLayoutColumn, kLayoutTwoRows, kLayoutCount };
extern const wchar_t* const kLayoutNames[kLayoutCount];

// Tiles grouped by category, in place of the orientation: one column per
// category with its metrics stacked, or one row per category with its
// metrics side by side.
enum Grouping { kGroupNone, kGroupColumns, kGroupRows, kGroupingCount };
extern const wchar_t* const kGroupingNames[kGroupingCount];

enum Theme { kThemeDark, kThemeLight };

// Where the widget sits on its monitor's work area: row * 3 + column, or
// free (where it was dragged; dragging always frees it).
enum Anchor {
	kAnchorFree = -1,
	kAnchorTopLeft,
	kAnchorTop,
	kAnchorTopRight,
	kAnchorLeft,
	kAnchorCenter,
	kAnchorRight,
	kAnchorBottomLeft,
	kAnchorBottom,
	kAnchorBottomRight,
	kAnchorCount
};
extern const wchar_t* const kAnchorNames[kAnchorCount];
constexpr int kAnchorMarginMax = 100;

struct Palette {
	COLORREF bg;
	COLORREF border;
	COLORREF tile;
	COLORREF tileBorder;
	COLORREF value;
	COLORREF label;
	COLORREF noValue;
};

Palette ThemePalette(int theme);
// Syntax-highlight-like default value color of a category for a theme.
COLORREF DefaultCategoryColor(int theme, int category);

// The widget's icon (tray, settings window): a small bar chart in the CPU,
// GPU and memory colors. The caller destroys it.
HICON MakeWidgetIcon(int size);

// Digit color of a category's tray icon: the widget's category color, read
// from the ini so it works without the widget, with untouched defaults
// following the taskbar's light / dark theme. CLR_INVALID when category
// colors are off. Any thread.
COLORREF TrayCategoryColor(int category);

constexpr int kScaleMin = 50;
constexpr int kScaleMax = 150;
constexpr int kOpacityMin = 20;

struct Settings {
	DWORD metrics; // bit per Metric
	int layout;
	int grouping; // Grouping; when on, `layout` does not apply
	bool categoryColors; // value color per category instead of the single reading color
	COLORREF categoryColor[kCategoryCount];
	int scale; // %, kScaleMin..kScaleMax
	int opacity; // %, kOpacityMin..100
	int theme;
	bool customColors; // use the three colors below instead of the theme's
	COLORREF valueColor;
	COLORREF labelColor;
	COLORREF tileColor;
	wchar_t font[LF_FACESIZE]; // value font
	bool fontBold;
	bool tempColors; // orange / red for hot temperatures
	bool topMost;
	bool snap;
	bool locked;
	bool superIoFans; // read motherboard fans from the Super I/O chip
	int anchor; // Anchor
	int anchorMargin; // px between an anchored widget and the screen edges
	int monitor; // kMonitorFollow, or a fixed monitor index (see MonitorList)
	bool trayIcon; // tray icon that opens the settings
};

// monitor == kMonitorFollow: the widget anchors to whatever monitor it sits on
// (the old behavior). A fixed index (0-based, in MonitorList order) pins the
// widget to that monitor and drags it back if anything moves it off.
constexpr int kMonitorFollow = -1;
constexpr int kMaxMonitors = 16;

// Monitors in a stable left-to-right, top-to-bottom order so a saved index
// keeps meaning the same screen across restarts. `primary` marks the one at the
// origin; `name` is a short "1", "2", ... label for the UI.
struct MonitorList {
	int count;
	HMONITOR handle[kMaxMonitors];
	RECT bounds[kMaxMonitors]; // full monitor rect (rcMonitor)
	bool primary[kMaxMonitors];
};
MonitorList EnumMonitorsOrdered();

Settings& CurrentSettings();

// Saves the settings and re-applies them to the widget window.
// `metricsChanged` also refreshes the readings of newly enabled tiles.
void ApplySettings(bool metricsChanged);

// The defaults, e.g. for resetting one group of options.
Settings DefaultWidgetSettings();

// Everything back to the defaults (the widget keeps its position).
void ResetSettings();

// Motherboard fans: closes the sensor, which puts back the chip registers
// it changed, and reconnects on the next reading.
void ResetFanSensor();
// "IT8688E: ok", "starting...", "off", or why it is unavailable.
void FanSensorStatus(wchar_t* out, size_t cap);
// True when the CPU temperature or fan sensor failed because PawnIO is not
// installed (the menus and settings then offer the pawnio.eu link).
bool PawnIoMissing();
// Opens https://pawnio.eu in the default browser.
void OpenPawnIoSite();

// DesktopWidget.cpp: the widget run loop. main.cpp owns the single-instance
// mutex and calls this once; it returns when the user chooses Exit.
void RunWidget();

// WidgetSettings.cpp
void ShowSettingsWindow(HWND widgetWnd);
HWND SettingsWindow(); // nullptr when closed
void CloseSettingsWindow();
// Brings an open settings window in line with a change made elsewhere
// (e.g. dragging the widget frees its anchor).
void SyncSettingsWindow();

} // namespace widget
