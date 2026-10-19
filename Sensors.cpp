#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <winternl.h>
#include <d3dkmthk.h>

#include "Sensors.h"

#include <cstdio>
#include <intrin.h>

namespace {

ULONGLONG FtToUll(const FILETIME& ft) {
	ULARGE_INTEGER u;
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return u.QuadPart;
}

// Two dependent 1-cycle ops per iteration (rol, add). noinline keeps the
// optimizer from folding the chain into the caller.
__declspec(noinline) unsigned __int64 ClockChain(unsigned __int64 x, int n) {
	for (int i = 0; i < n; ++i) {
		x = _rotl64(x, 1) + 0x9E3779B97F4A7C15ull;
	}
	return x;
}

LONGLONG Delta(const LARGE_INTEGER& cur, const LARGE_INTEGER& prev) {
	return cur.QuadPart > prev.QuadPart ? cur.QuadPart - prev.QuadPart : 0;
}

bool QueryDiskPerf(HANDLE h, DISK_PERFORMANCE& out) {
	DWORD got = 0;
	return DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, nullptr, 0, &out, sizeof(out), &got, nullptr)
		&& got >= sizeof(out);
}

// Volumes spanning several disks fail IOCTL_STORAGE_GET_DEVICE_NUMBER and
// are simply left unlabeled.
void FillDiskLetters(DiskSensor& s) {
	for (DiskSensorDisk& d : s.disks) {
		d.letters[0] = 0;
	}
	const DWORD mask = GetLogicalDrives();
	for (int i = 0; i < 26; ++i) {
		if (!(mask & (1u << i))) {
			continue;
		}
		wchar_t root[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', 0 };
		const UINT type = GetDriveTypeW(root);
		if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
			continue;
		}
		wchar_t path[8];
		swprintf_s(path, L"\\\\.\\%c:", root[0]);
		HANDLE v = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (v == INVALID_HANDLE_VALUE) {
			continue;
		}
		STORAGE_DEVICE_NUMBER sdn{};
		DWORD got = 0;
		if (DeviceIoControl(v, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof(sdn), &got, nullptr)
				&& sdn.DeviceType == FILE_DEVICE_DISK && sdn.DeviceNumber < kDiskMaxDisks) {
			wchar_t* l = s.disks[sdn.DeviceNumber].letters;
			const int len = lstrlenW(l);
			if (len + 4 < 32) {
				swprintf_s(l + len, 32 - len, len ? L" %c:" : L"%c:", root[0]);
			}
		}
		CloseHandle(v);
	}
}

// Opens drives that appeared since the last scan; vanished ones are closed
// by Disk_Sample when their IOCTL fails.
void RescanDisks(DiskSensor& s) {
	for (int n = 0; n < kDiskMaxDisks; ++n) {
		DiskSensorDisk& d = s.disks[n];
		if (d.h) {
			continue;
		}
		wchar_t path[32];
		swprintf_s(path, L"\\\\.\\PhysicalDrive%d", n);
		HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			continue;
		}
		d.h = h;
		d.havePrev = QueryDiskPerf(h, d.prev);
	}
	FillDiskLetters(s);
}

using QueryAdapterInfoFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
using QueryStatisticsFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
using CloseAdapterFn = NTSTATUS(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);
using EnumAdapters2Fn = NTSTATUS(APIENTRY*)(D3DKMT_ENUMADAPTERS2*);

void CloseAdapterHandle(FARPROC fn, D3DKMT_HANDLE h) {
	if (!fn || !h) {
		return;
	}
	D3DKMT_CLOSEADAPTER c = {};
	c.hAdapter = h;
	reinterpret_cast<CloseAdapterFn>(fn)(&c);
}

LONGLONG GpuNodeRunningTime(const GpuSensor& s, ULONG node) {
	D3DKMT_QUERYSTATISTICS q = {};
	q.Type = D3DKMT_QUERYSTATISTICS_NODE;
	q.AdapterLuid = s.luid;
	q.QueryNode.NodeId = node;
	if (reinterpret_cast<QueryStatisticsFn>(s.queryStatistics)(&q) != 0) {
		return 0;
	}
	return q.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;
}

using GetIfTable2Fn = decltype(&GetIfTable2);
using FreeMibTableFn = decltype(&FreeMibTable);

// Physical, up interfaces only. Filter (LWF/QoS) rows mirror the same NIC
// and would double-count; loopback and virtual adapters are skipped.
bool ReadNetTotals(const NetSensor& s, ULONG64& in, ULONG64& out, ULONG64& linkBps) {
	in = 0;
	out = 0;
	linkBps = 0;
	PMIB_IF_TABLE2 table = nullptr;
	if (reinterpret_cast<GetIfTable2Fn>(s.getIfTable2)(&table) != NO_ERROR || !table) {
		return false;
	}
	for (ULONG i = 0; i < table->NumEntries; ++i) {
		const MIB_IF_ROW2& r = table->Table[i];
		if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.OperStatus != IfOperStatusUp) {
			continue;
		}
		if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.InterfaceAndOperStatusFlags.FilterInterface) {
			continue;
		}
		in += r.InOctets;
		out += r.OutOctets;
		const ULONG64 link = r.ReceiveLinkSpeed > r.TransmitLinkSpeed ? r.ReceiveLinkSpeed : r.TransmitLinkSpeed;
		if (link > linkBps) {
			linkBps = link;
		}
	}
	reinterpret_cast<FreeMibTableFn>(s.freeMibTable)(table);
	return true;
}

} // namespace

// ---- CPU -----------------------------------------------------------------

bool CpuLoad_Sample(CpuLoadSensor& s, int& pct) {
	FILETIME idle, kernel, user;
	if (!GetSystemTimes(&idle, &kernel, &user)) {
		return false;
	}
	const ULONGLONG i = FtToUll(idle);
	const ULONGLONG k = FtToUll(kernel);
	const ULONGLONG u = FtToUll(user);
	bool ok = false;
	if (s.primed) {
		// Kernel time includes idle time.
		const ULONGLONG idleD = i - s.idle;
		const ULONGLONG totalD = (k - s.kernel) + (u - s.user);
		if (totalD > 0) {
			int v = static_cast<int>(((totalD - idleD) * 100 + totalD / 2) / totalD);
			if (v < 0) v = 0;
			if (v > 100) v = 100;
			pct = v;
			ok = true;
		}
	}
	s.idle = i;
	s.kernel = k;
	s.user = u;
	s.primed = true;
	return ok;
}

int CpuClock_MeasureMhz() {
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	constexpr int kIters = 200000;
	unsigned __int64 x = __rdtsc();
	double best = 0.0;
	// Interrupts and migrations only ever make a run look slower, so the
	// fastest of a few short runs is the real clock.
	for (int run = 0; run < 5; ++run) {
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		x = ClockChain(x, kIters);
		QueryPerformanceCounter(&t1);
		const LONGLONG ticks = t1.QuadPart - t0.QuadPart;
		if (ticks > 0) {
			const double mhz = 2.0 * kIters * static_cast<double>(freq.QuadPart) / static_cast<double>(ticks) / 1e6;
			if (mhz > best) {
				best = mhz;
			}
		}
	}
	static volatile unsigned __int64 s_sink;
	s_sink = x;
	return static_cast<int>(best + 0.5);
}

// ---- RAM -----------------------------------------------------------------

bool Ram_Read(RamReading& out) {
	MEMORYSTATUSEX ms = {};
	ms.dwLength = sizeof(ms);
	if (!GlobalMemoryStatusEx(&ms) || ms.ullTotalPhys == 0) {
		return false;
	}
	out.loadPct = static_cast<int>(ms.dwMemoryLoad);
	out.totalGb = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
	out.usedGb = static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
	return true;
}

// Both resolved at run time so the hook DLL's import list stays unchanged.
bool Commit_Read(CommitReading& out) {
	using GetPerfInfoFn = BOOL(WINAPI*)(PERFORMANCE_INFORMATION*, DWORD);
	static GetPerfInfoFn s_fn = reinterpret_cast<GetPerfInfoFn>(
		GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetPerformanceInfo"));
	if (!s_fn) {
		return false;
	}
	PERFORMANCE_INFORMATION pi = {};
	pi.cb = sizeof(pi);
	if (!s_fn(&pi, sizeof(pi))) {
		return false;
	}
	const double page = static_cast<double>(pi.PageSize);
	out.usedGb = static_cast<double>(pi.CommitTotal) * page / (1024.0 * 1024.0 * 1024.0);
	out.limitGb = static_cast<double>(pi.CommitLimit) * page / (1024.0 * 1024.0 * 1024.0);
	return true;
}

bool PageFile_ReadUsedGb(double& gb) {
	struct PageFileInfo {
		ULONG NextEntryOffset;
		ULONG TotalSize;
		ULONG TotalInUse;
		ULONG PeakUsage;
		UNICODE_STRING PageFileName;
	};
	using QuerySysInfoFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
	static QuerySysInfoFn s_fn = reinterpret_cast<QuerySysInfoFn>(
		GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
	if (!s_fn) {
		return false;
	}
	alignas(8) BYTE buf[4096];
	ULONG len = 0;
	const LONG st = s_fn(18 /* SystemPageFileInformation */, buf, sizeof(buf), &len);
	if (st < 0) {
		return false;
	}
	ULONG pageSize = 0;
	{
		using GetPerfInfoFn = BOOL(WINAPI*)(PERFORMANCE_INFORMATION*, DWORD);
		static GetPerfInfoFn s_pi = reinterpret_cast<GetPerfInfoFn>(
			GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetPerformanceInfo"));
		PERFORMANCE_INFORMATION pi = {};
		pi.cb = sizeof(pi);
		if (s_pi && s_pi(&pi, sizeof(pi))) {
			pageSize = static_cast<ULONG>(pi.PageSize);
		}
	}
	if (pageSize == 0) {
		return false;
	}
	ULONGLONG pages = 0;
	if (len >= sizeof(PageFileInfo)) {
		ULONG off = 0;
		for (;;) {
			if (off + sizeof(PageFileInfo) > len) {
				break;
			}
			const PageFileInfo* p = reinterpret_cast<const PageFileInfo*>(buf + off);
			pages += p->TotalInUse;
			if (p->NextEntryOffset == 0) {
				break;
			}
			off += p->NextEntryOffset;
		}
	}
	gb = static_cast<double>(pages) * pageSize / (1024.0 * 1024.0 * 1024.0);
	return true;
}

// ---- GPU -----------------------------------------------------------------

bool Gpu_Open(GpuSensor& s) {
	s = {};
	HMODULE gdi = LoadLibraryW(L"gdi32.dll");
	if (!gdi) {
		return false;
	}
	auto enumAdapters = reinterpret_cast<EnumAdapters2Fn>(GetProcAddress(gdi, "D3DKMTEnumAdapters2"));
	s.queryAdapterInfo = GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
	s.queryStatistics = GetProcAddress(gdi, "D3DKMTQueryStatistics");
	s.closeAdapter = GetProcAddress(gdi, "D3DKMTCloseAdapter");
	if (!enumAdapters || !s.queryAdapterInfo || !s.queryStatistics || !s.closeAdapter) {
		return false;
	}
	D3DKMT_ENUMADAPTERS2 ea = {};
	if (enumAdapters(&ea) != 0 || ea.NumAdapters == 0) {
		return false;
	}
	D3DKMT_ADAPTERINFO infos[16] = {};
	if (ea.NumAdapters > 16) {
		ea.NumAdapters = 16;
	}
	ea.pAdapters = infos;
	if (enumAdapters(&ea) != 0) {
		return false;
	}
	const auto query = reinterpret_cast<QueryAdapterInfoFn>(s.queryAdapterInfo);
	const auto queryStats = reinterpret_cast<QueryStatisticsFn>(s.queryStatistics);
	// Pick the discrete GPU: among the hardware render adapters, the one with
	// the most dedicated (non-aperture) video memory. On a machine with an
	// integrated GPU plus a discrete card (e.g. AMD iGPU + RTX), the iGPU
	// often enumerates first, so "first hardware adapter" would read the
	// wrong card's temperature, clock, load and VRAM. The same adapter is
	// then used for every reading, so they all stay consistent.
	ULONGLONG bestVram = 0;
	ULONG bestNodes = 0;
	ULONG bestSegments = 0;
	for (ULONG i = 0; i < ea.NumAdapters; ++i) {
		D3DKMT_ADAPTERTYPE type = {};
		D3DKMT_QUERYADAPTERINFO q = { infos[i].hAdapter, KMTQAITYPE_ADAPTERTYPE, &type, sizeof(type) };
		const bool hardware = query(&q) == 0 && type.RenderSupported && !type.SoftwareDevice;
		if (!hardware) {
			CloseAdapterHandle(s.closeAdapter, infos[i].hAdapter);
			continue;
		}
		// Dedicated memory and segment/node counts for this adapter.
		D3DKMT_QUERYSTATISTICS qs = {};
		qs.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
		qs.AdapterLuid = infos[i].AdapterLuid;
		ULONG nodeCount = 0;
		ULONG segmentCount = 0;
		if (queryStats(&qs) == 0) {
			nodeCount = qs.QueryResult.AdapterInformation.NodeCount;
			segmentCount = qs.QueryResult.AdapterInformation.NbSegments;
		}
		ULONGLONG dedicated = 0;
		for (ULONG seg = 0; seg < segmentCount; ++seg) {
			D3DKMT_QUERYSTATISTICS sq = {};
			sq.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
			sq.AdapterLuid = infos[i].AdapterLuid;
			sq.QuerySegment.SegmentId = seg;
			if (queryStats(&sq) != 0) {
				continue;
			}
			const D3DKMT_QUERYSTATISTICS_SEGMENT_INFORMATION& info = sq.QueryResult.SegmentInformation;
			if (!info.Aperture) {
				dedicated += info.CommitLimit;
			}
		}
		// Prefer the largest dedicated VRAM. When nothing reports VRAM yet
		// (dedicated == 0 everywhere), the first hardware adapter still wins.
		if (!s.ok || dedicated > bestVram) {
			if (s.ok) {
				CloseAdapterHandle(s.closeAdapter, s.adapter);
			}
			s.ok = true;
			s.adapter = infos[i].hAdapter;
			s.luid = infos[i].AdapterLuid;
			bestVram = dedicated;
			bestNodes = nodeCount;
			bestSegments = segmentCount;
		} else {
			CloseAdapterHandle(s.closeAdapter, infos[i].hAdapter);
		}
	}
	if (!s.ok) {
		return false;
	}
	s.nodeCount = bestNodes;
	s.segmentCount = bestSegments;
	if (s.nodeCount > kGpuMaxNodes) {
		s.nodeCount = kGpuMaxNodes;
	}
	return true;
}

bool Gpu_Sample(GpuSensor& s, GpuReading& out) {
	out.loadPct = -1;
	out.tempC = -1;
	out.clockMhz = -1;
	out.fanRpm = -1;
	out.vramUsedGb = -1.0;
	out.vramTotalGb = -1.0;
	if (!s.ok) {
		return false;
	}
	const auto query = reinterpret_cast<QueryAdapterInfoFn>(s.queryAdapterInfo);

	D3DKMT_ADAPTER_PERFDATA perf = {};
	D3DKMT_QUERYADAPTERINFO q = { s.adapter, KMTQAITYPE_ADAPTERPERFDATA, &perf, sizeof(perf) };
	if (query(&q) == 0) {
		// Temperature is in tenths of a degree C; 0 = not reported.
		if (perf.Temperature > 0) {
			out.tempC = static_cast<int>((perf.Temperature + 5) / 10);
		}
		// Zero-RPM cards stop the fan when idle; drivers without a tach
		// report 0 forever. Only trust 0 after a real speed was seen.
		if (perf.FanRPM > 0) {
			s.fanSeen = true;
		}
		if (s.fanSeen) {
			out.fanRpm = static_cast<int>(perf.FanRPM);
		}
	}

	// Engine clocks: the 3D engine (node 0 on every driver seen so far) is
	// the one that reports a frequency.
	for (ULONG n = 0; n < s.nodeCount && out.clockMhz < 0; ++n) {
		D3DKMT_NODE_PERFDATA np = {};
		np.NodeOrdinal = n;
		q = { s.adapter, KMTQAITYPE_NODEPERFDATA, &np, sizeof(np) };
		if (query(&q) == 0 && np.Frequency > 0) {
			out.clockMhz = static_cast<int>(np.Frequency / 1000000);
		}
	}

	// Dedicated memory: non-aperture segments (aperture ones map system RAM).
	ULONGLONG resident = 0;
	ULONGLONG limit = 0;
	for (ULONG seg = 0; seg < s.segmentCount; ++seg) {
		D3DKMT_QUERYSTATISTICS qs = {};
		qs.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
		qs.AdapterLuid = s.luid;
		qs.QuerySegment.SegmentId = seg;
		if (reinterpret_cast<QueryStatisticsFn>(s.queryStatistics)(&qs) != 0) {
			continue;
		}
		const D3DKMT_QUERYSTATISTICS_SEGMENT_INFORMATION& info = qs.QueryResult.SegmentInformation;
		if (info.Aperture) {
			continue;
		}
		resident += info.BytesResident;
		limit += info.CommitLimit;
	}
	if (limit > 0) {
		constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
		out.vramUsedGb = static_cast<double>(resident) / kGb;
		out.vramTotalGb = static_cast<double>(limit) / kGb;
	}

	// Load: busiest engine's running-time share of wall time (Task Manager).
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	LARGE_INTEGER qpf;
	QueryPerformanceFrequency(&qpf);
	const double wall100ns = static_cast<double>(now.QuadPart - s.prevQpc.QuadPart) * 1e7 / static_cast<double>(qpf.QuadPart);
	double best = 0.0;
	for (ULONG n = 0; n < s.nodeCount; ++n) {
		const LONGLONG run = GpuNodeRunningTime(s, n);
		if (s.primed && wall100ns > 0 && run > s.prevRunning[n]) {
			const double share = static_cast<double>(run - s.prevRunning[n]) / wall100ns;
			if (share > best) {
				best = share;
			}
		}
		s.prevRunning[n] = run;
	}
	if (s.primed) {
		int pct = static_cast<int>(best * 100.0 + 0.5);
		out.loadPct = pct > 100 ? 100 : pct;
	}
	s.prevQpc = now;
	s.primed = true;
	return true;
}

void Gpu_Close(GpuSensor& s) {
	if (s.ok) {
		CloseAdapterHandle(s.closeAdapter, s.adapter);
	}
	s = {};
}

// ---- Disk ----------------------------------------------------------------

void Disk_Open(DiskSensor& s) {
	s = {};
	RescanDisks(s);
	s.prevTick = GetTickCount64();
}

bool Disk_Sample(DiskSensor& s, DiskReading& out) {
	out.busiest = -1;
	out.busiestPct = 0;
	out.readMbps = 0.0;
	out.writeMbps = 0.0;
	out.count = 0;
	if (++s.ticksSinceScan >= 30) {
		s.ticksSinceScan = 0;
		RescanDisks(s);
	}
	const ULONGLONG tick = GetTickCount64();
	const double sec = tick > s.prevTick ? static_cast<double>(tick - s.prevTick) / 1000.0 : 1.0;
	s.prevTick = tick;
	constexpr double kMb = 1024.0 * 1024.0;
	for (int n = 0; n < kDiskMaxDisks; ++n) {
		DiskSensorDisk& d = s.disks[n];
		if (!d.h) {
			continue;
		}
		DISK_PERFORMANCE cur{};
		if (!QueryDiskPerf(d.h, cur)) {
			CloseHandle(d.h);
			d.h = nullptr;
			d.havePrev = false;
			continue;
		}
		const bool had = d.havePrev;
		const DISK_PERFORMANCE prev = d.prev;
		d.prev = cur;
		d.havePrev = true;
		// Media-less readers report a frozen QueryTime; skip them.
		const LONGLONG dQuery = had ? Delta(cur.QueryTime, prev.QueryTime) : 0;
		if (dQuery <= 0) {
			continue;
		}
		// IdleTime includes the idle stretch in progress, so it can land
		// slightly past QueryTime; clamp.
		double idle = static_cast<double>(Delta(cur.IdleTime, prev.IdleTime)) / static_cast<double>(dQuery);
		if (idle > 1.0) {
			idle = 1.0;
		}
		DiskLine& line = out.lines[out.count++];
		line.number = n;
		line.activePct = static_cast<int>((1.0 - idle) * 100.0 + 0.5);
		line.readMbps = static_cast<double>(Delta(cur.BytesRead, prev.BytesRead)) / sec / kMb;
		line.writeMbps = static_cast<double>(Delta(cur.BytesWritten, prev.BytesWritten)) / sec / kMb;
		out.readMbps += line.readMbps;
		out.writeMbps += line.writeMbps;
		if (out.busiest < 0 || line.activePct > out.busiestPct) {
			out.busiest = n;
			out.busiestPct = line.activePct;
		}
	}
	return out.busiest >= 0;
}

void Disk_Label(const DiskSensor& s, int number, wchar_t* out, size_t cap) {
	const wchar_t* letters = number >= 0 && number < kDiskMaxDisks ? s.disks[number].letters : L"";
	if (letters[0]) {
		swprintf_s(out, cap, L"Disk %d (%s)", number, letters);
	} else {
		swprintf_s(out, cap, L"Disk %d", number);
	}
}

void Disk_Close(DiskSensor& s) {
	for (DiskSensorDisk& d : s.disks) {
		if (d.h) {
			CloseHandle(d.h);
		}
	}
	s = {};
}

// ---- Network -------------------------------------------------------------

bool Net_Open(NetSensor& s) {
	s = {};
	HMODULE m = LoadLibraryW(L"iphlpapi.dll");
	if (!m) {
		return false;
	}
	s.getIfTable2 = GetProcAddress(m, "GetIfTable2");
	s.freeMibTable = GetProcAddress(m, "FreeMibTable");
	s.ok = s.getIfTable2 && s.freeMibTable;
	return s.ok;
}

bool Net_Sample(NetSensor& s, NetReading& out) {
	if (!s.ok) {
		return false;
	}
	ULONG64 in = 0;
	ULONG64 outOctets = 0;
	ULONG64 link = 0;
	const ULONGLONG tick = GetTickCount64();
	if (!ReadNetTotals(s, in, outOctets, link)) {
		s.primed = false;
		return false;
	}
	const bool had = s.primed && tick > s.prevTick;
	const double sec = had ? static_cast<double>(tick - s.prevTick) / 1000.0 : 0.0;
	if (had) {
		// An adapter going down makes the summed counters drop; treat that
		// interval as zero rather than a huge unsigned wrap.
		const ULONG64 dIn = in >= s.prevIn ? in - s.prevIn : 0;
		const ULONG64 dOut = outOctets >= s.prevOut ? outOctets - s.prevOut : 0;
		out.downBps = static_cast<double>(dIn) / sec;
		out.upBps = static_cast<double>(dOut) / sec;
		out.linkMbit = static_cast<double>(link) / 1e6;
	}
	s.prevIn = in;
	s.prevOut = outOctets;
	s.prevTick = tick;
	s.primed = true;
	return had;
}

void Net_FormatRate(double bytesPerSec, wchar_t* out, size_t cap) {
	static const wchar_t kUnits[] = { L'K', L'M', L'G' };
	double v = bytesPerSec / 1024.0;
	int unit = 0;
	while (v >= 999.5 && unit < 2) {
		v /= 1024.0;
		++unit;
	}
	if (v < 0.05) {
		lstrcpynW(out, L"0", static_cast<int>(cap));
	} else if (v < 9.95) {
		swprintf_s(out, cap, L"%.1f%c", v, kUnits[unit]);
	} else {
		swprintf_s(out, cap, L"%.0f%c", v, kUnits[unit]);
	}
}

void Net_FormatRateSplit(double bytesPerSec, wchar_t* num, size_t numCap, const wchar_t** unit) {
	static const wchar_t* const kUnits[] = { L"KB/s", L"MB/s", L"GB/s" };
	double v = bytesPerSec / 1024.0;
	int u = 0;
	while (v >= 999.5 && u < 2) {
		v /= 1024.0;
		++u;
	}
	*unit = kUnits[u];
	if (v < 0.05) {
		lstrcpynW(num, L"0", static_cast<int>(numCap));
	} else if (v < 9.95) {
		swprintf_s(num, numCap, L"%.1f", v);
	} else {
		swprintf_s(num, numCap, L"%.0f", v);
	}
}
