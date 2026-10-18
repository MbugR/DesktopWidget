#include "Autostart.h"

#include <windows.h>
#include <cstdio>

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DesktopWidget";

// This exe's full path, quoted (spaces in the path are common under
// Program Files or a user profile).
bool QuotedExePath(wchar_t* out, size_t cap) {
	wchar_t path[MAX_PATH];
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return false;
	}
	return swprintf_s(out, cap, L"\"%s\"", path) > 0;
}

} // namespace

bool Autostart_IsEnabled() {
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
		return false;
	}
	wchar_t value[MAX_PATH + 2];
	DWORD cb = sizeof(value) - sizeof(wchar_t);
	DWORD type = 0;
	const LSTATUS st = RegQueryValueExW(key, kRunValue, nullptr, &type, reinterpret_cast<LPBYTE>(value), &cb);
	RegCloseKey(key);
	if (st != ERROR_SUCCESS || type != REG_SZ || cb < sizeof(wchar_t)) {
		return false;
	}
	value[cb / sizeof(wchar_t)] = 0;
	wchar_t want[MAX_PATH + 2];
	if (!QuotedExePath(want, _countof(want))) {
		return false;
	}
	// Enabled when it points at this exe; a value left by an older location
	// counts as "not enabled" so toggling on rewrites it to the current path.
	return lstrcmpiW(value, want) == 0;
}

bool Autostart_SetEnabled(bool enable) {
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
		return false;
	}
	LSTATUS st;
	if (enable) {
		wchar_t cmd[MAX_PATH + 2];
		if (!QuotedExePath(cmd, _countof(cmd))) {
			RegCloseKey(key);
			return false;
		}
		st = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd),
			static_cast<DWORD>((lstrlenW(cmd) + 1) * sizeof(wchar_t)));
	} else {
		st = RegDeleteValueW(key, kRunValue);
		if (st == ERROR_FILE_NOT_FOUND) {
			st = ERROR_SUCCESS; // already gone
		}
	}
	RegCloseKey(key);
	return st == ERROR_SUCCESS;
}
