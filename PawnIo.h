#pragma once

// PawnIO plumbing shared by the sensors that need ring-0 access (CpuTemp.cpp,
// Fans.cpp). PawnIO is the signed driver LibreHardwareMonitor and FanControl
// use; each handle runs one small signed module that only exposes the
// accesses its hardware needs.

#include <windows.h>

// A module as shipped in LibreHardwareMonitor, pinned to one commit.
struct PawnModule {
	const wchar_t* file; // under LibreHardwareMonitorLib/Resources/PawnIo/
	const char* gitSha1; // git blob hash of that file at the pinned commit
};

extern const PawnModule kPawnModuleAmdFamily17;
extern const PawnModule kPawnModuleLpcIo;

constexpr size_t kPawnStatusLen = 64;
using PawnStatus = wchar_t[kPawnStatusLen];

// Opens a PawnIO handle and loads `module` into it. When needed it first
// brings the driver up (PawnIO 2.x is a PnP driver: it creates the
// Root\PawnIO device node if there is none, or starts a stopped service)
// and downloads the module once into the app's data folder. Blocking for up to
// several seconds: call it off the UI thread. Concurrent calls are
// serialized. On failure returns nullptr and writes a short reason.
HANDLE PawnIo_Open(const PawnModule& module, PawnStatus& status);

bool PawnIo_Execute(HANDLE h, const char* fn, const ULONG64* in, size_t inCount, ULONG64* out, size_t outCount);

// System-wide mutexes that other hardware monitors (LibreHardwareMonitor,
// HWiNFO, NZXT CAM, ...) take around the same hardware.
constexpr wchar_t kPawnPciMutex[] = L"Global\\Access_PCI";
constexpr wchar_t kPawnIsaMutex[] = L"Global\\Access_ISABUS.HTP.Method";

// Created open to everyone so non-elevated tools can still take it.
HANDLE PawnIo_OpenMutex(const wchar_t* name);
// True when owned within `ms` (or when there is no mutex to take).
bool PawnIo_Lock(HANDLE mutex, DWORD ms);
void PawnIo_Unlock(HANDLE mutex);
