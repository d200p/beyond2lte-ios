/* SPDX-License-Identifier: BSD-2-Clause
 * First Wi-Fi bring-up checkpoint for beyond2lte / Exynos 9820.
 *
 * b2l_wifi_diag=1 samples shared PMU, GPIO and CMU registers once, read only.
 * b2l_wifi_diag=2 additionally pulses WLAN power with PERST held low, tests
 * PHY isolation bypass readback, then restores the owned bits. Neither mode
 * accesses the PCIe controller/config/BAR, firmware or network registration.
 * A powered PHY alone does not establish that DBI/ELBI accesses are safe.
 * All addresses/offsets are sourced in docs/wifi-bringup.md.
 */
#include <arm64/b2l_early.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
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

static void
wifi_write(const struct wifi_region *region, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)(region->mapped + offset) = value;
    __asm__ volatile ("dsb sy" ::: "memory");
}

/* Change and restore only owned bits, preserving every other pin and PMU bit.
 * An all-ones register is not used as the input to a read-modify-write. */
static int
wifi_bits(const struct wifi_region *region, unsigned offset, uint32_t mask, uint32_t value)
{
    uint32_t before = wifi_read(region, offset);
    if (before == UINT32_MAX) return -1;
    uint32_t after = (before & ~mask) | (value & mask);
    if (after != before) wifi_write(region, offset, after);
    uint32_t checked = wifi_read(region, offset);
    return checked != UINT32_MAX && (checked & mask) == (value & mask) ? 0 : -1;
}

static int
wifi_restore_bits(const struct wifi_region *region, unsigned offset, uint32_t mask, uint32_t value)
{
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        if (!wifi_bits(region, offset, mask, value)) return 0;
        delay_for_interval(1, NSEC_PER_MSEC);
    }
    return -1;
}

static void
wifi_power_probe(struct wifi_region regions[4])
{
    const uint32_t wlan_function = 15u << 20, wlan_level = 1u << 5;
    const uint32_t reset_function = 15u << 8, reset_level = 1u << 2;
    uint32_t phy = wifi_read(&regions[0], 0x71c);
    uint32_t wc = wifi_read(&regions[1], 0x80), wd = wifi_read(&regions[1], 0x84);
    uint32_t rc = wifi_read(&regions[2], 0x20), rd = wifi_read(&regions[2], 0x24);
    /* This first active trial accepts only the idle state measured in epoch 48.
     * Do not interrupt an already-owned pin, powered PHY or active endpoint. */
    if (phy == UINT32_MAX || wc == UINT32_MAX || wd == UINT32_MAX ||
        rc == UINT32_MAX || rd == UINT32_MAX || (phy & 1u) ||
        (wc & wlan_function) || (wd & wlan_level) ||
        (rc & reset_function) || (rd & reset_level)) {
        wlog("power probe skipped: shared state differs from verified idle baseline\n");
        return;
    }
    /* Exact observed clock words; this probe changes no clock setting. These
     * checks do not authorize accessing ELBI/DBI or training the link. */
    for (unsigned off = 0x2030; off <= 0x2044; off += 4) {
        if (wifi_read(&regions[3], off) != 0x200000) {
            wlog("power probe skipped: gate +%#x differs from baseline\n", off);
            return;
        }
    }
    for (unsigned off = 0x3034; off <= 0x3044; off += 4) {
        if (wifi_read(&regions[3], off) != 0x3f0002) {
            wlog("power probe skipped: QCH +%#x differs from baseline\n", off);
            return;
        }
    }
    wlog("power probe begin: hold PERST low; temporary WLAN power and PHY bypass\n");
    /* Program the low data latch before changing each pad from input to output.
     * Hold PERST asserted throughout; no link or DMA can be started here. */
    int failed = wifi_bits(&regions[2], 0x24, reset_level, 0) ||
        wifi_bits(&regions[2], 0x20, reset_function, 1u << 8) ||
        wifi_bits(&regions[1], 0x84, wlan_level, 0) ||
        wifi_bits(&regions[1], 0x80, wlan_function, 1u << 20) ||
        wifi_bits(&regions[1], 0x84, wlan_level, wlan_level);
    if (!failed) {
        delay_for_interval(200, NSEC_PER_MSEC); /* Samsung WIFI_TURNON_DELAY */
        failed = wifi_bits(&regions[0], 0x71c, 1u, 1u);
        if (!failed) {
            delay_for_interval(1, NSEC_PER_MSEC);
            failed = (wifi_read(&regions[2], 0x24) & reset_level) != 0 ||
                (wifi_read(&regions[1], 0x84) & wlan_level) == 0;
        }
    }
    wlog("power probe readback %s; controller remains unprobed\n", failed ? "failed" : "passed");
    /* Re-isolate before powering down. Restore data while pads are still
     * outputs, then restore mux fields. Attempt every restoration on failure. */
    int restore_failed = 0;
    restore_failed |= wifi_restore_bits(&regions[0], 0x71c, 1u, phy);
    restore_failed |= wifi_restore_bits(&regions[1], 0x84, wlan_level, wd);
    restore_failed |= wifi_restore_bits(&regions[2], 0x24, reset_level, rd);
    restore_failed |= wifi_restore_bits(&regions[1], 0x80, wlan_function, wc);
    restore_failed |= wifi_restore_bits(&regions[2], 0x20, reset_function, rc);
    wlog("power probe restore %s; no firmware or networking\n", restore_failed ? "FAILED" : "verified");
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

    if (!PE_parse_boot_argn("b2l_wifi_diag", &mode, sizeof(mode)) || mode < 1 || mode > 2) {
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
    if (mode == 2) wifi_power_probe(regions);
out:
    for (unsigned i = count; i > 0; i--) {
        if (regions[i - 1].mapped)
            ml_io_unmap(regions[i - 1].mapped, regions[i - 1].size);
    }
}
