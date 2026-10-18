// PawnIO-based hardware access. Third-party attribution:
//   The signed PawnIO modules (AMDFamily17.bin, LpcIO.bin) are downloaded from
//   LibreHardwareMonitor's resources, and the driver they load is PawnIO:
//     * LibreHardwareMonitor/LibreHardwareMonitor
//         https://github.com/LibreHardwareMonitor/LibreHardwareMonitor  (MPL-2.0)
//     * PawnIO  https://pawnio.eu / https://github.com/namazso/PawnIO
//   Only the prebuilt .bin resources are fetched at runtime (verified by the
//   SHA-1s above); no upstream source is compiled into this file.

#include "PawnIo.h"
#include "HostCommon.h"

#include <windows.h>
#include <winhttp.h>
#include <setupapi.h>
#include <newdev.h>
#include <sddl.h>
#include <cstdio>
#include <cstring>

extern "C" IMAGE_DOS_HEADER __ImageBase;

const PawnModule kPawnModuleAmdFamily17 = { L"AMDFamily17.bin", "795107e2d150d54c9ff7fd53367f6a6c48ec0a71" };
const PawnModule kPawnModuleLpcIo = { L"LpcIO.bin", "d80aa0bcc73fa8ff5d787a21dfca8a1961e643d0" };

namespace {

constexpr wchar_t kPawnIoService[] = L"PawnIO";
constexpr wchar_t kPawnIoHardwareId[] = L"Root\\PawnIO";
// Class of pawnio.inf (SoftwareDevice).
constexpr GUID kSoftwareDeviceClass = { 0x62f9c741, 0xb25a, 0x46ce, { 0xb5, 0x4c, 0x9b, 0xcc, 0xce, 0x08, 0xb6, 0xf2 } };
constexpr wchar_t kPawnIoDevice[] = L"\\\\?\\GLOBALROOT\\Device\\PawnIO";
constexpr DWORD kPawnIoDeviceType = 41394;
constexpr DWORD kIoctlLoadBinary = CTL_CODE(kPawnIoDeviceType, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kIoctlExecuteFn = CTL_CODE(kPawnIoDeviceType, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr size_t kFnNameLength = 32;

constexpr wchar_t kModuleHost[] = L"raw.githubusercontent.com";
constexpr wchar_t kModuleDir[] =
	L"/LibreHardwareMonitor/LibreHardwareMonitor/d6cb2604646287da7a21e414b6539055cefb6a81"
	L"/LibreHardwareMonitorLib/Resources/PawnIo/";
constexpr DWORD kModuleMaxSize = 256 * 1024;

// One bring-up at a time: two sensors opening at once must not both create
// a device node.
SRWLOCK g_openLock = SRWLOCK_INIT;

void SetStatus(PawnStatus& status, const wchar_t* text) {
	lstrcpynW(status, text, static_cast<int>(kPawnStatusLen));
}

// ---- PawnIO service --------------------------------------------------------------

HANDLE OpenPawnIoDevice() {
	HANDLE h = CreateFileW(kPawnIoDevice, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
	return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

// The service is installed demand-start; start it and wait until the device
// shows up.
bool StartPawnIoService(PawnStatus& status) {
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!scm) {
		SetStatus(status, L"cannot open the service manager");
		return false;
	}
	SC_HANDLE svc = OpenServiceW(scm, kPawnIoService, SERVICE_QUERY_STATUS | SERVICE_START);
	if (!svc) {
		const DWORD err = GetLastError();
		SetStatus(status, err == ERROR_SERVICE_DOES_NOT_EXIST ? L"PawnIO driver is not installed"
			: err == ERROR_ACCESS_DENIED ? L"no admin rights to start PawnIO"
			: L"cannot open the PawnIO service");
		CloseServiceHandle(scm);
		return false;
	}
	bool ok = false;
	if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
		SetStatus(status, GetLastError() == ERROR_ACCESS_DENIED ? L"no admin rights to start PawnIO" : L"PawnIO service failed to start");
	} else {
		for (int i = 0; i < 50; ++i) {
			SERVICE_STATUS st = {};
			if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) {
				ok = true;
				break;
			}
			Sleep(100);
		}
		if (!ok) {
			SetStatus(status, L"PawnIO service did not start");
		}
	}
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return ok;
}

// ---- PawnIO device node -------------------------------------------------------------

// setupapi.dll / newdev.dll are loaded at runtime like every non-core DLL.
struct SetupApi {
	decltype(&SetupDiGetClassDevsW) GetClassDevs;
	decltype(&SetupDiEnumDeviceInfo) EnumDeviceInfo;
	decltype(&SetupDiGetDeviceRegistryPropertyW) GetDeviceRegistryProperty;
	decltype(&SetupDiCreateDeviceInfoList) CreateDeviceInfoList;
	decltype(&SetupDiCreateDeviceInfoW) CreateDeviceInfo;
	decltype(&SetupDiSetDeviceRegistryPropertyW) SetDeviceRegistryProperty;
	decltype(&SetupDiCallClassInstaller) CallClassInstaller;
	decltype(&SetupDiDestroyDeviceInfoList) DestroyDeviceInfoList;
	decltype(&DiInstallDevice) InstallDevice;
	decltype(&DiInstallDriverW) InstallDriver;
};

template <typename T>
bool ResolveFn(HMODULE m, const char* name, T& fn) {
	fn = reinterpret_cast<T>(GetProcAddress(m, name));
	return fn != nullptr;
}

bool LoadSetupApi(SetupApi& api) {
	HMODULE setup = LoadLibraryW(L"setupapi.dll");
	HMODULE newdev = LoadLibraryW(L"newdev.dll");
	return setup && newdev
		&& ResolveFn(setup, "SetupDiGetClassDevsW", api.GetClassDevs)
		&& ResolveFn(setup, "SetupDiEnumDeviceInfo", api.EnumDeviceInfo)
		&& ResolveFn(setup, "SetupDiGetDeviceRegistryPropertyW", api.GetDeviceRegistryProperty)
		&& ResolveFn(setup, "SetupDiCreateDeviceInfoList", api.CreateDeviceInfoList)
		&& ResolveFn(setup, "SetupDiCreateDeviceInfoW", api.CreateDeviceInfo)
		&& ResolveFn(setup, "SetupDiSetDeviceRegistryPropertyW", api.SetDeviceRegistryProperty)
		&& ResolveFn(setup, "SetupDiCallClassInstaller", api.CallClassInstaller)
		&& ResolveFn(setup, "SetupDiDestroyDeviceInfoList", api.DestroyDeviceInfoList)
		&& ResolveFn(newdev, "DiInstallDevice", api.InstallDevice)
		&& ResolveFn(newdev, "DiInstallDriverW", api.InstallDriver);
}

// True when the PawnIO kernel service is already installed (driver is in the
// store). We only need to add the bundled driver when it is not.
bool PawnIoServiceInstalled() {
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!scm) {
		return false;
	}
	SC_HANDLE svc = OpenServiceW(scm, kPawnIoService, SERVICE_QUERY_STATUS);
	const bool present = svc != nullptr;
	if (svc) {
		CloseServiceHandle(svc);
	}
	CloseServiceHandle(scm);
	return present;
}

// Locate an optional bundled PawnIO driver next to this exe
// (PawnIODriver\pawnio.inf). The app does not ship the driver, so this
// succeeds only when a PawnIODriver folder has been placed beside the exe;
// otherwise the machine must already have PawnIO installed. Returns false if
// the inf is not present.
bool FindBundledInf(wchar_t* out, size_t cap) {
	wchar_t mod[MAX_PATH] = {};
	if (GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), mod, MAX_PATH)) {
		wchar_t* slash = wcsrchr(mod, L'\\');
		if (slash) {
			*slash = 0;
			swprintf_s(out, cap, L"%s\\PawnIODriver\\pawnio.inf", mod);
			if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
				return true;
			}
		}
	}
	return false;
}

// Install the bundled, WHQL-signed PawnIO driver into the driver store so the
// device node can bind to it. Needs admin. After this succeeds the service
// exists and CreatePawnIoDevNode's DiInstallDevice will find the driver.
bool InstallPawnIoDriver(PawnStatus& status, const SetupApi& api) {
	wchar_t inf[MAX_PATH];
	if (!FindBundledInf(inf, MAX_PATH)) {
		SetStatus(status, L"PawnIO driver files are missing (PawnIODriver folder)");
		return false;
	}
	BOOL reboot = FALSE;
	if (!api.InstallDriver(nullptr, inf, DIIRFLAG_FORCE_INF, &reboot)) {
		SetStatus(status, GetLastError() == ERROR_ACCESS_DENIED
			? L"no admin rights to install the PawnIO driver"
			: L"could not install the PawnIO driver");
		return false;
	}
	return true;
}

// Any root-enumerated device (present or not) whose hardware IDs include
// Root\PawnIO.
bool PawnIoDevNodeExists(const SetupApi& api) {
	HDEVINFO set = api.GetClassDevs(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES);
	if (set == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool found = false;
	SP_DEVINFO_DATA dev = {};
	dev.cbSize = sizeof(dev);
	for (DWORD i = 0; !found && api.EnumDeviceInfo(set, i, &dev); ++i) {
		wchar_t ids[512] = {};
		if (!api.GetDeviceRegistryProperty(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
				sizeof(ids) - 2 * sizeof(wchar_t), nullptr)) {
			continue;
		}
		for (const wchar_t* id = ids; *id; id += lstrlenW(id) + 1) {
			if (lstrcmpiW(id, kPawnIoHardwareId) == 0) {
				found = true;
				break;
			}
		}
	}
	api.DestroyDeviceInfoList(set);
	return found;
}

// devcon-style install: register ROOT\PAWNIO\nnnn with the Root\PawnIO
// hardware ID, then let DiInstallDevice pick the driver from the store.
bool CreatePawnIoDevNode(PawnStatus& status, const SetupApi& api) {
	HDEVINFO set = api.CreateDeviceInfoList(&kSoftwareDeviceClass, nullptr);
	if (set == INVALID_HANDLE_VALUE) {
		SetStatus(status, L"cannot create the PawnIO device");
		return false;
	}
	SP_DEVINFO_DATA dev = {};
	dev.cbSize = sizeof(dev);
	bool registered = false;
	bool ok = false;
	static const wchar_t kIds[] = L"Root\\PawnIO\0"; // REG_MULTI_SZ
	if (api.CreateDeviceInfo(set, L"PawnIO", &kSoftwareDeviceClass, nullptr, nullptr, DICD_GENERATE_ID, &dev)
			&& api.SetDeviceRegistryProperty(set, &dev, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(kIds), sizeof(kIds))
			&& api.CallClassInstaller(DIF_REGISTERDEVICE, set, &dev)) {
		registered = true;
		BOOL reboot = FALSE;
		ok = api.InstallDevice(nullptr, set, &dev, nullptr, 0, &reboot) != FALSE;
	}
	if (!ok) {
		SetStatus(status, registered ? L"PawnIO driver is not installed" : L"no admin rights to add the PawnIO device");
		if (registered) {
			api.CallClassInstaller(DIF_REMOVE, set, &dev); // no half-installed node left behind
		}
	}
	api.DestroyDeviceInfoList(set);
	return ok;
}

// Device missing: add the device node if there is none, start the service
// if the node exists but the driver is not running, then wait for the
// device to appear.
HANDLE BringUpPawnIoDevice(PawnStatus& status) {
	SetupApi api = {};
	if (!LoadSetupApi(api)) {
		SetStatus(status, L"setupapi unavailable");
		return nullptr;
	}
	if (!PawnIoDevNodeExists(api)) {
		// On a machine where PawnIO was never installed the driver is not in the
		// store, so creating the node would just fail with "driver not installed".
		// Install the bundled, WHQL-signed driver first (needs admin), then add
		// the node so PnP can bind it.
		if (!PawnIoServiceInstalled() && !InstallPawnIoDriver(status, api)) {
			return nullptr;
		}
		if (!CreatePawnIoDevNode(status, api)) {
			return nullptr;
		}
	} else if (!StartPawnIoService(status)) {
		return nullptr;
	}
	for (int i = 0; i < 50; ++i) {
		if (HANDLE h = OpenPawnIoDevice()) {
			return h;
		}
		Sleep(100);
	}
	SetStatus(status, L"PawnIO device did not appear");
	return nullptr;
}

// ---- module file ---------------------------------------------------------------------

// git hashes a blob as SHA-1("blob <size>\0" + data); that is what the
// GitHub tree lists, so the pin can be checked without a separate digest.
bool GitBlobSha1Matches(const BYTE* data, DWORD size, const char* expectedHex) {
	HCRYPTPROV prov = 0;
	if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
		return false;
	}
	bool match = false;
	HCRYPTHASH hash = 0;
	if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
		char header[32];
		const int headerLen = sprintf_s(header, "blob %lu", static_cast<unsigned long>(size)) + 1; // with the NUL
		BYTE digest[20];
		DWORD digestLen = sizeof(digest);
		if (CryptHashData(hash, reinterpret_cast<const BYTE*>(header), headerLen, 0)
				&& CryptHashData(hash, data, size, 0)
				&& CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLen, 0) && digestLen == 20) {
			char hex[41];
			for (int i = 0; i < 20; ++i) {
				sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
			}
			match = lstrcmpiA(hex, expectedHex) == 0;
		}
		CryptDestroyHash(hash);
	}
	CryptReleaseContext(prov, 0);
	return match;
}

bool ReadWholeFile(const wchar_t* path, BYTE* buf, DWORD cap, DWORD& got) {
	got = 0;
	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return false;
	}
	const bool ok = ReadFile(f, buf, cap, &got, nullptr) && got > 0 && got < cap;
	CloseHandle(f);
	return ok;
}

bool WriteWholeFile(const wchar_t* path, const BYTE* data, DWORD size) {
	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD written = 0;
	const bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
	CloseHandle(f);
	return ok;
}

// winhttp.dll is loaded at runtime like every non-core DLL here, so the exe
// has no static dependency on it.
struct WinHttp {
	decltype(&WinHttpOpen) Open;
	decltype(&WinHttpConnect) Connect;
	decltype(&WinHttpOpenRequest) OpenRequest;
	decltype(&WinHttpSendRequest) SendRequest;
	decltype(&WinHttpReceiveResponse) ReceiveResponse;
	decltype(&WinHttpQueryHeaders) QueryHeaders;
	decltype(&WinHttpReadData) ReadData;
	decltype(&WinHttpCloseHandle) CloseHandle;
};

bool LoadWinHttp(WinHttp& api) {
	HMODULE m = LoadLibraryW(L"winhttp.dll");
	return m
		&& ResolveFn(m, "WinHttpOpen", api.Open)
		&& ResolveFn(m, "WinHttpConnect", api.Connect)
		&& ResolveFn(m, "WinHttpOpenRequest", api.OpenRequest)
		&& ResolveFn(m, "WinHttpSendRequest", api.SendRequest)
		&& ResolveFn(m, "WinHttpReceiveResponse", api.ReceiveResponse)
		&& ResolveFn(m, "WinHttpQueryHeaders", api.QueryHeaders)
		&& ResolveFn(m, "WinHttpReadData", api.ReadData)
		&& ResolveFn(m, "WinHttpCloseHandle", api.CloseHandle);
}

// HTTPS GET into `buf`. False on any network/HTTP error or oversize body.
bool HttpsGet(const wchar_t* host, const wchar_t* path, BYTE* buf, DWORD cap, DWORD& got) {
	got = 0;
	WinHttp api = {};
	if (!LoadWinHttp(api)) {
		return false;
	}
	HINTERNET session = api.Open(L"DesktopWidget/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		// Before Windows 8.1 there is no automatic proxy mode.
		session = api.Open(L"DesktopWidget/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0);
	}
	if (!session) {
		return false;
	}
	bool ok = false;
	HINTERNET conn = api.Connect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
	HINTERNET req = conn ? api.OpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	if (req && api.SendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
			&& api.ReceiveResponse(req, nullptr)) {
		DWORD status = 0;
		DWORD len = sizeof(status);
		if (api.QueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
				&status, &len, WINHTTP_NO_HEADER_INDEX) && status == 200) {
			ok = true;
			for (;;) {
				DWORD n = 0;
				if (got >= cap) {
					ok = false;
					break;
				}
				if (!api.ReadData(req, buf + got, cap - got, &n)) {
					ok = false;
					break;
				}
				if (n == 0) {
					break;
				}
				got += n;
			}
		}
	}
	if (req) api.CloseHandle(req);
	if (conn) api.CloseHandle(conn);
	api.CloseHandle(session);
	return ok && got > 0;
}

// Cached copy ("PawnIO.<file>" in the app's data folder) when it still
// matches the pin, otherwise a fresh download.
bool GetModule(const PawnModule& module, PawnStatus& status, BYTE* buf, DWORD cap, DWORD& size) {
	wchar_t dir[MAX_PATH];
	wchar_t path[MAX_PATH] = {};
	if (Host_CacheFolder(dir, MAX_PATH)) {
		swprintf_s(path, L"%sPawnIO.%s", dir, module.file);
		if (ReadWholeFile(path, buf, cap, size) && GitBlobSha1Matches(buf, size, module.gitSha1)) {
			return true;
		}
	}
	wchar_t url[512];
	swprintf_s(url, L"%s%s", kModuleDir, module.file);
	if (!HttpsGet(kModuleHost, url, buf, cap, size)) {
		SetStatus(status, L"module download failed (offline?)");
		return false;
	}
	if (!GitBlobSha1Matches(buf, size, module.gitSha1)) {
		SetStatus(status, L"downloaded module failed the hash check");
		return false;
	}
	if (path[0]) {
		WriteWholeFile(path, buf, size);
	}
	return true;
}

HANDLE OpenLocked(const PawnModule& module, PawnStatus& status) {
	HANDLE device = OpenPawnIoDevice();
	if (!device) {
		device = BringUpPawnIoDevice(status);
		if (!device) {
			return nullptr;
		}
	}
	BYTE* buf = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, kModuleMaxSize));
	DWORD size = 0;
	bool ok = buf && GetModule(module, status, buf, kModuleMaxSize, size);
	if (ok) {
		DWORD got = 0;
		ok = DeviceIoControl(device, kIoctlLoadBinary, buf, size, nullptr, 0, &got, nullptr) != FALSE;
		if (!ok) {
			SetStatus(status, L"PawnIO rejected the module");
		}
	}
	if (buf) {
		HeapFree(GetProcessHeap(), 0, buf);
	}
	if (!ok) {
		CloseHandle(device);
		return nullptr;
	}
	SetStatus(status, L"ok");
	return device;
}

} // namespace

HANDLE PawnIo_Open(const PawnModule& module, PawnStatus& status) {
	AcquireSRWLockExclusive(&g_openLock);
	HANDLE h = OpenLocked(module, status);
	ReleaseSRWLockExclusive(&g_openLock);
	return h;
}

bool PawnIo_Execute(HANDLE h, const char* fn, const ULONG64* in, size_t inCount, ULONG64* out, size_t outCount) {
	BYTE buf[kFnNameLength + 8 * sizeof(ULONG64)] = {};
	if (inCount > 8) {
		return false;
	}
	lstrcpynA(reinterpret_cast<char*>(buf), fn, static_cast<int>(kFnNameLength));
	if (inCount) {
		memcpy(buf + kFnNameLength, in, inCount * sizeof(ULONG64));
	}
	DWORD got = 0;
	return DeviceIoControl(h, kIoctlExecuteFn, buf, static_cast<DWORD>(kFnNameLength + inCount * sizeof(ULONG64)),
		out, static_cast<DWORD>(outCount * sizeof(ULONG64)), &got, nullptr) && got == outCount * sizeof(ULONG64);
}

HANDLE PawnIo_OpenMutex(const wchar_t* name) {
	SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
	PSECURITY_DESCRIPTOR sd = nullptr;
	if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr)) {
		sa.lpSecurityDescriptor = sd;
	}
	HANDLE m = CreateMutexW(&sa, FALSE, name);
	if (!m) {
		m = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
	}
	if (sd) {
		LocalFree(sd);
	}
	return m;
}

bool PawnIo_Lock(HANDLE mutex, DWORD ms) {
	if (!mutex) {
		return true;
	}
	const DWORD w = WaitForSingleObject(mutex, ms);
	return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
}

void PawnIo_Unlock(HANDLE mutex) {
	if (mutex) {
		ReleaseMutex(mutex);
	}
}
