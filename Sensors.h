#pragma once

// Hardware readings shared by the tray modules and the desktop widget.
// Everything here works without PDH (perf counters can be disabled system
// wide) and without extra static imports: iphlpapi.dll is resolved at
// runtime, the D3DKMT calls come from gdi32.dll by GetProcAddress.
//
// Every sensor is a plain struct owned by one thread; no globals.

#include <windows.h>
#include <winioctl.h>

// ---- CPU -----------------------------------------------------------------

struct CpuLoadSensor {
	ULONGLONG idle;
	ULONGLONG kernel;
	ULONGLONG user;
	bool primed;
};

// Total CPU busy % since the previous call. False on the first call.
bool CpuLoad_Sample(CpuLoadSensor& s, int& pct);

// Clock of the core running this thread, in MHz, from a timed chain of
// dependent 1-cycle instructions (~0.5 ms of work). The burst itself wakes
// the core to its boost clock, so this is the "effective max core clock"
// monitoring tools show. P-state tables are useless here: on CPPC Ryzens
// the kernel only reports 0% / 255% states.
int CpuClock_MeasureMhz();

// Package temperature (Tctl/Tdie) via the PawnIO driver; see CpuTemp.cpp.
struct CpuTempSensor {
	HANDLE device;
	HANDLE pciMutex;
	double offset;
	bool ready;
	wchar_t status[64]; // "ok" or why it is unavailable
};

// Blocking for up to several seconds: may start the PawnIO service and
// download the module. Call it off the UI thread.
bool CpuTemp_Open(CpuTempSensor& s);
bool CpuTemp_Read(CpuTempSensor& s, double& celsius);
void CpuTemp_Close(CpuTempSensor& s);

// ---- Motherboard fans (Super I/O via PawnIO; see Fans.cpp) ------------------

constexpr int kFanChannels = 7;

struct FanSensor {
	HANDLE device;
	HANDLE isaMutex;
	int family; // Fans.cpp's chip family (ITE, Nuvoton count / RPM registers)
	ULONG64 slot; // 0 = config port 0x2E, 1 = 0x4E
	USHORT base; // hardware monitor I/O base
	int count; // tachometer channels the chip has
	bool disabled[kFanChannels];
	USHORT fanReg[kFanChannels]; // Nuvoton: bank << 8 | register of the high byte
	int minValid; // Nuvoton: smallest count / RPM value that is a reading
	bool vendorCheck; // Nuvoton: verify the vendor ID on every read
	USHORT vendorReg; // Nuvoton: bank << 8 | register of the vendor ID high byte
	bool relockIoSpace; // Fans_Close puts the Nuvoton I/O space lock back
	int cpuChannel; // tachometer input wired to CPU_FAN on typical boards
	int sysChannel[2]; // ... and to the first two system fan headers
	bool ready;
	wchar_t chip[16]; // "IT8688E", "NCT6798D"
	wchar_t status[64]; // "ok" or why it is unavailable
};

struct FanReading {
	int rpm[kFanChannels]; // -1 when the channel is absent or unreadable
};

// Blocking like CpuTemp_Open; call it off the UI thread. Only reads the
// fan tachometers; the chip's fan control registers are never written.
bool Fans_Open(FanSensor& s);
// False when the bus is busy (try again) or the chip stopped answering;
// in the latter case `ready` drops and the sensor should be reopened.
bool Fans_Read(FanSensor& s, FanReading& out);
// Puts back every chip register Fans_Open changed, then closes.
void Fans_Close(FanSensor& s);

// ---- RAM -----------------------------------------------------------------

struct RamReading {
	int loadPct;
	double usedGb;
	double totalGb;
};

bool Ram_Read(RamReading& out);

// ---- GPU (D3DKMT, same source as Task Manager) ---------------------------

constexpr int kGpuMaxNodes = 64;

struct GpuSensor {
	bool ok;
	UINT adapter; // D3DKMT_HANDLE
	LUID luid;
	ULONG nodeCount;
	ULONG segmentCount;
	LONGLONG prevRunning[kGpuMaxNodes];
	LARGE_INTEGER prevQpc;
	bool primed;
	bool fanSeen; // 0 RPM means "stopped" only once the driver has reported a speed
	FARPROC queryAdapterInfo;
	FARPROC queryStatistics;
	FARPROC closeAdapter;
};

struct GpuReading {
	int loadPct; // busiest engine, -1 until primed
	int tempC; // -1 when the driver does not report it
	int clockMhz; // -1 when unknown
	int fanRpm; // -1 when unknown
	double vramUsedGb; // dedicated memory resident; -1 when unknown
	double vramTotalGb;
};

// Picks the discrete GPU: the hardware render adapter with the most
// dedicated video memory. False when there is none.
bool Gpu_Open(GpuSensor& s);
bool Gpu_Sample(GpuSensor& s, GpuReading& out);
void Gpu_Close(GpuSensor& s);

// ---- Disk (IOCTL_DISK_PERFORMANCE per PhysicalDriveN) ---------------------

constexpr int kDiskMaxDisks = 16;

struct DiskSensorDisk {
	HANDLE h; // nullptr when PhysicalDriveN is absent
	wchar_t letters[32]; // "C: D:"
	DISK_PERFORMANCE prev;
	bool havePrev;
};

struct DiskSensor {
	DiskSensorDisk disks[kDiskMaxDisks];
	ULONGLONG prevTick;
	int ticksSinceScan;
};

struct DiskLine {
	int number;
	int activePct;
	double readMbps;
	double writeMbps;
};

struct DiskReading {
	int busiest; // disk number, -1 when no disk reported
	int busiestPct;
	double readMbps; // all disks, MB/s
	double writeMbps;
	int count;
	DiskLine lines[kDiskMaxDisks];
};

void Disk_Open(DiskSensor& s);
// Call about once a second; rescans for new drives every 30 calls.
bool Disk_Sample(DiskSensor& s, DiskReading& out);
// "Disk 0 (C: D:)" or "Disk 0".
void Disk_Label(const DiskSensor& s, int number, wchar_t* out, size_t cap);
void Disk_Close(DiskSensor& s);

// ---- Network (GetIfTable2 over physical up interfaces) --------------------

struct NetSensor {
	bool ok;
	FARPROC getIfTable2;
	FARPROC freeMibTable;
	ULONG64 prevIn;
	ULONG64 prevOut;
	ULONGLONG prevTick;
	bool primed;
};

struct NetReading {
	double downBps; // bytes/s
	double upBps;
	double linkMbit;
};

bool Net_Open(NetSensor& s);
// False on the first call and when the interface table is unavailable.
bool Net_Sample(NetSensor& s, NetReading& out);

// Bytes/s in at most 4 chars: "0", "9.5K", "850K", "12M", "1.2G".
void Net_FormatRate(double bytesPerSec, wchar_t* out, size_t cap);
// Split form for large displays: number "9.5" / "850" plus unit "KB/s".
void Net_FormatRateSplit(double bytesPerSec, wchar_t* num, size_t numCap, const wchar_t** unit);
