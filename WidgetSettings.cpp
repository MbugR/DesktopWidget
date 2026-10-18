// Settings window for the desktop widget, laid out like NZXT CAM's Mini Mode
// page: dark cards for theme, colors, font, display (opacity / size),
// orientation, position (a spot on the screen or free), behavior, the
// panels to show and the motherboard fan sensor (status, on/off, reset).
// Every change applies to the widget immediately and is saved; there is no
// OK/Cancel. Each card's "Reset" puts its own options back to the
// defaults, "Reset all settings" all of them.
//
// Runs on the widget's thread (modeless; the widget loop calls
// IsDialogMessage for keyboard navigation). comctl32 (trackbars), comdlg32
// (color picker), uxtheme and dwmapi are loaded at runtime like every
// non-core DLL here.

#include "DesktopWidget.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <initializer_list>

extern "C" IMAGE_DOS_HEADER __ImageBase;

using namespace widget;

namespace {

constexpr wchar_t kClass[] = L"DesktopWidgetSettingsWnd";

enum ControlId {
	kIdThemeDark = 1001,
	kIdThemeLight,
	kIdCustomColors = 1010,
	kIdColorValue,
	kIdColorLabel,
	kIdColorTile,
	kIdResetColors,
	kIdTempColors,
	kIdCategoryColors,
	kIdResetCategoryColors,
	kIdFont = 1020,
	kIdFontBold,
	kIdOpacity = 1030,
	kIdScale,
	kIdAnchorMargin,
	kIdLayoutBase = 1040, // + Layout
	kIdGroupingBase = 1045, // + Grouping
	kIdTopMost = 1050,
	kIdSnap,
	kIdLock,
	kIdTrayIcon,
	kIdSuperIoFans = 1060,
	kIdResetFans,
	kIdMonitor = 1065,
	kIdCatColorBase = 1070, // + Category
	kIdAnchorBase = 1080, // + Anchor
	kIdMetricBase = 1100, // + Metric
	kIdClose = 1200,
	kIdResetAll,
	// Card "Reset" buttons (Custom Interface Colors, Category Colors and
	// Motherboard Fans have theirs above).
	kIdResetTheme = 1210,
	kIdResetDisplay,
	kIdResetFont,
	kIdResetOrientation,
	kIdResetPosition,
	kIdResetCustomization,
	kIdResetPanels,
};

constexpr UINT_PTR kStatusTimer = 1;

// Colors of the window itself (always dark, like CAM).
constexpr COLORREF kWinBg = RGB(24, 24, 27);
constexpr COLORREF kCardBg = RGB(35, 35, 39);
constexpr COLORREF kCardBorder = RGB(58, 58, 64);
constexpr COLORREF kText = RGB(222, 222, 226);
constexpr COLORREF kDimText = RGB(140, 140, 146);
constexpr COLORREF kInputBg = RGB(22, 22, 25);
constexpr COLORREF kButtonBg = RGB(50, 50, 56);
constexpr COLORREF kButtonBorder = RGB(84, 84, 92);
constexpr COLORREF kAccent = RGB(79, 193, 255);

// Layout: two columns of cards.
constexpr int kMargin = 12;
constexpr int kColW = 360;
constexpr int kPad = 14;
constexpr int kHeaderH = 26;
constexpr int kRowH = 28;
constexpr int kCardGap = 10;
constexpr int kClientW = kMargin * 3 + kColW * 2;
// The Position card's buttons, one per spot of a 3 x 3 grid.
constexpr int kAnchorBtnW = 44;
constexpr int kAnchorBtnH = 26;
constexpr int kAnchorBtnGap = 4;

struct TextItem {
	int x;
	int y;
	const wchar_t* text;
	bool header;
};

struct Card {
	RECT rc;
};

HWND g_wnd;
HWND g_widget;
HFONT g_uiFont;
HFONT g_headerFont;
HBRUSH g_winBrush;
HBRUSH g_cardBrush;
HBRUSH g_inputBrush;
TextItem g_texts[32];
int g_textCount;
Card g_cards[12];
int g_cardCount;
RECT g_opacityValueRc;
RECT g_scaleValueRc;
RECT g_anchorMarginValueRc;
RECT g_anchorStatusRc;
RECT g_fanStatusRc;
HICON g_iconSmall;
HICON g_iconBig;
int g_clientH;
COLORREF g_customColors[16];

// ---- runtime-loaded helpers ------------------------------------------------------

bool InitTrackbarClass() {
	static int s_state = 0; // 0 = not tried, 1 = ok, -1 = failed
	if (s_state == 0) {
		s_state = -1;
		HMODULE m = LoadLibraryW(L"comctl32.dll");
		auto init = m ? reinterpret_cast<BOOL(WINAPI*)(const INITCOMMONCONTROLSEX*)>(GetProcAddress(m, "InitCommonControlsEx")) : nullptr;
		INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
		if (init && init(&icc)) {
			s_state = 1;
		}
	}
	return s_state == 1;
}

// Classic (unthemed) controls honor WM_CTLCOLOR*, themed ones ignore the
// text color, which would leave black text on the dark cards.
// (Plain lazy pointers, no function-local statics with initializers: those
// pull in TLS-based guards, and this DLL is also loaded by Verifier.)
using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
SetWindowThemeFn g_setWindowTheme;
bool g_setWindowThemeTried;

void Unthemed(HWND h) {
	if (!g_setWindowThemeTried) {
		g_setWindowThemeTried = true;
		HMODULE m = LoadLibraryW(L"uxtheme.dll");
		g_setWindowTheme = m ? reinterpret_cast<SetWindowThemeFn>(GetProcAddress(m, "SetWindowTheme")) : nullptr;
	}
	if (g_setWindowTheme) {
		g_setWindowTheme(h, L"", L"");
	}
}

void DarkCaption(HWND h) {
	HMODULE m = LoadLibraryW(L"dwmapi.dll");
	auto set = m ? reinterpret_cast<HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD)>(GetProcAddress(m, "DwmSetWindowAttribute")) : nullptr;
	if (set) {
		const BOOL on = TRUE;
		set(h, 20, &on, sizeof(on)); // DWMWA_USE_IMMERSIVE_DARK_MODE
	}
}

bool PickColor(HWND owner, COLORREF& color) {
	HMODULE m = LoadLibraryW(L"comdlg32.dll");
	auto choose = m ? reinterpret_cast<BOOL(WINAPI*)(LPCHOOSECOLORW)>(GetProcAddress(m, "ChooseColorW")) : nullptr;
	if (!choose) {
		return false;
	}
	CHOOSECOLORW cc = {};
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = owner;
	cc.rgbResult = color;
	cc.lpCustColors = g_customColors;
	cc.Flags = CC_RGBINIT | CC_FULLOPEN;
	if (!choose(&cc)) {
		return false;
	}
	color = cc.rgbResult;
	return true;
}

// ---- building the window ------------------------------------------------------------

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
	HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr);
	SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), FALSE);
	return c;
}

HWND AddCheck(int id, const wchar_t* text, int x, int y, int w, bool checked, bool group = false) {
	HWND c = Add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP | (group ? WS_GROUP : 0), x, y, w, 22, id);
	Unthemed(c);
	SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
	return c;
}

HWND AddRadio(int id, const wchar_t* text, int x, int y, int w, bool checked, bool first) {
	HWND c = Add(L"BUTTON", text, BS_AUTORADIOBUTTON | (first ? WS_GROUP | WS_TABSTOP : 0), x, y, w, 22, id);
	Unthemed(c);
	SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
	return c;
}

HWND AddButton(int id, const wchar_t* text, int x, int y, int w, int h) {
	return Add(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP | WS_GROUP, x, y, w, h, id);
}

HWND AddTrackbar(int id, int x, int y, int w, int lo, int hi, int pos) {
	HWND c = Add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP | WS_GROUP, x, y, w, 26, id);
	Unthemed(c);
	SendMessageW(c, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
	SendMessageW(c, TBM_SETPAGESIZE, 0, 10);
	SendMessageW(c, TBM_SETPOS, TRUE, pos);
	return c;
}

void AddText(int x, int y, const wchar_t* text, bool header = false) {
	if (g_textCount < 32) {
		g_texts[g_textCount++] = { x, y, text, header };
	}
}

// Cards grow downward; `Begin` returns the first content row.
struct CardBuilder {
	int x;
	int top;
	int y;
	int Begin(int col, int startY, const wchar_t* title) {
		x = kMargin + col * (kColW + kMargin);
		top = startY;
		AddText(x + kPad, top + kPad - 2, title, true);
		y = top + kPad + kHeaderH;
		return y;
	}
	int End() {
		if (g_cardCount < static_cast<int>(_countof(g_cards))) {
			g_cards[g_cardCount++].rc = { x, top, x + kColW, y + kPad - 6 };
		}
		return y + kPad - 6 + kCardGap;
	}
};

// The card's "Reset" button, at the right of its title.
void AddCardReset(const CardBuilder& c, int id) {
	AddButton(id, L"Reset", c.x + kColW - kPad - 80, c.top + kPad - 4, 80, 26);
}

int CALLBACK AddFontFamily(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
	HWND combo = reinterpret_cast<HWND>(lp);
	const wchar_t* name = lf->lfFaceName;
	if (name[0] && name[0] != L'@'
			&& SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name)) == CB_ERR) {
		SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
	}
	return 1;
}

void FillFonts(HWND combo) {
	HDC dc = GetDC(nullptr);
	LOGFONTW lf = {};
	lf.lfCharSet = DEFAULT_CHARSET;
	EnumFontFamiliesExW(dc, &lf, AddFontFamily, reinterpret_cast<LPARAM>(combo), 0);
	ReleaseDC(nullptr, dc);
	const Settings& s = CurrentSettings();
	LRESULT idx = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(s.font));
	if (idx == CB_ERR) {
		idx = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(L"Bahnschrift"));
	}
	SendMessageW(combo, CB_SETCURSEL, idx == CB_ERR ? 0 : idx, 0);
}

// Monitor picker: item 0 = "Follow", then one per monitor in stable order.
// Item data holds the setting value (kMonitorFollow or the 0-based index).
void FillMonitors(HWND combo) {
	SendMessageW(combo, CB_RESETCONTENT, 0, 0);
	LRESULT row = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Follow (any monitor)"));
	SendMessageW(combo, CB_SETITEMDATA, row, static_cast<LPARAM>(kMonitorFollow));
	const MonitorList ml = EnumMonitorsOrdered();
	const Settings& s = CurrentSettings();
	LRESULT sel = 0; // Follow by default
	for (int i = 0; i < ml.count; ++i) {
		wchar_t buf[48];
		const RECT& b = ml.bounds[i];
		swprintf_s(buf, L"Monitor %d  (%dx%d)%s", i + 1, b.right - b.left, b.bottom - b.top,
			ml.primary[i] ? L"  ★" : L"");
		row = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
		SendMessageW(combo, CB_SETITEMDATA, row, static_cast<LPARAM>(i));
		if (s.monitor == i) {
			sel = row;
		}
	}
	SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

bool IsCategorySwatch(int id) {
	return id >= kIdCatColorBase && id < kIdCatColorBase + kCategoryCount;
}

bool IsAnchorButton(int id) {
	return id >= kIdAnchorBase && id < kIdAnchorBase + kAnchorCount;
}

bool IsSwatch(int id) {
	return id == kIdColorValue || id == kIdColorLabel || id == kIdColorTile || IsCategorySwatch(id);
}

void EnableLayoutControls() {
	const Settings& s = CurrentSettings();
	for (int i = 0; i < kLayoutCount; ++i) {
		EnableWindow(GetDlgItem(g_wnd, kIdLayoutBase + i), s.grouping == kGroupNone);
	}
}

void EnableColorControls() {
	const Settings& s = CurrentSettings();
	for (int id : { kIdColorValue, kIdColorLabel, kIdColorTile }) {
		EnableWindow(GetDlgItem(g_wnd, id), s.customColors);
	}
	for (int c = 0; c < kCategoryCount; ++c) {
		EnableWindow(GetDlgItem(g_wnd, kIdCatColorBase + c), s.categoryColors);
	}
}

void BuildControls() {
	const Settings& s = CurrentSettings();
	g_textCount = 0;
	g_cardCount = 0;
	CardBuilder c = {};

	// Left column.
	int y = c.Begin(0, kMargin, L"Theme");
	AddCardReset(c, kIdResetTheme);
	AddRadio(kIdThemeDark, L"Dark", c.x + kPad, y, 100, s.theme == kThemeDark, true);
	AddRadio(kIdThemeLight, L"Light", c.x + kPad + 120, y, 100, s.theme == kThemeLight, false);
	c.y = y + kRowH;
	y = c.End();

	y = c.Begin(0, y, L"Custom Interface Colors");
	AddCardReset(c, kIdResetColors);
	AddCheck(kIdCustomColors, L"Enable", c.x + kPad, y, 150, s.customColors, true);
	y += kRowH + 2;
	const struct {
		int id;
		const wchar_t* label;
	} kColorRows[] = {
		{ kIdColorValue, L"Reading" },
		{ kIdColorLabel, L"Reading title" },
		{ kIdColorTile, L"Tile background" },
	};
	for (const auto& row : kColorRows) {
		AddText(c.x + kPad, y + 4, row.label);
		AddButton(row.id, L"", c.x + kPad + 120, y, 130, 26);
		y += kRowH + 4;
	}
	AddCheck(kIdTempColors, L"Orange / red for hot temperatures", c.x + kPad, y, 300, s.tempColors, true);
	c.y = y + kRowH;
	y = c.End();

	// Like syntax highlighting: one value color per category. The CPU, RAM,
	// disk and network tray icons follow it too.
	y = c.Begin(0, y, L"Category Colors");
	AddCardReset(c, kIdResetCategoryColors);
	AddCheck(kIdCategoryColors, L"Color readings by category (also tray icons)", c.x + kPad, y, kColW - 2 * kPad, s.categoryColors, true);
	y += kRowH + 2;
	for (int cat = 0; cat < kCategoryCount; ++cat) {
		const int x = c.x + kPad + (cat % 2) * 170;
		const int rowY = y + (cat / 2) * (kRowH + 4);
		AddText(x, rowY + 4, kCategoryNames[cat]);
		AddButton(kIdCatColorBase + cat, L"", x + 60, rowY, 96, 26);
	}
	c.y = y + ((kCategoryCount + 1) / 2) * (kRowH + 4) - 4;
	y = c.End();

	y = c.Begin(0, y, L"Display");
	AddCardReset(c, kIdResetDisplay);
	AddText(c.x + kPad, y + 4, L"Opacity");
	AddTrackbar(kIdOpacity, c.x + kPad + 70, y, 200, kOpacityMin, 100, s.opacity);
	g_opacityValueRc = { c.x + kPad + 278, y + 4, c.x + kColW - kPad, y + 22 };
	y += kRowH + 6;
	AddText(c.x + kPad, y + 4, L"Size");
	AddTrackbar(kIdScale, c.x + kPad + 70, y, 200, kScaleMin, kScaleMax, s.scale);
	g_scaleValueRc = { c.x + kPad + 278, y + 4, c.x + kColW - kPad, y + 22 };
	c.y = y + kRowH;
	y = c.End();

	y = c.Begin(0, y, L"Font");
	AddCardReset(c, kIdResetFont);
	AddText(c.x + kPad, y + 4, L"Value font");
	HWND combo = Add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
		c.x + kPad + 80, y, 180, 320, kIdFont);
	Unthemed(combo);
	FillFonts(combo);
	AddCheck(kIdFontBold, L"Bold", c.x + kPad + 272, y + 1, 60, s.fontBold, true);
	c.y = y + kRowH + 2;
	const int leftBottom = c.End();

	// Right column.
	y = c.Begin(1, kMargin, L"Orientation");
	AddCardReset(c, kIdResetOrientation);
	for (int i = 0; i < kLayoutCount; ++i) {
		AddRadio(kIdLayoutBase + i, kLayoutNames[i], c.x + kPad + i * 110, y, 105, s.layout == i, i == 0);
	}
	y += kRowH;
	// Grouping replaces the orientation, whose radios then gray out.
	const int groupX[kGroupingCount] = { 0, 108, 228 };
	const int groupW[kGroupingCount] = { 100, 116, 104 };
	for (int i = 0; i < kGroupingCount; ++i) {
		AddRadio(kIdGroupingBase + i, kGroupingNames[i], c.x + kPad + groupX[i], y, groupW[i], s.grouping == i, i == 0);
	}
	c.y = y + kRowH;
	y = c.End();

	// A 3 x 3 grid standing for the screen: pin the widget to a corner, an
	// edge center or the center of its monitor, or leave it where dragged.
	y = c.Begin(1, y, L"Position");
	AddCardReset(c, kIdResetPosition);
	for (int a = 0; a < kAnchorCount; ++a) {
		AddButton(kIdAnchorBase + a, L"", c.x + kPad + (a % 3) * (kAnchorBtnW + kAnchorBtnGap),
			y + (a / 3) * (kAnchorBtnH + kAnchorBtnGap), kAnchorBtnW, kAnchorBtnH);
	}
	const int gridH = 3 * kAnchorBtnH + 2 * kAnchorBtnGap;
	const int px = c.x + kPad + 3 * kAnchorBtnW + 2 * kAnchorBtnGap + 18;
	AddText(px, y, L"Gap to the screen edge");
	AddTrackbar(kIdAnchorMargin, px - 4, y + 20, 128, 0, kAnchorMarginMax, s.anchorMargin);
	g_anchorMarginValueRc = { px + 128, y + 24, c.x + kColW - kPad, y + 42 };
	g_anchorStatusRc = { px, y + 48, c.x + kColW - kPad, y + gridH + 4 };
	// Pin the widget to one monitor (it is dragged back if moved off), or follow.
	const int my = y + gridH + 8;
	AddText(c.x + kPad, my + 4, L"Monitor");
	HWND mcombo = Add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
		c.x + kPad + 64, my, kColW - 2 * kPad - 64, 240, kIdMonitor);
	Unthemed(mcombo);
	FillMonitors(mcombo);
	c.y = my + kRowH;
	y = c.End();

	y = c.Begin(1, y, L"Customization");
	AddCardReset(c, kIdResetCustomization);
	AddCheck(kIdTopMost, L"Always on top", c.x + kPad, y, 115, s.topMost, true);
	AddCheck(kIdSnap, L"Snap to edges", c.x + kPad + 118, y, 110, s.snap);
	AddCheck(kIdLock, L"Lock movement", c.x + kPad + 232, y, kColW - 2 * kPad - 232, s.locked);
	y += kRowH;
	AddCheck(kIdTrayIcon, L"Tray icon (click opens these settings)", c.x + kPad, y, 330, s.trayIcon, true);
	c.y = y + kRowH;
	y = c.End();

	y = c.Begin(1, y, L"Panels");
	AddCardReset(c, kIdResetPanels);
	for (int i = 0; i < kMetricCount; ++i) {
		const int m = kOrder[i];
		const int col = i % 3;
		const int row = i / 3;
		AddCheck(kIdMetricBase + m, kMetrics[m].label, c.x + kPad + col * 110, y + row * (kRowH - 2), 106,
			(s.metrics & (1u << m)) != 0, i == 0);
	}
	c.y = y + ((kMetricCount + 2) / 3) * (kRowH - 2);
	y = c.End();

	// Motherboard fans: what the Super I/O access is doing, a switch to stop
	// it entirely, and a reset that puts the chip back as it was found.
	y = c.Begin(1, y, L"Motherboard Fans");
	AddButton(kIdResetFans, L"Reset fan sensor", c.x + kColW - kPad - 130, c.top + kPad - 4, 130, 26);
	g_fanStatusRc = { c.x + kPad, y + 2, c.x + kColW - kPad, y + 20 };
	y += kRowH - 4;
	AddCheck(kIdSuperIoFans, L"Read fans from the Super I/O chip (read only)", c.x + kPad, y, 320, s.superIoFans, true);
	c.y = y + kRowH;
	const int rightBottom = c.End();

	const int bottom = leftBottom > rightBottom ? leftBottom : rightBottom;
	AddButton(kIdResetAll, L"Reset all settings", kMargin, bottom, 140, 30);
	AddButton(kIdClose, L"Close", kClientW - kMargin - 100, bottom, 100, 30);
	g_clientH = bottom + 30 + kMargin;
	EnableColorControls();
	EnableLayoutControls();
}

void SetCheck(int id, bool on) {
	SendMessageW(GetDlgItem(g_wnd, id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Puts every control back in line with the settings (after a reset).
void SyncControls() {
	const Settings& s = CurrentSettings();
	SetCheck(kIdThemeDark, s.theme == kThemeDark);
	SetCheck(kIdThemeLight, s.theme == kThemeLight);
	SetCheck(kIdCustomColors, s.customColors);
	SetCheck(kIdTempColors, s.tempColors);
	HWND combo = GetDlgItem(g_wnd, kIdFont);
	const LRESULT idx = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(s.font));
	if (idx != CB_ERR) {
		SendMessageW(combo, CB_SETCURSEL, idx, 0);
	}
	SetCheck(kIdFontBold, s.fontBold);
	HWND mcombo = GetDlgItem(g_wnd, kIdMonitor);
	if (mcombo) {
		const LRESULT n = SendMessageW(mcombo, CB_GETCOUNT, 0, 0);
		for (LRESULT i = 0; i < n; ++i) {
			if (static_cast<int>(SendMessageW(mcombo, CB_GETITEMDATA, i, 0)) == s.monitor) {
				SendMessageW(mcombo, CB_SETCURSEL, i, 0);
				break;
			}
		}
	}
	SendMessageW(GetDlgItem(g_wnd, kIdOpacity), TBM_SETPOS, TRUE, s.opacity);
	SendMessageW(GetDlgItem(g_wnd, kIdScale), TBM_SETPOS, TRUE, s.scale);
	SendMessageW(GetDlgItem(g_wnd, kIdAnchorMargin), TBM_SETPOS, TRUE, s.anchorMargin);
	SetCheck(kIdTrayIcon, s.trayIcon);
	for (int i = 0; i < kLayoutCount; ++i) {
		SetCheck(kIdLayoutBase + i, s.layout == i);
	}
	SetCheck(kIdTopMost, s.topMost);
	SetCheck(kIdSnap, s.snap);
	SetCheck(kIdLock, s.locked);
	SetCheck(kIdSuperIoFans, s.superIoFans);
	SetCheck(kIdCategoryColors, s.categoryColors);
	for (int i = 0; i < kGroupingCount; ++i) {
		SetCheck(kIdGroupingBase + i, s.grouping == i);
	}
	EnableLayoutControls();
	for (int m = 0; m < kMetricCount; ++m) {
		SetCheck(kIdMetricBase + m, (s.metrics & (1u << m)) != 0);
	}
	EnableColorControls();
	InvalidateRect(g_wnd, nullptr, TRUE);
	for (HWND child = GetWindow(g_wnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
		InvalidateRect(child, nullptr, TRUE);
	}
}

// ---- painting ---------------------------------------------------------------------------

void PaintWindow(HWND hwnd) {
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(hwnd, &ps);
	RECT client;
	GetClientRect(hwnd, &client);
	FillRect(dc, &client, g_winBrush);

	HPEN pen = CreatePen(PS_SOLID, 1, kCardBorder);
	HGDIOBJ oldPen = SelectObject(dc, pen);
	HGDIOBJ oldBrush = SelectObject(dc, g_cardBrush);
	for (int i = 0; i < g_cardCount; ++i) {
		const RECT& r = g_cards[i].rc;
		RoundRect(dc, r.left, r.top, r.right, r.bottom, 8, 8);
	}
	SelectObject(dc, oldBrush);
	SelectObject(dc, oldPen);
	DeleteObject(pen);

	SetBkMode(dc, TRANSPARENT);
	HGDIOBJ oldFont = SelectObject(dc, g_uiFont);
	for (int i = 0; i < g_textCount; ++i) {
		const TextItem& t = g_texts[i];
		SelectObject(dc, t.header ? g_headerFont : g_uiFont);
		SetTextColor(dc, t.header ? RGB(255, 255, 255) : kText);
		TextOutW(dc, t.x, t.y, t.text, lstrlenW(t.text));
	}
	const Settings& s = CurrentSettings();
	wchar_t buf[16];
	SelectObject(dc, g_uiFont);
	SetTextColor(dc, kDimText);
	swprintf_s(buf, L"%d%%", s.opacity);
	DrawTextW(dc, buf, -1, &g_opacityValueRc, DT_LEFT | DT_SINGLELINE);
	swprintf_s(buf, L"%d%%", s.scale);
	DrawTextW(dc, buf, -1, &g_scaleValueRc, DT_LEFT | DT_SINGLELINE);
	swprintf_s(buf, L"%d px", s.anchorMargin);
	DrawTextW(dc, buf, -1, &g_anchorMarginValueRc, DT_LEFT | DT_SINGLELINE);
	wchar_t anchor[96];
	if (s.anchor == kAnchorFree) {
		swprintf_s(anchor, L"Free: where dragged\nClick a spot to pin it");
	} else {
		swprintf_s(anchor, L"Pinned: %s\nDrag the widget to unpin", kAnchorNames[s.anchor]);
	}
	DrawTextW(dc, anchor, -1, &g_anchorStatusRc, DT_LEFT | DT_WORDBREAK);
	wchar_t status[128];
	FanSensorStatus(status, 128);
	DrawTextW(dc, status, -1, &g_fanStatusRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
	SelectObject(dc, oldFont);
	EndPaint(hwnd, &ps);
}

COLORREF& SwatchColor(int id) {
	Settings& s = CurrentSettings();
	if (IsCategorySwatch(id)) {
		return s.categoryColor[id - kIdCatColorBase];
	}
	return id == kIdColorValue ? s.valueColor : id == kIdColorLabel ? s.labelColor : s.tileColor;
}

void DrawButton(const DRAWITEMSTRUCT& di) {
	HDC dc = di.hDC;
	RECT rc = di.rcItem;
	const bool disabled = (di.itemState & ODS_DISABLED) != 0;
	const bool pressed = (di.itemState & ODS_SELECTED) != 0;
	const int id = static_cast<int>(di.CtlID);
	const bool pinned = IsAnchorButton(id) && CurrentSettings().anchor == id - kIdAnchorBase;
	FillRect(dc, &rc, g_cardBrush);
	HBRUSH bg = CreateSolidBrush(pressed ? RGB(38, 38, 42) : pinned ? RGB(36, 56, 70) : kButtonBg);
	HPEN pen = CreatePen(PS_SOLID, 1, pinned ? kAccent : (di.itemState & ODS_FOCUS) ? RGB(140, 140, 150) : kButtonBorder);
	HGDIOBJ oldBrush = SelectObject(dc, bg);
	HGDIOBJ oldPen = SelectObject(dc, pen);
	RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
	SelectObject(dc, oldBrush);
	SelectObject(dc, oldPen);
	DeleteObject(bg);
	DeleteObject(pen);

	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, disabled ? kDimText : kText);
	HGDIOBJ oldFont = SelectObject(dc, g_uiFont);
	if (IsAnchorButton(id)) {
		// The button is the screen; a small block marks the widget's spot.
		const int a = id - kIdAnchorBase;
		const int bw = 14;
		const int bh = 5;
		const int x = rc.left + 4 + (a % 3) * (rc.right - rc.left - 8 - bw) / 2;
		const int y = rc.top + 3 + (a / 3) * (rc.bottom - rc.top - 6 - bh) / 2;
		const RECT block = { x, y, x + bw, y + bh };
		HBRUSH fill = CreateSolidBrush(pinned ? kAccent : RGB(118, 118, 126));
		FillRect(dc, &block, fill);
		DeleteObject(fill);
	} else if (IsSwatch(id)) {
		// Swatch + hex, like CAM's color pickers.
		const COLORREF c = SwatchColor(id);
		RECT sw = { rc.left + 6, rc.top + 5, rc.left + 6 + (rc.bottom - rc.top - 10), rc.bottom - 5 };
		HBRUSH fill = CreateSolidBrush(disabled ? RGB(70, 70, 76) : c);
		FillRect(dc, &sw, fill);
		DeleteObject(fill);
		wchar_t hex[16];
		swprintf_s(hex, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
		RECT tr = { sw.right + 8, rc.top, rc.right - 4, rc.bottom };
		DrawTextW(dc, hex, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	} else {
		wchar_t text[64];
		GetWindowTextW(di.hwndItem, text, 64);
		DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	SelectObject(dc, oldFont);
}

// ---- reacting to the controls ------------------------------------------------------------

bool Checked(int id) {
	return SendMessageW(GetDlgItem(g_wnd, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void ResetColorsToTheme(Settings& s) {
	const Palette p = ThemePalette(s.theme);
	s.valueColor = p.value;
	s.labelColor = p.label;
	s.tileColor = p.tile;
}

void InvalidateSwatches() {
	for (int id : { kIdColorValue, kIdColorLabel, kIdColorTile }) {
		InvalidateRect(GetDlgItem(g_wnd, id), nullptr, FALSE);
	}
	for (int c = 0; c < kCategoryCount; ++c) {
		InvalidateRect(GetDlgItem(g_wnd, kIdCatColorBase + c), nullptr, FALSE);
	}
}

void InvalidateAnchorControls() {
	for (int a = 0; a < kAnchorCount; ++a) {
		InvalidateRect(GetDlgItem(g_wnd, kIdAnchorBase + a), nullptr, FALSE);
	}
	InvalidateRect(g_wnd, &g_anchorStatusRc, TRUE);
}

void SetTheme(Settings& s, int theme) {
	const int oldTheme = s.theme;
	s.theme = theme;
	if (!s.customColors) {
		ResetColorsToTheme(s); // custom colors start from the chosen theme
	}
	// Category colors still at the old theme's default follow the theme;
	// ones the user picked stay.
	for (int c = 0; c < kCategoryCount; ++c) {
		if (s.categoryColor[c] == DefaultCategoryColor(oldTheme, c)) {
			s.categoryColor[c] = DefaultCategoryColor(s.theme, c);
		}
	}
}

bool IsCardReset(int id) {
	return (id >= kIdResetTheme && id <= kIdResetPanels) || id == kIdResetColors || id == kIdResetCategoryColors;
}

// Puts the options of one card back to the defaults.
void ResetCard(int id, Settings& s) {
	const Settings d = DefaultWidgetSettings();
	switch (id) {
	case kIdResetTheme:
		SetTheme(s, d.theme);
		break;
	case kIdResetColors:
		s.customColors = d.customColors;
		ResetColorsToTheme(s);
		s.tempColors = d.tempColors;
		break;
	case kIdResetCategoryColors:
		s.categoryColors = d.categoryColors;
		for (int c = 0; c < kCategoryCount; ++c) {
			s.categoryColor[c] = DefaultCategoryColor(s.theme, c);
		}
		break;
	case kIdResetDisplay:
		s.opacity = d.opacity;
		s.scale = d.scale;
		break;
	case kIdResetFont:
		lstrcpynW(s.font, d.font, LF_FACESIZE);
		s.fontBold = d.fontBold;
		break;
	case kIdResetOrientation:
		s.layout = d.layout;
		s.grouping = d.grouping;
		break;
	case kIdResetPosition:
		s.anchor = d.anchor; // free: the widget stays where it is
		s.anchorMargin = d.anchorMargin;
		s.monitor = d.monitor;
		break;
	case kIdResetCustomization:
		s.topMost = d.topMost;
		s.snap = d.snap;
		s.locked = d.locked;
		s.trayIcon = d.trayIcon;
		break;
	case kIdResetPanels:
		s.metrics = d.metrics;
		break;
	default:
		break;
	}
}

void OnCommand(int id, int code) {
	Settings& s = CurrentSettings();
	bool metricsChanged = false;
	if (id == kIdThemeDark || id == kIdThemeLight) {
		SetTheme(s, id == kIdThemeLight ? kThemeLight : kThemeDark);
		InvalidateSwatches();
	} else if (IsCardReset(id)) {
		ResetCard(id, s);
		ApplySettings(id == kIdResetPanels);
		SyncControls();
		return;
	} else if (id == kIdCustomColors) {
		s.customColors = Checked(id);
		EnableColorControls();
	} else if (IsSwatch(id)) {
		COLORREF& target = SwatchColor(id);
		COLORREF c = target;
		if (!PickColor(g_wnd, c)) {
			return;
		}
		target = c;
		InvalidateSwatches();
	} else if (id == kIdCategoryColors) {
		s.categoryColors = Checked(id);
		EnableColorControls();
	} else if (id >= kIdGroupingBase && id < kIdGroupingBase + kGroupingCount) {
		s.grouping = id - kIdGroupingBase;
		EnableLayoutControls();
	} else if (id == kIdTempColors) {
		s.tempColors = Checked(id);
	} else if (id == kIdFont && code == CBN_SELCHANGE) {
		HWND combo = GetDlgItem(g_wnd, kIdFont);
		const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
		if (sel == CB_ERR || SendMessageW(combo, CB_GETLBTEXTLEN, sel, 0) >= LF_FACESIZE) {
			return;
		}
		SendMessageW(combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(s.font));
	} else if (id == kIdFontBold) {
		s.fontBold = Checked(id);
	} else if (id == kIdMonitor && code == CBN_SELCHANGE) {
		HWND combo = GetDlgItem(g_wnd, kIdMonitor);
		const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
		if (sel == CB_ERR) {
			return;
		}
		s.monitor = static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, sel, 0));
	} else if (id >= kIdLayoutBase && id < kIdLayoutBase + kLayoutCount) {
		s.layout = id - kIdLayoutBase;
	} else if (id == kIdTopMost) {
		s.topMost = Checked(id);
	} else if (id == kIdSnap) {
		s.snap = Checked(id);
	} else if (id == kIdLock) {
		s.locked = Checked(id);
	} else if (id == kIdTrayIcon) {
		s.trayIcon = Checked(id);
	} else if (IsAnchorButton(id)) {
		const int a = id - kIdAnchorBase;
		s.anchor = s.anchor == a ? kAnchorFree : a; // the pinned spot again unpins
		InvalidateAnchorControls();
	} else if (id == kIdSuperIoFans) {
		s.superIoFans = Checked(id);
		metricsChanged = true; // closes or reopens the sensor right away
		InvalidateRect(g_wnd, &g_fanStatusRc, TRUE);
	} else if (id == kIdResetFans) {
		ResetFanSensor();
		InvalidateRect(g_wnd, &g_fanStatusRc, TRUE);
		return;
	} else if (id == kIdResetAll) {
		if (MessageBoxW(g_wnd, L"Reset every widget setting (theme, colors, size, font, orientation, position, "
				L"behavior, panels) to the defaults and reconnect the fan sensor?\r\n"
				L"The widget stays where it is on the screen.", L"Reset all settings", MB_YESNO | MB_ICONQUESTION) != IDYES) {
			return;
		}
		ResetSettings();
		ResetFanSensor();
		SyncControls();
		return;
	} else if (id >= kIdMetricBase && id < kIdMetricBase + kMetricCount) {
		const DWORD next = s.metrics ^ (1u << (id - kIdMetricBase));
		if (!next) {
			// Keep at least one tile.
			SendMessageW(GetDlgItem(g_wnd, id), BM_SETCHECK, BST_CHECKED, 0);
			return;
		}
		s.metrics = next;
		metricsChanged = true;
	} else if (id == kIdClose || id == IDCANCEL) {
		DestroyWindow(g_wnd);
		return;
	} else {
		return;
	}
	ApplySettings(metricsChanged);
}

void OnTrackbar(HWND bar) {
	Settings& s = CurrentSettings();
	const int pos = static_cast<int>(SendMessageW(bar, TBM_GETPOS, 0, 0));
	const int id = GetDlgCtrlID(bar);
	if (id == kIdOpacity) {
		if (pos == s.opacity) {
			return;
		}
		s.opacity = pos;
		InvalidateRect(g_wnd, &g_opacityValueRc, TRUE);
	} else if (id == kIdScale) {
		if (pos == s.scale) {
			return;
		}
		s.scale = pos;
		InvalidateRect(g_wnd, &g_scaleValueRc, TRUE);
	} else if (id == kIdAnchorMargin) {
		if (pos == s.anchorMargin) {
			return;
		}
		s.anchorMargin = pos;
		InvalidateRect(g_wnd, &g_anchorMarginValueRc, TRUE);
	} else {
		return;
	}
	ApplySettings(false);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_PAINT:
		PaintWindow(hwnd);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN: {
		HDC dc = reinterpret_cast<HDC>(wParam);
		SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lParam)) ? kText : kDimText);
		SetBkColor(dc, kCardBg);
		return reinterpret_cast<LRESULT>(g_cardBrush);
	}
	case WM_CTLCOLORLISTBOX:
	case WM_CTLCOLOREDIT: {
		HDC dc = reinterpret_cast<HDC>(wParam);
		SetTextColor(dc, kText);
		SetBkColor(dc, kInputBg);
		return reinterpret_cast<LRESULT>(g_inputBrush);
	}
	case WM_DRAWITEM:
		DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
		return TRUE;
	case WM_COMMAND:
		OnCommand(LOWORD(wParam), HIWORD(wParam));
		return 0;
	case WM_HSCROLL:
		if (lParam) {
			OnTrackbar(reinterpret_cast<HWND>(lParam));
		}
		return 0;
	case WM_TIMER:
		if (wParam == kStatusTimer) {
			InvalidateRect(hwnd, &g_fanStatusRc, FALSE);
		}
		return 0;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		g_wnd = nullptr;
		return 0;
	case WM_NCDESTROY:
		// The window no longer draws its icons.
		for (HICON* icon : { &g_iconSmall, &g_iconBig }) {
			if (*icon) {
				DestroyIcon(*icon);
				*icon = nullptr;
			}
		}
		break;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateResources() {
	if (!g_uiFont) {
		g_uiFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
		g_headerFont = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
		g_winBrush = CreateSolidBrush(kWinBg);
		g_cardBrush = CreateSolidBrush(kCardBg);
		g_inputBrush = CreateSolidBrush(kInputBg);
	}
}

void DestroyResources() {
	for (HGDIOBJ o : { static_cast<HGDIOBJ>(g_uiFont), static_cast<HGDIOBJ>(g_headerFont), static_cast<HGDIOBJ>(g_winBrush),
			static_cast<HGDIOBJ>(g_cardBrush), static_cast<HGDIOBJ>(g_inputBrush) }) {
		if (o) {
			DeleteObject(o);
		}
	}
	g_uiFont = nullptr;
	g_headerFont = nullptr;
	g_winBrush = nullptr;
	g_cardBrush = nullptr;
	g_inputBrush = nullptr;
}

} // namespace

namespace widget {

void ShowSettingsWindow(HWND widgetWnd) {
	if (g_wnd) {
		if (IsIconic(g_wnd)) {
			ShowWindow(g_wnd, SW_RESTORE);
		}
		SetWindowPos(g_wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		SetForegroundWindow(g_wnd);
		return;
	}
	g_widget = widgetWnd;
	InitTrackbarClass();
	CreateResources();
	const HINSTANCE inst = reinterpret_cast<HINSTANCE>(&__ImageBase);
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = SettingsProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	wc.lpszClassName = kClass;
	RegisterClassExW(&wc); // fails harmlessly on the second open

	constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
	g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"Desktop Widget Settings", kStyle, CW_USEDEFAULT, CW_USEDEFAULT,
		100, 100, nullptr, nullptr, inst, nullptr);
	if (!g_wnd) {
		return;
	}
	DarkCaption(g_wnd);
	g_iconSmall = MakeWidgetIcon(GetSystemMetrics(SM_CXSMICON));
	g_iconBig = MakeWidgetIcon(GetSystemMetrics(SM_CXICON));
	SendMessageW(g_wnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_iconSmall));
	SendMessageW(g_wnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_iconBig));
	BuildControls();
	SetTimer(g_wnd, kStatusTimer, 1000, nullptr); // fan sensor status line

	RECT frame = { 0, 0, kClientW, g_clientH };
	AdjustWindowRectEx(&frame, kStyle, FALSE, WS_EX_CONTROLPARENT);
	const int w = frame.right - frame.left;
	const int h = frame.bottom - frame.top;
	// Always opens centered on the primary monitor; after that it is an
	// ordinary window that moves freely across monitors (no clamp, no snap).
	MONITORINFO mi = {};
	mi.cbSize = sizeof(mi);
	GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
	const int x = (mi.rcWork.left + mi.rcWork.right - w) / 2;
	const int y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
	// Not ShowWindow: the host is started hidden, and the process's first
	// ShowWindow on an overlapped window takes STARTUPINFO's SW_HIDE instead.
	SetWindowPos(g_wnd, HWND_TOP, x > mi.rcWork.left ? x : mi.rcWork.left, y > mi.rcWork.top ? y : mi.rcWork.top, w, h,
		SWP_SHOWWINDOW);
	// Seen once: the fresh window came up minimized (cause not found; not
	// reproducible since). Never leave a just-opened window in the taskbar.
	if (IsIconic(g_wnd)) {
		ShowWindow(g_wnd, SW_RESTORE);
	}
	SetForegroundWindow(g_wnd);
}

HWND SettingsWindow() {
	return g_wnd;
}

void SyncSettingsWindow() {
	if (g_wnd) {
		SyncControls();
	}
}

void CloseSettingsWindow() {
	if (g_wnd) {
		DestroyWindow(g_wnd);
	}
	UnregisterClassW(kClass, reinterpret_cast<HINSTANCE>(&__ImageBase));
	DestroyResources();
}

} // namespace widget
