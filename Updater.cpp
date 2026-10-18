// Updater: manual update from the rolling GitHub release `release`. CI
// publishes three assets there: the exe, version.txt (the build number N of
// version 1.0.N) and <Name>.exe.sha256 (hex SHA-256 of the exe). The
// releases/latest/download/<asset> URLs redirect to the current files.

#include "Updater.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef APP_BUILD
#define APP_BUILD 0 // local build; CI passes /p:BuildNumber=N
#endif

namespace {

constexpr wchar_t kAppName[] = L"DesktopWidget";
constexpr wchar_t kOwner[] = L"MbugR";
constexpr wchar_t kHost[] = L"github.com";
constexpr size_t kMaxText = 4096;
constexpr size_t kMinExe = 32 * 1024;
constexpr size_t kMaxExe = 64 * 1024 * 1024;

LONG g_busy;
void (*g_requestExit)();

int Say(const wchar_t* text, UINT flags) {
	return MessageBoxW(nullptr, text, kAppName, flags | MB_SETFOREGROUND | MB_TOPMOST);
}

// GET https://github.com/<owner>/<app>/releases/latest/download/<file> into
// `out` (WinHTTP follows the redirect to the asset host). `status` is the
// final HTTP status, 0 when the request itself failed (`err` = last error).
bool Download(const wchar_t* file, size_t maxBytes, std::vector<char>& out, DWORD& status, DWORD& err) {
	out.clear();
	status = 0;
	err = 0;
	wchar_t path[256];
	swprintf_s(path, L"/%s/%s/releases/latest/download/%s", kOwner, kAppName, file);
	wchar_t agent[64];
	swprintf_s(agent, L"%s/1.0.%d", kAppName, APP_BUILD);
	HINTERNET session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		// WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1.
		session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0);
	}
	if (!session) {
		err = GetLastError();
		return false;
	}
	WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
	HINTERNET conn = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
	HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	bool ok = false;
	if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
		&& WinHttpReceiveResponse(req, nullptr)) {
		DWORD code = 0;
		DWORD len = sizeof(code);
		if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			&code, &len, WINHTTP_NO_HEADER_INDEX)) {
			status = code;
		}
		if (status == 200) {
			ok = true;
			char buf[16384];
			DWORD got = 0;
			for (;;) {
				if (!WinHttpReadData(req, buf, sizeof(buf), &got)) {
					err = GetLastError();
					ok = false;
					break;
				}
				if (got == 0) {
					break;
				}
				if (out.size() + got > maxBytes) {
					err = ERROR_FILE_TOO_LARGE;
					ok = false;
					break;
				}
				out.insert(out.end(), buf, buf + got);
			}
		}
	}
	else {
		err = GetLastError();
	}
	if (req) {
		WinHttpCloseHandle(req);
	}
	if (conn) {
		WinHttpCloseHandle(conn);
	}
	WinHttpCloseHandle(session);
	return ok;
}

// A short reason for a failed Download.
void DescribeFailure(const wchar_t* what, DWORD status, DWORD err, wchar_t* out, size_t cap) {
	if (status == 404) {
		swprintf_s(out, cap, L"Could not get %s: not found on GitHub (no release yet, or the repository is not public).", what);
	}
	else if (status != 0 && status != 200) {
		swprintf_s(out, cap, L"Could not get %s: GitHub answered HTTP %lu.", what, status);
	}
	else if (err == ERROR_FILE_TOO_LARGE) {
		swprintf_s(out, cap, L"Could not get %s: the file is too large.", what);
	}
	else {
		swprintf_s(out, cap, L"Could not get %s: network error %lu. Check the internet connection.", what, err);
	}
}

// Lowercase hex SHA-256 of `data` into out[65].
bool Sha256Hex(const std::vector<char>& data, char* out) {
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
		return false;
	}
	BCRYPT_HASH_HANDLE hash = nullptr;
	UCHAR digest[32] = {};
	bool ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))
		&& BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
			static_cast<ULONG>(data.size()), 0))
		&& BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
	if (hash) {
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(alg, 0);
	if (!ok) {
		return false;
	}
	static const char kHex[] = "0123456789abcdef";
	for (int i = 0; i < 32; ++i) {
		out[i * 2] = kHex[digest[i] >> 4];
		out[i * 2 + 1] = kHex[digest[i] & 15];
	}
	out[64] = 0;
	return true;
}

// The 64 hex digits at the start of a .sha256 file ("<hex>" or
// "<hex>  name"), lowercased into out[65]; false when there are none.
bool ParseSha(const std::vector<char>& text, char* out) {
	size_t n = 0;
	for (size_t i = 0; i < text.size() && n < 64; ++i) {
		char c = text[i];
		if (c >= 'A' && c <= 'F') {
			c = static_cast<char>(c - 'A' + 'a');
		}
		const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		if (hex) {
			out[n++] = c;
			continue;
		}
		if (n > 0) {
			return false; // digits interrupted
		}
	}
	out[n] = 0;
	return n == 64;
}

// PowerShell single-quoted literal: ' doubled.
std::wstring PsQuote(const std::wstring& s) {
	std::wstring r = L"'";
	for (wchar_t c : s) {
		r += c;
		if (c == L'\'') {
			r += c;
		}
	}
	r += L"'";
	return r;
}

// Starts the hidden helper that swaps `staged` over `exe` once this process
// has exited, then starts the new exe. CREATE_NO_WINDOW, not
// DETACHED_PROCESS: a console-less powershell dies before the swap.
bool LaunchSwapHelper(const std::wstring& staged, const std::wstring& exe, const std::wstring& dir) {
	wchar_t sys[MAX_PATH];
	const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return false;
	}
	const std::wstring ps = std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
	wchar_t pid[16];
	swprintf_s(pid, L"%lu", GetCurrentProcessId());
	// No double quotes inside: the whole script is one quoted argument.
	std::wstring script = L"Wait-Process -Id ";
	script += pid;
	script += L" -ErrorAction SilentlyContinue; $ok = $false; for ($i = 0; $i -lt 20 -and -not $ok; $i++) { try { Move-Item -LiteralPath ";
	script += PsQuote(staged);
	script += L" -Destination ";
	script += PsQuote(exe);
	script += L" -Force -ErrorAction Stop; $ok = $true } catch { Start-Sleep -Milliseconds 500 } }; Start-Process -FilePath ";
	script += PsQuote(exe);
	script += L" -WorkingDirectory ";
	script += PsQuote(dir);
	std::wstring cmd = L"\"" + ps + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + script + L"\"";
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(ps.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

// Downloads, verifies and stages the new exe, starts the swap helper and
// asks the app to exit. Reports every failure itself.
void Update() {
	std::vector<wchar_t> buf(32768);
	const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
	if (len == 0 || len >= buf.size()) {
		Say(L"Could not find this program's own file.", MB_OK | MB_ICONWARNING);
		return;
	}
	const std::wstring exe(buf.data(), len);
	const size_t slash = exe.find_last_of(L'\\');
	if (slash == std::wstring::npos) {
		Say(L"Could not find this program's own folder.", MB_OK | MB_ICONWARNING);
		return;
	}
	const std::wstring dir = exe.substr(0, slash);
	const std::wstring staged = dir + L"\\" + kAppName + L".exe.new";

	// Writable folder? Find out before downloading anything.
	HANDLE file = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		wchar_t msg[1024];
		swprintf_s(msg, L"Cannot write to the program folder:\n%s\n\nRun %s from a folder you can write to "
			L"(not Program Files) to update it, or download the new version manually.", dir.c_str(), kAppName);
		Say(msg, MB_OK | MB_ICONWARNING);
		return;
	}

	wchar_t msg[512];
	std::vector<char> exeData;
	std::vector<char> shaText;
	DWORD status = 0;
	DWORD err = 0;
	wchar_t exeName[64];
	swprintf_s(exeName, L"%s.exe", kAppName);
	wchar_t shaName[64];
	swprintf_s(shaName, L"%s.exe.sha256", kAppName);
	bool ok = Download(exeName, kMaxExe, exeData, status, err);
	if (!ok) {
		DescribeFailure(L"the new version", status, err, msg, _countof(msg));
	}
	if (ok) {
		ok = Download(shaName, kMaxText, shaText, status, err);
		if (!ok) {
			DescribeFailure(L"the checksum", status, err, msg, _countof(msg));
		}
	}
	char want[65] = {};
	char have[65] = {};
	if (ok && !ParseSha(shaText, want)) {
		ok = false;
		swprintf_s(msg, L"The checksum file on GitHub is not valid. Nothing was changed.");
	}
	if (ok && (exeData.size() < kMinExe || exeData[0] != 'M' || exeData[1] != 'Z')) {
		ok = false;
		swprintf_s(msg, L"The downloaded file is not a valid program. Nothing was changed.");
	}
	if (ok && (!Sha256Hex(exeData, have) || strcmp(want, have) != 0)) {
		ok = false;
		swprintf_s(msg, L"The downloaded file does not match its checksum. Nothing was changed.");
	}
	if (ok) {
		DWORD written = 0;
		ok = WriteFile(file, exeData.data(), static_cast<DWORD>(exeData.size()), &written, nullptr)
			&& written == exeData.size();
		if (!ok) {
			swprintf_s(msg, L"Could not save the new version (error %lu). Nothing was changed.", GetLastError());
		}
	}
	if (!CloseHandle(file) && ok) {
		ok = false;
		swprintf_s(msg, L"Could not save the new version. Nothing was changed.");
	}
	if (ok && !LaunchSwapHelper(staged, exe, dir)) {
		ok = false;
		swprintf_s(msg, L"Could not start PowerShell to install the update (error %lu). Nothing was changed.", GetLastError());
	}
	if (!ok) {
		DeleteFileW(staged.c_str());
		Say(msg, MB_OK | MB_ICONWARNING);
		return;
	}
	if (g_requestExit) {
		g_requestExit(); // the helper restarts us once we are gone
	}
}

void Check() {
	std::vector<char> body;
	DWORD status = 0;
	DWORD err = 0;
	wchar_t msg[512];
	if (!Download(L"version.txt", kMaxText, body, status, err)) {
		DescribeFailure(L"the latest version number", status, err, msg, _countof(msg));
		Say(msg, MB_OK | MB_ICONWARNING);
		return;
	}
	body.push_back(0);
	const long remote = strtol(body.data(), nullptr, 10);
	if (remote <= 0) {
		Say(L"GitHub sent an unexpected version number.", MB_OK | MB_ICONWARNING);
		return;
	}
	if (remote <= APP_BUILD) {
		swprintf_s(msg, L"You have the latest version (1.0.%d).", APP_BUILD);
		Say(msg, MB_OK | MB_ICONINFORMATION);
		return;
	}
	if constexpr (APP_BUILD == 0) {
		swprintf_s(msg, L"Version 1.0.%ld is available (you have 1.0.0, a local development build). Update now?", remote);
	}
	else {
		swprintf_s(msg, L"Version 1.0.%ld is available (you have 1.0.%d). Update now?", remote, APP_BUILD);
	}
	if (Say(msg, MB_YESNO | MB_ICONQUESTION) == IDYES) {
		Update();
	}
}

DWORD WINAPI CheckThread(LPVOID) {
	Check();
	InterlockedExchange(&g_busy, 0);
	return 0;
}

} // namespace

void Updater_CheckAsync(void (*requestExit)()) {
	if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
		return; // a check is already running
	}
	g_requestExit = requestExit;
	HANDLE thread = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
	if (!thread) {
		InterlockedExchange(&g_busy, 0);
		Say(L"Could not start the update check.", MB_OK | MB_ICONWARNING);
		return;
	}
	CloseHandle(thread);
}
