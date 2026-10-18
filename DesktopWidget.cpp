// DesktopWidget — NZXT-CAM-style desktop widget: tiles with big values (CPU
// and GPU load/clock/temperature/fan, motherboard fans, RAM and VRAM, disk,
// network). Values are colored per category (CPU, GPU, memory, ...) like
// syntax highlighting, and tiles can be grouped: one column per category
// with its metrics stacked, or one row per category side by side. Drag
// anywhere to move, also across monitors; the window never leaves the work
// area of the monitor under the cursor, even partly, and optionally snaps
// to its edges, or sits pinned to a spot of its monitor (a corner, an edge
// center, the center) and stays there as it grows or shrinks. Double-click,
// "Settings..." or the tray icon opens the settings window
// (WidgetSettings.cpp); the right-click menu has the quick options.
// Settings live in %APPDATA%\DesktopWidget\settings.ini under [Widget].

#include "DesktopWidget.h"
#include "Sensors.h"
#include "HostCommon.h"
#include "TrayIcon.h"
#include "Autostart.h"
#include "Updater.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" IMAGE_DOS_HEADER __ImageBase;

using namespace widget;

namespace widget {

const wchar_t* const kCategoryNames[kCategoryCount] = { L"CPU", L"GPU", L"Memory", L"Disk", L"Network", L"Fans" };

const MetricInfo kMetrics[kMetricCount] = {
	{ L"CPU Load", L"CPU load", kCatCpu },
	{ L"CPU Clock", L"CPU clock", kCatCpu },
	{ L"CPU Temp", L"CPU temperature", kCatCpu },
	{ L"GPU Load", L"GPU load", kCatGpu },
	{ L"GPU Temp", L"GPU temperature", kCatGpu },
	{ L"GPU Clock", L"GPU clock", kCatGpu },
	{ L"GPU Fan", L"GPU fan", kCatGpu },
	{ L"RAM Load", L"RAM load", kCatMemory },
	{ L"Disk Active", L"Disk activity", kCatDisk },
	{ L"Download", L"Network download", kCatNetwork },
	{ L"Upload", L"Network upload", kCatNetwork },
	{ L"RAM Used", L"RAM used (GB)", kCatMemory },
	{ L"VRAM Used", L"VRAM used (GB)", kCatGpu },
	{ L"VRAM Load", L"VRAM load", kCatGpu },
	{ L"CPU Fan", L"CPU fan", kCatCpu },
	{ L"SYS Fan 1", L"System fan 1", kCatBoard },
	{ L"SYS Fan 2", L"System fan 2", kCatBoard },
};

const int kOrder[kMetricCount] = {
	kCpuTemp, kCpuLoad, kCpuClock, kCpuFan,
	kGpuTemp, kGpuLoad, kGpuClock, kGpuFan, kVramUsed, kVramLoad,
	kRamUsed, kRamLoad, kDiskActive, kNetDown, kNetUp,
	kSysFan1, kSysFan2,
};

const wchar_t* const kLayoutNames[kLayoutCount] = { L"Horizontal", L"Vertical", L"Two rows" };

const wchar_t* const kGroupingNames[kGroupingCount] = { L"No grouping", L"Category columns", L"Category rows" };

const wchar_t* const kAnchorNames[kAnchorCount] = {
	L"Top left", L"Top center", L"Top right",
	L"Middle left", L"Center", L"Middle right",
	L"Bottom left", L"Bottom center", L"Bottom right",
};

Palette ThemePalette(int theme) {
	if (theme == kThemeLight) {
		return { RGB(232, 232, 236), RGB(196, 196, 202), RGB(250, 250, 252), RGB(208, 208, 214),
			RGB(24, 150, 44), RGB(60, 60, 66), RGB(165, 165, 170) };
	}
	return { RGB(20, 20, 20), RGB(52, 52, 52), RGB(36, 36, 36), RGB(60, 60, 60),
		RGB(92, 224, 92), RGB(205, 205, 205), RGB(120, 120, 120) };
}

COLORREF DefaultCategoryColor(int theme, int category) {
	// Blue CPU, green GPU, purple memory, yellow disk, teal network, pink fans;
	// darker shades on the light theme.
	constexpr COLORREF kDark[kCategoryCount] = {
		RGB(79, 193, 255), RGB(108, 216, 108), RGB(197, 134, 192), RGB(220, 220, 150), RGB(78, 201, 176), RGB(244, 143, 177),
	};
	constexpr COLORREF kLight[kCategoryCount] = {
		RGB(0, 112, 193), RGB(30, 142, 62), RGB(156, 63, 160), RGB(154, 122, 0), RGB(14, 138, 122), RGB(194, 24, 91),
	};
	if (category < 0 || category >= kCategoryCount) {
		return ThemePalette(theme).value;
	}
	return theme == kThemeLight ? kLight[category] : kDark[category];
}

} // namespace widget

namespace {

constexpr wchar_t kWidgetMutex[] = L"DesktopWidget";
constexpr wchar_t kWidgetQuitEvent[] = L"DesktopWidgetQuit";
// Signals the running widget to open its settings window.
constexpr wchar_t kWidgetSettingsEvent[] = L"DesktopWidgetSettings";
constexpr wchar_t kWidgetClass[] = L"DesktopWidgetWnd";
constexpr wchar_t kSection[] = L"Widget";
constexpr UINT_PTR kTimerId = 1;

constexpr DWORD Bit(int m) {
	return 1u << m;
}

constexpr DWORD kDefaultMetrics = Bit(kCpuTemp) | Bit(kCpuLoad) | Bit(kCpuClock) | Bit(kGpuTemp) | Bit(kGpuClock)
	| Bit(kRamUsed) | Bit(kNetDown) | Bit(kNetUp);

const int kScalePresets[] = { 75, 100, 125, 150 };
constexpr int kScalePresetCount = 4;
const int kOpacities[] = { 100, 90, 80, 70, 60, 50 };
constexpr int kOpacityCount = 6;

// Context menu ids.
constexpr UINT kCmdSettings = 90;
constexpr UINT kCmdMetricBase = 100;
constexpr UINT kCmdLayoutBase = 200;
constexpr UINT kCmdScaleBase = 210;
constexpr UINT kCmdOpacityBase = 220;
constexpr UINT kCmdAnchorBase = 230; // + Anchor
constexpr UINT kCmdAnchorFree = 240;
constexpr UINT kCmdGroupingBase = 250; // + Grouping
constexpr UINT kCmdTopMost = 300;
constexpr UINT kCmdLock = 301;
constexpr UINT kCmdClose = 302;
constexpr UINT kCmdSnap = 303;
constexpr UINT kCmdCategoryColors = 305;
constexpr UINT kCmdUpdate = 306;
constexpr UINT kCmdMonitorFollow = 310;
constexpr UINT kCmdMonitorBase = 311; // + monitor index

struct Tile {
	wchar_t num[16];
	const wchar_t* unit;
	bool superUnit; // "°" sits at the top, other units on the baseline
	bool noValue;
	int tempC; // >= 0 for temperature tiles (drives the warning colors)
};

struct Fonts {
	HFONT value;
	HFONT unit;
	HFONT degree; // "°" is drawn larger than text units
	HFONT label;
	int valueH;
	int unitH;
	int degreeH;
	wchar_t face[LF_FACESIZE];
	int weight;
};

wchar_t g_ini[MAX_PATH];
Settings g_set;
Tile g_tiles[kMetricCount];
Fonts g_fonts;
HANDLE g_quit;
HWND g_wnd;
bool g_dragging;
bool g_dragMoved;
POINT g_dragOffset;

CpuLoadSensor g_cpu;
GpuSensor g_gpu;
bool g_gpuTried;
DiskSensor g_disk;
bool g_diskOpen;
NetSensor g_net;
bool g_netTried;

// The PawnIO sensors (CPU temperature, motherboard fans) open on a worker
// thread: the first open may add the driver's device node and download a
// module, which must not freeze the widget. A failed open is retried later.
enum OpenState : LONG { kOpenIdle, kOpenBusy, kOpenReady, kOpenFailed };

struct AsyncSensor {
	volatile LONG state;
	HANDLE thread;
	ULONGLONG retryAt;
	bool (*open)();
};

constexpr ULONGLONG kOpenRetryMs = 60000;

CpuTempSensor g_cpuTemp;
FanSensor g_fans;

bool OpenCpuTemp() {
	return CpuTemp_Open(g_cpuTemp);
}

bool OpenFans() {
	return Fans_Open(g_fans);
}

AsyncSensor g_cpuTempOpen = { kOpenIdle, nullptr, 0, OpenCpuTemp };
AsyncSensor g_fansOpen = { kOpenIdle, nullptr, 0, OpenFans };

constexpr wchar_t kNoValue[] = L"\x2014";

// ---- settings --------------------------------------------------------------

// GetPrivateProfileInt clamps negatives to 0, so parse strings ourselves.
bool IniReadInt(const wchar_t* key, int& out) {
	wchar_t buf[32];
	GetPrivateProfileStringW(kSection, key, L"", buf, 32, g_ini);
	if (!buf[0]) {
		return false;
	}
	out = _wtoi(buf);
	return true;
}

int IniInt(const wchar_t* key, int def) {
	int v = def;
	IniReadInt(key, v);
	return v;
}

void IniWriteInt(const wchar_t* key, int v) {
	if (!g_ini[0]) {
		return;
	}
	wchar_t buf[32];
	swprintf_s(buf, L"%d", v);
	WritePrivateProfileStringW(kSection, key, buf, g_ini);
}

// Colors are stored as "#RRGGBB".
COLORREF IniColorAt(const wchar_t* ini, const wchar_t* key, COLORREF def) {
	wchar_t buf[32];
	GetPrivateProfileStringW(kSection, key, L"", buf, 32, ini);
	if (buf[0] != L'#' || lstrlenW(buf) != 7) {
		return def;
	}
	const unsigned long v = wcstoul(buf + 1, nullptr, 16);
	return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

COLORREF IniColor(const wchar_t* key, COLORREF def) {
	return IniColorAt(g_ini, key, def);
}

void IniWriteColor(const wchar_t* key, COLORREF c) {
	if (!g_ini[0]) {
		return;
	}
	wchar_t buf[16];
	swprintf_s(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
	WritePrivateProfileStringW(kSection, key, buf, g_ini);
}

int Clamp(int v, int lo, int hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

Settings DefaultSettings() {
	Settings s = {};
	s.metrics = kDefaultMetrics;
	s.layout = kLayoutRow;
	s.scale = 100;
	s.opacity = 100;
	s.theme = kThemeDark;
	const Palette def = ThemePalette(s.theme);
	s.valueColor = def.value;
	s.labelColor = def.label;
	s.tileColor = def.tile;
	lstrcpynW(s.font, L"Bahnschrift", LF_FACESIZE);
	s.tempColors = true;
	s.topMost = true;
	s.snap = true;
	s.superIoFans = true;
	s.categoryColors = true;
	for (int c = 0; c < kCategoryCount; ++c) {
		s.categoryColor[c] = DefaultCategoryColor(s.theme, c);
	}
	s.anchor = kAnchorFree;
	s.anchorMargin = 16;
	s.monitor = kMonitorFollow;
	s.trayIcon = true;
	return s;
}

// Ini key of a category color: "CatColorCPU", "CatColorGPU", ...
void CategoryColorKey(int c, wchar_t* key, size_t cap) {
	swprintf_s(key, cap, L"CatColor%s", kCategoryNames[c]);
}

void LoadSettings() {
	Host_IniPath(g_ini, MAX_PATH);
	const Settings d = DefaultSettings();
	Settings s = d;
	s.metrics = static_cast<DWORD>(IniInt(L"Metrics", static_cast<int>(d.metrics))) & (Bit(kMetricCount) - 1);
	if (!s.metrics) {
		s.metrics = d.metrics;
	}
	s.layout = Clamp(IniInt(L"Layout", d.layout), 0, kLayoutCount - 1);
	int scale = d.scale;
	if (IniReadInt(L"Scale", scale)) {
		s.scale = Clamp(scale, kScaleMin, kScaleMax);
	} else {
		// Older builds stored a Normal/Large/Huge index.
		static const int kOldSizes[] = { 100, 125, 150 };
		s.scale = kOldSizes[Clamp(IniInt(L"Size", 0), 0, 2)];
	}
	s.opacity = Clamp(IniInt(L"Opacity", d.opacity), kOpacityMin, 100);
	s.theme = Clamp(IniInt(L"Theme", d.theme), kThemeDark, kThemeLight);
	s.customColors = IniInt(L"CustomColors", d.customColors ? 1 : 0) != 0;
	const Palette def = ThemePalette(s.theme);
	s.valueColor = IniColor(L"ValueColor", def.value);
	s.labelColor = IniColor(L"LabelColor", def.label);
	s.tileColor = IniColor(L"TileColor", def.tile);
	GetPrivateProfileStringW(kSection, L"Font", d.font, s.font, LF_FACESIZE, g_ini);
	s.fontBold = IniInt(L"FontBold", d.fontBold ? 1 : 0) != 0;
	s.tempColors = IniInt(L"TempColors", d.tempColors ? 1 : 0) != 0;
	s.topMost = IniInt(L"TopMost", d.topMost ? 1 : 0) != 0;
	s.snap = IniInt(L"Snap", d.snap ? 1 : 0) != 0;
	s.locked = IniInt(L"Locked", d.locked ? 1 : 0) != 0;
	s.superIoFans = IniInt(L"SuperIoFans", d.superIoFans ? 1 : 0) != 0;
	int grouping = d.grouping;
	if (!IniReadInt(L"Grouping", grouping) && IniInt(L"GroupByCategory", 0) != 0) {
		// Older builds: one switch, rows when the orientation was vertical.
		grouping = s.layout == kLayoutColumn ? kGroupRows : kGroupColumns;
	}
	s.grouping = Clamp(grouping, kGroupNone, kGroupingCount - 1);
	s.categoryColors = IniInt(L"CategoryColors", d.categoryColors ? 1 : 0) != 0;
	for (int c = 0; c < kCategoryCount; ++c) {
		wchar_t key[32];
		CategoryColorKey(c, key, 32);
		s.categoryColor[c] = IniColor(key, DefaultCategoryColor(s.theme, c));
	}
	s.anchor = Clamp(IniInt(L"Anchor", d.anchor), kAnchorFree, kAnchorCount - 1);
	s.anchorMargin = Clamp(IniInt(L"AnchorMargin", d.anchorMargin), 0, kAnchorMarginMax);
	s.monitor = Clamp(IniInt(L"Monitor", d.monitor), kMonitorFollow, kMaxMonitors - 1);
	s.trayIcon = IniInt(L"TrayIcon", d.trayIcon ? 1 : 0) != 0;
	g_set = s;
}

void SaveSettings() {
	IniWriteInt(L"Metrics", static_cast<int>(g_set.metrics));
	IniWriteInt(L"Layout", g_set.layout);
	IniWriteInt(L"Scale", g_set.scale);
	IniWriteInt(L"Opacity", g_set.opacity);
	IniWriteInt(L"Theme", g_set.theme);
	IniWriteInt(L"CustomColors", g_set.customColors ? 1 : 0);
	IniWriteColor(L"ValueColor", g_set.valueColor);
	IniWriteColor(L"LabelColor", g_set.labelColor);
	IniWriteColor(L"TileColor", g_set.tileColor);
	if (g_ini[0]) {
		WritePrivateProfileStringW(kSection, L"Font", g_set.font, g_ini);
	}
	IniWriteInt(L"FontBold", g_set.fontBold ? 1 : 0);
	IniWriteInt(L"TempColors", g_set.tempColors ? 1 : 0);
	IniWriteInt(L"TopMost", g_set.topMost ? 1 : 0);
	IniWriteInt(L"Snap", g_set.snap ? 1 : 0);
	IniWriteInt(L"Locked", g_set.locked ? 1 : 0);
	IniWriteInt(L"SuperIoFans", g_set.superIoFans ? 1 : 0);
	IniWriteInt(L"Grouping", g_set.grouping);
	if (g_ini[0]) {
		WritePrivateProfileStringW(kSection, L"GroupByCategory", nullptr, g_ini); // superseded by Grouping
	}
	IniWriteInt(L"CategoryColors", g_set.categoryColors ? 1 : 0);
	for (int c = 0; c < kCategoryCount; ++c) {
		wchar_t key[32];
		CategoryColorKey(c, key, 32);
		IniWriteColor(key, g_set.categoryColor[c]);
	}
	IniWriteInt(L"Anchor", g_set.anchor);
	IniWriteInt(L"AnchorMargin", g_set.anchorMargin);
	IniWriteInt(L"Monitor", g_set.monitor);
	IniWriteInt(L"TrayIcon", g_set.trayIcon ? 1 : 0);
}

// Tray icon colors (TrayCategoryColor): every tray module asks once a
// second, so the ini is re-read at most every two seconds.
constexpr ULONGLONG kTrayColorTtlMs = 2000;
SRWLOCK g_trayColorLock = SRWLOCK_INIT;
ULONGLONG g_trayColorRead;
bool g_trayColorOn;
COLORREF g_trayColor[kCategoryCount];

void ReadTrayColors() {
	wchar_t ini[MAX_PATH];
	Host_IniPath(ini, MAX_PATH);
	const Settings d = DefaultSettings();
	g_trayColorOn = GetPrivateProfileIntW(kSection, L"CategoryColors", d.categoryColors ? 1 : 0, ini) != 0;
	const int theme = Clamp(static_cast<int>(GetPrivateProfileIntW(kSection, L"Theme", d.theme, ini)), kThemeDark, kThemeLight);
	const int taskbarTheme = Tray_TaskbarLight() ? kThemeLight : kThemeDark;
	for (int c = 0; c < kCategoryCount; ++c) {
		wchar_t key[32];
		CategoryColorKey(c, key, 32);
		const COLORREF def = DefaultCategoryColor(theme, c);
		const COLORREF set = IniColorAt(ini, key, def);
		// A hand-picked color is kept; a theme default follows the taskbar.
		g_trayColor[c] = set == def ? DefaultCategoryColor(taskbarTheme, c) : set;
	}
}

// Theme palette with the custom colors applied; tile border follows the
// tile color so a custom tile still gets a visible edge.
Palette CurrentPalette() {
	Palette p = ThemePalette(g_set.theme);
	if (g_set.customColors) {
		p.value = g_set.valueColor;
		p.label = g_set.labelColor;
		p.tile = g_set.tileColor;
		const int lum = (GetRValue(p.tile) * 3 + GetGValue(p.tile) * 6 + GetBValue(p.tile)) / 10;
		const int d = lum < 128 ? 24 : -24;
		p.tileBorder = RGB(Clamp(GetRValue(p.tile) + d, 0, 255), Clamp(GetGValue(p.tile) + d, 0, 255),
			Clamp(GetBValue(p.tile) + d, 0, 255));
	}
	return p;
}

// ---- geometry ----------------------------------------------------------------

struct Geometry {
	int tileW;
	int tileH;
	int gap;
	int pad;
	int cols;
	int rows;
	int count;
	int width;
	int height;
};

bool Wants(int m) {
	return (g_set.metrics & Bit(m)) != 0;
}

struct Cell {
	int metric;
	int col;
	int row;
};

// Grid position of every shown tile, in kOrder order. Returns the count and
// the grid size. Grouped by category, each category starts a new column
// (horizontal, two rows) or a new row (vertical); kOrder keeps categories
// contiguous.
int LayoutCells(Cell (&cells)[kMetricCount], int& cols, int& rows) {
	int n = 0;
	for (int m : kOrder) {
		if (Wants(m)) {
			cells[n++] = { m, 0, 0 };
		}
	}
	cols = 1;
	rows = 1;
	if (n == 0) {
		return 0;
	}
	if (g_set.grouping != kGroupNone) {
		const bool vertical = g_set.grouping == kGroupRows; // categories follow each other downwards
		int line = -1;
		int pos = 0;
		int prevCat = -1;
		int longest = 0;
		for (int i = 0; i < n; ++i) {
			const int cat = kMetrics[cells[i].metric].category;
			if (cat != prevCat) {
				++line;
				pos = 0;
				prevCat = cat;
			}
			cells[i].col = vertical ? pos : line;
			cells[i].row = vertical ? line : pos;
			++pos;
			if (pos > longest) {
				longest = pos;
			}
		}
		cols = vertical ? longest : line + 1;
		rows = vertical ? line + 1 : longest;
		return n;
	}
	switch (g_set.layout) {
	case kLayoutColumn:
		rows = n;
		break;
	case kLayoutTwoRows:
		rows = n > 1 ? 2 : 1;
		cols = (n + rows - 1) / rows;
		break;
	default:
		cols = n;
		break;
	}
	for (int i = 0; i < n; ++i) {
		cells[i].col = g_set.layout == kLayoutColumn ? 0 : i % cols;
		cells[i].row = g_set.layout == kLayoutColumn ? i : i / cols;
	}
	return n;
}

Geometry ComputeGeometry() {
	const int s = g_set.scale;
	Geometry g{};
	g.tileW = 120 * s / 100;
	g.tileH = 76 * s / 100;
	g.gap = (6 * s + 50) / 100;
	g.pad = (6 * s + 50) / 100;
	Cell cells[kMetricCount];
	g.count = LayoutCells(cells, g.cols, g.rows);
	g.width = g.pad * 2 + g.cols * g.tileW + (g.cols - 1) * g.gap;
	g.height = g.pad * 2 + g.rows * g.tileH + (g.rows - 1) * g.gap;
	return g;
}

// ---- fonts -------------------------------------------------------------------

bool FontInstalled(const wchar_t* face) {
	HDC dc = GetDC(nullptr);
	HFONT f = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
	HGDIOBJ prev = SelectObject(dc, f);
	wchar_t got[LF_FACESIZE] = {};
	GetTextFaceW(dc, LF_FACESIZE, got);
	SelectObject(dc, prev);
	DeleteObject(f);
	ReleaseDC(nullptr, dc);
	return lstrcmpiW(got, face) == 0;
}

HFONT MakeFont(int h, int weight, const wchar_t* face) {
	return CreateFontW(-h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, face);
}

void DestroyFonts() {
	if (g_fonts.value) DeleteObject(g_fonts.value);
	if (g_fonts.unit) DeleteObject(g_fonts.unit);
	if (g_fonts.degree) DeleteObject(g_fonts.degree);
	if (g_fonts.label) DeleteObject(g_fonts.label);
	g_fonts = {};
}

void CreateFonts() {
	DestroyFonts();
	// A face that is not installed (ini edited by hand, font removed) falls
	// back to Bahnschrift, then Segoe UI.
	const wchar_t* face = g_set.font;
	if (!face[0] || !FontInstalled(face)) {
		face = FontInstalled(L"Bahnschrift") ? L"Bahnschrift" : L"Segoe UI";
	}
	lstrcpynW(g_fonts.face, face, LF_FACESIZE);
	g_fonts.weight = g_set.fontBold ? FW_BOLD : FW_NORMAL;
	const int s = g_set.scale;
	g_fonts.valueH = 36 * s / 100;
	g_fonts.unitH = 16 * s / 100;
	g_fonts.degreeH = 26 * s / 100;
	g_fonts.value = MakeFont(g_fonts.valueH, g_fonts.weight, g_fonts.face);
	g_fonts.unit = MakeFont(g_fonts.unitH, g_fonts.weight, g_fonts.face);
	g_fonts.degree = MakeFont(g_fonts.degreeH, g_fonts.weight, g_fonts.face);
	const int labelH = 13 * s / 100;
	g_fonts.label = MakeFont(labelH < 9 ? 9 : labelH, FW_NORMAL, L"Segoe UI");
}

// ---- readings ----------------------------------------------------------------

void SetTile(int m, const wchar_t* num, const wchar_t* unit) {
	Tile& t = g_tiles[m];
	lstrcpynW(t.num, num, 16);
	t.unit = unit;
	t.superUnit = false;
	t.noValue = false;
	t.tempC = -1;
}

void SetNoValue(int m) {
	SetTile(m, kNoValue, L"");
	g_tiles[m].noValue = true;
}

void SetTileInt(int m, int v, const wchar_t* unit) {
	if (v < 0) {
		SetNoValue(m);
		return;
	}
	wchar_t buf[16];
	swprintf_s(buf, L"%d", v);
	SetTile(m, buf, unit);
}

void SetTileTemp(int m, int c) {
	if (c < 0) {
		SetNoValue(m);
		return;
	}
	SetTileInt(m, c, L"\x00B0");
	g_tiles[m].superUnit = true;
	g_tiles[m].tempC = c;
}

void SetTileGb(int m, double gb) {
	if (gb < 0) {
		SetNoValue(m);
		return;
	}
	wchar_t buf[16];
	swprintf_s(buf, gb < 99.95 ? L"%.1f" : L"%.0f", gb);
	SetTile(m, buf, L"GB");
}

DWORD WINAPI AsyncOpenThread(LPVOID param) {
	AsyncSensor& a = *static_cast<AsyncSensor*>(param);
	const bool ok = a.open();
	if (!ok) {
		a.retryAt = GetTickCount64() + kOpenRetryMs;
	}
	InterlockedExchange(&a.state, ok ? kOpenReady : kOpenFailed);
	return 0;
}

LONG AsyncState(const AsyncSensor& a) {
	return InterlockedCompareExchange(const_cast<volatile LONG*>(&a.state), 0, 0);
}

// True once the sensor is open; otherwise starts (or retries) the open.
bool AsyncReady(AsyncSensor& a) {
	const LONG state = AsyncState(a);
	if (state == kOpenReady) {
		return true;
	}
	if (state == kOpenBusy || (state == kOpenFailed && GetTickCount64() < a.retryAt)) {
		return false;
	}
	if (a.thread) {
		CloseHandle(a.thread);
		a.thread = nullptr;
	}
	InterlockedExchange(&a.state, kOpenBusy);
	a.thread = CreateThread(nullptr, 0, AsyncOpenThread, &a, 0, nullptr);
	if (!a.thread) {
		a.retryAt = GetTickCount64() + kOpenRetryMs;
		InterlockedExchange(&a.state, kOpenFailed);
	}
	return false;
}

// Waits for a pending open so the sensor can be closed.
void AsyncStop(AsyncSensor& a) {
	if (a.thread) {
		WaitForSingleObject(a.thread, 15000);
		CloseHandle(a.thread);
		a.thread = nullptr;
	}
	InterlockedExchange(&a.state, kOpenIdle);
	a.retryAt = 0;
}

void SampleCpuTemp() {
	if (AsyncReady(g_cpuTempOpen)) {
		double c = 0.0;
		if (CpuTemp_Read(g_cpuTemp, c)) {
			SetTileTemp(kCpuTemp, static_cast<int>(c + 0.5));
		}
	} else if (AsyncState(g_cpuTempOpen) == kOpenFailed) {
		SetNoValue(kCpuTemp);
	}
}

// Closes the fan sensor (Fans_Close puts back what it changed on the chip)
// and lets the next sample open it again.
void CloseFans() {
	AsyncStop(g_fansOpen);
	Fans_Close(g_fans);
}

void SampleFans() {
	const int metrics[] = { kCpuFan, kSysFan1, kSysFan2 };
	if (!g_set.superIoFans) {
		if (AsyncState(g_fansOpen) != kOpenIdle) {
			CloseFans();
		}
		for (int m : metrics) {
			SetNoValue(m);
		}
		return;
	}
	if (AsyncReady(g_fansOpen)) {
		FanReading r;
		if (Fans_Read(g_fans, r)) {
			const int channels[] = { g_fans.cpuChannel, g_fans.sysChannel[0], g_fans.sysChannel[1] };
			for (int i = 0; i < 3; ++i) {
				SetTileInt(metrics[i], r.rpm[channels[i]], L"RPM");
			}
		} else if (!g_fans.ready) {
			// The chip stopped answering (e.g. locked again after sleep):
			// close it and reconnect shortly.
			wchar_t status[64];
			lstrcpynW(status, g_fans.status, 64);
			CloseFans();
			lstrcpynW(g_fans.status, status, 64);
			g_fansOpen.retryAt = GetTickCount64() + 5000;
			InterlockedExchange(&g_fansOpen.state, kOpenFailed);
			for (int m : metrics) {
				SetNoValue(m);
			}
		}
	} else if (AsyncState(g_fansOpen) == kOpenFailed) {
		for (int m : metrics) {
			SetNoValue(m);
		}
	}
}

void SampleAll() {
	if (Wants(kCpuLoad)) {
		int pct = 0;
		if (CpuLoad_Sample(g_cpu, pct)) {
			SetTileInt(kCpuLoad, pct, L"%");
		}
	}
	if (Wants(kCpuClock)) {
		SetTileInt(kCpuClock, CpuClock_MeasureMhz(), L"MHz");
	}
	if (Wants(kCpuTemp)) {
		SampleCpuTemp();
	}
	if (Wants(kCpuFan) || Wants(kSysFan1) || Wants(kSysFan2)) {
		SampleFans();
	}
	if (Wants(kGpuLoad) || Wants(kGpuTemp) || Wants(kGpuClock) || Wants(kGpuFan) || Wants(kVramUsed) || Wants(kVramLoad)) {
		if (!g_gpuTried) {
			g_gpuTried = true;
			Gpu_Open(g_gpu);
		}
		GpuReading r;
		Gpu_Sample(g_gpu, r);
		if (r.loadPct >= 0 || !g_gpu.ok) {
			SetTileInt(kGpuLoad, r.loadPct, L"%");
		}
		SetTileTemp(kGpuTemp, r.tempC);
		SetTileInt(kGpuClock, r.clockMhz, L"MHz");
		SetTileInt(kGpuFan, r.fanRpm, L"RPM");
		SetTileGb(kVramUsed, r.vramUsedGb);
		SetTileInt(kVramLoad, r.vramTotalGb > 0 ? static_cast<int>(r.vramUsedGb * 100.0 / r.vramTotalGb + 0.5) : -1, L"%");
	}
	if (Wants(kRamLoad) || Wants(kRamUsed)) {
		RamReading r;
		if (Ram_Read(r)) {
			SetTileInt(kRamLoad, r.loadPct, L"%");
			SetTileGb(kRamUsed, r.usedGb);
		}
	}
	if (Wants(kDiskActive)) {
		if (!g_diskOpen) {
			g_diskOpen = true;
			Disk_Open(g_disk);
		}
		DiskReading r;
		if (Disk_Sample(g_disk, r)) {
			SetTileInt(kDiskActive, r.busiestPct, L"%");
		}
	}
	if (Wants(kNetDown) || Wants(kNetUp)) {
		if (!g_netTried) {
			g_netTried = true;
			Net_Open(g_net);
		}
		NetReading r;
		if (Net_Sample(g_net, r)) {
			wchar_t num[16];
			const wchar_t* unit = L"";
			Net_FormatRateSplit(r.downBps, num, 16, &unit);
			SetTile(kNetDown, num, unit);
			Net_FormatRateSplit(r.upBps, num, 16, &unit);
			SetTile(kNetUp, num, unit);
		}
	}
}

// ---- painting ----------------------------------------------------------------

void FillRectColor(HDC dc, const RECT& rc, COLORREF c) {
	HBRUSH b = CreateSolidBrush(c);
	FillRect(dc, &rc, b);
	DeleteObject(b);
}

COLORREF ValueColor(int m, const Palette& p) {
	const Tile& t = g_tiles[m];
	if (t.noValue || !t.num[0]) {
		return p.noValue;
	}
	// Hot temperatures win over the category color: they are a warning.
	if (g_set.tempColors && t.tempC >= 85) {
		return RGB(240, 80, 64);
	}
	if (g_set.tempColors && t.tempC >= 70) {
		return RGB(250, 160, 40);
	}
	if (g_set.categoryColors) {
		return g_set.categoryColor[kMetrics[m].category];
	}
	return p.value;
}

void DrawTile(HDC dc, const RECT& rc, int m, int corner, const Palette& p) {
	HBRUSH brush = CreateSolidBrush(p.tile);
	HPEN pen = CreatePen(PS_SOLID, 1, p.tileBorder);
	HGDIOBJ oldBrush = SelectObject(dc, brush);
	HGDIOBJ oldPen = SelectObject(dc, pen);
	RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, corner, corner);
	SelectObject(dc, oldBrush);
	SelectObject(dc, oldPen);
	DeleteObject(brush);
	DeleteObject(pen);

	const Tile& t = g_tiles[m];
	const wchar_t* num = t.num[0] ? t.num : kNoValue;
	const wchar_t* unit = t.num[0] && t.unit ? t.unit : L"";
	const int nNum = lstrlenW(num);
	const int nUnit = lstrlenW(unit);
	const int tileW = rc.right - rc.left;
	const int tileH = rc.bottom - rc.top;

	// Shrink value + unit together when a long value ("1023 KB/s") or a wide
	// font would not fit the tile.
	HFONT valueFont = g_fonts.value;
	HFONT unitFont = t.superUnit ? g_fonts.degree : g_fonts.unit;
	const int unitH = t.superUnit ? g_fonts.degreeH : g_fonts.unitH;
	HFONT tmpValue = nullptr;
	HFONT tmpUnit = nullptr;
	SIZE szNum = {};
	SIZE szUnit = {};
	SelectObject(dc, valueFont);
	GetTextExtentPoint32W(dc, num, nNum, &szNum);
	SelectObject(dc, unitFont);
	GetTextExtentPoint32W(dc, unit, nUnit, &szUnit);
	const int unitGap = nUnit ? 2 : 0;
	const int avail = tileW - 10;
	const int total = szNum.cx + unitGap + szUnit.cx;
	if (total > avail && total > 0) {
		tmpValue = MakeFont(g_fonts.valueH * avail / total, g_fonts.weight, g_fonts.face);
		tmpUnit = MakeFont(unitH * avail / total, g_fonts.weight, g_fonts.face);
		valueFont = tmpValue;
		unitFont = tmpUnit;
		SelectObject(dc, valueFont);
		GetTextExtentPoint32W(dc, num, nNum, &szNum);
		SelectObject(dc, unitFont);
		GetTextExtentPoint32W(dc, unit, nUnit, &szUnit);
	}

	TEXTMETRICW tmV = {};
	TEXTMETRICW tmU = {};
	TEXTMETRICW tmL = {};
	SelectObject(dc, valueFont);
	GetTextMetricsW(dc, &tmV);
	SelectObject(dc, unitFont);
	GetTextMetricsW(dc, &tmU);
	SelectObject(dc, g_fonts.label);
	GetTextMetricsW(dc, &tmL);

	// Digits span roughly internal-leading .. ascent of the cell; stack that
	// visible part and the label, centered in the tile.
	const int digitsH = tmV.tmAscent - tmV.tmInternalLeading;
	const int labelH = tmL.tmAscent - tmL.tmInternalLeading + tmL.tmDescent;
	const int spacing = tileH / 9;
	const int blockTop = rc.top + (tileH - (digitsH + spacing + labelH)) / 2;
	const int baseline = blockTop + digitsH;

	SetBkMode(dc, TRANSPARENT);
	SetTextAlign(dc, TA_LEFT | TA_BASELINE);
	int x = rc.left + (tileW - (szNum.cx + unitGap + szUnit.cx)) / 2;
	SetTextColor(dc, ValueColor(m, p));
	SelectObject(dc, valueFont);
	TextOutW(dc, x, baseline, num, nNum);
	x += szNum.cx + unitGap;
	if (nUnit) {
		SelectObject(dc, unitFont);
		// "°" hangs from the top of the digits; MHz, %, KB/s sit on the baseline.
		const int unitBase = t.superUnit ? baseline - digitsH + (tmU.tmAscent - tmU.tmInternalLeading) : baseline;
		TextOutW(dc, x, unitBase, unit, nUnit);
	}

	SelectObject(dc, g_fonts.label);
	SetTextColor(dc, p.label);
	SetTextAlign(dc, TA_CENTER | TA_TOP);
	TextOutW(dc, rc.left + tileW / 2, baseline + spacing - tmL.tmInternalLeading, kMetrics[m].label, lstrlenW(kMetrics[m].label));
	SetTextAlign(dc, TA_LEFT | TA_TOP);

	if (tmpValue) DeleteObject(tmpValue);
	if (tmpUnit) DeleteObject(tmpUnit);
}

void Paint(HWND hwnd) {
	PAINTSTRUCT ps;
	HDC wdc = BeginPaint(hwnd, &ps);
	RECT client;
	GetClientRect(hwnd, &client);
	HDC dc = CreateCompatibleDC(wdc);
	HBITMAP bmp = CreateCompatibleBitmap(wdc, client.right, client.bottom);
	HGDIOBJ oldBmp = SelectObject(dc, bmp);
	HGDIOBJ oldFont = SelectObject(dc, g_fonts.value);

	const Palette p = CurrentPalette();
	FillRectColor(dc, client, p.border);
	RECT inner = { 1, 1, client.right - 1, client.bottom - 1 };
	FillRectColor(dc, inner, p.bg);

	const Geometry g = ComputeGeometry();
	const int corner = 10 * g_set.scale / 100;
	Cell cells[kMetricCount];
	int cols = 0;
	int rows = 0;
	const int n = LayoutCells(cells, cols, rows);
	for (int i = 0; i < n; ++i) {
		const int left = g.pad + cells[i].col * (g.tileW + g.gap);
		const int top = g.pad + cells[i].row * (g.tileH + g.gap);
		RECT rc = { left, top, left + g.tileW, top + g.tileH };
		DrawTile(dc, rc, cells[i].metric, corner, p);
	}

	BitBlt(wdc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
	SelectObject(dc, oldFont);
	SelectObject(dc, oldBmp);
	DeleteObject(bmp);
	DeleteDC(dc);
	EndPaint(hwnd, &ps);
}

// ---- window --------------------------------------------------------------------

// Resize to the current geometry and re-apply z-order and opacity; the
// WM_WINDOWPOSCHANGING clamp keeps the result on screen.
void ApplyWindowSettings(HWND hwnd) {
	const Geometry g = ComputeGeometry();
	SetWindowPos(hwnd, g_set.topMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, g.width, g.height,
		SWP_NOMOVE | SWP_NOACTIVATE);
	SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(g_set.opacity * 255 / 100), LWA_ALPHA);
	InvalidateRect(hwnd, nullptr, FALSE);
}

// Moves `r` to the anchor on the work area of the monitor it mostly covers;
// a free widget stays put.
// Target monitor for anchoring: the fixed one the user picked when it still
// exists, else the monitor the widget currently sits on. Falls back to nearest
// so a pinned index that no longer exists (a monitor was unplugged) still lands
// somewhere sane.
HMONITOR AnchorMonitor(const RECT& r) {
	if (g_set.monitor != kMonitorFollow) {
		const MonitorList ml = EnumMonitorsOrdered();
		if (g_set.monitor >= 0 && g_set.monitor < ml.count) {
			return ml.handle[g_set.monitor];
		}
	}
	return MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
}

void AnchorRect(RECT& r) {
	if (g_set.anchor < 0 || g_set.anchor >= kAnchorCount) {
		return;
	}
	MONITORINFO mi = {};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(AnchorMonitor(r), &mi)) {
		return;
	}
	const RECT& wa = mi.rcWork;
	const int w = r.right - r.left;
	const int h = r.bottom - r.top;
	const int m = g_set.anchorMargin;
	const int col = g_set.anchor % 3;
	const int row = g_set.anchor / 3;
	const int x = col == 0 ? wa.left + m : col == 1 ? (wa.left + wa.right - w) / 2 : wa.right - w - m;
	const int y = row == 0 ? wa.top + m : row == 1 ? (wa.top + wa.bottom - h) / 2 : wa.bottom - h - m;
	OffsetRect(&r, x - r.left, y - r.top);
}

// Back to the anchor after a resize or a display change; a free widget is
// only kept on screen.
void Reposition(HWND hwnd) {
	if (g_set.anchor == kAnchorFree) {
		Place_Reclamp(hwnd);
		return;
	}
	RECT r;
	GetWindowRect(hwnd, &r);
	AnchorRect(r);
	SetWindowPos(hwnd, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// When a fixed monitor is chosen, drag the widget back if anything moved it
// onto another screen. A free (non-anchored) widget keeps its relative spot by
// shifting with the monitor delta; an anchored one just re-anchors. No-op while
// following, while dragging, or when already on the right monitor.
void KeepOnChosenMonitor(HWND hwnd) {
	if (g_set.monitor == kMonitorFollow || g_dragging) {
		return;
	}
	const MonitorList ml = EnumMonitorsOrdered();
	if (g_set.monitor < 0 || g_set.monitor >= ml.count) {
		return;
	}
	RECT r;
	GetWindowRect(hwnd, &r);
	if (MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST) == ml.handle[g_set.monitor]) {
		return; // already home
	}
	if (g_set.anchor != kAnchorFree) {
		Reposition(hwnd); // AnchorRect already targets the chosen monitor
		return;
	}
	// Free widget: translate it onto the chosen monitor, same offset from the
	// top-left corner it had on whichever monitor it drifted to.
	MONITORINFO cur = {};
	cur.cbSize = sizeof(cur);
	if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &cur)) {
		return;
	}
	const RECT& dst = ml.bounds[g_set.monitor];
	RECT moved = r;
	OffsetRect(&moved, (dst.left - cur.rcMonitor.left), (dst.top - cur.rcMonitor.top));
	Place_FitToWorkArea(moved, false);
	SetWindowPos(hwnd, nullptr, moved.left, moved.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	Place_Save(kSection, hwnd);
}

// A real drag (not just a click) leaves the widget where it was dropped.
void EndDrag(HWND hwnd) {
	g_dragging = false;
	Place_Save(kSection, hwnd);
	bool changed = false;
	if (g_dragMoved && g_set.anchor != kAnchorFree) {
		g_set.anchor = kAnchorFree;
		IniWriteInt(L"Anchor", kAnchorFree);
		changed = true;
	}
	// A deliberate drop onto another monitor re-picks that monitor, so the
	// watchdog does not immediately yank the widget back to the old one.
	if (g_dragMoved && g_set.monitor != kMonitorFollow) {
		RECT r;
		GetWindowRect(hwnd, &r);
		const HMONITOR dropped = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
		const MonitorList ml = EnumMonitorsOrdered();
		for (int i = 0; i < ml.count; ++i) {
			if (ml.handle[i] == dropped && g_set.monitor != i) {
				g_set.monitor = i;
				IniWriteInt(L"Monitor", i);
				changed = true;
				break;
			}
		}
	}
	if (changed) {
		SyncSettingsWindow();
	}
}

// ---- tray icon ---------------------------------------------------------------

constexpr int kIconBars = 3;
constexpr int kIconCategories[kIconBars] = { kCatCpu, kCatGpu, kCatMemory };
constexpr int kIconBarHeights[kIconBars] = { 70, 100, 45 };

TraySlot g_traySlot = kTrayInvalidSlot;
COLORREF g_trayIconColors[kIconBars];

// Category colors as the tray shows them; the theme defaults while category
// colors are off, so the icon stays recognizable.
void IconColors(COLORREF (&out)[kIconBars]) {
	const int theme = Tray_TaskbarLight() ? kThemeLight : kThemeDark;
	for (int i = 0; i < kIconBars; ++i) {
		const COLORREF c = TrayCategoryColor(kIconCategories[i]);
		out[i] = c != CLR_INVALID ? c : DefaultCategoryColor(theme, kIconCategories[i]);
	}
}

void RemoveTrayIcon() {
	if (g_traySlot != kTrayInvalidSlot) {
		Tray_Stop(g_traySlot);
		Tray_Shutdown();
		g_traySlot = kTrayInvalidSlot;
	}
}

// Tray / context menu "Check for updates". The check runs on a worker
// thread; once an update is staged it signals the quit event, so the widget
// shuts down as Exit does and main.cpp releases the instance mutex.
void CheckForUpdates() {
	Updater_CheckAsync([]() {
		if (g_quit) {
			SetEvent(g_quit);
		}
	});
}

// Adds, updates or removes the tray icon to match the settings. Runs every
// tick, so the colors follow the settings and the taskbar theme.
void SyncTrayIcon() {
	if (!g_set.trayIcon) {
		RemoveTrayIcon();
		return;
	}
	bool fresh = false;
	if (g_traySlot == kTrayInvalidSlot) {
		TrayCallbacks cb{};
		cb.onShow = []() {
			HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kWidgetSettingsEvent);
			if (ev) {
				SetEvent(ev);
				CloseHandle(ev);
			}
		};
		cb.showLabel = L"Settings...";
		cb.closeLabel = L"Exit";
		cb.onClose = []() {
			if (g_quit) {
				SetEvent(g_quit);
			}
		};
		// "Start with Windows" and "Check for updates" above the Exit item.
		// onMenu adds them; the Show (Settings) and Close (Exit) items are
		// supplied by TrayIcon.cpp.
		cb.onMenu = [](HMENU menu, int startId) -> int {
			AppendMenuW(menu, MF_STRING | (Autostart_IsEnabled() ? MF_CHECKED : MF_UNCHECKED), startId,
				L"Start with Windows");
			AppendMenuW(menu, MF_STRING, startId + 1, L"Check for updates");
			return 2;
		};
		cb.onMenuCommand = [](UINT, int localId) {
			if (localId == 0) {
				Autostart_SetEnabled(!Autostart_IsEnabled());
			} else if (localId == 1) {
				CheckForUpdates();
			}
		};
		g_traySlot = Tray_Start(L"Desktop Widget - click for settings", L"Desktop Widget", 0xB009, true, cb);
		if (g_traySlot == kTrayInvalidSlot) {
			return;
		}
		fresh = true;
	}
	COLORREF colors[kIconBars];
	IconColors(colors);
	if (fresh || memcmp(colors, g_trayIconColors, sizeof(colors)) != 0) {
		memcpy(g_trayIconColors, colors, sizeof(colors));
		Tray_SetBarsIcon(g_traySlot, colors, kIconBarHeights, kIconBars);
	}
}

void ShowContextMenu(HWND hwnd, POINT pt) {
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, kCmdSettings, L"Settings...");
	SetMenuDefaultItem(menu, kCmdSettings, FALSE);
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	for (int m : kOrder) {
		const wchar_t* text = kMetrics[m].menu;
		wchar_t buf[128];
		const bool gpuMetric = m == kGpuLoad || m == kGpuTemp || m == kGpuClock || m == kGpuFan || m == kVramUsed || m == kVramLoad;
		const bool fanMetric = m == kCpuFan || m == kSysFan1 || m == kSysFan2;
		const AsyncSensor* pawn = m == kCpuTemp ? &g_cpuTempOpen : fanMetric ? &g_fansOpen : nullptr;
		const LONG pawnState = pawn ? AsyncState(*pawn) : kOpenIdle;
		if (fanMetric && !g_set.superIoFans) {
			swprintf_s(buf, L"%s (off in settings)", text);
			text = buf;
		} else if (pawnState == kOpenFailed) {
			swprintf_s(buf, L"%s (%s)", text, m == kCpuTemp ? g_cpuTemp.status : g_fans.status);
			text = buf;
		} else if (pawnState == kOpenBusy) {
			swprintf_s(buf, L"%s (starting PawnIO...)", text);
			text = buf;
		} else if (gpuMetric && g_gpuTried && !g_gpu.ok) {
			swprintf_s(buf, L"%s (no GPU found)", text);
			text = buf;
		}
		AppendMenuW(menu, MF_STRING | (Wants(m) ? MF_CHECKED : MF_UNCHECKED), kCmdMetricBase + m, text);
	}
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

	HMENU layout = CreatePopupMenu();
	for (int i = 0; i < kLayoutCount; ++i) {
		AppendMenuW(layout, MF_STRING | (g_set.layout == i ? MF_CHECKED : MF_UNCHECKED), kCmdLayoutBase + i, kLayoutNames[i]);
	}
	AppendMenuW(layout, MF_SEPARATOR, 0, nullptr);
	for (int i = 0; i < kGroupingCount; ++i) {
		AppendMenuW(layout, MF_STRING | (g_set.grouping == i ? MF_CHECKED : MF_UNCHECKED), kCmdGroupingBase + i, kGroupingNames[i]);
	}
	AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layout), L"Orientation");
	HMENU position = CreatePopupMenu();
	for (int i = 0; i < kAnchorCount; ++i) {
		AppendMenuW(position, MF_STRING | (g_set.anchor == i ? MF_CHECKED : MF_UNCHECKED), kCmdAnchorBase + i, kAnchorNames[i]);
	}
	AppendMenuW(position, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(position, MF_STRING | (g_set.anchor == kAnchorFree ? MF_CHECKED : MF_UNCHECKED), kCmdAnchorFree,
		L"Free (where dragged)");
	AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(position), L"Position");
	HMENU monitors = CreatePopupMenu();
	AppendMenuW(monitors, MF_STRING | (g_set.monitor == kMonitorFollow ? MF_CHECKED : MF_UNCHECKED),
		kCmdMonitorFollow, L"Follow (any monitor)");
	AppendMenuW(monitors, MF_SEPARATOR, 0, nullptr);
	const MonitorList ml = EnumMonitorsOrdered();
	for (int i = 0; i < ml.count; ++i) {
		wchar_t buf[48];
		const RECT& b = ml.bounds[i];
		swprintf_s(buf, L"Monitor %d  (%dx%d)%s", i + 1, b.right - b.left, b.bottom - b.top,
			ml.primary[i] ? L"  ★" : L"");
		AppendMenuW(monitors, MF_STRING | (g_set.monitor == i ? MF_CHECKED : MF_UNCHECKED), kCmdMonitorBase + i, buf);
	}
	AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(monitors), L"Monitor");
	AppendMenuW(menu, MF_STRING | (g_set.categoryColors ? MF_CHECKED : MF_UNCHECKED), kCmdCategoryColors, L"Color by category");
	HMENU size = CreatePopupMenu();
	for (int i = 0; i < kScalePresetCount; ++i) {
		wchar_t buf[16];
		swprintf_s(buf, L"%d%%", kScalePresets[i]);
		AppendMenuW(size, MF_STRING | (g_set.scale == kScalePresets[i] ? MF_CHECKED : MF_UNCHECKED), kCmdScaleBase + i, buf);
	}
	AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(size), L"Size");
	HMENU opacity = CreatePopupMenu();
	for (int i = 0; i < kOpacityCount; ++i) {
		wchar_t buf[16];
		swprintf_s(buf, L"%d%%", kOpacities[i]);
		AppendMenuW(opacity, MF_STRING | (g_set.opacity == kOpacities[i] ? MF_CHECKED : MF_UNCHECKED), kCmdOpacityBase + i, buf);
	}
	AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity), L"Opacity");
	AppendMenuW(menu, MF_STRING | (g_set.topMost ? MF_CHECKED : MF_UNCHECKED), kCmdTopMost, L"Always on top");
	AppendMenuW(menu, MF_STRING | (g_set.snap ? MF_CHECKED : MF_UNCHECKED), kCmdSnap, L"Snap to edges");
	AppendMenuW(menu, MF_STRING | (g_set.locked ? MF_CHECKED : MF_UNCHECKED), kCmdLock, L"Lock movement");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, kCmdUpdate, L"Check for updates");
	AppendMenuW(menu, MF_STRING, kCmdClose, L"Close widget");

	SetForegroundWindow(hwnd);
	const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
	PostMessageW(hwnd, WM_NULL, 0, 0);
	DestroyMenu(menu); // destroys the submenus too

	bool metricsChanged = false;
	if (cmd == kCmdSettings) {
		ShowSettingsWindow(hwnd);
		return;
	} else if (cmd >= kCmdMetricBase && cmd < kCmdMetricBase + kMetricCount) {
		const DWORD next = g_set.metrics ^ Bit(static_cast<int>(cmd - kCmdMetricBase));
		if (!next) {
			return; // keep at least one tile
		}
		g_set.metrics = next;
		metricsChanged = true;
	} else if (cmd >= kCmdLayoutBase && cmd < kCmdLayoutBase + kLayoutCount) {
		g_set.layout = static_cast<int>(cmd - kCmdLayoutBase);
	} else if (cmd >= kCmdAnchorBase && cmd < kCmdAnchorBase + kAnchorCount) {
		g_set.anchor = static_cast<int>(cmd - kCmdAnchorBase);
	} else if (cmd == kCmdAnchorFree) {
		g_set.anchor = kAnchorFree;
	} else if (cmd >= kCmdScaleBase && cmd < kCmdScaleBase + kScalePresetCount) {
		g_set.scale = kScalePresets[cmd - kCmdScaleBase];
	} else if (cmd >= kCmdOpacityBase && cmd < kCmdOpacityBase + kOpacityCount) {
		g_set.opacity = kOpacities[cmd - kCmdOpacityBase];
	} else if (cmd == kCmdTopMost) {
		g_set.topMost = !g_set.topMost;
	} else if (cmd == kCmdSnap) {
		g_set.snap = !g_set.snap;
	} else if (cmd >= kCmdGroupingBase && cmd < kCmdGroupingBase + kGroupingCount) {
		g_set.grouping = static_cast<int>(cmd - kCmdGroupingBase);
	} else if (cmd == kCmdCategoryColors) {
		g_set.categoryColors = !g_set.categoryColors;
	} else if (cmd == kCmdMonitorFollow) {
		g_set.monitor = kMonitorFollow;
	} else if (cmd >= kCmdMonitorBase && cmd < kCmdMonitorBase + kMaxMonitors) {
		g_set.monitor = static_cast<int>(cmd - kCmdMonitorBase);
	} else if (cmd == kCmdLock) {
		g_set.locked = !g_set.locked;
	} else if (cmd == kCmdUpdate) {
		CheckForUpdates();
		return;
	} else if (cmd == kCmdClose) {
		if (g_quit) {
			SetEvent(g_quit);
		}
		return;
	} else {
		return;
	}
	ApplySettings(metricsChanged);
	SyncSettingsWindow();
}

LRESULT CALLBACK WidgetProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_PAINT:
		Paint(hwnd);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_TIMER:
		if (wParam == kTimerId) {
			SampleAll();
			InvalidateRect(hwnd, nullptr, FALSE);
			SyncTrayIcon();
			KeepOnChosenMonitor(hwnd);
		}
		return 0;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_LBUTTONDOWN: {
		if (g_set.locked) {
			return 0;
		}
		POINT pt;
		GetCursorPos(&pt);
		RECT rc;
		GetWindowRect(hwnd, &rc);
		g_dragOffset.x = pt.x - rc.left;
		g_dragOffset.y = pt.y - rc.top;
		g_dragging = true;
		g_dragMoved = false;
		SetCapture(hwnd);
		if (!g_set.topMost) {
			SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		return 0;
	}
	case WM_MOUSEMOVE:
		if (g_dragging) {
			POINT pt;
			GetCursorPos(&pt);
			RECT rc;
			GetWindowRect(hwnd, &rc);
			const POINT was = { rc.left, rc.top };
			OffsetRect(&rc, pt.x - g_dragOffset.x - rc.left, pt.y - g_dragOffset.y - rc.top);
			Place_FitToCursorMonitor(rc, g_set.snap);
			if (rc.left != was.x || rc.top != was.y) {
				g_dragMoved = true;
				SetWindowPos(hwnd, nullptr, rc.left, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			}
		}
		return 0;
	case WM_LBUTTONUP:
		if (g_dragging) {
			EndDrag(hwnd); // before ReleaseCapture, which sends WM_CAPTURECHANGED
			ReleaseCapture();
		}
		return 0;
	case WM_CAPTURECHANGED:
		if (g_dragging) {
			EndDrag(hwnd);
		}
		return 0;
	case WM_LBUTTONDBLCLK:
		ShowSettingsWindow(hwnd);
		return 0;
	case WM_RBUTTONUP: {
		POINT pt;
		GetCursorPos(&pt);
		ShowContextMenu(hwnd, pt);
		return 0;
	}
	case WM_WINDOWPOSCHANGING: {
		WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(lParam);
		if (g_set.topMost) {
			// Win+M / "Show desktop" and app z-order churn try to hide us or
			// push us behind other windows. Veto the hide and re-assert topmost
			// on every pos change, which also re-syncs z-order right at startup.
			wp->flags &= ~SWP_HIDEWINDOW;
			wp->flags |= SWP_SHOWWINDOW;
			wp->flags &= ~SWP_NOZORDER;
			wp->hwndInsertAfter = HWND_TOPMOST;
		}
		// Every move or resize, from any source, ends fully on screen.
		Place_OnWindowPosChanging(hwnd, wp);
		break;
	}
	case WM_SYSCOMMAND:
		// Swallow minimize (Win+M, Win+D, taskbar) so the widget never folds away.
		if ((wParam & 0xFFF0) == SC_MINIMIZE) {
			return 0;
		}
		break;
	case WM_DISPLAYCHANGE:
		Reposition(hwnd);
		KeepOnChosenMonitor(hwnd);
		break;
	case WM_SETTINGCHANGE:
		if (wParam == SPI_SETWORKAREA) {
			Reposition(hwnd);
		}
		break;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateWidgetWindow() {
	const HINSTANCE inst = reinterpret_cast<HINSTANCE>(&__ImageBase);
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_DBLCLKS;
	wc.lpfnWndProc = WidgetProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = kWidgetClass;
	RegisterClassExW(&wc); // fails harmlessly when Master restarts the widget

	const Geometry g = ComputeGeometry();
	POINT pos;
	RECT r;
	if (Place_Load(kSection, pos)) {
		r = { pos.x, pos.y, pos.x + g.width, pos.y + g.height };
	} else {
		RECT wa;
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
		r = { wa.right - g.width - 16, wa.top + 16, wa.right - 16, wa.top + 16 + g.height };
	}
	Place_FitToWorkArea(r, false);
	AnchorRect(r);

	DWORD ex = WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE;
	if (g_set.topMost) {
		ex |= WS_EX_TOPMOST;
	}
	HWND hwnd = CreateWindowExW(ex, kWidgetClass, L"Desktop Widget", WS_POPUP, r.left, r.top, g.width, g.height,
		nullptr, nullptr, inst, nullptr);
	if (!hwnd) {
		return nullptr;
	}
	SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(g_set.opacity * 255 / 100), LWA_ALPHA);
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	if (g_set.topMost) {
		// Re-assert topmost at startup in case another topmost window already
		// sits above us (the EX_TOPMOST style alone does not guarantee the top).
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	return hwnd;
}

} // namespace

namespace widget {

// Monitors in a stable order: left-to-right by x, then top-to-bottom by y, so a
// saved index keeps pointing at the same physical screen across restarts and
// display-setting changes (the OS enumeration order is not stable).
MonitorList EnumMonitorsOrdered() {
	struct Acc {
		HMONITOR h[kMaxMonitors];
		RECT rc[kMaxMonitors];
		bool prim[kMaxMonitors];
		int n;
	} acc = {};
	auto cb = [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
		Acc* a = reinterpret_cast<Acc*>(lp);
		if (a->n >= kMaxMonitors) {
			return TRUE;
		}
		MONITORINFO mi = {};
		mi.cbSize = sizeof(mi);
		if (!GetMonitorInfoW(mon, &mi)) {
			return TRUE;
		}
		a->h[a->n] = mon;
		a->rc[a->n] = mi.rcMonitor;
		a->prim[a->n] = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
		++a->n;
		return TRUE;
	};
	EnumDisplayMonitors(nullptr, nullptr, cb, reinterpret_cast<LPARAM>(&acc));

	// Insertion sort so the primary monitor is index 0 ("Monitor 1", as Windows
	// numbers its primary display), then the rest by (left, top). Count is tiny.
	int idx[kMaxMonitors];
	for (int i = 0; i < acc.n; ++i) {
		idx[i] = i;
	}
	for (int i = 1; i < acc.n; ++i) {
		const int cur = idx[i];
		int j = i - 1;
		while (j >= 0) {
			const int ja = idx[j];
			const RECT& a = acc.rc[ja];
			const RECT& b = acc.rc[cur];
			bool greater;
			if (acc.prim[ja] != acc.prim[cur]) {
				// The primary sorts before everything else.
				greater = acc.prim[cur];
			}
			else {
				greater = (a.left > b.left) || (a.left == b.left && a.top > b.top);
			}
			if (!greater) {
				break;
			}
			idx[j + 1] = idx[j];
			--j;
		}
		idx[j + 1] = cur;
	}

	MonitorList out = {};
	out.count = acc.n;
	for (int i = 0; i < acc.n; ++i) {
		out.handle[i] = acc.h[idx[i]];
		out.bounds[i] = acc.rc[idx[i]];
		out.primary[i] = acc.prim[idx[i]];
	}
	return out;
}

Settings& CurrentSettings() {
	return g_set;
}

void ApplySettings(bool metricsChanged) {
	SaveSettings();
	CreateFonts();
	if (metricsChanged) {
		SampleAll();
	}
	if (g_wnd) {
		ApplyWindowSettings(g_wnd);
		Reposition(g_wnd); // an anchored widget stays on its spot as it resizes
		KeepOnChosenMonitor(g_wnd); // pull onto the chosen monitor right away
		Place_Save(kSection, g_wnd);
	}
	SyncTrayIcon();
}

Settings DefaultWidgetSettings() {
	return DefaultSettings();
}

void ResetSettings() {
	g_set = DefaultSettings();
	ApplySettings(true);
}

HICON MakeWidgetIcon(int size) {
	COLORREF colors[kIconBars];
	IconColors(colors);
	return Tray_MakeBarsIcon(colors, kIconBarHeights, kIconBars, size);
}

COLORREF TrayCategoryColor(int category) {
	if (category < 0 || category >= kCategoryCount) {
		return CLR_INVALID;
	}
	AcquireSRWLockExclusive(&g_trayColorLock);
	const ULONGLONG now = GetTickCount64();
	if (!g_trayColorRead || now - g_trayColorRead >= kTrayColorTtlMs) {
		ReadTrayColors();
		g_trayColorRead = now;
	}
	const COLORREF color = g_trayColorOn ? g_trayColor[category] : CLR_INVALID;
	ReleaseSRWLockExclusive(&g_trayColorLock);
	return color;
}

void ResetFanSensor() {
	CloseFans();
	if (g_set.superIoFans && (Wants(kCpuFan) || Wants(kSysFan1) || Wants(kSysFan2))) {
		SampleFans(); // starts the reconnect right away
	}
}

void FanSensorStatus(wchar_t* out, size_t cap) {
	if (!g_set.superIoFans) {
		swprintf_s(out, cap, L"Off: the Super I/O chip is not accessed");
		return;
	}
	switch (AsyncState(g_fansOpen)) {
	case kOpenReady:
		swprintf_s(out, cap, L"%s: reading fans", g_fans.chip);
		break;
	case kOpenBusy:
		swprintf_s(out, cap, L"Connecting...");
		break;
	case kOpenFailed:
		swprintf_s(out, cap, L"Unavailable: %s", g_fans.status);
		break;
	default:
		swprintf_s(out, cap, Wants(kCpuFan) || Wants(kSysFan1) || Wants(kSysFan2)
			? L"Connecting..." : L"Not connected (no fan panel shown)");
		break;
	}
}

} // namespace widget

// The widget's run loop. main.cpp owns the single-instance mutex and calls
// this once; it returns when the user picks Exit / Close widget. Everything
// the widget needs is in this app.
void widget::RunWidget() {
	g_quit = CreateEventW(nullptr, TRUE, FALSE, kWidgetQuitEvent);
	if (g_quit) {
		ResetEvent(g_quit);
	}
	// Auto-reset; another launch may have set it before the widget started,
	// which then opens the settings right away.
	HANDLE settingsEv = CreateEventW(nullptr, FALSE, FALSE, kWidgetSettingsEvent);

	LoadSettings();
	for (Tile& t : g_tiles) {
		t = {};
	}
	g_cpu = {};
	g_gpuTried = false;
	g_diskOpen = false;
	g_netTried = false;
	CreateFonts();
	SampleAll(); // primes the delta-based sensors; absolute ones show at once

	g_wnd = CreateWidgetWindow();
	if (g_wnd) {
		// Re-apply every loaded setting at startup through the same path the
		// runtime settings dialog uses, so z-order (topmost), opacity, geometry
		// and anchor position are all reconciled against the saved config, not
		// just whatever CreateWidgetWindow set up.
		ApplyWindowSettings(g_wnd);
		Reposition(g_wnd);
		KeepOnChosenMonitor(g_wnd);
		SetTimer(g_wnd, kTimerId, 1000, nullptr);
		SyncTrayIcon();
		HANDLE waits[2];
		DWORD n = 0;
		const DWORD quitIndex = g_quit ? n : MAXDWORD;
		if (g_quit) {
			waits[n++] = g_quit;
		}
		const DWORD settingsIndex = settingsEv ? n : MAXDWORD;
		if (settingsEv) {
			waits[n++] = settingsEv;
		}
		bool running = true;
		while (running) {
			const DWORD r = MsgWaitForMultipleObjects(n, waits, FALSE, INFINITE, QS_ALLINPUT);
			if (r == WAIT_OBJECT_0 + quitIndex) {
				break;
			}
			if (r == WAIT_OBJECT_0 + settingsIndex) {
				ShowSettingsWindow(g_wnd);
				continue;
			}
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				if (msg.message == WM_QUIT) {
					running = false;
					break;
				}
				HWND dlg = SettingsWindow();
				if (dlg && IsDialogMessageW(dlg, &msg)) {
					continue;
				}
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}
		CloseSettingsWindow();
		RemoveTrayIcon();
		KillTimer(g_wnd, kTimerId);
		DestroyWindow(g_wnd);
		g_wnd = nullptr;
	}
	UnregisterClassW(kWidgetClass, reinterpret_cast<HINSTANCE>(&__ImageBase));

	DestroyFonts();
	AsyncStop(g_cpuTempOpen);
	CpuTemp_Close(g_cpuTemp);
	CloseFans();
	Gpu_Close(g_gpu);
	if (g_diskOpen) {
		Disk_Close(g_disk);
	}
	if (settingsEv) {
		CloseHandle(settingsEv);
	}
	if (g_quit) {
		CloseHandle(g_quit);
		g_quit = nullptr;
	}
}
