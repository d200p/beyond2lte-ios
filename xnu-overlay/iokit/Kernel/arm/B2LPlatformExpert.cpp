/* SPDX-License-Identifier: BSD-2-Clause
 * Minimal non-Apple device-tree platform for the Exynos 9820 first boot.
 */
#include <pexpert/arm64/board_config.h>
#if defined(B2L)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <uuid/uuid.h>
#include "b2l_platform_uuid.h"
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
extern "C" void b2l_early_trace(const char *fmt, ...);
/* configd's InterfaceNamer logs "timed out waiting for IOKit to quiesce" and
 * SystemConfiguration clients (Settings) wait on it. Report the root busy
 * state and every busy service so the holder can be identified. Read-only. */
extern "C" void
b2l_iokit_busy_report(void)
{
	IOService *root = IOService::getServiceRoot();
	if (!root) return;
	uint32_t busy = root->getBusyState();
	b2l_early_trace("B2L IOKIT: root busy=%u\n", busy);
	if (!busy) return;
	IORegistryIterator *it = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
	if (!it) return;
	unsigned listed = 0;
	while (IORegistryEntry *e = it->getNextObject()) {
		IOService *s = OSDynamicCast(IOService, e);
		if (!s || !s->getBusyState() || listed >= 24) continue;
		listed++;
		b2l_early_trace("B2L IOKIT: busy=%u state=0x%llx %s (%s) depth=%u\n", s->getBusyState(),
		    (unsigned long long)s->getState(), s->getName(), s->getMetaClass()->getClassName(), e->getDepth(gIOServicePlane));
	}
	it->release();
}

class B2LPlatformExpert : public IODTPlatformExpert {
	OSDeclareDefaultStructors(B2LPlatformExpert);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
	int haltRestart(unsigned int type) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(B2LPlatformExpert, IODTPlatformExpert);

/* From StachowiakDawid/beyond2lte-ios (B2LPower.cpp): real power-off and restart
 * through the PMU instead of "CPU halted" / "Halt/Restart Timed Out". */
extern "C" void b2l_power_init(void);
extern "C" bool b2l_power_halt_restart(unsigned int type);

int
B2LPlatformExpert::haltRestart(unsigned int type)
{
	return b2l_power_halt_restart(type) ? 0 : IODTPlatformExpert::haltRestart(type);
}

/*
 * From StachowiakDawid/beyond2lte-ios build 0142.
 * configd's InterfaceNamer reports nothing until it has opened this service, or
 * until its timeout passes, and everything that lists network interfaces waits for
 * that report: Settings froze behind it. The real class lives in IONetworkingFamily,
 * which this kernel does not have. configd only uses the connection to register
 * interfaces, and this port has none, so the user client has no methods.
 */
class IONetworkStack : public IOService {
	OSDeclareDefaultStructors(IONetworkStack);
};
OSDefineMetaClassAndStructors(IONetworkStack, IOService);

class IONetworkStackUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IONetworkStackUserClient);
public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);

IOReturn
IONetworkStackUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

bool
B2LPlatformExpert::start(IOService *provider)
{
	IOLog("B2L: starting device-tree platform expert\n");
	if (!IODTPlatformExpert::start(provider)) {
		return false;
	}
	b2l_power_init();
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSData *wifi=chosen?OSDynamicCast(OSData,chosen->getProperty("mac-address-wifi0")):nullptr;
    bool wifiPresent=wifi&&wifi->getLength()==6;
    IOLog("B2L: WiFi MAC metadata %s (address redacted)\n",wifiPresent?"present, 6 bytes":"absent");
    /* From StachowiakDawid/beyond2lte-ios build 0138: gethostuuid() (configd,
     * timed, rapportd) waits with no timeout for IOPlatformUUID, which XNU only
     * derives from iBoot's chip ids. Publish a stable one when XNU has not. */
    if(provider->getProperty(kIOPlatformUUIDKey)==nullptr){
        uint8_t uuid[16];
        uuid_string_t text;
        b2l_platform_uuid(wifiPresent?wifi->getBytesNoCopy():nullptr,wifiPresent?wifi->getLength():0,uuid);
        uuid_unparse(uuid,text);
        OSString *string=OSString::withCString(text);
        provider->setProperty(kIOPlatformUUIDKey,string);
        publishResource(kIOPlatformUUIDKey,string);
        OSSafeReleaseNULL(string);
        IOLog("B2L: IOPlatformUUID published, %s\n",wifiPresent?"unit-specific":"constant");
    }
    if(chosen)chosen->release();
    uint32_t sleepDisabled=0;
    if(PE_get_default("sleep-disabled",&sleepDisabled,sizeof(sleepDisabled)) && sleepDisabled)
        IOLog("B2L PM: system sleep disabled until CPU suspend/resume support is available\n");
	registerService();
	IONetworkStack *stack = OSTypeAlloc(IONetworkStack);
	if (stack != nullptr && stack->init() && stack->attach(this)) {
		stack->setProperty(kIOUserClientClassKey, "IONetworkStackUserClient");
		stack->registerService();
		IOLog("B2L: IONetworkStack published for configd (no interfaces)\n");
	}
	OSSafeReleaseNULL(stack);
	IOLog("B2L: platform expert registered; software product iPhone17,2, Samsung B2L hardware\n");
	return true;
}

const char *B2LPlatformExpert::deleteList(void) { return "()"; }
const char *B2LPlatformExpert::excludeList(void)
{
	return "('chosen', 'defaults', 'cpus', 'options')";
}

void
B2LPlatformExpert::processTopLevel(IORegistryEntry *root)
{
	/* There is no Apple NVRAM /options service.  Publish the real boot CPU
	 * without fabricating a firmware NVRAM device. */
	IORegistryEntry *cpus = root->childFromPath("cpus", gIODTPlane);
	if (cpus != nullptr) {
		createNubs(this, IODTFindMatchingEntries(cpus, kIODTExclusive, nullptr));
		cpus->release();
	}
	createNubs(this, IODTFindMatchingEntries(root, kIODTExclusive, excludeList()));
    IORegistryEntry *armio=root->childFromPath("arm-io",gIODTPlane);
    if(armio){createNubs(this,IODTFindMatchingEntries(armio,kIODTExclusive,"('gic')"));armio->release();}

}
#endif
