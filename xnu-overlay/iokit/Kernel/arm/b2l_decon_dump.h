/* SPDX-License-Identifier: BSD-2-Clause
 * How the boot chain left the display controller: DECON_F at 0x19030000 and
 * the six DPU_DMA input channels from 0x19071000. Read only, and in small
 * groups that each reach the persistent log before the next, because walking
 * unknown registers froze the bus once before (0133). Offsets are from the
 * public dpu20 driver: GLOBAL_CONTROL 0x0, SHADOW_REG_UPDATE_REQ 0x60,
 * HW_SW_TRIG_CONTROL 0x70, DATA_PATH_CONTROL_2 0x230, WIN_CONTROL_0/1 and
 * START/END_POSITION at 0x1000 + 0x30 per window; IDMA ENABLE 0x0, IN_CON 0x8,
 * SRC_SIZE 0x10, SRC_OFFSET 0x14, IMG_SIZE 0x18, IN_BASE_ADDR_Y 0x40.
 */
#ifndef _B2L_DECON_DUMP_H
#define _B2L_DECON_DUMP_H

#define B2L_DECON_BASE          0x19030000ull
#define B2L_DECON_BYTES         0x2000
#define B2L_DPU_DMA_BASE        0x19070000ull
#define B2L_DPU_DMA_BYTES       0x7000
#define B2L_DECON_WINDOWS       6
#define B2L_DPU_DMA_CHANNELS    6

#endif /* _B2L_DECON_DUMP_H */
