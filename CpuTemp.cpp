// CPU package temperature through the PawnIO driver (the one
// LibreHardwareMonitor and FanControl use). User mode has no other source:
// the ACPI thermal zone is a fixed dummy on most boards.
//
// CpuTemp_Open loads the AMDFamily17 module (PawnIo_Open brings the driver
// up and downloads the module when needed). Reads then go through the
// module's ioctl_read_smn under the system-wide "Global\Access_PCI" mutex
// that other hardware monitors take as well.
//
// Only AMD family 17h-1Ah (Zen 1-5) is supported for now.

#include "Sensors.h"
#include "PawnIo.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>

namespace {

// Family 17h+ current temperature register (THM_TCON_CUR_TMP).
constexpr ULONG64 kSmnCurTemp = 0x00059800;
constexpr DWORD kTempRangeSel = 0x80000;
constexpr DWORD kTempTjSelMask = 0x30000;

bool IsAmdZen() {
	int regs[4] = {};
	__cpuid(regs, 0);
	char vendor[13] = {};
	memcpy(vendor, &regs[1], 4);
	memcpy(vendor + 4, &regs[3], 4);
	memcpy(vendor + 8, &regs[2], 4);
	if (lstrcmpA(vendor, "AuthenticAMD") != 0) {
		return false;
	}
	__cpuid(regs, 1);
	int family = (regs[0] >> 8) & 0xF;
	if (family == 0xF) {
		family += (regs[0] >> 20) & 0xFF;
	}
	return family >= 0x17 && family <= 0x1A;
}

// Tctl runs ahead of the real die temperature on a few early parts
// (Linux k10temp's table).
double TctlOffset() {
	int regs[4] = {};
	char brand[49] = {};
	for (int i = 0; i < 3; ++i) {
		__cpuid(regs, 0x80000002 + i);
		memcpy(brand + i * 16, regs, 16);
	}
	if (strstr(brand, "1600X") || strstr(brand, "1700X") || strstr(brand, "1800X")) {
		return -20.0;
	}
	if (strstr(brand, "Threadripper 19") || strstr(brand, "Threadripper 29")) {
		return -27.0;
	}
	if (strstr(brand, "2700X")) {
		return -10.0;
	}
	return 0.0;
}

} // namespace

bool CpuTemp_Open(CpuTempSensor& s) {
	s = {};
	if (!IsAmdZen()) {
		lstrcpynW(s.status, L"only AMD Ryzen is supported", static_cast<int>(kPawnStatusLen));
		return false;
	}
	s.device = PawnIo_Open(kPawnModuleAmdFamily17, s.status);
	if (!s.device) {
		return false;
	}
	s.pciMutex = PawnIo_OpenMutex(kPawnPciMutex);
	s.offset = TctlOffset();
	s.ready = true;
	return true;
}

bool CpuTemp_Read(CpuTempSensor& s, double& celsius) {
	if (!s.ready) {
		return false;
	}
	if (!PawnIo_Lock(s.pciMutex, 20)) {
		return false; // another monitor is busy on the bus; try next tick
	}
	const ULONG64 in = kSmnCurTemp;
	ULONG64 out = 0;
	const bool ok = PawnIo_Execute(s.device, "ioctl_read_smn", &in, 1, &out, 1);
	PawnIo_Unlock(s.pciMutex);
	if (!ok) {
		return false;
	}
	const DWORD raw = static_cast<DWORD>(out);
	double t = static_cast<double>(raw >> 21) * 0.125;
	if ((raw & kTempRangeSel) || (raw & kTempTjSelMask) == kTempTjSelMask) {
		t -= 49.0;
	}
	celsius = t + s.offset;
	return true;
}

void CpuTemp_Close(CpuTempSensor& s) {
	if (s.device) {
		CloseHandle(s.device);
	}
	if (s.pciMutex) {
		CloseHandle(s.pciMutex);
	}
	s = {};
}
