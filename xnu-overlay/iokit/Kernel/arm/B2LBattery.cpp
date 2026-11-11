/* SPDX-License-Identifier: BSD-2-Clause
 * Battery status for the S10+: read the MAX77705 fuel gauge and charger over
 * HSI2C45 (CMGP USI03, 0x15D60000; stock DT hsi2c_45 / max77705@66) and
 * publish an AppleSmartBattery IOPMPowerSource, which powerd matches.
 *
 * Only reads go to the MAX77705: no charger, regulator or fuel-gauge model
 * register is ever written. The bus itself is brought up exactly like the
 * platform UEFI HsI2cDxe does (pins GPM12-0/GPM13-0 function 2, USI mode I2C,
 * reset, 400 kHz timing from the 200 MHz input clock) unless the boot chain
 * already left it configured. Boot argument b2l_battery=0 disables the driver.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <arm64/b2l_diag.h>
#include "b2l_battery_model.h"

#define B2L_BAT_HSI2C        0x15D60000ull
#define B2L_BAT_GPIO         0x15C30000ull   /* pinctrl@15C30000 (CMGP) */
#define B2L_BAT_GPM12_CON    0x180
#define B2L_BAT_GPM13_CON    0x1A0
#define B2L_BAT_USI_PAGE     0x15C22000ull
#define B2L_BAT_USI_OFF      0x030           /* USI_CMGP03 configuration */
#define B2L_BAT_USI_I2C      0x4             /* I2C_SW_CONF */
#define B2L_BAT_CLOCK        200000000u
#define B2L_BAT_POLL_MS      20000

/* The power source powerd matches by class name (IOServiceMatching). */
class AppleSmartBattery : public IOPMPowerSource {
	OSDeclareDefaultStructors(AppleSmartBattery);
	void setNumber(const char *key, int64_t value, unsigned bits = 32);
	void setBool(const char *key, bool value);
public:
	void apply(const struct b2l_battery_state *s);
};
OSDefineMetaClassAndStructors(AppleSmartBattery, IOPMPowerSource);

void
AppleSmartBattery::setNumber(const char *key, int64_t value, unsigned bits)
{
	const OSSymbol *k = OSSymbol::withCString(key);
	OSNumber *n = OSNumber::withNumber((unsigned long long)value, bits);
	if (k && n) setPSProperty(k, n);
	OSSafeReleaseNULL(n);
	OSSafeReleaseNULL(k);
}

/* Publish one decoded reading; updateStatus() notifies powerd on change. */
void
AppleSmartBattery::apply(const struct b2l_battery_state *s)
{
	setBatteryInstalled(true);
	setExternalConnected(s->external);
	setExternalChargeCapable(s->external);
	setIsCharging(s->charging);
	setBool("FullyCharged", s->full);
	setCurrentCapacity(s->percent);
	setMaxCapacity(100);
	setAtWarnLevel(!s->external && s->percent <= 10);
	setAtCriticalLevel(!s->external && s->percent <= 5);
	setVoltage(s->millivolts);
	setAmperage(s->avg_milliamps);
	setNumber("InstantAmperage", s->milliamps);
	setNumber("AppleRawCurrentCapacity", s->remaining_mah);
	setNumber("AppleRawMaxCapacity", s->full_mah);
	setNumber("NominalChargeCapacity", s->full_mah);
	if (s->design_mah) setNumber("DesignCapacity", s->design_mah);
	setCycleCount(s->cycles);
	if (s->temp_valid) setNumber("Temperature", s->temp_centi_c);
	int remaining = s->charging ? s->minutes_to_full : s->minutes_to_empty;
	setTimeRemaining(remaining >= 0 ? remaining : 0);
	setNumber("AvgTimeToEmpty", s->minutes_to_empty >= 0 ? s->minutes_to_empty : 65535);
	setNumber("AvgTimeToFull", s->minutes_to_full >= 0 ? s->minutes_to_full : 65535);
	updateStatus();
}

void
AppleSmartBattery::setBool(const char *key, bool value)
{
	const OSSymbol *k = OSSymbol::withCString(key);
	if (k) setPSProperty(k, value ? kOSBooleanTrue : kOSBooleanFalse);
	OSSafeReleaseNULL(k);
}

class B2LBattery : public IOService {
	OSDeclareDefaultStructors(B2LBattery);
	IOMemoryMap *i2cMap, *gpioMap, *usiMap;
	volatile uint8_t *i2c;
	IOWorkLoop *loop;
	IOTimerEventSource *timer;
	AppleSmartBattery *battery;
	unsigned failures, logs, transferLogs;
	struct b2l_battery_state last;
	bool published;

	uint32_t r(unsigned off) { uint32_t v = *(volatile uint32_t *)(i2c + off); __asm__ volatile("dmb oshld" ::: "memory"); return v; }
	void w(unsigned off, uint32_t v) { *(volatile uint32_t *)(i2c + off) = v; __asm__ volatile("dsb oshst" ::: "memory"); }
	static uint64_t uptimeUS() { uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000; }
	static IOMemoryMap *mapPage(uint64_t phys);
	void quiesce() { w(0x20, 0); w(0x24, r(0x24)); w(0x44, r(0x44) & ~(1u << 31)); w(0, r(0) & ~0xc0u); }
	void resetController() { w(0, r(0) | (1u << 31)); w(0, r(0) & ~(1u << 31)); w(0, 8); w(8, 0x00ffffff); w(0x40, r(0x40) | (1u << 31)); }
	bool message(uint8_t addr, uint8_t *buf, unsigned n, bool read);
	bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, unsigned n);
	bool readFG(uint8_t reg, uint16_t *out);
	const char *bringUpBus();
	void sample();
	static void poll(OSObject *owner, IOTimerEventSource *sender);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(B2LBattery, IOService);

IOMemoryMap *
B2LBattery::mapPage(uint64_t phys)
{
	IOMemoryDescriptor *d = IOMemoryDescriptor::withPhysicalAddress(phys, 4096, kIODirectionInOut);
	IOMemoryMap *m = d ? d->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(d);
	return m;
}

/* One HSI2C transaction with a STOP, as B2LInput's touch transfer. */
bool
B2LBattery::message(uint8_t addr, uint8_t *buf, unsigned n, bool read)
{
	if (!buf || !n || n > 8) return false;
	const unsigned budgetUS = 20000;
	const uint64_t begin = uptimeUS();
	resetController();
	w(0x48, r(0x48) & ~(1u << 31));
	w(4, 3u | (n << 4) | (n << 16));
	w(0, (r(0) & ~0xc0u) | 8u | (read ? 0x40u : 0x80u));
	w(0x40, r(0x40) | (1u << 31));
	w(0x70, (r(0x70) & ~0xff0fffffu) | ((uint32_t)addr << 10));
	w(0x24, r(0x24)); w(0x20, 0x80);
	w(0x44, (r(0x44) & ~0x8003ffffu) | 0x80020000u | (read ? 0x10000u : 0) | n);
	unsigned at = 0;
	while (uptimeUS() - begin < budgetUS) {
		while (at < n) {
			uint32_t fifo = r(0x30);
			if (read) {
				if (fifo & (1u << 24)) break;
				buf[at++] = (uint8_t)r(0x38);
			} else {
				if ((fifo & 0x7f) >= 16) break;
				w(0x34, buf[at++]);
			}
		}
		if (at == n && (r(0x24) & 0x80) && (read || (r(0x30) & 0x100))) {
			bool nack = (r(0x24) & (1u << 10)) != 0;      /* no device */
			quiesce();
			return !nack;
		}
		IODelay(5);
	}
	if (transferLogs++ < 8) {
		b2l_diag_record("B2L BATTERY I2C: addr=0x%x read=%d n=%u done=%u int=0x%x trans=0x%x fifo=0x%x\n",
		    addr, read, n, at, r(0x24), r(0x50), r(0x30));
	}
	resetController(); quiesce();
	return false;
}

bool
B2LBattery::readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, unsigned n)
{
	return message(addr, &reg, 1, false) && message(addr, buf, n, true);
}

bool
B2LBattery::readFG(uint8_t reg, uint16_t *out)
{
	uint8_t b[2];
	if (!readRegs(B2L_FG_ADDR, reg, b, 2)) return false;
	*out = (uint16_t)(b[0] | (b[1] << 8));
	return true;
}

/* Returns "reused" or "configured", or the failing stage. */
const char *
B2LBattery::bringUpBus()
{
	gpioMap = mapPage(B2L_BAT_GPIO);
	usiMap = mapPage(B2L_BAT_USI_PAGE);
	i2cMap = mapPage(B2L_BAT_HSI2C);
	if (!gpioMap || !usiMap || !i2cMap) return "map";
	i2c = (volatile uint8_t *)i2cMap->getVirtualAddress();
	volatile uint32_t *usi = (volatile uint32_t *)(usiMap->getVirtualAddress() + B2L_BAT_USI_OFF);
	volatile uint32_t *gpm12 = (volatile uint32_t *)(gpioMap->getVirtualAddress() + B2L_BAT_GPM12_CON);
	volatile uint32_t *gpm13 = (volatile uint32_t *)(gpioMap->getVirtualAddress() + B2L_BAT_GPM13_CON);

	uint32_t div, t, t1;
	b2l_hsi2c_fast_timing(B2L_BAT_CLOCK, &div, &t, &t1);
	bool pins = (*gpm12 & 0xF) == 2 && (*gpm13 & 0xF) == 2;
	bool mode = (*usi & 0x7) == B2L_BAT_USI_I2C;
	bool timing = r(0x60) && r(0x64) && ((r(0x68) >> 16) & 0xFF);
	b2l_diag_record("B2L BATTERY BUS: gpm12=0x%x gpm13=0x%x usi=0x%x fs1=0x%x fs2=0x%x fs3=0x%x conf=0x%x\n",
	    *gpm12, *gpm13, *usi, r(0x60), r(0x64), r(0x68), r(0x40));
	if (pins && mode && timing) return "reused";

	if (!pins) {
		*gpm12 = (*gpm12 & ~0xFu) | 2u;
		*gpm13 = (*gpm13 & ~0xFu) | 2u;
	}
	if (!mode) *usi = B2L_BAT_USI_I2C;
	__asm__ volatile("dsb sy" ::: "memory");
	w(0xC4, 0);                                    /* usi_con */
	w(0, r(0) | (1u << 31)); w(0, r(0) & ~(1u << 31));
	w(0x68, (r(0x68) & ~0x00FF0000u) | (div << 16));
	w(0x64, (r(0x64) & ~0x000000FFu) | t);
	w(0x60, (r(0x60) & ~0x00FF0000u) | (t1 << 16));
	w(0, 8);                                       /* HSI2C_MASTER */
	w(8, 0x00FFFFFF);                              /* trailing count */
	w(0x40, r(0x40) | (1u << 31));                 /* auto mode */
	return "configured";
}

void
B2LBattery::sample()
{
	struct b2l_battery_raw raw = {};
	bool ok = readFG(B2L_FG_REPSOC, &raw.repsoc) && readFG(B2L_FG_VCELL, &raw.vcell);
	uint16_t v = 0;
	if (ok) {
		if (readFG(B2L_FG_REPCAP, &raw.repcap) && readFG(B2L_FG_FULLCAPREP, &raw.fullcaprep) &&
		    readFG(B2L_FG_DESIGNCAP, &raw.designcap) && readFG(B2L_FG_CYCLES, &raw.cycles) &&
		    readFG(B2L_FG_TTE, &raw.tte) && readFG(B2L_FG_TTF, &raw.ttf)) {
			if (readFG(B2L_FG_CURRENT, &v)) raw.current = (int16_t)v;
			if (readFG(B2L_FG_AVGCURRENT, &v)) raw.avgcurrent = (int16_t)v;
			if (readFG(B2L_FG_TEMP, &v)) raw.temp = (int16_t)v;
		}
		uint8_t c[3];
		if (readRegs(B2L_CHG_ADDR, B2L_CHG_INT_OK, c, 3)) {
			raw.charger_ok = true;
			raw.int_ok = c[0]; raw.details00 = c[1]; raw.details01 = c[2];
		}
	}
	struct b2l_battery_state s;
	b2l_battery_decode(&raw, &s);
	if (!ok || !s.valid) {
		if (failures++ < 8) {
			b2l_diag_record("B2L BATTERY: fuel gauge %s (repsoc=0x%x vcell=0x%x)\n",
			    ok ? "implausible" : "unreadable", raw.repsoc, raw.vcell);
		}
		return;
	}
	failures = 0;
	if (!published || s.percent != last.percent || s.external != last.external ||
	    s.charging != last.charging || s.full != last.full) {
		if (logs++ < 200) {
			b2l_diag_record("B2L BATTERY: %u%% %umV %dmA avg=%dmA %u/%umAh design=%umAh temp=%d cycles=%u ext=%d charging=%d full=%d chg=%d int_ok=0x%x d00=0x%x d01=0x%x\n",
			    s.percent, s.millivolts, s.milliamps, s.avg_milliamps, s.remaining_mah, s.full_mah,
			    s.design_mah, s.temp_valid ? s.temp_centi_c : 0, s.cycles, s.external, s.charging, s.full,
			    raw.charger_ok, raw.int_ok, raw.details00, raw.details01);
		}
	}
	last = s;

	battery->apply(&s);
	if (!published) {
		published = true;
		battery->registerService();
		b2l_diag_record("B2L BATTERY: AppleSmartBattery published\n");
	}
}

void
B2LBattery::poll(OSObject *owner, IOTimerEventSource *sender)
{
	((B2LBattery *)owner)->sample();
	sender->setTimeoutMS(B2L_BAT_POLL_MS);
}

bool
B2LBattery::start(IOService *provider)
{
	if (!IOService::start(provider)) return false;
	int enabled = 1;
	if (PE_parse_boot_argn("b2l_battery", &enabled, sizeof(enabled)) && !enabled) {
		b2l_diag_record("B2L BATTERY: disabled by boot argument\n");
		return false;
	}
	const char *bus = bringUpBus();
	b2l_diag_record("B2L BATTERY: HSI2C45 %s\n", bus);
	if (!i2c) return false;
	battery = OSTypeAlloc(AppleSmartBattery);
	if (!battery || !battery->init() || !battery->attach(this) || !battery->start(this)) {
		OSSafeReleaseNULL(battery);
		return false;
	}
	loop = IOWorkLoop::workLoop();
	timer = IOTimerEventSource::timerEventSource(this, &B2LBattery::poll);
	if (!loop || !timer || loop->addEventSource(timer) != kIOReturnSuccess) return false;
	timer->setTimeoutMS(1);
	return true;
}
