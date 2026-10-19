#include "Autostart.h"

#include <windows.h>
#include <taskschd.h>
#include <comdef.h>
#include <string>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

constexpr wchar_t kTaskName[] = L"DesktopWidget";
// Old per-user autostart, left by versions before the scheduled task. When
// the task is enabled we migrate off it (delete the Run value).
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DesktopWidget";

// This exe's full path (no quotes: the task action keeps path and arguments
// in separate fields).
bool ExePath(wchar_t* out, size_t cap) {
	wchar_t path[MAX_PATH];
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH || n + 1 > cap) {
		return false;
	}
	lstrcpynW(out, path, static_cast<int>(cap));
	return true;
}

// Working directory = the folder the exe lives in.
void ExeDir(const wchar_t* exe, wchar_t* out, size_t cap) {
	lstrcpynW(out, exe, static_cast<int>(cap));
	wchar_t* slash = nullptr;
	for (wchar_t* p = out; *p; ++p) {
		if (*p == L'\\') {
			slash = p;
		}
	}
	if (slash) {
		*slash = 0;
	}
}

// "DOMAIN\\user" for the current user, for the logon trigger's UserId.
// GetUserNameEx lives in secur32; load it at runtime so we do not add a link
// dependency for a single call.
bool CurrentUser(wchar_t* out, size_t cap) {
	using Fn = BOOLEAN(WINAPI*)(int, wchar_t*, DWORD*);
	HMODULE sec = LoadLibraryW(L"secur32.dll");
	if (!sec) {
		return false;
	}
	auto fn = reinterpret_cast<Fn>(GetProcAddress(sec, "GetUserNameExW"));
	const int NameSamCompatible = 2;
	DWORD n = static_cast<DWORD>(cap);
	BOOL ok = fn ? fn(NameSamCompatible, out, &n) : FALSE;
	FreeLibrary(sec);
	return ok != FALSE;
}

// RAII for COM init on this thread.
struct ComInit {
	HRESULT hr;
	ComInit() {
		hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	}
	~ComInit() {
		if (SUCCEEDED(hr)) {
			CoUninitialize();
		}
	}
	bool ok() const {
		return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
	}
};

// Connects to the Task Scheduler and opens the root "\\" folder.
bool ConnectRootFolder(ITaskService** svcOut, ITaskFolder** folderOut) {
	ITaskService* svc = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
		IID_ITaskService, reinterpret_cast<void**>(&svc));
	if (FAILED(hr)) {
		return false;
	}
	hr = svc->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
	if (FAILED(hr)) {
		svc->Release();
		return false;
	}
	ITaskFolder* folder = nullptr;
	hr = svc->GetFolder(_bstr_t(L"\\"), &folder);
	if (FAILED(hr)) {
		svc->Release();
		return false;
	}
	*svcOut = svc;
	*folderOut = folder;
	return true;
}

// The action path of the registered task, lowercased comparison with the
// current exe. Returns true and fills exePath on success.
bool GetTaskExePath(ITaskFolder* folder, std::wstring& outPath) {
	IRegisteredTask* task = nullptr;
	if (FAILED(folder->GetTask(_bstr_t(kTaskName), &task)) || !task) {
		return false;
	}
	ITaskDefinition* def = nullptr;
	bool got = false;
	if (SUCCEEDED(task->get_Definition(&def)) && def) {
		IActionCollection* actions = nullptr;
		if (SUCCEEDED(def->get_Actions(&actions)) && actions) {
			IAction* action = nullptr;
			if (SUCCEEDED(actions->get_Item(1, &action)) && action) {
				IExecAction* exec = nullptr;
				if (SUCCEEDED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec))) && exec) {
					BSTR path = nullptr;
					if (SUCCEEDED(exec->get_Path(&path)) && path) {
						outPath = path;
						got = true;
						SysFreeString(path);
					}
					exec->Release();
				}
				action->Release();
			}
			actions->Release();
		}
		def->Release();
	}
	task->Release();
	return got;
}

// Removes the legacy HKCU Run value if present (migration off the old
// per-user autostart). Ignored when absent.
void RemoveLegacyRunValue() {
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
		RegDeleteValueW(key, kRunValue);
		RegCloseKey(key);
	}
}

// Builds and registers the "DesktopWidget" task: logon trigger for the
// current user, highest privileges (so PawnIO sensors work without a UAC
// prompt at logon), interactive, no time limit, runs on battery, start when
// available. Any existing task of the same name is replaced.
bool RegisterTask(ITaskService* svc, ITaskFolder* folder, const wchar_t* exe, const wchar_t* dir, const wchar_t* user) {
	ITaskDefinition* def = nullptr;
	if (FAILED(svc->NewTask(0, &def)) || !def) {
		return false;
	}
	bool ok = false;

	// Settings.
	ITaskSettings* set = nullptr;
	if (SUCCEEDED(def->get_Settings(&set)) && set) {
		set->put_StartWhenAvailable(VARIANT_TRUE);
		set->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
		set->put_StopIfGoingOnBatteries(VARIANT_FALSE);
		set->put_ExecutionTimeLimit(_bstr_t(L"PT0S")); // no limit
		set->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
		set->Release();
	}

	// Principal: run with highest privileges, interactive token.
	IPrincipal* principal = nullptr;
	if (SUCCEEDED(def->get_Principal(&principal)) && principal) {
		principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
		principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
		if (user && *user) {
			principal->put_UserId(_bstr_t(user));
		}
		principal->Release();
	}

	// Logon trigger for this user.
	ITriggerCollection* triggers = nullptr;
	if (SUCCEEDED(def->get_Triggers(&triggers)) && triggers) {
		ITrigger* trigger = nullptr;
		if (SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) && trigger) {
			ILogonTrigger* logon = nullptr;
			if (SUCCEEDED(trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(&logon))) && logon) {
				if (user && *user) {
					logon->put_UserId(_bstr_t(user));
				}
				logon->Release();
			}
			trigger->Release();
		}
		triggers->Release();
	}

	// Action: run the exe from its own directory.
	IActionCollection* actions = nullptr;
	if (SUCCEEDED(def->get_Actions(&actions)) && actions) {
		IAction* action = nullptr;
		if (SUCCEEDED(actions->Create(TASK_ACTION_EXEC, &action)) && action) {
			IExecAction* exec = nullptr;
			if (SUCCEEDED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec))) && exec) {
				exec->put_Path(_bstr_t(exe));
				exec->put_WorkingDirectory(_bstr_t(dir));
				exec->Release();
			}
			action->Release();
		}
		actions->Release();
	}

	// Register (replace any existing task of the same name).
	IRegisteredTask* registered = nullptr;
	HRESULT hr = folder->RegisterTaskDefinition(
		_bstr_t(kTaskName), def, TASK_CREATE_OR_UPDATE,
		_variant_t(), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN,
		_variant_t(L""), &registered);
	if (SUCCEEDED(hr) && registered) {
		registered->Release();
		ok = true;
	}
	def->Release();
	return ok;
}

} // namespace

bool Autostart_IsEnabled() {
	ComInit com;
	if (!com.ok()) {
		return false;
	}
	ITaskService* svc = nullptr;
	ITaskFolder* folder = nullptr;
	if (!ConnectRootFolder(&svc, &folder)) {
		return false;
	}
	std::wstring taskExe;
	bool enabled = false;
	if (GetTaskExePath(folder, taskExe)) {
		wchar_t exe[MAX_PATH];
		if (ExePath(exe, _countof(exe))) {
			// Enabled when the task exists and its action points at this exe.
			// A stale path (exe moved) counts as "not enabled" so toggling on
			// rewrites it to the current path.
			enabled = lstrcmpiW(taskExe.c_str(), exe) == 0;
		}
	}
	folder->Release();
	svc->Release();
	return enabled;
}

bool Autostart_SetEnabled(bool enable) {
	ComInit com;
	if (!com.ok()) {
		return false;
	}
	ITaskService* svc = nullptr;
	ITaskFolder* folder = nullptr;
	if (!ConnectRootFolder(&svc, &folder)) {
		return false;
	}
	bool ok = false;
	if (enable) {
		wchar_t exe[MAX_PATH];
		wchar_t dir[MAX_PATH];
		wchar_t user[256];
		if (ExePath(exe, _countof(exe))) {
			ExeDir(exe, dir, _countof(dir));
			if (!CurrentUser(user, _countof(user))) {
				user[0] = 0;
			}
			ok = RegisterTask(svc, folder, exe, dir, user);
			if (ok) {
				RemoveLegacyRunValue(); // migrate off the old Run value
			}
		}
	} else {
		HRESULT hr = folder->DeleteTask(_bstr_t(kTaskName), 0);
		ok = SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
		RemoveLegacyRunValue(); // also clear any leftover old value
	}
	folder->Release();
	svc->Release();
	return ok;
}

bool Autostart_RefreshPath() {
	// When autostart is on but the task's action path is stale (exe moved),
	// rewrite it to the current path. No-op when disabled or already current.
	ComInit com;
	if (!com.ok()) {
		return false;
	}
	ITaskService* svc = nullptr;
	ITaskFolder* folder = nullptr;
	if (!ConnectRootFolder(&svc, &folder)) {
		return false;
	}
	std::wstring taskExe;
	bool exists = GetTaskExePath(folder, taskExe);
	folder->Release();
	svc->Release();
	if (!exists) {
		return false; // not enabled
	}
	wchar_t exe[MAX_PATH];
	if (!ExePath(exe, _countof(exe))) {
		return false;
	}
	if (lstrcmpiW(taskExe.c_str(), exe) == 0) {
		return true; // already current
	}
	return Autostart_SetEnabled(true);
}

void Autostart_StartupSync() {
	// Migration: an old HKCU Run value means a previous version used per-user
	// autostart. Recreate it as the scheduled task and drop the Run value.
	HKEY key;
	bool hadRunValue = false;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
		hadRunValue = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
		RegCloseKey(key);
	}
	if (hadRunValue) {
		Autostart_SetEnabled(true); // creates the task and removes the Run value
		return;
	}
	Autostart_RefreshPath(); // keep the task's path current if enabled
}
