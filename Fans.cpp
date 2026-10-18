// Motherboard fan speeds from the Super I/O chip, through PawnIO's LpcIO
// module (the one LibreHardwareMonitor uses; register layouts follow its
// IT87XX.cs / Nct677X.cs and Linux it87 / nct6775). Read only: the fan
// control registers are never written.
//
// Supported:
//   - ITE IT86xx/IT87xx with 16-bit tachometers at 0x2E (Gigabyte and many
//     others; tested on IT8688E);
//   - Nuvoton NCT6771F..NCT6799D / NCT610X hardware monitors (bank-select
//     register access) at 0x2E or 0x4E. The EC-type NCT668x (MSI) are not.
//
// Fans_Open, under the ISA bus mutex other monitors take too:
//   1. ITE first (87 01 55 55 on 0x2E); only when there is none, Nuvoton
//      (87 87 on 0x2E, then 0x4E). Unknown chips are left alone;
//   2. reads the hardware monitor's I/O base from its logical device, lets
//      the module find the chip's I/O ranges (only those are open to port
//      I/O afterwards), puts the selected logical device back and leaves
//      config mode;
//   3. NCT679x can have the hardware monitor I/O space locked; the lock is
//      lifted, remembered, and set again by Fans_Close.
// Reads on Nuvoton save and restore the bank select register and check the
// vendor ID each time, so a chip locked again (BIOS after sleep) shows up
// as an error instead of bogus speeds.

#include "Sensors.h"
#include "PawnIo.h"

#include <windows.h>
#include <cstdio>
#include <initializer_list>

namespace {

enum Family { kFamilyNone, kFamilyIte, kFamilyNctCount13, kFamilyNctRpm16 };
enum DetectResult { kNotFound, kFound, kUnsupported };

// Super I/O config space.
constexpr ULONG64 kConfigControlReg = 0x02;
constexpr ULONG64 kLdnReg = 0x07;
constexpr ULONG64 kChipIdReg = 0x20;
constexpr ULONG64 kChipRevisionReg = 0x21;
constexpr ULONG64 kBaseReg = 0x60;

// ITE.
constexpr ULONG64 kIteEnvironmentLdn = 0x04;
constexpr BYTE kIteConfigurationReg = 0x00;
constexpr BYTE kIteFan16BitReg = 0x0C;
constexpr BYTE kIteVendorIdReg = 0x58;
constexpr BYTE kIteTachLow[6] = { 0x0D, 0x0E, 0x0F, 0x80, 0x82, 0x4C };
constexpr BYTE kIteTachHigh[6] = { 0x18, 0x19, 0x1A, 0x81, 0x83, 0x4D };

// Nuvoton.
constexpr ULONG64 kNctHardwareMonitorLdn = 0x0B;
constexpr ULONG64 kNctIoSpaceLockReg = 0x28;
constexpr BYTE kNctIoSpaceLockBit = 0x10;
constexpr BYTE kNctBankSelectReg = 0x4E;
constexpr USHORT kNctVendorId = 0x5CA3;
constexpr int kNctMaxCount13 = 0x1FFF;

struct IteChip {
	USHORT id;
	const wchar_t* name;
	int fans;
};

// Chips with 16-bit fan counters and the standard tachometer registers.
constexpr IteChip kIteChips[] = {
	{ 0x8613, L"IT8613E", 5 },
	{ 0x8620, L"IT8620E", 5 },
	{ 0x8628, L"IT8628E", 6 },
	{ 0x8631, L"IT8631E", 2 },
	{ 0x8638, L"IT8638E", 2 },
	{ 0x8655, L"IT8655E", 3 },
	{ 0x8686, L"IT8686E", 6 },
	{ 0x8688, L"IT8688E", 6 },
	{ 0x8696, L"IT8696E", 6 },
	{ 0x8721, L"IT8721F", 5 },
	{ 0x8728, L"IT8728F", 5 },
	{ 0x8771, L"IT8771E", 5 },
	{ 0x8772, L"IT8772E", 5 },
};

enum NctLayout {
	kNctCount13, // 13-bit counts at 0x4B0 + 2i (0x4CC for the 7th)
	kNctRpm656, // 16-bit RPM at 0x656 + 2i
	kNctRpm030, // 16-bit RPM at 0x030 + 2i
};

struct NctChip {
	BYTE id;
	BYTE revision;
	BYTE revisionMask;
	const wchar_t* name;
	int fans;
	NctLayout layout;
	int minValid; // counts: smallest valid count; RPM: highest RPM that means "stopped"
	bool ioSpaceLock; // NCT679x-class: hardware monitor I/O space can be locked
	bool vendorCheck; // NCT6701D does not answer with the vendor ID
	USHORT vendorReg;
};

constexpr NctChip kNctChips[] = {
	{ 0xB4, 0x70, 0xF0, L"NCT6771F", 4, kNctRpm656, 20, false, true, 0x804F },
	{ 0xC3, 0x30, 0xF0, L"NCT6776F", 5, kNctRpm656, 164, false, true, 0x804F },
	{ 0xC4, 0x50, 0xF0, L"NCT610XD", 3, kNctRpm030, 164, false, true, 0x80FE },
	{ 0xC5, 0x60, 0xF0, L"NCT6779D", 5, kNctCount13, 0x15, false, true, 0x804F },
	{ 0xC8, 0x03, 0xFF, L"NCT6791D", 6, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xC9, 0x11, 0xFF, L"NCT6792D", 6, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xC9, 0x13, 0xFF, L"NCT6792DA", 6, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xD1, 0x21, 0xFF, L"NCT6793D", 6, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xD3, 0x52, 0xFF, L"NCT6795D", 6, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xD4, 0x23, 0xFF, L"NCT6796D", 6, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xD4, 0x2A, 0xFF, L"NCT6796D-R", 7, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xD4, 0x51, 0xFF, L"NCT6797D", 7, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xD4, 0x2B, 0xFF, L"NCT6798D", 7, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xD8, 0x02, 0xFF, L"NCT6799D", 7, kNctCount13, 0x15, true, true, 0x804F },
	{ 0xD8, 0x06, 0xFF, L"NCT6701D", 7, kNctCount13, 0x15, true, false, 0x804F },
};

// Recognized but not handled: their hardware monitor sits behind an EC.
struct NamedChip {
	BYTE id;
	BYTE revision;
	const wchar_t* name;
};

constexpr NamedChip kNctEcChips[] = {
	{ 0xC7, 0x32, L"NCT6683D" },
	{ 0xD4, 0x40, L"NCT6686D" },
	{ 0xD4, 0x41, L"NCT6686D" },
	{ 0xD5, 0x92, L"NCT6687D" },
};

void SetStatus(FanSensor& s, const wchar_t* text) {
	lstrcpynW(s.status, text, static_cast<int>(sizeof(s.status) / sizeof(s.status[0])));
}

ULONG64 ConfigPort(const FanSensor& s) {
	return s.slot == 0 ? 0x2E : 0x4E;
}

// ---- module calls --------------------------------------------------------------

bool SelectSlot(FanSensor& s, ULONG64 slot) {
	s.slot = slot;
	return PawnIo_Execute(s.device, "ioctl_select_slot", &slot, 1, nullptr, 0);
}

bool PortOut(const FanSensor& s, ULONG64 port, ULONG64 value) {
	const ULONG64 in[2] = { port, value };
	return PawnIo_Execute(s.device, "ioctl_pio_outb", in, 2, nullptr, 0);
}

bool PortIn(const FanSensor& s, ULONG64 port, BYTE& value) {
	ULONG64 out = 0;
	if (!PawnIo_Execute(s.device, "ioctl_pio_inb", &port, 1, &out, 1)) {
		return false;
	}
	value = static_cast<BYTE>(out);
	return true;
}

bool ConfigOut(const FanSensor& s, ULONG64 reg, ULONG64 value) {
	const ULONG64 in[2] = { reg, value };
	return PawnIo_Execute(s.device, "ioctl_superio_outb", in, 2, nullptr, 0);
}

bool ConfigIn(const FanSensor& s, ULONG64 reg, BYTE& value) {
	ULONG64 out = 0;
	if (!PawnIo_Execute(s.device, "ioctl_superio_inb", &reg, 1, &out, 1)) {
		return false;
	}
	value = static_cast<BYTE>(out);
	return true;
}

bool ConfigInWord(const FanSensor& s, ULONG64 reg, USHORT& value) {
	ULONG64 out = 0;
	if (!PawnIo_Execute(s.device, "ioctl_superio_inw", &reg, 1, &out, 1)) {
		return false;
	}
	value = static_cast<USHORT>(out);
	return true;
}

// Base address of logical device `ldn`, read twice (a half-updated value
// differs). Lets the module whitelist the chip's I/O ranges first and puts
// the originally selected logical device back afterwards. In config mode.
bool ReadDeviceBase(FanSensor& s, ULONG64 ldn, USHORT& base) {
	BYTE originalLdn = 0;
	const bool haveLdn = ConfigIn(s, kLdnReg, originalLdn);
	USHORT verify = 0;
	bool ok = PawnIo_Execute(s.device, "ioctl_find_bars", nullptr, 0, nullptr, 0)
		&& ConfigOut(s, kLdnReg, ldn)
		&& ConfigInWord(s, kBaseReg, base);
	Sleep(1);
	ok = ok && ConfigInWord(s, kBaseReg, verify) && base == verify;
	if (haveLdn) {
		ConfigOut(s, kLdnReg, originalLdn);
	}
	return ok;
}

bool ValidBase(USHORT base) {
	return base >= 0x100 && (base & 0xF007) == 0;
}

// ---- ITE ---------------------------------------------------------------------------

bool IteRead(const FanSensor& s, BYTE reg, BYTE& value) {
	return PortOut(s, s.base + 5u, reg) && PortIn(s, s.base + 6u, value);
}

DetectResult DetectIte(FanSensor& s) {
	if (!SelectSlot(s, 0)) {
		SetStatus(s, L"LpcIO module failed");
		return kUnsupported;
	}
	for (ULONG64 b : { 0x87, 0x01, 0x55, 0x55 }) {
		if (!PortOut(s, 0x2E, b)) {
			SetStatus(s, L"Super I/O port access denied");
			return kUnsupported;
		}
	}
	USHORT id = 0;
	if (!ConfigInWord(s, kChipIdReg, id) || id == 0 || id == 0xFFFF) {
		return kNotFound;
	}
	const IteChip* chip = nullptr;
	for (const IteChip& c : kIteChips) {
		if (c.id == id) {
			chip = &c;
			break;
		}
	}
	if (!chip) {
		if ((id >> 8) != 0x86 && (id >> 8) != 0x87) {
			return kNotFound; // someone else's chip in config mode; the ITE sequence did not enter it
		}
		ConfigOut(s, kConfigControlReg, 0x02);
		swprintf_s(s.status, L"unsupported ITE chip IT%04X", id);
		return kUnsupported;
	}
	USHORT base = 0;
	const bool ok = ReadDeviceBase(s, kIteEnvironmentLdn, base);
	ConfigOut(s, kConfigControlReg, 0x02); // leave config mode
	if (!ok || !ValidBase(base)) {
		SetStatus(s, L"Super I/O reports no valid fan controller");
		return kUnsupported;
	}
	s.family = kFamilyIte;
	s.base = base;
	lstrcpynW(s.chip, chip->name, 16);
	s.count = chip->fans;
	s.cpuChannel = 0;
	s.sysChannel[0] = 1;
	s.sysChannel[1] = 2;

	BYTE vendor = 0;
	BYTE config = 0;
	if (!IteRead(s, kIteVendorIdReg, vendor) || (vendor != 0x90 && vendor != 0x7F)
			|| !IteRead(s, kIteConfigurationReg, config) || ((config & 0x10) == 0 && id != 0x8655)) {
		SetStatus(s, L"fan controller did not answer");
		return kUnsupported;
	}
	// Inputs 4-6 can be switched off (pins used for something else).
	BYTE modes = 0;
	if (IteRead(s, kIteFan16BitReg, modes)) {
		if (s.count >= 5) {
			s.disabled[3] = (modes & 0x10) == 0;
			s.disabled[4] = (modes & 0x20) == 0;
		}
		if (s.count >= 6) {
			s.disabled[5] = (modes & 0x04) == 0;
		}
	}
	return kFound;
}

bool IteReadFans(FanSensor& s, FanReading& out) {
	for (int i = 0; i < s.count && i < 6; ++i) {
		if (s.disabled[i]) {
			continue;
		}
		BYTE lo = 0;
		BYTE hi = 0;
		if (!IteRead(s, kIteTachLow[i], lo) || !IteRead(s, kIteTachHigh[i], hi)) {
			continue;
		}
		const int count = lo | (hi << 8);
		// 0xFFFF = stopped; tiny counts are noise from an unconnected input.
		if (count > 0x3F) {
			out.rpm[i] = count < 0xFFFF ? 1350000 / (count * 2) : 0;
		}
	}
	return true;
}

// ---- Nuvoton -------------------------------------------------------------------------

// Hardware monitor register `addr` (bank << 8 | index) via the bank select
// register. Callers save and restore the bank around a batch.
bool NctRead(const FanSensor& s, USHORT addr, BYTE& value) {
	return PortOut(s, s.base + 5u, kNctBankSelectReg) && PortOut(s, s.base + 6u, addr >> 8)
		&& PortOut(s, s.base + 5u, addr & 0xFF) && PortIn(s, s.base + 6u, value);
}

bool NctSaveBank(const FanSensor& s, BYTE& bank) {
	return PortOut(s, s.base + 5u, kNctBankSelectReg) && PortIn(s, s.base + 6u, bank);
}

void NctRestoreBank(const FanSensor& s, BYTE bank) {
	PortOut(s, s.base + 5u, kNctBankSelectReg);
	PortOut(s, s.base + 6u, bank);
}

bool NctVendorOk(const FanSensor& s) {
	BYTE hi = 0;
	BYTE lo = 0;
	return NctRead(s, s.vendorReg, hi) && NctRead(s, static_cast<USHORT>(s.vendorReg & 0xFF), lo)
		&& ((hi << 8) | lo) == kNctVendorId;
}

bool NctEnter(const FanSensor& s) {
	const ULONG64 port = ConfigPort(s);
	return PortOut(s, port, 0x87) && PortOut(s, port, 0x87);
}

void NctExit(const FanSensor& s) {
	PortOut(s, ConfigPort(s), 0xAA);
}

// Clears (lock = false) or sets the hardware monitor I/O space lock. In
// config mode; the selected logical device is put back.
bool NctSetIoSpaceLock(const FanSensor& s, bool lock, bool* wasLocked) {
	BYTE originalLdn = 0;
	const bool haveLdn = ConfigIn(s, kLdnReg, originalLdn);
	BYTE v = 0;
	bool ok = ConfigOut(s, kLdnReg, kNctHardwareMonitorLdn) && ConfigIn(s, kNctIoSpaceLockReg, v);
	if (ok) {
		if (wasLocked) {
			*wasLocked = (v & kNctIoSpaceLockBit) != 0;
		}
		const BYTE want = lock ? (v | kNctIoSpaceLockBit) : (v & ~kNctIoSpaceLockBit);
		if (want != v) {
			ok = ConfigOut(s, kNctIoSpaceLockReg, want);
		}
	}
	if (haveLdn) {
		ConfigOut(s, kLdnReg, originalLdn);
	}
	return ok;
}

DetectResult DetectNuvoton(FanSensor& s, ULONG64 slot) {
	if (!SelectSlot(s, slot) || !NctEnter(s)) {
		return kNotFound;
	}
	BYTE id = 0;
	BYTE revision = 0;
	if (!ConfigIn(s, kChipIdReg, id) || !ConfigIn(s, kChipRevisionReg, revision) || id == 0 || id == 0xFF) {
		return kNotFound; // nothing entered config mode
	}
	const NctChip* chip = nullptr;
	for (const NctChip& c : kNctChips) {
		if (c.id == id && (revision & c.revisionMask) == c.revision) {
			chip = &c;
			break;
		}
	}
	if (!chip) {
		NctExit(s);
		const wchar_t* name = nullptr;
		for (const NamedChip& c : kNctEcChips) {
			if (c.id == id && c.revision == revision) {
				name = c.name;
			}
		}
		if (name) {
			swprintf_s(s.status, L"%s (EC type) is not supported yet", name);
		} else {
			swprintf_s(s.status, L"unsupported Super I/O chip %02X%02X", id, revision);
		}
		return kUnsupported;
	}
	USHORT base = 0;
	bool ok = ReadDeviceBase(s, kNctHardwareMonitorLdn, base);
	if (ok && chip->ioSpaceLock) {
		bool wasLocked = false;
		ok = NctSetIoSpaceLock(s, false, &wasLocked);
		s.relockIoSpace = ok && wasLocked;
	}
	NctExit(s);
	if (!ok || !ValidBase(base)) {
		SetStatus(s, L"Super I/O reports no valid fan controller");
		return kUnsupported;
	}
	s.family = chip->layout == kNctCount13 ? kFamilyNctCount13 : kFamilyNctRpm16;
	s.base = base;
	lstrcpynW(s.chip, chip->name, 16);
	s.count = chip->fans;
	s.minValid = chip->minValid;
	s.vendorCheck = chip->vendorCheck;
	s.vendorReg = chip->vendorReg;
	for (int i = 0; i < s.count && i < kFanChannels; ++i) {
		switch (chip->layout) {
		case kNctCount13:
			s.fanReg[i] = static_cast<USHORT>(i < 6 ? 0x4B0 + 2 * i : 0x4CC);
			break;
		case kNctRpm656:
			s.fanReg[i] = static_cast<USHORT>(0x656 + 2 * i);
			break;
		default:
			s.fanReg[i] = static_cast<USHORT>(0x030 + 2 * i);
			break;
		}
	}
	// Nuvoton boards wire input 1 to SYS_FAN and input 2 to CPU_FAN.
	s.cpuChannel = 1;
	s.sysChannel[0] = 0;
	s.sysChannel[1] = 2;

	BYTE bank = 0;
	if (!NctSaveBank(s, bank)) {
		SetStatus(s, L"fan controller did not answer");
		return kUnsupported;
	}
	const bool vendorOk = !s.vendorCheck || NctVendorOk(s);
	NctRestoreBank(s, bank);
	if (!vendorOk) {
		SetStatus(s, L"fan controller did not answer");
		return kUnsupported;
	}
	return kFound;
}

bool NctReadFans(FanSensor& s, FanReading& out) {
	BYTE bank = 0;
	if (!NctSaveBank(s, bank)) {
		return false;
	}
	if (s.vendorCheck && !NctVendorOk(s)) {
		NctRestoreBank(s, bank);
		s.ready = false;
		SetStatus(s, L"fan controller stopped answering");
		return false;
	}
	for (int i = 0; i < s.count && i < kFanChannels; ++i) {
		BYTE hi = 0;
		BYTE lo = 0;
		if (!NctRead(s, s.fanReg[i], hi) || !NctRead(s, static_cast<USHORT>(s.fanReg[i] + 1), lo)) {
			continue;
		}
		if (s.family == kFamilyNctCount13) {
			const int count = (hi << 5) | (lo & 0x1F);
			if (count >= kNctMaxCount13) {
				out.rpm[i] = 0; // stopped or nothing connected
			} else if (count >= s.minValid) {
				out.rpm[i] = 1350000 / count;
			}
		} else {
			const int rpm = (hi << 8) | lo;
			out.rpm[i] = rpm > s.minValid ? rpm : 0;
		}
	}
	NctRestoreBank(s, bank);
	return true;
}

// Sets the Nuvoton I/O space lock again if Fans_Open lifted it.
void NctRelock(FanSensor& s) {
	if (!SelectSlot(s, s.slot) || !NctEnter(s)) {
		return;
	}
	BYTE id = 0;
	if (ConfigIn(s, kChipIdReg, id) && id != 0 && id != 0xFF) {
		NctSetIoSpaceLock(s, true, nullptr);
	}
	NctExit(s);
}

// Finds the chip. Runs with the ISA mutex held.
bool Detect(FanSensor& s) {
	DetectResult r = DetectIte(s);
	if (r == kNotFound) {
		r = DetectNuvoton(s, 0);
	}
	if (r == kNotFound) {
		r = DetectNuvoton(s, 1);
	}
	if (r == kNotFound) {
		SetStatus(s, L"no supported Super I/O chip found");
	}
	return r == kFound;
}

} // namespace

bool Fans_Open(FanSensor& s) {
	s = {};
	s.device = PawnIo_Open(kPawnModuleLpcIo, s.status);
	if (!s.device) {
		return false;
	}
	s.isaMutex = PawnIo_OpenMutex(kPawnIsaMutex);
	bool ok = false;
	if (!PawnIo_Lock(s.isaMutex, 1000)) {
		SetStatus(s, L"Super I/O busy (another monitor)");
	} else {
		ok = Detect(s);
		PawnIo_Unlock(s.isaMutex);
	}
	if (!ok) {
		wchar_t status[64];
		lstrcpynW(status, s.status, 64);
		Fans_Close(s); // also undoes a lifted I/O space lock
		lstrcpynW(s.status, status, 64);
		return false;
	}
	s.ready = true;
	SetStatus(s, L"ok");
	return true;
}

bool Fans_Read(FanSensor& s, FanReading& out) {
	for (int& r : out.rpm) {
		r = -1;
	}
	if (!s.ready) {
		return false;
	}
	if (!PawnIo_Lock(s.isaMutex, 20)) {
		return false; // another monitor is on the bus; try next tick
	}
	const bool ok = s.family == kFamilyIte ? IteReadFans(s, out) : NctReadFans(s, out);
	PawnIo_Unlock(s.isaMutex);
	return ok;
}

void Fans_Close(FanSensor& s) {
	if (s.device && s.relockIoSpace && PawnIo_Lock(s.isaMutex, 1000)) {
		NctRelock(s);
		PawnIo_Unlock(s.isaMutex);
	}
	if (s.device) {
		CloseHandle(s.device);
	}
	if (s.isaMutex) {
		CloseHandle(s.isaMutex);
	}
	s = {};
}
