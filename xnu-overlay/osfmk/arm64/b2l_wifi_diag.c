/* SPDX-License-Identifier: BSD-2-Clause
 * First Wi-Fi bring-up checkpoint for beyond2lte / Exynos 9820.
 *
 * b2l_wifi_diag=1 samples shared PMU, GPIO and CMU registers once. No writes,
 * PCIe controller/config/BAR reads, firmware loading or network registration.
 * A powered PHY alone does not establish that DBI/ELBI accesses are safe.
 * All addresses/offsets are sourced in docs/wifi-bringup.md.
 */
#include <arm64/b2l_early.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>

#define wlog(...) b2l_early_trace("B2L WIFI: " __VA_ARGS__)

struct wifi_region {
    vm_offset_t physical, mapped;
    vm_size_t size;
};

static uint32_t
wifi_read(const struct wifi_region *region, unsigned offset)
{
    return *(volatile const uint32_t *)(region->mapped + offset);
}

void
b2l_wifi_diag_start(void)
{
    uint32_t mode = 0;
    struct wifi_region regions[] = {
        { 0x15860000, 0, 0x4000 }, /* PMU: PCIe0 PHY isolation */
        { 0x10430000, 0, 0x4000 }, /* PERIC0 GPIO: GPG0-5 WLAN_REG_ON */
        { 0x13c40000, 0, 0x4000 }, /* FSYS1 GPIO: GPF2-0 CLKREQ, GPF2-2 PERST */
        { 0x13c00000, 0, 0x4000 }, /* CMU_FSYS1, shared with booted UFS */
    };
    static const unsigned gates[] = { 0x2030, 0x2034, 0x2038, 0x203c, 0x2040, 0x2044 };
    static const unsigned qch[] = { 0x3034, 0x3038, 0x303c, 0x3040, 0x3044 };
    unsigned count = sizeof(regions) / sizeof(regions[0]);

    if (!PE_parse_boot_argn("b2l_wifi_diag", &mode, sizeof(mode)) || mode != 1) {
        wlog("diagnostic disabled; no MMIO access\n");
        return;
    }
    /* Map every shared block before reading any of them. Release all mappings
     * even when an allocation fails. No controller region is ever mapped. */
    for (unsigned i = 0; i < count; i++) {
        regions[i].mapped = ml_io_map_unmappable(regions[i].physical,
            regions[i].size, VM_WIMG_IO);
        if (!regions[i].mapped) {
            wlog("shared-block map failed at %#lx; no registers sampled\n",
                (unsigned long)regions[i].physical);
            goto out;
        }
    }
    wlog("shared-block snapshot begin; no PCIe controller access\n");
    uint32_t phy = wifi_read(&regions[0], 0x71c);
    uint32_t wlan_con = wifi_read(&regions[1], 0x80);
    uint32_t wlan_dat = wifi_read(&regions[1], 0x84);
    uint32_t pcie_con = wifi_read(&regions[2], 0x20);
    uint32_t pcie_dat = wifi_read(&regions[2], 0x24);
    wlog("PMU PHY_CONTROL=%#x; GPG0 CON=%#x DAT=%#x; GPF2 CON=%#x DAT=%#x\n",
        phy, wlan_con, wlan_dat, pcie_con, pcie_dat);
    if (phy == UINT32_MAX || wlan_con == UINT32_MAX || wlan_dat == UINT32_MAX ||
        pcie_con == UINT32_MAX || pcie_dat == UINT32_MAX) {
        wlog("invalid all-ones shared register; pin state undecoded\n");
        goto out;
    }
    wlog("PHY isolation_bypass=%u; WLAN_REG_ON function=%u level=%u; "
        "PERST function=%u level=%u; CLKREQ function=%u level=%u\n",
        phy & 1u, (wlan_con >> 20) & 15u, (wlan_dat >> 5) & 1u,
        (pcie_con >> 8) & 15u, (pcie_dat >> 2) & 1u,
        pcie_con & 15u, pcie_dat & 1u);
    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        uint32_t value = wifi_read(&regions[3], gates[i]);
        wlog("CMU gate +%#x raw=%#x\n", gates[i], value);
    }
    for (unsigned i = 0; i < sizeof(qch) / sizeof(qch[0]); i++) {
        uint32_t value = wifi_read(&regions[3], qch[i]);
        wlog("CMU QCH +%#x raw=%#x\n", qch[i], value);
    }
    wlog("snapshot complete; PCI identity, firmware and Wi-Fi remain unprobed\n");
out:
    for (unsigned i = count; i > 0; i--) {
        if (regions[i - 1].mapped)
            ml_io_unmap(regions[i - 1].mapped, regions[i - 1].size);
    }
}
