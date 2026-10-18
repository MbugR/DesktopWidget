#include "TrayIcon.h"

#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// NB: We resolve Shell_NotifyIconW dynamically instead of linking shell32.lib.
// A static import of shell32 breaks Verifier'd CRT initialization on some
// elevated Win11 launches (0xc0000142 during __scrt init).

#pragma comment(lib, "user32.lib")

namespace {

constexpr UINT WM_TRAY_ICON = WM_APP + 71;
constexpr UINT WM_TRAY_SET_NUMIC = WM_APP + 72;
constexpr UINT WM_TRAY_SET_TEXTIC = WM_APP + 73;
constexpr UINT WM_TRAY_ADD_SLOT = WM_APP + 74;
constexpr UINT WM_TRAY_REMOVE_SLOT = WM_APP + 75;
constexpr UINT WM_TRAY_SET_TIP = WM_APP + 76;
constexpr UINT WM_TRAY_SET_DUALIC = WM_APP + 77;
constexpr UINT WM_TRAY_SET_RATEIC = WM_APP + 78;
constexpr UINT WM_TRAY_SET_BARSIC = WM_APP + 79;
constexpr UINT ID_SHOW_BASE = 40001; // + slot
constexpr UINT ID_CLOSE_BASE = 40050; // + slot
constexpr UINT ID_EXTRA_BASE = 40100; // + slot * 100 (each slot gets 100 ids)
constexpr int kMaxSlots = 12;
constexpr int kMaxBars = 4;

struct Slot {
	bool live;
	UINT tag;
	NOTIFYICONDATAW nid;
	TrayCallbacks cb;
	wchar_t tip[128];
	wchar_t textIcon[8];
	wchar_t glyph;
	bool rateDown;
	COLORREF barColors[kMaxBars];
	int barHeights[kMaxBars];
	int barCount;
	wchar_t dualTop[8];
	wchar_t dualBottom[8];
	COLORREF dualTopColor;
	COLORREF dualBottomColor;
	COLORREF textIconColor;
};

Slot g_slots[kMaxSlots] = {};
// Guards slot allocation and host-thread lifecycle. SRWLOCK_INIT is a
// constant initializer — no dynamic init, no lazy-init race when several
// slots are started at once.
SRWLOCK g_lock = SRWLOCK_INIT;

HWND g_hostWnd = nullptr;
HANDLE g_hostThread = nullptr;
DWORD g_hostThreadId = 0;
volatile LONG g_hostRunning = 0;
wchar_t g_hostClass[64] = L"DesktopWidgetTrayHost";

using Shell_NotifyIconW_t = BOOL(WINAPI*)(DWORD, PNOTIFYICONDATAW);
Shell_NotifyIconW_t Pfn_Shell_NotifyIconW = nullptr;

bool ResolveShell() {
	if (Pfn_Shell_NotifyIconW) return true;
	HMODULE m = LoadLibraryW(L"shell32.dll");
	if (!m) return false;
	Pfn_Shell_NotifyIconW = reinterpret_cast<Shell_NotifyIconW_t>(GetProcAddress(m, "Shell_NotifyIconW"));
	return Pfn_Shell_NotifyIconW != nullptr;
}

HICON LoadHostIcon() {
	HMODULE mod = GetModuleHandleW(nullptr);
	HICON icon = LoadIconW(mod, MAKEINTRESOURCEW(1));
	if (!icon) {
		HMODULE dll = nullptr;
		if (GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&LoadHostIcon),
				&dll) && dll) {
			icon = LoadIconW(dll, MAKEINTRESOURCEW(1));
		}
	}
	if (!icon) {
		icon = LoadIconW(nullptr, IDI_APPLICATION);
	}
	return icon;
}

// Load color: green, through yellow, to red at 100 %.
COLORREF NumericIconColor(int v) {
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	int r, g;
	if (v < 50) {
		const int t = v * 255 / 50;
		r = 60 + (230 - 60) * t / 255;
		g = 200 + (190 - 200) * t / 255;
	} else {
		const int t = (v - 50) * 255 / 50;
		r = 230;
		g = 190 + (80 - 190) * t / 255;
	}
	return RGB(r, g, 80);
}

// CreateFont silently substitutes a missing face; confirm via GetTextFaceW.
// Also demands TrueType: a FontSubstitutes entry (tweakers map "Segoe UI"
// to the bitmap "MS Sans Serif") keeps the name but cannot scale.
bool FontInstalled(HDC hdc, const wchar_t* face) {
	HFONT f = CreateFontW(
		16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
	if (!f) return false;
	HGDIOBJ prev = SelectObject(hdc, f);
	wchar_t got[LF_FACESIZE] = {};
	GetTextFaceW(hdc, LF_FACESIZE, got);
	TEXTMETRICW tm = {};
	GetTextMetricsW(hdc, &tm);
	SelectObject(hdc, prev);
	DeleteObject(f);
	return lstrcmpiW(got, face) == 0 && (tm.tmPitchAndFamily & TMPF_TRUETYPE);
}

// Win11 ships "Segoe Fluent Icons", Win10 "Segoe MDL2 Assets"; both share
// codepoints for the glyphs we use. nullptr → neither installed.
const wchar_t* GlyphFontFace(HDC hdc) {
	static const wchar_t* const kFaces[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
	for (const wchar_t* face : kFaces) {
		if (FontInstalled(hdc, face)) return face;
	}
	return nullptr;
}

// ---- icon rendering ---------------------------------------------------------
// Icons are drawn at the tray's own size (16 px at 100 %) instead of being
// shrunk from 32 px, and text is fitted by its ink rather than its cell, so
// digits get the full icon height instead of about half of it. Text is
// antialiased onto a scratch bitmap and blended in with straight alpha.

// Tray icon edge in pixels at the taskbar's DPI.
int TrayIconSize() {
	UINT dpi = 0;
	if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr)) {
		dpi = GetDpiForWindow(taskbar);
	}
	if (!dpi) dpi = GetDpiForSystem();
	const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
	return size < 16 ? 16 : size > 64 ? 64 : size;
}

int Luma(COLORREF c) {
	return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

// `c` moved toward black on a light taskbar (white on a dark one) just far
// enough to stand out from it; an outline would cost the text 2 px.
COLORREF ReadableOn(COLORREF c, bool lightTaskbar) {
	// Luma difference; thin colored strokes look paler on a light background.
	const int kMinContrast = lightTaskbar ? 120 : 90;
	const int bg = lightTaskbar ? 238 : 32;
	const int to = lightTaskbar ? 0 : 255;
	for (int pct = 0; pct <= 100; pct += 5) {
		const COLORREF m = RGB(
			GetRValue(c) + (to - GetRValue(c)) * pct / 100,
			GetGValue(c) + (to - GetGValue(c)) * pct / 100,
			GetBValue(c) + (to - GetBValue(c)) * pct / 100);
		if (abs(Luma(m) - bg) >= kMinContrast) return m;
	}
	return RGB(to, to, to);
}

// Straight-alpha "over": paints `color` at coverage `a` (0..255) onto `dst`.
void BlendOver(uint32_t& dst, COLORREF color, int a) {
	if (a <= 0) return;
	if (a > 255) a = 255;
	const int da = static_cast<int>(dst >> 24);
	const int keep = da * (255 - a) / 255; // what shows through of dst
	const int oa = a + keep;
	const int r = (GetRValue(color) * a + static_cast<int>((dst >> 16) & 0xFF) * keep) / oa;
	const int g = (GetGValue(color) * a + static_cast<int>((dst >> 8) & 0xFF) * keep) / oa;
	const int b = (GetBValue(color) * a + static_cast<int>(dst & 0xFF) * keep) / oa;
	dst = (static_cast<uint32_t>(oa) << 24) | (r << 16) | (g << 8) | b;
}

// 32-bit top-down straight-alpha pixels plus the icon's (all-opaque) mask.
struct IconCanvas {
	HBITMAP color;
	HBITMAP mask;
	uint32_t* px;
	int size;
};

// Releases the canvas; builds the icon when `build` is true.
HICON CanvasEnd(IconCanvas& c, bool build) {
	HICON icon = nullptr;
	if (build && c.color && c.mask) {
		ICONINFO ii = {};
		ii.fIcon = TRUE;
		ii.hbmMask = c.mask;
		ii.hbmColor = c.color;
		icon = CreateIconIndirect(&ii);
	}
	if (c.color) DeleteObject(c.color);
	if (c.mask) DeleteObject(c.mask);
	c = {};
	return icon;
}

bool CanvasBegin(IconCanvas& c, int size) {
	c = {};
	c.size = size;
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = size;
	bi.bmiHeader.biHeight = -size;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* pixels = nullptr;
	c.color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
	static const BYTE kZeroMask[64 * 8] = {}; // 64 rows of 64 bits, word-aligned
	c.mask = CreateBitmap(size, size, 1, 1, kZeroMask);
	if (!c.color || !c.mask) {
		CanvasEnd(c, false);
		return false;
	}
	c.px = static_cast<uint32_t*>(pixels);
	memset(c.px, 0, static_cast<size_t>(size) * size * 4);
	return true;
}

// Scratch white-on-black rendering of one string. Grayscale antialiasing
// keeps the channels equal, so green alone is the glyph coverage.
struct InkMask {
	HDC dc;
	HBITMAP bm;
	HGDIOBJ oldBm;
	uint32_t* px;
	int w;
	int h;
	int origin;
};

// Fainter antialiasing fringe does not count when measuring the ink.
constexpr int kInkMin = 24;

void InkEnd(InkMask& m) {
	if (m.dc && m.oldBm) SelectObject(m.dc, m.oldBm);
	if (m.bm) DeleteObject(m.bm);
	if (m.dc) DeleteDC(m.dc);
	m = {};
}

bool InkBegin(InkMask& m, int iconSize) {
	m = {};
	// Room for a cell up to three icons tall (the fitting search tries that
	// much) and several characters wide.
	m.w = iconSize * 12;
	m.h = iconSize * 4;
	m.origin = iconSize / 2;
	m.dc = CreateCompatibleDC(nullptr);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = m.w;
	bi.bmiHeader.biHeight = -m.h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	m.bm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!m.dc || !m.bm) {
		InkEnd(m);
		return false;
	}
	m.px = static_cast<uint32_t*>(bits);
	m.oldBm = SelectObject(m.dc, m.bm);
	SetBkMode(m.dc, TRANSPARENT);
	SetTextColor(m.dc, RGB(255, 255, 255));
	SetTextAlign(m.dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
	SetGraphicsMode(m.dc, GM_ADVANCED);
	return true;
}

int InkAt(const InkMask& m, int x, int y) {
	if (x < 0 || y < 0 || x >= m.w || y >= m.h) return 0;
	return static_cast<int>((m.px[static_cast<size_t>(y) * m.w + x] >> 8) & 0xFF);
}

// A fitted font, drawn `scaleX` times its natural width.
struct InkFit {
	HFONT font;
	float scaleX;
};

// Draws `text` and returns its ink bounds: empty when there is no ink, or
// when it runs off the scratch bitmap (too big to matter).
RECT InkDraw(InkMask& m, const InkFit& fit, const wchar_t* text) {
	GdiFlush();
	memset(m.px, 0, static_cast<size_t>(m.w) * m.h * 4);
	HGDIOBJ prev = SelectObject(m.dc, fit.font);
	// A world transform narrows by any fraction; lfWidth only moves in whole
	// average character widths (about 20 % at icon sizes) and nudges the
	// hinted height as it goes.
	const XFORM xf = { fit.scaleX, 0.0f, 0.0f, 1.0f, static_cast<float>(m.origin), static_cast<float>(m.origin) };
	SetWorldTransform(m.dc, &xf);
	TextOutW(m.dc, 0, 0, text, lstrlenW(text));
	ModifyWorldTransform(m.dc, nullptr, MWT_IDENTITY);
	SelectObject(m.dc, prev);
	GdiFlush();
	RECT r = { m.w, m.h, 0, 0 };
	for (int y = 0; y < m.h; ++y) {
		for (int x = 0; x < m.w; ++x) {
			if (InkAt(m, x, y) >= kInkMin) {
				if (x < r.left) r.left = x;
				if (x >= r.right) r.right = x + 1;
				if (y < r.top) r.top = y;
				if (y >= r.bottom) r.bottom = y + 1;
			}
		}
	}
	if (r.left >= r.right || r.left == 0 || r.top == 0 || r.right == m.w || r.bottom == m.h) {
		return {};
	}
	return r;
}

struct InkFont {
	const wchar_t* face;
	int weight;
	int minWidthPct; // how far it may be narrowed (100: never)
};

// Bold strokes stay solid at 16 px; the narrower cuts keep "100" or "850K"
// tall. Bahnschrift ships with Windows 10 1709+, Tahoma and Arial with all.
constexpr InkFont kNumberFonts[] = {
	{ L"Bahnschrift Bold SemiCondensed", FW_NORMAL, 70 },
	{ L"Tahoma", FW_BOLD, 60 },
	{ L"Arial", FW_BOLD, 60 },
};
// Up to four characters ("850K", "1.2M").
constexpr InkFont kRateFonts[] = {
	{ L"Bahnschrift Bold Condensed", FW_NORMAL, 70 },
	{ L"Tahoma", FW_BOLD, 60 },
	{ L"Arial", FW_BOLD, 60 },
};

template <size_t N>
const InkFont& PickFont(HDC hdc, const InkFont (&fonts)[N]) {
	for (const InkFont& f : fonts) {
		if (FontInstalled(hdc, f.face)) return f;
	}
	return fonts[N - 1];
}

HFONT MakeInkFont(const InkFont& f, int h) {
	return CreateFontW(
		h, 0, 0, 0, f.weight, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
		ANTIALIASED_QUALITY, DEFAULT_PITCH, f.face);
}

// Ink extent of the widest and of the tallest of `texts` (empty ones are
// skipped); zero when one of them does not fit the scratch bitmap.
SIZE InkExtent(InkMask& m, const InkFit& fit, const wchar_t* const* texts, int count) {
	SIZE s = {};
	for (int i = 0; i < count; ++i) {
		if (!texts[i][0]) continue;
		const RECT r = InkDraw(m, fit, texts[i]);
		if (r.right <= r.left) return {};
		if (r.right - r.left > s.cx) s.cx = r.right - r.left;
		if (r.bottom - r.top > s.cy) s.cy = r.bottom - r.top;
	}
	return s;
}

bool InkFits(const SIZE& s, int boxW, int boxH) {
	return s.cx > 0 && s.cx <= boxW && s.cy <= boxH;
}

// Largest font of `spec` in which the ink of every text fits boxW x boxH:
// the height first; text still too wide is narrowed, down to minWidthPct
// of its natural width, before the height gives way. The caller deletes
// the font.
InkFit FitInkFont(InkMask& m, const InkFont& spec, const wchar_t* const* texts, int count, int boxW, int boxH) {
	int lo = 4;
	int hi = boxH * 3;
	int best = 0;
	while (lo <= hi) {
		const int h = (lo + hi) / 2;
		const InkFit fit = { MakeInkFont(spec, h), 1.0f };
		const SIZE s = InkExtent(m, fit, texts, count);
		DeleteObject(fit.font);
		if (s.cy > 0 && s.cy <= boxH && s.cx * spec.minWidthPct <= boxW * 100) {
			best = h;
			lo = h + 1;
		} else {
			hi = h - 1;
		}
	}
	const float minScale = spec.minWidthPct / 100.0f - 0.05f;
	for (int h = best; h >= 4; --h) {
		InkFit fit = { MakeInkFont(spec, h), 1.0f };
		const SIZE s = InkExtent(m, fit, texts, count);
		if (s.cy > 0 && s.cy <= boxH) {
			if (s.cx <= boxW) return fit;
			// Ink edges round, so a scale of exactly box / ink can still come
			// out a pixel too wide: step down a little.
			for (fit.scaleX = static_cast<float>(boxW) / s.cx; fit.scaleX >= minScale; fit.scaleX -= 0.02f) {
				if (InkFits(InkExtent(m, fit, texts, count), boxW, boxH)) return fit;
			}
		}
		DeleteObject(fit.font);
	}
	return {};
}

// Paints `text` in `color` with its ink centered on `box`.
void InkPaint(IconCanvas& c, InkMask& m, const InkFit& fit, const wchar_t* text, const RECT& box, COLORREF color) {
	if (!text[0]) return;
	const RECT ink = InkDraw(m, fit, text);
	if (ink.right <= ink.left) return;
	const int dx = box.left + (box.right - box.left - (ink.right - ink.left)) / 2 - ink.left;
	const int dy = box.top + (box.bottom - box.top - (ink.bottom - ink.top)) / 2 - ink.top;
	for (int y = 0; y < c.size; ++y) {
		for (int x = 0; x < c.size; ++x) {
			BlendOver(c.px[y * c.size + x], color, InkAt(m, x - dx, y - dy));
		}
	}
}

// Big centered 0..100 value over a load-colored progress bar. The digits
// take `color` (a category color), or the load color when CLR_INVALID.
HICON MakeNumericIcon(int value, COLORREF color, int size) {
	if (value < 0) value = 0;
	if (value > 100) value = 100;
	IconCanvas c;
	if (!CanvasBegin(c, size)) return nullptr;
	InkMask m;
	if (!InkBegin(m, size)) return CanvasEnd(c, false);
	const bool light = Tray_TaskbarLight();
	const COLORREF load = NumericIconColor(value);
	int barH = size / 8;
	if (barH < 2) barH = 2;
	wchar_t buf[8];
	wsprintfW(buf, L"%d", value);
	const wchar_t* texts[] = { buf };
	const RECT box = { 0, 0, size, size - barH - 1 };
	const InkFit fit = FitInkFont(m, PickFont(m.dc, kNumberFonts), texts, 1, box.right, box.bottom);
	if (fit.font) {
		InkPaint(c, m, fit, buf, box, ReadableOn(color == CLR_INVALID ? load : color, light));
		DeleteObject(fit.font);
	}
	InkEnd(m);
	const uint32_t fill = 0xFF000000u | (GetRValue(load) << 16) | (GetGValue(load) << 8) | GetBValue(load);
	// Faint track so the bar is visible at 0.
	const uint32_t track = light ? 0x38000000u : 0x40FFFFFFu;
	const int barFill = size * value / 100;
	for (int y = size - barH; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			c.px[y * size + x] = x < barFill ? fill : track;
		}
	}
	return CanvasEnd(c, true);
}

// Short text filling the icon. `glyph` (optional): a single icon-font
// codepoint drawn instead of `text` when an icon font is installed; `text`
// is the fallback.
HICON MakeTextIcon(const wchar_t* text, COLORREF color, int size, wchar_t glyph = 0) {
	if (!text || !text[0]) return nullptr;
	IconCanvas c;
	if (!CanvasBegin(c, size)) return nullptr;
	InkMask m;
	if (!InkBegin(m, size)) return CanvasEnd(c, false);
	InkFont spec = PickFont(m.dc, kNumberFonts);
	const wchar_t* face = glyph ? GlyphFontFace(m.dc) : nullptr;
	wchar_t glyphBuf[2] = { glyph, 0 };
	if (face) {
		text = glyphBuf;
		spec = { face, FW_NORMAL, 100 };
	}
	const wchar_t* texts[] = { text };
	const RECT box = { 0, 0, size, size };
	const InkFit fit = FitInkFont(m, spec, texts, 1, size, size);
	if (fit.font) {
		InkPaint(c, m, fit, text, box, ReadableOn(color, Tray_TaskbarLight()));
		DeleteObject(fit.font);
	}
	InkEnd(m);
	return CanvasEnd(c, fit.font != nullptr);
}

// Two stacked rows (e.g. download over upload), each in its own color, both
// at the same font size.
HICON MakeDualTextIcon(const wchar_t* top, COLORREF topColor, const wchar_t* bottom, COLORREF bottomColor, int size) {
	IconCanvas c;
	if (!CanvasBegin(c, size)) return nullptr;
	InkMask m;
	if (!InkBegin(m, size)) return CanvasEnd(c, false);
	const bool light = Tray_TaskbarLight();
	const int rowH = (size - 1) / 2;
	const RECT upper = { 0, 0, size, rowH };
	const RECT lower = { 0, size - rowH, size, size };
	const wchar_t* texts[] = { top, bottom };
	const InkFit fit = FitInkFont(m, PickFont(m.dc, kRateFonts), texts, 2, size, rowH);
	if (fit.font) {
		InkPaint(c, m, fit, top, upper, ReadableOn(topColor, light));
		InkPaint(c, m, fit, bottom, lower, ReadableOn(bottomColor, light));
		DeleteObject(fit.font);
	}
	InkEnd(m);
	return CanvasEnd(c, fit.font != nullptr);
}

// One rate (e.g. "850K") over a small arrow pointing down (download) or up.
HICON MakeRateIcon(const wchar_t* text, COLORREF color, bool down, int size) {
	if (!text || !text[0]) return nullptr;
	IconCanvas c;
	if (!CanvasBegin(c, size)) return nullptr;
	InkMask m;
	if (!InkBegin(m, size)) return CanvasEnd(c, false);
	color = ReadableOn(color, Tray_TaskbarLight());
	// Four characters make width the limit, so the arrow costs no height.
	int arrowH = size / 5;
	if (arrowH < 3) arrowH = 3;
	const wchar_t* texts[] = { text };
	const RECT box = { 0, 0, size, size - arrowH - 1 };
	const InkFit fit = FitInkFont(m, PickFont(m.dc, kRateFonts), texts, 1, box.right, box.bottom);
	if (fit.font) {
		InkPaint(c, m, fit, text, box, color);
		DeleteObject(fit.font);
	}
	InkEnd(m);
	const uint32_t px = 0xFF000000u | (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color);
	const int mid = size / 2;
	for (int i = 0; i < arrowH; ++i) {
		const int half = down ? arrowH - 1 - i : i; // row half-width: wide end away from the tip
		const int y = size - arrowH + i;
		for (int x = mid - half; x <= mid + half; ++x) {
			c.px[y * size + x] = px;
		}
	}
	return CanvasEnd(c, fit.font != nullptr);
}

// A small bar chart: `count` bars (up to kMaxBars) of `heightsPct` % of the
// icon in `colors`, standing on the bottom edge.
HICON MakeBarsIcon(const COLORREF* colors, const int* heightsPct, int count, int size) {
	if (count < 1) return nullptr;
	if (count > kMaxBars) count = kMaxBars;
	IconCanvas c;
	if (!CanvasBegin(c, size)) return nullptr;
	const bool light = Tray_TaskbarLight();
	int gap = size / 8;
	if (gap < 1) gap = 1;
	const int barW = (size - gap * (count - 1)) / count;
	const int left = (size - barW * count - gap * (count - 1)) / 2;
	for (int i = 0; i < count; ++i) {
		const COLORREF color = ReadableOn(colors[i], light);
		const uint32_t px = 0xFF000000u | (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color);
		int h = size * heightsPct[i] / 100;
		if (h < 2) h = 2;
		if (h > size) h = size;
		const int x0 = left + i * (barW + gap);
		for (int y = size - h; y < size; ++y) {
			for (int x = x0; x < x0 + barW; ++x) {
				c.px[y * size + x] = px;
			}
		}
	}
	return CanvasEnd(c, true);
}

// Given a raw uID or WM_COMMAND id, find the slot it belongs to.
int SlotFromTag(UINT tag) {
	for (int i = 0; i < kMaxSlots; ++i) {
		if (g_slots[i].live && g_slots[i].tag == tag) return i;
	}
	return -1;
}

int SlotFromCommandId(UINT id) {
	// Command ids are laid out per slot: SHOW_BASE+slot, CLOSE_BASE+slot,
	// EXTRA_BASE + slot*100 + local.
	for (int i = 0; i < kMaxSlots; ++i) {
		if (!g_slots[i].live) continue;
		if (id == ID_SHOW_BASE + static_cast<UINT>(i)) return i;
		if (id == ID_CLOSE_BASE + static_cast<UINT>(i)) return i;
		const UINT extraLo = ID_EXTRA_BASE + static_cast<UINT>(i) * 100;
		if (id >= extraLo && id < extraLo + 100) return i;
	}
	return -1;
}

void ShowContextMenu(HWND wnd, int slot) {
	if (slot < 0 || slot >= kMaxSlots || !g_slots[slot].live) return;
	HMENU menu = CreatePopupMenu();
	if (!menu) return;
	Slot& s = g_slots[slot];
	// Per-slot Extra base so multiple slots' onMenu don't collide on ids.
	int nextExtra = ID_EXTRA_BASE + slot * 100;
	if (s.cb.onShow) {
		AppendMenuW(menu, MF_STRING, ID_SHOW_BASE + slot, s.cb.showLabel ? s.cb.showLabel : L"Show");
	}
	if (s.cb.onMenu) {
		const int before = GetMenuItemCount(menu);
		s.cb.onMenu(menu, nextExtra);
		if (GetMenuItemCount(menu) > before) {
			AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		}
	}
	AppendMenuW(menu, MF_STRING, ID_CLOSE_BASE + slot, s.cb.closeLabel ? s.cb.closeLabel : L"Close");
	POINT pt = {};
	GetCursorPos(&pt);
	SetForegroundWindow(wnd);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, wnd, nullptr);
	PostMessageW(wnd, WM_NULL, 0, 0);
	DestroyMenu(menu);
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_TRAY_SET_NUMIC) {
		if (!Pfn_Shell_NotifyIconW) return 0;
		const int slot = static_cast<int>(HIWORD(wParam));
		const int v = static_cast<int>(static_cast<short>(LOWORD(wParam)));
		if (slot < 0 || slot >= kMaxSlots || !g_slots[slot].live) return 0;
		Slot& s = g_slots[slot];
		HICON newIcon = (v < 0) ? LoadHostIcon() : MakeNumericIcon(v, static_cast<COLORREF>(lParam), TrayIconSize());
		if (newIcon) {
			HICON old = s.nid.hIcon;
			s.nid.hIcon = newIcon;
			s.nid.uFlags = NIF_ICON;
			Pfn_Shell_NotifyIconW(NIM_MODIFY, &s.nid);
			if (old) DestroyIcon(old);
		}
		return 0;
	}
	if (msg == WM_TRAY_SET_TEXTIC) {
		if (!Pfn_Shell_NotifyIconW) return 0;
		const int slot = static_cast<int>(wParam);
		if (slot < 0 || slot >= kMaxSlots || !g_slots[slot].live) return 0;
		Slot& s = g_slots[slot];
		HICON newIcon = MakeTextIcon(s.textIcon, s.textIconColor, TrayIconSize(), s.glyph);
		if (newIcon) {
			HICON old = s.nid.hIcon;
			s.nid.hIcon = newIcon;
			s.nid.uFlags = NIF_ICON;
			Pfn_Shell_NotifyIconW(NIM_MODIFY, &s.nid);
			if (old) DestroyIcon(old);
		}
		return 0;
	}
	if (msg == WM_TRAY_SET_DUALIC) {
		if (!Pfn_Shell_NotifyIconW) return 0;
		const int slot = static_cast<int>(wParam);
		if (slot < 0 || slot >= kMaxSlots || !g_slots[slot].live) return 0;
		Slot& s = g_slots[slot];
		HICON newIcon = MakeDualTextIcon(s.dualTop, s.dualTopColor, s.dualBottom, s.dualBottomColor, TrayIconSize());
		if (newIcon) {
			HICON old = s.nid.hIcon;
			s.nid.hIcon = newIcon;
			s.nid.uFlags = NIF_ICON;
			Pfn_Shell_NotifyIconW(NIM_MODIFY, &s.nid);
			if (old) DestroyIcon(old);
		}
		return 0;
	}
	if (msg == WM_TRAY_SET_RATEIC || msg == WM_TRAY_SET_BARSIC) {
		if (!Pfn_Shell_NotifyIconW) return 0;
		const int slot = static_cast<int>(wParam);
		if (slot < 0 || slot >= kMaxSlots || !g_slots[slot].live) return 0;
		Slot& s = g_slots[slot];
		HICON newIcon = msg == WM_TRAY_SET_RATEIC
			? MakeRateIcon(s.textIcon, s.textIconColor, s.rateDown, TrayIconSize())
			: MakeBarsIcon(s.barColors, s.barHeights, s.barCount, TrayIconSize());
		if (newIcon) {
			HICON old = s.nid.hIcon;
			s.nid.hIcon = newIcon;
			s.nid.uFlags = NIF_ICON;
			Pfn_Shell_NotifyIconW(NIM_MODIFY, &s.nid);
			if (old) DestroyIcon(old);
		}
		return 0;
	}
	if (msg == WM_TRAY_SET_TIP) {
		if (!Pfn_Shell_NotifyIconW) return 0;
		const int slot = static_cast<int>(wParam);
		if (slot < 0 || slot >= kMaxSlots || !g_slots[slot].live) return 0;
		Slot& s = g_slots[slot];
		lstrcpynW(s.nid.szTip, s.tip, 128);
		s.nid.uFlags = NIF_TIP;
		Pfn_Shell_NotifyIconW(NIM_MODIFY, &s.nid);
		return 0;
	}
	if (msg == WM_TRAY_ADD_SLOT) {
		if (!Pfn_Shell_NotifyIconW) return 0;
		const int slot = static_cast<int>(wParam);
		if (slot < 0 || slot >= kMaxSlots || !g_slots[slot].live) return 0;
		Slot& s = g_slots[slot];
		s.nid.cbSize = sizeof(s.nid);
		s.nid.hWnd = hwnd;
		s.nid.uID = s.tag;
		// No NIF_GUID: earlier standalone-rundll32 runs may have registered
		// the same GUID from a different HWND, and Shell then refuses the
		// re-registration silently (NIM_ADD returns FALSE, tray never
		// appears). (HWND, uID) alone is a stable identity in the shared
		// host and is what the shell uses when GUID is absent.
		s.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		s.nid.uCallbackMessage = WM_TRAY_ICON;
		s.nid.hIcon = LoadHostIcon();
		lstrcpynW(s.nid.szTip, s.tip, 128);
		if (!Pfn_Shell_NotifyIconW(NIM_ADD, &s.nid)) {
			// Best-effort recover: delete any stale registration under this
			// (hwnd,uID) — if the shell had a phantom entry lingering — and
			// try again once.
			Pfn_Shell_NotifyIconW(NIM_DELETE, &s.nid);
			Pfn_Shell_NotifyIconW(NIM_ADD, &s.nid);
		}
		s.nid.uVersion = NOTIFYICON_VERSION_4;
		Pfn_Shell_NotifyIconW(NIM_SETVERSION, &s.nid);
		return 0;
	}
	if (msg == WM_TRAY_REMOVE_SLOT) {
		if (!Pfn_Shell_NotifyIconW) return 0;
		const int slot = static_cast<int>(wParam);
		if (slot < 0 || slot >= kMaxSlots) return 0;
		Slot& s = g_slots[slot];
		if (s.live) {
			Pfn_Shell_NotifyIconW(NIM_DELETE, &s.nid);
			if (s.nid.hIcon) {
				DestroyIcon(s.nid.hIcon);
				s.nid.hIcon = nullptr;
			}
			s.live = false;
		}
		return 0;
	}
	if (msg == WM_TRAY_ICON) {
		// NOTIFYICON_VERSION_4: LOWORD(lParam) = event, HIWORD(lParam) = uID,
		// wParam = anchor coordinates (not the uID).
		const int slot = SlotFromTag(HIWORD(lParam));
		if (slot < 0) return 0;
		const UINT ev = LOWORD(lParam);
		if (ev == WM_LBUTTONUP || ev == NIN_SELECT || ev == NIN_KEYSELECT) {
			if (g_slots[slot].cb.onShow) g_slots[slot].cb.onShow();
			return 0;
		}
		if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
			ShowContextMenu(hwnd, slot);
			return 0;
		}
		return 0;
	}
	if (msg == WM_COMMAND) {
		const UINT id = LOWORD(wParam);
		const int slot = SlotFromCommandId(id);
		if (slot >= 0) {
			Slot& s = g_slots[slot];
			if (id == ID_SHOW_BASE + static_cast<UINT>(slot) && s.cb.onShow) { s.cb.onShow(); return 0; }
			if (id == ID_CLOSE_BASE + static_cast<UINT>(slot)) { if (s.cb.onClose) s.cb.onClose(); return 0; }
			if (s.cb.onMenuCommand) {
				const UINT startId = ID_EXTRA_BASE + static_cast<UINT>(slot) * 100;
				s.cb.onMenuCommand(id, static_cast<int>(id - startId));
				return 0;
			}
		}
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI TrayThread(LPVOID) {
	if (!ResolveShell()) {
		InterlockedExchange(&g_hostRunning, 0);
		return 1;
	}
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = TrayProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = g_hostClass;
	RegisterClassExW(&wc);
	g_hostWnd = CreateWindowExW(
		0, wc.lpszClassName, L"DesktopWidget",
		WS_OVERLAPPED, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
	if (!g_hostWnd) {
		InterlockedExchange(&g_hostRunning, 0);
		return 1;
	}
	MSG msg = {};
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	// Clean any surviving slots on shutdown.
	for (int i = 0; i < kMaxSlots; ++i) {
		if (g_slots[i].live) {
			Pfn_Shell_NotifyIconW(NIM_DELETE, &g_slots[i].nid);
			if (g_slots[i].nid.hIcon) DestroyIcon(g_slots[i].nid.hIcon);
			g_slots[i].live = false;
		}
	}
	DestroyWindow(g_hostWnd);
	g_hostWnd = nullptr;
	InterlockedExchange(&g_hostRunning, 0);
	return 0;
}

// Caller holds g_lock exclusively.
bool EnsureHostThread() {
	if (g_hostThread && g_hostWnd && WaitForSingleObject(g_hostThread, 0) == WAIT_TIMEOUT) {
		return true;
	}
	if (g_hostThread) {
		WaitForSingleObject(g_hostThread, 2000);
		CloseHandle(g_hostThread);
		g_hostThread = nullptr;
	}
	g_hostWnd = nullptr;
	InterlockedExchange(&g_hostRunning, 1);
	g_hostThread = CreateThread(nullptr, 0, TrayThread, nullptr, 0, &g_hostThreadId);
	if (!g_hostThread) {
		InterlockedExchange(&g_hostRunning, 0);
		return false;
	}
	for (int i = 0; i < 500 && !g_hostWnd; ++i) {
		Sleep(10);
	}
	return g_hostWnd != nullptr;
}

} // namespace

TraySlot Tray_Start(const wchar_t* tipText, const wchar_t* windowText, UINT uniqueTag, bool separate, const TrayCallbacks& cb) {
	(void)windowText;
	(void)separate;
	AcquireSRWLockExclusive(&g_lock);
	if (!EnsureHostThread()) {
		ReleaseSRWLockExclusive(&g_lock);
		return kTrayInvalidSlot;
	}
	int slot = -1;
	for (int i = 0; i < kMaxSlots; ++i) {
		if (!g_slots[i].live) { slot = i; break; }
	}
	if (slot < 0) {
		ReleaseSRWLockExclusive(&g_lock);
		return kTrayInvalidSlot;
	}
	Slot& s = g_slots[slot];
	memset(&s, 0, sizeof(s));
	s.live = true;
	s.tag = uniqueTag ? uniqueTag : static_cast<UINT>(slot + 1);
	s.cb = cb;
	s.textIconColor = RGB(230, 230, 230);
	lstrcpynW(s.tip, tipText ? tipText : L"", 128);
	// The host thread never takes g_lock, so a synchronous send is safe here.
	SendMessageW(g_hostWnd, WM_TRAY_ADD_SLOT, static_cast<WPARAM>(slot), 0);
	ReleaseSRWLockExclusive(&g_lock);
	return slot;
}

void Tray_SetTip(TraySlot slot, const wchar_t* tipText) {
	if (slot < 0 || slot >= kMaxSlots || !g_hostWnd) return;
	if (!g_slots[slot].live) return;
	lstrcpynW(g_slots[slot].tip, tipText ? tipText : L"", 128);
	PostMessageW(g_hostWnd, WM_TRAY_SET_TIP, static_cast<WPARAM>(slot), 0);
}

void Tray_SetNumericIcon(TraySlot slot, int value, COLORREF color) {
	if (slot < 0 || slot >= kMaxSlots || !g_hostWnd) return;
	if (!g_slots[slot].live) return;
	// Pack slot (hi 16) + value (lo 16, signed for -1 sentinel).
	const short v16 = static_cast<short>(value);
	const WPARAM w = (static_cast<WPARAM>(slot) << 16) | static_cast<WPARAM>(static_cast<unsigned short>(v16));
	PostMessageW(g_hostWnd, WM_TRAY_SET_NUMIC, w, static_cast<LPARAM>(color));
}

bool Tray_TaskbarLight() {
	DWORD v = 0;
	DWORD cb = sizeof(v);
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
			L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb) != ERROR_SUCCESS) {
		return false;
	}
	return v != 0;
}

COLORREF Tray_SecondShade(COLORREF c) {
	// 45 % of the way to white on a dark taskbar, to black on a light one.
	const int to = Tray_TaskbarLight() ? 0 : 255;
	const int r = GetRValue(c) + (to - GetRValue(c)) * 45 / 100;
	const int g = GetGValue(c) + (to - GetGValue(c)) * 45 / 100;
	const int b = GetBValue(c) + (to - GetBValue(c)) * 45 / 100;
	return RGB(r, g, b);
}

void Tray_SetIconLabel(TraySlot, const wchar_t*) {
	// No-op — see the header note.
}

void Tray_SetTextIcon(TraySlot slot, const wchar_t* text, COLORREF color) {
	if (slot < 0 || slot >= kMaxSlots || !g_hostWnd) return;
	if (!g_slots[slot].live) return;
	lstrcpynW(g_slots[slot].textIcon, text ? text : L"", 4);
	g_slots[slot].glyph = 0;
	g_slots[slot].textIconColor = color;
	PostMessageW(g_hostWnd, WM_TRAY_SET_TEXTIC, static_cast<WPARAM>(slot), 0);
}

void Tray_SetDualTextIcon(TraySlot slot, const wchar_t* top, COLORREF topColor, const wchar_t* bottom, COLORREF bottomColor) {
	if (slot < 0 || slot >= kMaxSlots || !g_hostWnd) return;
	if (!g_slots[slot].live) return;
	Slot& s = g_slots[slot];
	lstrcpynW(s.dualTop, top ? top : L"", 8);
	lstrcpynW(s.dualBottom, bottom ? bottom : L"", 8);
	s.dualTopColor = topColor;
	s.dualBottomColor = bottomColor;
	PostMessageW(g_hostWnd, WM_TRAY_SET_DUALIC, static_cast<WPARAM>(slot), 0);
}

void Tray_SetGlyphIcon(TraySlot slot, wchar_t glyph, const wchar_t* fallbackText, COLORREF color) {
	if (slot < 0 || slot >= kMaxSlots || !g_hostWnd) return;
	if (!g_slots[slot].live) return;
	lstrcpynW(g_slots[slot].textIcon, fallbackText ? fallbackText : L"?", 4);
	g_slots[slot].glyph = glyph;
	g_slots[slot].textIconColor = color;
	PostMessageW(g_hostWnd, WM_TRAY_SET_TEXTIC, static_cast<WPARAM>(slot), 0);
}

void Tray_SetRateIcon(TraySlot slot, const wchar_t* text, COLORREF color, bool down) {
	if (slot < 0 || slot >= kMaxSlots || !g_hostWnd) return;
	if (!g_slots[slot].live) return;
	lstrcpynW(g_slots[slot].textIcon, text ? text : L"", 8);
	g_slots[slot].textIconColor = color;
	g_slots[slot].rateDown = down;
	PostMessageW(g_hostWnd, WM_TRAY_SET_RATEIC, static_cast<WPARAM>(slot), 0);
}

void Tray_SetBarsIcon(TraySlot slot, const COLORREF* colors, const int* heightsPct, int count) {
	if (slot < 0 || slot >= kMaxSlots || !g_hostWnd) return;
	if (!g_slots[slot].live || count < 1) return;
	Slot& s = g_slots[slot];
	s.barCount = count < kMaxBars ? count : kMaxBars;
	for (int i = 0; i < s.barCount; ++i) {
		s.barColors[i] = colors[i];
		s.barHeights[i] = heightsPct[i];
	}
	PostMessageW(g_hostWnd, WM_TRAY_SET_BARSIC, static_cast<WPARAM>(slot), 0);
}

HICON Tray_MakeBarsIcon(const COLORREF* colors, const int* heightsPct, int count, int size) {
	return MakeBarsIcon(colors, heightsPct, count, size);
}

bool Tray_ShouldRunSeparate() {
	// See TrayIcon.h — retained shim, always true.
	return true;
}

void Tray_Stop(TraySlot slot) {
	if (slot < 0 || slot >= kMaxSlots || !g_hostWnd) return;
	SendMessageW(g_hostWnd, WM_TRAY_REMOVE_SLOT, static_cast<WPARAM>(slot), 0);
}

void Tray_Shutdown() {
	// When several modules share the host, the last one out tears it down;
	// earlier callers must leave the other icons alone.
	AcquireSRWLockExclusive(&g_lock);
	bool anyLive = false;
	for (int i = 0; i < kMaxSlots; ++i) {
		if (g_slots[i].live) { anyLive = true; break; }
	}
	if (!anyLive && g_hostThread) {
		PostThreadMessageW(g_hostThreadId, WM_QUIT, 0, 0);
		WaitForSingleObject(g_hostThread, 2000);
		CloseHandle(g_hostThread);
		g_hostThread = nullptr;
		g_hostThreadId = 0;
	}
	ReleaseSRWLockExclusive(&g_lock);
}
