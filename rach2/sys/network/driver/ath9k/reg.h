/*
 * reg.h: the registers of Atheros' AR9003 family, its descriptors' fields
 * and its EEPROM and OTP windows, for the ath9k driver.
 *
 * No datasheet of the family is public. These definitions are therefore
 * ath9k's, from Linux v6.16 (commit
 * 038d61fd642278bab63ee8ef722c50d10ab01e8f): drivers/net/wireless/
 * ath/reg.h, and ath9k's reg.h, ar9003_mac.h, mac.h, ar9003_phy.h and reg_mci.h, in
 * that order, each section headed by the file it comes from. These changes
 * were made to them, and no value was changed by hand:
 *
 * - ath9k serves many chips and picks some registers at run time with
 *   AR_SREV_*(ah) tests. Each such test is resolved here for the chips of
 *   chips.c's table, which is the AR9565 alone, and the macro is left a
 *   constant without its `ah` argument: ath9k's AR_CH0_TOP(ah) is
 *   AR_CH0_TOP. No register of the AR9565 depends on its revision. A chip
 *   added to the table whose register differs, as ath9k's test at the
 *   macro shows, makes the macro a test of its row again.
 * - Definitions whose name belongs to another chip (AR5416, AR9100, AR9160,
 *   AR9280, AR9285, AR9287, AR9271, AR9330, AR9340, AR9462, AR9485, AR9550,
 *   AR9531, AR9561, AR9580, the AR9002 baseband, and the AR9300's own
 *   revision numbers and its 1.0 and SoC registers) are left out, and so
 *   are the AR5416 descriptor's word aliases. The AR9565's version,
 *   revisions and GPIO count are its row's, in chips.c.
 * - AR_SWITCH_TABLE_COM_ALL is AR_SWITCH_TABLE_COM_AR9462_ALL, the
 *   24-bit field the AR9565 shares. The reg_mci.h section keeps the
 *   coexistence controls used to leave Bluetooth unused.
 * - ath9k's enum of AR_INTR_SYNC_* bits is written as macros, since C89
 *   holds an enumeration constant to an int. C++ comments became C
 *   comments, and tabs spaces.
 *
 * The last section is this file's own: the EEPROM and OTP windows from
 * ath9k's eeprom.h and ar9003_eeprom.h, resolved the same way, and the
 * AR9003 descriptors of ar9003_mac.h with the kernel's integer types.
 *
 * A field is a mask NAME with its shift NAME_S, which io.h's SM, MS and
 * REG_RMW_FIELD read. Several masks and comments name chips, as ath9k's
 * do; the driver uses a part of the register set.
 *
 * The ath9k files carry this notice, which their parts here keep:
 *
 * Copyright (c) 2008-2011 Atheros Communications Inc.
 * Copyright (c) 2015 Qualcomm Atheros Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef _ATH9K_REG_H_
#define _ATH9K_REG_H_

#include "../../../lib/kint.h"

/* The registers ath9k shares with the other Atheros drivers (ath/reg.h). */

#define AR_MIBC     0x0040
#define AR_MIBC_COW 0x00000001
#define AR_MIBC_FMC 0x00000002
#define AR_MIBC_CMC 0x00000004
#define AR_MIBC_MCS 0x00000008

#define AR_STA_ID0           0x8000
#define AR_STA_ID1           0x8004
#define AR_STA_ID1_SADH_MASK 0x0000ffff

/*
 * BSSID mask registers. See ath_hw_set_bssid_mask()
 * for detailed documentation about these registers.
 */
#define AR_BSSMSKL 0x80e0
#define AR_BSSMSKU 0x80e4

#define AR_TFCNT 0x80ec
#define AR_RFCNT 0x80f0
#define AR_RCCNT 0x80f4
#define AR_CCCNT 0x80f8

#define AR_KEYTABLE_0            0x8800
#define AR_KEYTABLE(_n)          (AR_KEYTABLE_0 + ((_n) * 32))
#define AR_KEY_CACHE_SIZE        128
#define AR_RSVD_KEYTABLE_ENTRIES 4
#define AR_KEY_TYPE              0x00000007
#define AR_KEYTABLE_TYPE_40      0x00000000
#define AR_KEYTABLE_TYPE_104     0x00000001
#define AR_KEYTABLE_TYPE_128     0x00000003
#define AR_KEYTABLE_TYPE_TKIP    0x00000004
#define AR_KEYTABLE_TYPE_AES     0x00000005
#define AR_KEYTABLE_TYPE_CCM     0x00000006
#define AR_KEYTABLE_TYPE_CLR     0x00000007
#define AR_KEYTABLE_ANT          0x00000008
#define AR_KEYTABLE_VALID        0x00008000
#define AR_KEYTABLE_KEY0(_n)     (AR_KEYTABLE(_n) + 0)
#define AR_KEYTABLE_KEY1(_n)     (AR_KEYTABLE(_n) + 4)
#define AR_KEYTABLE_KEY2(_n)     (AR_KEYTABLE(_n) + 8)
#define AR_KEYTABLE_KEY3(_n)     (AR_KEYTABLE(_n) + 12)
#define AR_KEYTABLE_KEY4(_n)     (AR_KEYTABLE(_n) + 16)
#define AR_KEYTABLE_TYPE(_n)     (AR_KEYTABLE(_n) + 20)
#define AR_KEYTABLE_MAC0(_n)     (AR_KEYTABLE(_n) + 24)
#define AR_KEYTABLE_MAC1(_n)     (AR_KEYTABLE(_n) + 28)

/* The MAC and PCU (ath9k/reg.h). */

#define AR_CR     0x0008
#define AR_CR_RXE (0x0000000c)
#define AR_CR_RXD 0x00000020
#define AR_CR_SWI 0x00000040

#define AR_RXDP 0x000C

#define AR_CFG                           0x0014
#define AR_CFG_SWTD                      0x00000001
#define AR_CFG_SWTB                      0x00000002
#define AR_CFG_SWRD                      0x00000004
#define AR_CFG_SWRB                      0x00000008
#define AR_CFG_SWRG                      0x00000010
#define AR_CFG_AP_ADHOC_INDICATION       0x00000020
#define AR_CFG_PHOK                      0x00000100
#define AR_CFG_EEBS                      0x00000200
#define AR_CFG_CLK_GATE_DIS              0x00000400
#define AR_CFG_HALT_REQ                  0x00000800
#define AR_CFG_HALT_ACK                  0x00001000
#define AR_CFG_PCI_MASTER_REQ_Q_THRESH   0x00060000
#define AR_CFG_PCI_MASTER_REQ_Q_THRESH_S 17

#define AR_RXBP_THRESH      0x0018
#define AR_RXBP_THRESH_HP   0x0000000f
#define AR_RXBP_THRESH_HP_S 0
#define AR_RXBP_THRESH_LP   0x00003f00
#define AR_RXBP_THRESH_LP_S 8

#define AR_MIRT       0x0020
#define AR_MIRT_VAL   0x0000ffff
#define AR_MIRT_VAL_S 16

#define AR_IER         0x0024
#define AR_IER_ENABLE  0x00000001
#define AR_IER_DISABLE 0x00000000

#define AR_TIMT         0x0028
#define AR_TIMT_LAST    0x0000ffff
#define AR_TIMT_LAST_S  0
#define AR_TIMT_FIRST   0xffff0000
#define AR_TIMT_FIRST_S 16

#define AR_RIMT         0x002C
#define AR_RIMT_LAST    0x0000ffff
#define AR_RIMT_LAST_S  0
#define AR_RIMT_FIRST   0xffff0000
#define AR_RIMT_FIRST_S 16

#define AR_DMASIZE_4B   0x00000000
#define AR_DMASIZE_8B   0x00000001
#define AR_DMASIZE_16B  0x00000002
#define AR_DMASIZE_32B  0x00000003
#define AR_DMASIZE_64B  0x00000004
#define AR_DMASIZE_128B 0x00000005
#define AR_DMASIZE_256B 0x00000006
#define AR_DMASIZE_512B 0x00000007

#define AR_TXCFG                             0x0030
#define AR_TXCFG_DMASZ_MASK                  0x00000007
#define AR_TXCFG_DMASZ_4B                    0
#define AR_TXCFG_DMASZ_8B                    1
#define AR_TXCFG_DMASZ_16B                   2
#define AR_TXCFG_DMASZ_32B                   3
#define AR_TXCFG_DMASZ_64B                   4
#define AR_TXCFG_DMASZ_128B                  5
#define AR_TXCFG_DMASZ_256B                  6
#define AR_TXCFG_DMASZ_512B                  7
#define AR_FTRIG                             0x000003F0
#define AR_FTRIG_S                           4
#define AR_FTRIG_IMMED                       0x00000000
#define AR_FTRIG_64B                         0x00000010
#define AR_FTRIG_128B                        0x00000020
#define AR_FTRIG_192B                        0x00000030
#define AR_FTRIG_256B                        0x00000040
#define AR_FTRIG_512B                        0x00000080
#define AR_TXCFG_ADHOC_BEACON_ATIM_TX_POLICY 0x00000800

#define AR_RXCFG            0x0034
#define AR_RXCFG_CHIRP      0x00000008
#define AR_RXCFG_ZLFDMA     0x00000010
#define AR_RXCFG_DMASZ_MASK 0x00000007
#define AR_RXCFG_DMASZ_4B   0
#define AR_RXCFG_DMASZ_8B   1
#define AR_RXCFG_DMASZ_16B  2
#define AR_RXCFG_DMASZ_32B  3
#define AR_RXCFG_DMASZ_64B  4
#define AR_RXCFG_DMASZ_128B 5
#define AR_RXCFG_DMASZ_256B 6
#define AR_RXCFG_DMASZ_512B 7

#define AR_TOPS      0x0044
#define AR_TOPS_MASK 0x0000FFFF

#define AR_RXNPTO      0x0048
#define AR_RXNPTO_MASK 0x000003FF

#define AR_TXNPTO          0x004C
#define AR_TXNPTO_MASK     0x000003FF
#define AR_TXNPTO_QCU_MASK 0x000FFC00

#define AR_RPGTO      0x0050
#define AR_RPGTO_MASK 0x000003FF

#define AR_RPCNT      0x0054
#define AR_RPCNT_MASK 0x0000001F

#define AR_MACMISC                    0x0058
#define AR_MACMISC_PCI_EXT_FORCE      0x00000010
#define AR_MACMISC_DMA_OBS            0x000001E0
#define AR_MACMISC_DMA_OBS_S          5
#define AR_MACMISC_DMA_OBS_LINE_0     0
#define AR_MACMISC_DMA_OBS_LINE_1     1
#define AR_MACMISC_DMA_OBS_LINE_2     2
#define AR_MACMISC_DMA_OBS_LINE_3     3
#define AR_MACMISC_DMA_OBS_LINE_4     4
#define AR_MACMISC_DMA_OBS_LINE_5     5
#define AR_MACMISC_DMA_OBS_LINE_6     6
#define AR_MACMISC_DMA_OBS_LINE_7     7
#define AR_MACMISC_DMA_OBS_LINE_8     8
#define AR_MACMISC_MISC_OBS           0x00000E00
#define AR_MACMISC_MISC_OBS_S         9
#define AR_MACMISC_MISC_OBS_BUS_LSB   0x00007000
#define AR_MACMISC_MISC_OBS_BUS_LSB_S 12
#define AR_MACMISC_MISC_OBS_BUS_MSB   0x00038000
#define AR_MACMISC_MISC_OBS_BUS_MSB_S 15
#define AR_MACMISC_MISC_OBS_BUS_1     1

#define AR_INTCFG             0x005C
#define AR_INTCFG_MSI_RXOK    0x00000000
#define AR_INTCFG_MSI_RXINTM  0x00000004
#define AR_INTCFG_MSI_RXMINTR 0x00000006
#define AR_INTCFG_MSI_TXOK    0x00000000
#define AR_INTCFG_MSI_TXINTM  0x00000010
#define AR_INTCFG_MSI_TXMINTR 0x00000018

#define AR_DATABUF_SIZE      0x0060
#define AR_DATABUF_SIZE_MASK 0x00000FFF

#define AR_GTXTO                 0x0064
#define AR_GTXTO_TIMEOUT_COUNTER 0x0000FFFF
#define AR_GTXTO_TIMEOUT_LIMIT   0xFFFF0000
#define AR_GTXTO_TIMEOUT_LIMIT_S 16

#define AR_GTTM             0x0068
#define AR_GTTM_USEC        0x00000001
#define AR_GTTM_IGNORE_IDLE 0x00000002
#define AR_GTTM_RESET_IDLE  0x00000004
#define AR_GTTM_CST_USEC    0x00000008

#define AR_CST                 0x006C
#define AR_CST_TIMEOUT_COUNTER 0x0000FFFF
#define AR_CST_TIMEOUT_LIMIT   0xFFFF0000
#define AR_CST_TIMEOUT_LIMIT_S 16

#define AR_HP_RXDP 0x0074
#define AR_LP_RXDP 0x0078

#define AR_ISR         0x0080
#define AR_ISR_RXOK    0x00000001
#define AR_ISR_RXDESC  0x00000002
#define AR_ISR_HP_RXOK 0x00000001
#define AR_ISR_LP_RXOK 0x00000002
#define AR_ISR_RXERR   0x00000004
#define AR_ISR_RXNOPKT 0x00000008
#define AR_ISR_RXEOL   0x00000010
#define AR_ISR_RXORN   0x00000020
#define AR_ISR_TXOK    0x00000040
#define AR_ISR_TXDESC  0x00000080
#define AR_ISR_TXERR   0x00000100
#define AR_ISR_TXNOPKT 0x00000200
#define AR_ISR_TXEOL   0x00000400
#define AR_ISR_TXURN   0x00000800
#define AR_ISR_MIB     0x00001000
#define AR_ISR_SWI     0x00002000
#define AR_ISR_RXPHY   0x00004000
#define AR_ISR_RXKCM   0x00008000
#define AR_ISR_SWBA    0x00010000
#define AR_ISR_BRSSI   0x00020000
#define AR_ISR_BMISS   0x00040000
#define AR_ISR_BNR     0x00100000
#define AR_ISR_RXCHIRP 0x00200000
#define AR_ISR_BCNMISC 0x00800000
#define AR_ISR_TIM     0x00800000
#define AR_ISR_QCBROVF 0x02000000
#define AR_ISR_QCBRURN 0x04000000
#define AR_ISR_QTRIG   0x08000000
#define AR_ISR_GENTMR  0x10000000

#define AR_ISR_TXMINTR 0x00080000
#define AR_ISR_RXMINTR 0x01000000
#define AR_ISR_TXINTM  0x40000000
#define AR_ISR_RXINTM  0x80000000

#define AR_ISR_S0              0x0084
#define AR_ISR_S0_QCU_TXOK     0x000003FF
#define AR_ISR_S0_QCU_TXOK_S   0
#define AR_ISR_S0_QCU_TXDESC   0x03FF0000
#define AR_ISR_S0_QCU_TXDESC_S 16

#define AR_ISR_S1             0x0088
#define AR_ISR_S1_QCU_TXERR   0x000003FF
#define AR_ISR_S1_QCU_TXERR_S 0
#define AR_ISR_S1_QCU_TXEOL   0x03FF0000
#define AR_ISR_S1_QCU_TXEOL_S 16

#define AR_ISR_S2             0x008c
#define AR_ISR_S2_QCU_TXURN   0x000003FF
#define AR_ISR_S2_BB_WATCHDOG 0x00010000
#define AR_ISR_S2_CST         0x00400000
#define AR_ISR_S2_GTT         0x00800000
#define AR_ISR_S2_TIM         0x01000000
#define AR_ISR_S2_CABEND      0x02000000
#define AR_ISR_S2_DTIMSYNC    0x04000000
#define AR_ISR_S2_BCNTO       0x08000000
#define AR_ISR_S2_CABTO       0x10000000
#define AR_ISR_S2_DTIM        0x20000000
#define AR_ISR_S2_TSFOOR      0x40000000
#define AR_ISR_S2_TBTT_TIME   0x80000000

#define AR_ISR_S3             0x0090
#define AR_ISR_S3_QCU_QCBROVF 0x000003FF
#define AR_ISR_S3_QCU_QCBRURN 0x03FF0000

#define AR_ISR_S4           0x0094
#define AR_ISR_S4_QCU_QTRIG 0x000003FF
#define AR_ISR_S4_RESV0     0xFFFFFC00

#define AR_ISR_S5                   0x0098
#define AR_ISR_S5_TIMER_TRIG        0x000000FF
#define AR_ISR_S5_TIMER_THRESH      0x0007FE00
#define AR_ISR_S5_TIM_TIMER         0x00000010
#define AR_ISR_S5_DTIM_TIMER        0x00000020
#define AR_IMR_S5                   0x00b8
#define AR_IMR_S5_TIM_TIMER         0x00000010
#define AR_IMR_S5_DTIM_TIMER        0x00000020
#define AR_ISR_S5_GENTIMER_TRIG     0x0000FF80
#define AR_ISR_S5_GENTIMER_TRIG_S   0
#define AR_ISR_S5_GENTIMER_THRESH   0xFF800000
#define AR_ISR_S5_GENTIMER_THRESH_S 16
#define AR_IMR_S5_GENTIMER_TRIG     0x0000FF80
#define AR_IMR_S5_GENTIMER_TRIG_S   0
#define AR_IMR_S5_GENTIMER_THRESH   0xFF800000
#define AR_IMR_S5_GENTIMER_THRESH_S 16

#define AR_IMR         0x00a0
#define AR_IMR_RXOK    0x00000001
#define AR_IMR_RXDESC  0x00000002
#define AR_IMR_RXOK_HP 0x00000001
#define AR_IMR_RXOK_LP 0x00000002
#define AR_IMR_RXERR   0x00000004
#define AR_IMR_RXNOPKT 0x00000008
#define AR_IMR_RXEOL   0x00000010
#define AR_IMR_RXORN   0x00000020
#define AR_IMR_TXOK    0x00000040
#define AR_IMR_TXDESC  0x00000080
#define AR_IMR_TXERR   0x00000100
#define AR_IMR_TXNOPKT 0x00000200
#define AR_IMR_TXEOL   0x00000400
#define AR_IMR_TXURN   0x00000800
#define AR_IMR_MIB     0x00001000
#define AR_IMR_SWI     0x00002000
#define AR_IMR_RXPHY   0x00004000
#define AR_IMR_RXKCM   0x00008000
#define AR_IMR_SWBA    0x00010000
#define AR_IMR_BRSSI   0x00020000
#define AR_IMR_BMISS   0x00040000
#define AR_IMR_BNR     0x00100000
#define AR_IMR_RXCHIRP 0x00200000
#define AR_IMR_BCNMISC 0x00800000
#define AR_IMR_TIM     0x00800000
#define AR_IMR_QCBROVF 0x02000000
#define AR_IMR_QCBRURN 0x04000000
#define AR_IMR_QTRIG   0x08000000
#define AR_IMR_GENTMR  0x10000000

#define AR_IMR_TXMINTR 0x00080000
#define AR_IMR_RXMINTR 0x01000000
#define AR_IMR_TXINTM  0x40000000
#define AR_IMR_RXINTM  0x80000000

#define AR_IMR_S0              0x00a4
#define AR_IMR_S0_QCU_TXOK     0x000003FF
#define AR_IMR_S0_QCU_TXOK_S   0
#define AR_IMR_S0_QCU_TXDESC   0x03FF0000
#define AR_IMR_S0_QCU_TXDESC_S 16

#define AR_IMR_S1             0x00a8
#define AR_IMR_S1_QCU_TXERR   0x000003FF
#define AR_IMR_S1_QCU_TXERR_S 0
#define AR_IMR_S1_QCU_TXEOL   0x03FF0000
#define AR_IMR_S1_QCU_TXEOL_S 16

#define AR_IMR_S2             0x00ac
#define AR_IMR_S2_QCU_TXURN   0x000003FF
#define AR_IMR_S2_QCU_TXURN_S 0
#define AR_IMR_S2_BB_WATCHDOG 0x00010000
#define AR_IMR_S2_CST         0x00400000
#define AR_IMR_S2_GTT         0x00800000
#define AR_IMR_S2_TIM         0x01000000
#define AR_IMR_S2_CABEND      0x02000000
#define AR_IMR_S2_DTIMSYNC    0x04000000
#define AR_IMR_S2_BCNTO       0x08000000
#define AR_IMR_S2_CABTO       0x10000000
#define AR_IMR_S2_DTIM        0x20000000
#define AR_IMR_S2_TSFOOR      0x40000000

#define AR_IMR_S3               0x00b0
#define AR_IMR_S3_QCU_QCBROVF   0x000003FF
#define AR_IMR_S3_QCU_QCBRURN   0x03FF0000
#define AR_IMR_S3_QCU_QCBRURN_S 16

#define AR_IMR_S4           0x00b4
#define AR_IMR_S4_QCU_QTRIG 0x000003FF
#define AR_IMR_S4_RESV0     0xFFFFFC00

#define AR_IMR_S5              0x00b8
#define AR_IMR_S5_TIMER_TRIG   0x000000FF
#define AR_IMR_S5_TIMER_THRESH 0x0000FF00

#define AR_ISR_RAC             0x00c0
#define AR_ISR_S0_S            0x00c4
#define AR_ISR_S0_QCU_TXOK     0x000003FF
#define AR_ISR_S0_QCU_TXOK_S   0
#define AR_ISR_S0_QCU_TXDESC   0x03FF0000
#define AR_ISR_S0_QCU_TXDESC_S 16

#define AR_ISR_S1_S           0x00c8
#define AR_ISR_S1_QCU_TXERR   0x000003FF
#define AR_ISR_S1_QCU_TXERR_S 0
#define AR_ISR_S1_QCU_TXEOL   0x03FF0000
#define AR_ISR_S1_QCU_TXEOL_S 16

#define AR_ISR_S2_S (0x00d0)
#define AR_ISR_S3_S (0x00d4)
#define AR_ISR_S4_S (0x00d8)
#define AR_ISR_S5_S (0x00dc)
#define AR_DMADBG_0 0x00e0
#define AR_DMADBG_1 0x00e4
#define AR_DMADBG_2 0x00e8
#define AR_DMADBG_3 0x00ec
#define AR_DMADBG_4 0x00f0
#define AR_DMADBG_5 0x00f4
#define AR_DMADBG_6 0x00f8
#define AR_DMADBG_7 0x00fc

#define AR_NUM_QCU 10
#define AR_QCU_0   0x0001
#define AR_QCU_1   0x0002
#define AR_QCU_2   0x0004
#define AR_QCU_3   0x0008
#define AR_QCU_4   0x0010
#define AR_QCU_5   0x0020
#define AR_QCU_6   0x0040
#define AR_QCU_7   0x0080
#define AR_QCU_8   0x0100
#define AR_QCU_9   0x0200

#define AR_Q0_TXDP   0x0800
#define AR_Q1_TXDP   0x0804
#define AR_Q2_TXDP   0x0808
#define AR_Q3_TXDP   0x080c
#define AR_Q4_TXDP   0x0810
#define AR_Q5_TXDP   0x0814
#define AR_Q6_TXDP   0x0818
#define AR_Q7_TXDP   0x081c
#define AR_Q8_TXDP   0x0820
#define AR_Q9_TXDP   0x0824
#define AR_QTXDP(_i) (AR_Q0_TXDP + ((_i) << 2))

#define AR_Q_STATUS_RING_START 0x830
#define AR_Q_STATUS_RING_END   0x834

#define AR_Q_TXE   0x0840
#define AR_Q_TXE_M 0x000003FF

#define AR_Q_TXD   0x0880
#define AR_Q_TXD_M 0x000003FF

#define AR_Q0_CBRCFG             0x08c0
#define AR_Q1_CBRCFG             0x08c4
#define AR_Q2_CBRCFG             0x08c8
#define AR_Q3_CBRCFG             0x08cc
#define AR_Q4_CBRCFG             0x08d0
#define AR_Q5_CBRCFG             0x08d4
#define AR_Q6_CBRCFG             0x08d8
#define AR_Q7_CBRCFG             0x08dc
#define AR_Q8_CBRCFG             0x08e0
#define AR_Q9_CBRCFG             0x08e4
#define AR_QCBRCFG(_i)           (AR_Q0_CBRCFG + ((_i) << 2))
#define AR_Q_CBRCFG_INTERVAL     0x00FFFFFF
#define AR_Q_CBRCFG_INTERVAL_S   0
#define AR_Q_CBRCFG_OVF_THRESH   0xFF000000
#define AR_Q_CBRCFG_OVF_THRESH_S 24

#define AR_Q0_RDYTIMECFG           0x0900
#define AR_Q1_RDYTIMECFG           0x0904
#define AR_Q2_RDYTIMECFG           0x0908
#define AR_Q3_RDYTIMECFG           0x090c
#define AR_Q4_RDYTIMECFG           0x0910
#define AR_Q5_RDYTIMECFG           0x0914
#define AR_Q6_RDYTIMECFG           0x0918
#define AR_Q7_RDYTIMECFG           0x091c
#define AR_Q8_RDYTIMECFG           0x0920
#define AR_Q9_RDYTIMECFG           0x0924
#define AR_QRDYTIMECFG(_i)         (AR_Q0_RDYTIMECFG + ((_i) << 2))
#define AR_Q_RDYTIMECFG_DURATION   0x00FFFFFF
#define AR_Q_RDYTIMECFG_DURATION_S 0
#define AR_Q_RDYTIMECFG_EN         0x01000000

#define AR_Q_ONESHOTARM_SC       0x0940
#define AR_Q_ONESHOTARM_SC_M     0x000003FF
#define AR_Q_ONESHOTARM_SC_RESV0 0xFFFFFC00

#define AR_Q_ONESHOTARM_CC       0x0980
#define AR_Q_ONESHOTARM_CC_M     0x000003FF
#define AR_Q_ONESHOTARM_CC_RESV0 0xFFFFFC00

#define AR_Q0_MISC                      0x09c0
#define AR_Q1_MISC                      0x09c4
#define AR_Q2_MISC                      0x09c8
#define AR_Q3_MISC                      0x09cc
#define AR_Q4_MISC                      0x09d0
#define AR_Q5_MISC                      0x09d4
#define AR_Q6_MISC                      0x09d8
#define AR_Q7_MISC                      0x09dc
#define AR_Q8_MISC                      0x09e0
#define AR_Q9_MISC                      0x09e4
#define AR_QMISC(_i)                    (AR_Q0_MISC + ((_i) << 2))
#define AR_Q_MISC_FSP                   0x0000000F
#define AR_Q_MISC_FSP_ASAP              0
#define AR_Q_MISC_FSP_CBR               1
#define AR_Q_MISC_FSP_DBA_GATED         2
#define AR_Q_MISC_FSP_TIM_GATED         3
#define AR_Q_MISC_FSP_BEACON_SENT_GATED 4
#define AR_Q_MISC_FSP_BEACON_RCVD_GATED 5
#define AR_Q_MISC_ONE_SHOT_EN           0x00000010
#define AR_Q_MISC_CBR_INCR_DIS1         0x00000020
#define AR_Q_MISC_CBR_INCR_DIS0         0x00000040
#define AR_Q_MISC_BEACON_USE            0x00000080
#define AR_Q_MISC_CBR_EXP_CNTR_LIMIT_EN 0x00000100
#define AR_Q_MISC_RDYTIME_EXP_POLICY    0x00000200
#define AR_Q_MISC_RESET_CBR_EXP_CTR     0x00000400
#define AR_Q_MISC_DCU_EARLY_TERM_REQ    0x00000800
#define AR_Q_MISC_RESV0                 0xFFFFF000

#define AR_Q0_STS            0x0a00
#define AR_Q1_STS            0x0a04
#define AR_Q2_STS            0x0a08
#define AR_Q3_STS            0x0a0c
#define AR_Q4_STS            0x0a10
#define AR_Q5_STS            0x0a14
#define AR_Q6_STS            0x0a18
#define AR_Q7_STS            0x0a1c
#define AR_Q8_STS            0x0a20
#define AR_Q9_STS            0x0a24
#define AR_QSTS(_i)          (AR_Q0_STS + ((_i) << 2))
#define AR_Q_STS_PEND_FR_CNT 0x00000003
#define AR_Q_STS_RESV0       0x000000FC
#define AR_Q_STS_CBR_EXP_CNT 0x0000FF00
#define AR_Q_STS_RESV1       0xFFFF0000

#define AR_Q_RDYTIMESHDN   0x0a40
#define AR_Q_RDYTIMESHDN_M 0x000003FF

/* MAC Descriptor CRC check */
#define AR_Q_DESC_CRCCHK    0xa44
/* Enable CRC check on the descriptor fetched from host */
#define AR_Q_DESC_CRCCHK_EN 1

#define AR_NUM_DCU 10
#define AR_DCU_0   0x0001
#define AR_DCU_1   0x0002
#define AR_DCU_2   0x0004
#define AR_DCU_3   0x0008
#define AR_DCU_4   0x0010
#define AR_DCU_5   0x0020
#define AR_DCU_6   0x0040
#define AR_DCU_7   0x0080
#define AR_DCU_8   0x0100
#define AR_DCU_9   0x0200

#define AR_D0_QCUMASK      0x1000
#define AR_D1_QCUMASK      0x1004
#define AR_D2_QCUMASK      0x1008
#define AR_D3_QCUMASK      0x100c
#define AR_D4_QCUMASK      0x1010
#define AR_D5_QCUMASK      0x1014
#define AR_D6_QCUMASK      0x1018
#define AR_D7_QCUMASK      0x101c
#define AR_D8_QCUMASK      0x1020
#define AR_D9_QCUMASK      0x1024
#define AR_DQCUMASK(_i)    (AR_D0_QCUMASK + ((_i) << 2))
#define AR_D_QCUMASK       0x000003FF
#define AR_D_QCUMASK_RESV0 0xFFFFFC00

#define AR_D0_LCL_IFS        0x1040
#define AR_D1_LCL_IFS        0x1044
#define AR_D2_LCL_IFS        0x1048
#define AR_D3_LCL_IFS        0x104c
#define AR_D4_LCL_IFS        0x1050
#define AR_D5_LCL_IFS        0x1054
#define AR_D6_LCL_IFS        0x1058
#define AR_D7_LCL_IFS        0x105c
#define AR_D8_LCL_IFS        0x1060
#define AR_D9_LCL_IFS        0x1064
#define AR_DLCL_IFS(_i)      (AR_D0_LCL_IFS + ((_i) << 2))
#define AR_D_LCL_IFS_CWMIN   0x000003FF
#define AR_D_LCL_IFS_CWMIN_S 0
#define AR_D_LCL_IFS_CWMAX   0x000FFC00
#define AR_D_LCL_IFS_CWMAX_S 10
#define AR_D_LCL_IFS_AIFS    0x0FF00000
#define AR_D_LCL_IFS_AIFS_S  20

#define AR_D_LCL_IFS_RESV0 0xF0000000

#define AR_D0_RETRY_LIMIT         0x1080
#define AR_D1_RETRY_LIMIT         0x1084
#define AR_D2_RETRY_LIMIT         0x1088
#define AR_D3_RETRY_LIMIT         0x108c
#define AR_D4_RETRY_LIMIT         0x1090
#define AR_D5_RETRY_LIMIT         0x1094
#define AR_D6_RETRY_LIMIT         0x1098
#define AR_D7_RETRY_LIMIT         0x109c
#define AR_D8_RETRY_LIMIT         0x10a0
#define AR_D9_RETRY_LIMIT         0x10a4
#define AR_DRETRY_LIMIT(_i)       (AR_D0_RETRY_LIMIT + ((_i) << 2))
#define AR_D_RETRY_LIMIT_FR_SH    0x0000000F
#define AR_D_RETRY_LIMIT_FR_SH_S  0
#define AR_D_RETRY_LIMIT_STA_SH   0x00003F00
#define AR_D_RETRY_LIMIT_STA_SH_S 8
#define AR_D_RETRY_LIMIT_STA_LG   0x000FC000
#define AR_D_RETRY_LIMIT_STA_LG_S 14
#define AR_D_RETRY_LIMIT_RESV0    0xFFF00000

#define AR_D0_CHNTIME      0x10c0
#define AR_D1_CHNTIME      0x10c4
#define AR_D2_CHNTIME      0x10c8
#define AR_D3_CHNTIME      0x10cc
#define AR_D4_CHNTIME      0x10d0
#define AR_D5_CHNTIME      0x10d4
#define AR_D6_CHNTIME      0x10d8
#define AR_D7_CHNTIME      0x10dc
#define AR_D8_CHNTIME      0x10e0
#define AR_D9_CHNTIME      0x10e4
#define AR_DCHNTIME(_i)    (AR_D0_CHNTIME + ((_i) << 2))
#define AR_D_CHNTIME_DUR   0x000FFFFF
#define AR_D_CHNTIME_DUR_S 0
#define AR_D_CHNTIME_EN    0x00100000
#define AR_D_CHNTIME_RESV0 0xFFE00000

#define AR_D0_MISC                           0x1100
#define AR_D1_MISC                           0x1104
#define AR_D2_MISC                           0x1108
#define AR_D3_MISC                           0x110c
#define AR_D4_MISC                           0x1110
#define AR_D5_MISC                           0x1114
#define AR_D6_MISC                           0x1118
#define AR_D7_MISC                           0x111c
#define AR_D8_MISC                           0x1120
#define AR_D9_MISC                           0x1124
#define AR_DMISC(_i)                         (AR_D0_MISC + ((_i) << 2))
#define AR_D_MISC_BKOFF_THRESH               0x0000003F
#define AR_D_MISC_RETRY_CNT_RESET_EN         0x00000040
#define AR_D_MISC_CW_RESET_EN                0x00000080
#define AR_D_MISC_FRAG_WAIT_EN               0x00000100
#define AR_D_MISC_FRAG_BKOFF_EN              0x00000200
#define AR_D_MISC_CW_BKOFF_EN                0x00001000
#define AR_D_MISC_VIR_COL_HANDLING           0x0000C000
#define AR_D_MISC_VIR_COL_HANDLING_S         14
#define AR_D_MISC_VIR_COL_HANDLING_DEFAULT   0
#define AR_D_MISC_VIR_COL_HANDLING_IGNORE    1
#define AR_D_MISC_BEACON_USE                 0x00010000
#define AR_D_MISC_ARB_LOCKOUT_CNTRL          0x00060000
#define AR_D_MISC_ARB_LOCKOUT_CNTRL_S        17
#define AR_D_MISC_ARB_LOCKOUT_CNTRL_NONE     0
#define AR_D_MISC_ARB_LOCKOUT_CNTRL_INTRA_FR 1
#define AR_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL   2
#define AR_D_MISC_ARB_LOCKOUT_IGNORE         0x00080000
#define AR_D_MISC_SEQ_NUM_INCR_DIS           0x00100000
#define AR_D_MISC_POST_FR_BKOFF_DIS          0x00200000
#define AR_D_MISC_VIT_COL_CW_BKOFF_EN        0x00400000
#define AR_D_MISC_BLOWN_IFS_RETRY_EN         0x00800000
#define AR_D_MISC_RESV0                      0xFF000000

#define AR_D_SEQNUM 0x1140

#define AR_D_GBL_IFS_SIFS       0x1030
#define AR_D_GBL_IFS_SIFS_M     0x0000FFFF
#define AR_D_GBL_IFS_SIFS_RESV0 0xFFFFFFFF

#define AR_D_TXBLK_BASE            0x1038
#define AR_D_TXBLK_WRITE_BITMASK   0x0000FFFF
#define AR_D_TXBLK_WRITE_BITMASK_S 0
#define AR_D_TXBLK_WRITE_SLICE     0x000F0000
#define AR_D_TXBLK_WRITE_SLICE_S   16
#define AR_D_TXBLK_WRITE_DCU       0x00F00000
#define AR_D_TXBLK_WRITE_DCU_S     20
#define AR_D_TXBLK_WRITE_COMMAND   0x0F000000
#define AR_D_TXBLK_WRITE_COMMAND_S 24

#define AR_D_GBL_IFS_SLOT       0x1070
#define AR_D_GBL_IFS_SLOT_M     0x0000FFFF
#define AR_D_GBL_IFS_SLOT_RESV0 0xFFFF0000

#define AR_D_GBL_IFS_EIFS            0x10b0
#define AR_D_GBL_IFS_EIFS_M          0x0000FFFF
#define AR_D_GBL_IFS_EIFS_RESV0      0xFFFF0000
#define AR_D_GBL_IFS_EIFS_ASYNC_FIFO 363

#define AR_D_GBL_IFS_MISC                       0x10f0
#define AR_D_GBL_IFS_MISC_LFSR_SLICE_SEL        0x00000007
#define AR_D_GBL_IFS_MISC_TURBO_MODE            0x00000008
#define AR_D_GBL_IFS_MISC_USEC_DURATION         0x000FFC00
#define AR_D_GBL_IFS_MISC_DCU_ARBITER_DLY       0x00300000
#define AR_D_GBL_IFS_MISC_RANDOM_LFSR_SLICE_DIS 0x01000000
#define AR_D_GBL_IFS_MISC_SLOT_XMIT_WIND_LEN    0x06000000
#define AR_D_GBL_IFS_MISC_FORCE_XMIT_SLOT_BOUND 0x08000000
#define AR_D_GBL_IFS_MISC_IGNORE_BACKOFF        0x10000000

#define AR_D_FPCTL                  0x1230
#define AR_D_FPCTL_DCU              0x0000000F
#define AR_D_FPCTL_DCU_S            0
#define AR_D_FPCTL_PREFETCH_EN      0x00000010
#define AR_D_FPCTL_BURST_PREFETCH   0x00007FE0
#define AR_D_FPCTL_BURST_PREFETCH_S 5

#define AR_D_TXPSE        0x1270
#define AR_D_TXPSE_CTRL   0x000003FF
#define AR_D_TXPSE_RESV0  0x0000FC00
#define AR_D_TXPSE_STATUS 0x00010000
#define AR_D_TXPSE_RESV1  0xFFFE0000

#define AR_D_TXSLOTMASK     0x12f0
#define AR_D_TXSLOTMASK_NUM 0x0000000F

#define AR_CFG_LED                  0x1f04
#define AR_CFG_SCLK_RATE_IND        0x00000003
#define AR_CFG_SCLK_RATE_IND_S      0
#define AR_CFG_SCLK_32MHZ           0x00000000
#define AR_CFG_SCLK_4MHZ            0x00000001
#define AR_CFG_SCLK_1MHZ            0x00000002
#define AR_CFG_SCLK_32KHZ           0x00000003
#define AR_CFG_LED_BLINK_SLOW       0x00000008
#define AR_CFG_LED_BLINK_THRESH_SEL 0x00000070
#define AR_CFG_LED_MODE_SEL         0x00000380
#define AR_CFG_LED_MODE_SEL_S       7
#define AR_CFG_LED_POWER            0x00000280
#define AR_CFG_LED_POWER_S          7
#define AR_CFG_LED_NETWORK          0x00000300
#define AR_CFG_LED_NETWORK_S        7
#define AR_CFG_LED_MODE_PROP        0x0
#define AR_CFG_LED_MODE_RPROP       0x1
#define AR_CFG_LED_MODE_SPLIT       0x2
#define AR_CFG_LED_MODE_RAND        0x3
#define AR_CFG_LED_MODE_POWER_OFF   0x4
#define AR_CFG_LED_MODE_POWER_ON    0x5
#define AR_CFG_LED_MODE_NETWORK_OFF 0x4
#define AR_CFG_LED_MODE_NETWORK_ON  0x6
#define AR_CFG_LED_ASSOC_CTL        0x00000c00
#define AR_CFG_LED_ASSOC_CTL_S      10
#define AR_CFG_LED_ASSOC_NONE       0x0
#define AR_CFG_LED_ASSOC_ACTIVE     0x1
#define AR_CFG_LED_ASSOC_PENDING    0x2

#define AR_CFG_LED_BLINK_SLOW   0x00000008
#define AR_CFG_LED_BLINK_SLOW_S 3

#define AR_CFG_LED_BLINK_THRESH_SEL   0x00000070
#define AR_CFG_LED_BLINK_THRESH_SEL_S 4

#define AR_MAC_SLEEP            0x1f00
#define AR_MAC_SLEEP_MAC_AWAKE  0x00000000
#define AR_MAC_SLEEP_MAC_ASLEEP 0x00000001

#define AR_RC        0x4000
#define AR_RC_AHB    0x00000001
#define AR_RC_APB    0x00000002
#define AR_RC_HOSTIF 0x00000100

#define AR_WA               (0x4004)
#define AR_WA_BIT6          (1 << 6)
#define AR_WA_BIT7          (1 << 7)
#define AR_WA_BIT23         (1 << 23)
#define AR_WA_D3_L1_DISABLE (1 << 14)
#define AR_WA_UNTIE_RESET_EN      \
    (1 << 15) /* Enable PCI Reset \
                           to POR (power-on-reset) */
#define AR_WA_D3_TO_L1_DISABLE_REAL    (1 << 16)
#define AR_WA_ASPM_TIMER_BASED_DISABLE (1 << 17)
#define AR_WA_RESET_EN               \
    (1 << 18) /* Enable PCI-Reset to \
                 POR (bit 15) */
#define AR_WA_ANALOG_SHIFT (1 << 20)
#define AR_WA_POR_SHORT    (1 << 21) /* PCI-E Phy reset control */
#define AR_WA_BIT22        (1 << 22)
#define AR_WA_DEFAULT      0x0000073f

#define AR_PM_STATE                 0x4008
#define AR_PM_STATE_PME_D3COLD_VAUX 0x00100000

#define AR_HOST_TIMEOUT            (0x4018)
#define AR_HOST_TIMEOUT_APB_CNTR   0x0000FFFF
#define AR_HOST_TIMEOUT_APB_CNTR_S 0
#define AR_HOST_TIMEOUT_LCL_CNTR   0xFFFF0000
#define AR_HOST_TIMEOUT_LCL_CNTR_S 16

#define AR_EEPROM             0x401c
#define AR_EEPROM_ABSENT      0x00000100
#define AR_EEPROM_CORRUPT     0x00000200
#define AR_EEPROM_PROT_MASK   0x03FFFC00
#define AR_EEPROM_PROT_MASK_S 10

#define EEPROM_PROTECT_RP_0_31      0x0001
#define EEPROM_PROTECT_WP_0_31      0x0002
#define EEPROM_PROTECT_RP_32_63     0x0004
#define EEPROM_PROTECT_WP_32_63     0x0008
#define EEPROM_PROTECT_RP_64_127    0x0010
#define EEPROM_PROTECT_WP_64_127    0x0020
#define EEPROM_PROTECT_RP_128_191   0x0040
#define EEPROM_PROTECT_WP_128_191   0x0080
#define EEPROM_PROTECT_RP_192_255   0x0100
#define EEPROM_PROTECT_WP_192_255   0x0200
#define EEPROM_PROTECT_RP_256_511   0x0400
#define EEPROM_PROTECT_WP_256_511   0x0800
#define EEPROM_PROTECT_RP_512_1023  0x1000
#define EEPROM_PROTECT_WP_512_1023  0x2000
#define EEPROM_PROTECT_RP_1024_2047 0x4000
#define EEPROM_PROTECT_WP_1024_2047 0x8000

#define AR_SREV (0x4020)

#define AR_SREV_ID        (0x000000FF)
#define AR_SREV_VERSION   0x000000F0
#define AR_SREV_VERSION_S 4
#define AR_SREV_REVISION  0x00000007

#define AR_SREV_ID2             0xFFFFFFFF
#define AR_SREV_VERSION2        0xFFFC0000
#define AR_SREV_VERSION2_S      18
#define AR_SREV_TYPE2           0x0003F000
#define AR_SREV_TYPE2_S         12
#define AR_SREV_TYPE2_CHAIN     0x00001000
#define AR_SREV_TYPE2_HOST_MODE 0x00002000
#define AR_SREV_REVISION2       0x00000F00
#define AR_SREV_REVISION2_S     8

#define AR_RADIO_SREV_MAJOR   0xf0
#define AR_RAD5133_SREV_MAJOR 0xc0
#define AR_RAD2133_SREV_MAJOR 0xd0
#define AR_RAD5122_SREV_MAJOR 0xe0
#define AR_RAD2122_SREV_MAJOR 0xf0

#define AR_AHB_MODE                        0x4024
#define AR_AHB_EXACT_WR_EN                 0x00000000
#define AR_AHB_BUF_WR_EN                   0x00000001
#define AR_AHB_EXACT_RD_EN                 0x00000000
#define AR_AHB_CACHELINE_RD_EN             0x00000002
#define AR_AHB_PREFETCH_RD_EN              0x00000004
#define AR_AHB_PAGE_SIZE_1K                0x00000000
#define AR_AHB_PAGE_SIZE_2K                0x00000008
#define AR_AHB_PAGE_SIZE_4K                0x00000010
#define AR_AHB_CUSTOM_BURST_EN             0x000000C0
#define AR_AHB_CUSTOM_BURST_EN_S           6
#define AR_AHB_CUSTOM_BURST_ASYNC_FIFO_VAL 3

#define AR_INTR_RTC_IRQ         0x00000001
#define AR_INTR_MAC_IRQ         0x00000002
#define AR_INTR_EEP_PROT_ACCESS 0x00000004
#define AR_INTR_MAC_AWAKE       0x00020000
#define AR_INTR_MAC_ASLEEP      0x00040000
#define AR_INTR_SPURIOUS        0xFFFFFFFF

#define AR_INTR_SYNC_CAUSE     (0x4028)
#define AR_INTR_SYNC_CAUSE_CLR (0x4028)

#define AR_INTR_SYNC_ENABLE        (0x402c)
#define AR_INTR_SYNC_ENABLE_GPIO   0xFFFC0000
#define AR_INTR_SYNC_ENABLE_GPIO_S 18

#define AR_INTR_SYNC_RTC_IRQ               0x00000001
#define AR_INTR_SYNC_MAC_IRQ               0x00000002
#define AR_INTR_SYNC_EEPROM_ILLEGAL_ACCESS 0x00000004
#define AR_INTR_SYNC_APB_TIMEOUT           0x00000008
#define AR_INTR_SYNC_PCI_MODE_CONFLICT     0x00000010
#define AR_INTR_SYNC_HOST1_FATAL           0x00000020
#define AR_INTR_SYNC_HOST1_PERR            0x00000040
#define AR_INTR_SYNC_TRCV_FIFO_PERR        0x00000080
#define AR_INTR_SYNC_RADM_CPL_EP           0x00000100
#define AR_INTR_SYNC_RADM_CPL_DLLP_ABORT   0x00000200
#define AR_INTR_SYNC_RADM_CPL_TLP_ABORT    0x00000400
#define AR_INTR_SYNC_RADM_CPL_ECRC_ERR     0x00000800
#define AR_INTR_SYNC_RADM_CPL_TIMEOUT      0x00001000
#define AR_INTR_SYNC_LOCAL_TIMEOUT         0x00002000
#define AR_INTR_SYNC_PM_ACCESS             0x00004000
#define AR_INTR_SYNC_MAC_AWAKE             0x00008000
#define AR_INTR_SYNC_MAC_ASLEEP            0x00010000
#define AR_INTR_SYNC_MAC_SLEEP_ACCESS      0x00020000
#define AR_INTR_SYNC_ALL                   0x0003FFFF
#define AR_INTR_SYNC_DEFAULT                                                                       \
    (AR_INTR_SYNC_HOST1_FATAL | AR_INTR_SYNC_HOST1_PERR | AR_INTR_SYNC_RADM_CPL_EP |               \
     AR_INTR_SYNC_RADM_CPL_DLLP_ABORT | AR_INTR_SYNC_RADM_CPL_TLP_ABORT |                          \
     AR_INTR_SYNC_RADM_CPL_ECRC_ERR | AR_INTR_SYNC_RADM_CPL_TIMEOUT | AR_INTR_SYNC_LOCAL_TIMEOUT | \
     AR_INTR_SYNC_MAC_SLEEP_ACCESS)
#define AR_INTR_SYNC_SPURIOUS 0xFFFFFFFF

#define AR_INTR_ASYNC_MASK        (0x4030)
#define AR_INTR_ASYNC_MASK_GPIO   0xFFFC0000
#define AR_INTR_ASYNC_MASK_GPIO_S 18
#define AR_INTR_ASYNC_MASK_MCI    0x00000080
#define AR_INTR_ASYNC_MASK_MCI_S  7

#define AR_INTR_SYNC_MASK        (0x4034)
#define AR_INTR_SYNC_MASK_GPIO   0xFFFC0000
#define AR_INTR_SYNC_MASK_GPIO_S 18

#define AR_INTR_ASYNC_CAUSE_CLR (0x4038)
#define AR_INTR_ASYNC_CAUSE     (0x4038)
#define AR_INTR_ASYNC_CAUSE_MCI 0x00000080
#define AR_INTR_ASYNC_USED      (AR_INTR_MAC_IRQ | AR_INTR_ASYNC_CAUSE_MCI)

/* Asynchronous Interrupt Enable Register */
#define AR_INTR_ASYNC_ENABLE_MCI   0x00000080
#define AR_INTR_ASYNC_ENABLE_MCI_S 7

#define AR_INTR_ASYNC_ENABLE        (0x403c)
#define AR_INTR_ASYNC_ENABLE_GPIO   0xFFFC0000
#define AR_INTR_ASYNC_ENABLE_GPIO_S 18

#define AR_PCIE_SERDES      0x4040
#define AR_PCIE_SERDES2     0x4044
#define AR_PCIE_PM_CTRL     (0x4014)
#define AR_PCIE_PM_CTRL_ENA 0x00080000

#define AR_PCIE_PHY_REG3 0x18c08

/* Define correct GPIO numbers and MASK bits to indicate the WMAC
 * GPIO resource.
 * Allow SOC chips(AR9340, AR9531, AR9550, AR9561) to access all GPIOs
 * which rely on gpiolib framework. But restrict SOC AR9330 only to
 * access WMAC GPIO which has the same design with the old chips.
 */
#define AR_NUM_GPIO     14
#define AR9300_NUM_GPIO 16

#define AR_GPIO_MASK     0x00003FFF
#define AR9300_GPIO_MASK 0x0000F4FF

#define AR_GPIO_IN_OUT       (0x4048)
#define AR_GPIO_IN_VAL       0x0FFFC000
#define AR_GPIO_IN_VAL_S     14
#define AR928X_GPIO_IN_VAL   0x000FFC00
#define AR928X_GPIO_IN_VAL_S 10

#define AR_GPIO_IN           (0x404c)
#define AR9300_GPIO_IN_VAL   0x0001FFFF
#define AR9300_GPIO_IN_VAL_S 0

#define AR_GPIO_OE_OUT         (0x4050)
#define AR_GPIO_OE_OUT_DRV     0x3
#define AR_GPIO_OE_OUT_DRV_NO  0x0
#define AR_GPIO_OE_OUT_DRV_LOW 0x1
#define AR_GPIO_OE_OUT_DRV_HI  0x2
#define AR_GPIO_OE_OUT_DRV_ALL 0x3

#define AR_GPIO_INTR_POL       (0x4058)
#define AR_GPIO_INTR_POL_VAL   0x0001FFFF
#define AR_GPIO_INTR_POL_VAL_S 0

#define AR_GPIO_INPUT_EN_VAL                  (0x405c)
#define AR_GPIO_INPUT_EN_VAL_BT_PRIORITY_DEF  0x00000004
#define AR_GPIO_INPUT_EN_VAL_BT_PRIORITY_S    2
#define AR_GPIO_INPUT_EN_VAL_BT_FREQUENCY_DEF 0x00000008
#define AR_GPIO_INPUT_EN_VAL_BT_FREQUENCY_S   3
#define AR_GPIO_INPUT_EN_VAL_BT_ACTIVE_DEF    0x00000010
#define AR_GPIO_INPUT_EN_VAL_BT_ACTIVE_S      4
#define AR_GPIO_INPUT_EN_VAL_RFSILENT_DEF     0x00000080
#define AR_GPIO_INPUT_EN_VAL_RFSILENT_DEF_S   7
#define AR_GPIO_INPUT_EN_VAL_BT_PRIORITY_BB   0x00000400
#define AR_GPIO_INPUT_EN_VAL_BT_PRIORITY_BB_S 10
#define AR_GPIO_INPUT_EN_VAL_BT_ACTIVE_BB     0x00001000
#define AR_GPIO_INPUT_EN_VAL_BT_ACTIVE_BB_S   12
#define AR_GPIO_INPUT_EN_VAL_RFSILENT_BB      0x00008000
#define AR_GPIO_INPUT_EN_VAL_RFSILENT_BB_S    15
#define AR_GPIO_RTC_RESET_OVERRIDE_ENABLE     0x00010000
#define AR_GPIO_JTAG_DISABLE                  0x00020000

#define AR_GPIO_INPUT_MUX1               (0x4060)
#define AR_GPIO_INPUT_MUX1_BT_ACTIVE     0x000f0000
#define AR_GPIO_INPUT_MUX1_BT_ACTIVE_S   16
#define AR_GPIO_INPUT_MUX1_BT_PRIORITY   0x00000f00
#define AR_GPIO_INPUT_MUX1_BT_PRIORITY_S 8

#define AR_GPIO_INPUT_MUX2             (0x4064)
#define AR_GPIO_INPUT_MUX2_CLK25       0x0000000f
#define AR_GPIO_INPUT_MUX2_CLK25_S     0
#define AR_GPIO_INPUT_MUX2_RFSILENT    0x000000f0
#define AR_GPIO_INPUT_MUX2_RFSILENT_S  4
#define AR_GPIO_INPUT_MUX2_RTC_RESET   0x00000f00
#define AR_GPIO_INPUT_MUX2_RTC_RESET_S 8

#define AR_GPIO_OUTPUT_MUX1 (0x4068)
#define AR_GPIO_OUTPUT_MUX2 (0x406c)
#define AR_GPIO_OUTPUT_MUX3 (0x4070)

#define AR_INPUT_STATE (0x4074)

#define AR_EEPROM_STATUS_DATA               (0x4084)
#define AR_EEPROM_STATUS_DATA_VAL           0x0000ffff
#define AR_EEPROM_STATUS_DATA_VAL_S         0
#define AR_EEPROM_STATUS_DATA_BUSY          0x00010000
#define AR_EEPROM_STATUS_DATA_BUSY_ACCESS   0x00020000
#define AR_EEPROM_STATUS_DATA_PROT_ACCESS   0x00040000
#define AR_EEPROM_STATUS_DATA_ABSENT_ACCESS 0x00080000

#define AR_OBS (0x4088)

#define AR_GPIO_PDPU (0x4090)

#define AR_PCIE_MSI                            (0x40a4)
#define AR_PCIE_MSI_ENABLE                     0x00000001
#define AR_PCIE_MSI_HW_DBI_WR_EN               0x02000000
#define AR_PCIE_MSI_HW_INT_PENDING_ADDR        0xFFA0C1FF /* bits 8..11: value must be 0x5060 */
#define AR_PCIE_MSI_HW_INT_PENDING_ADDR_MSI_64 0xFFA0C9FF /* bits 8..11: value must be 0x5064 */

#define AR_INTR_PRIO_TX   0x00000001
#define AR_INTR_PRIO_RXLP 0x00000002
#define AR_INTR_PRIO_RXHP 0x00000004

#define AR_INTR_PRIO_SYNC_ENABLE        (0x40c4)
#define AR_INTR_PRIO_ASYNC_MASK         (0x40c8)
#define AR_INTR_PRIO_SYNC_MASK          (0x40cc)
#define AR_INTR_PRIO_ASYNC_ENABLE       (0x40d4)
#define AR_ENT_OTP                      0x40d8
#define AR_ENT_OTP_CHAIN2_DISABLE       0x00020000
#define AR_ENT_OTP_49GHZ_DISABLE        0x00100000
#define AR_ENT_OTP_MIN_PKT_SIZE_DISABLE 0x00800000

#define AR_CH0_BB_DPLL1          0x16180
#define AR_CH0_BB_DPLL1_REFDIV   0xF8000000
#define AR_CH0_BB_DPLL1_REFDIV_S 27
#define AR_CH0_BB_DPLL1_NINI     0x07FC0000
#define AR_CH0_BB_DPLL1_NINI_S   18
#define AR_CH0_BB_DPLL1_NFRAC    0x0003FFFF
#define AR_CH0_BB_DPLL1_NFRAC_S  0

#define AR_CH0_BB_DPLL2              0x16184
#define AR_CH0_BB_DPLL2_LOCAL_PLL    0x40000000
#define AR_CH0_BB_DPLL2_LOCAL_PLL_S  30
#define AR_CH0_DPLL2_KI              0x3C000000
#define AR_CH0_DPLL2_KI_S            26
#define AR_CH0_DPLL2_KD              0x03F80000
#define AR_CH0_DPLL2_KD_S            19
#define AR_CH0_BB_DPLL2_EN_NEGTRIG   0x00040000
#define AR_CH0_BB_DPLL2_EN_NEGTRIG_S 18
#define AR_CH0_BB_DPLL2_PLL_PWD      0x00010000
#define AR_CH0_BB_DPLL2_PLL_PWD_S    16
#define AR_CH0_BB_DPLL2_OUTDIV       0x0000E000
#define AR_CH0_BB_DPLL2_OUTDIV_S     13

#define AR_CH0_BB_DPLL3               0x16188
#define AR_CH0_BB_DPLL3_PHASE_SHIFT   0x3F800000
#define AR_CH0_BB_DPLL3_PHASE_SHIFT_S 23

#define AR_CH0_DDR_DPLL2           0x16244
#define AR_CH0_DDR_DPLL3           0x16248
#define AR_CH0_DPLL3_PHASE_SHIFT   0x3F800000
#define AR_CH0_DPLL3_PHASE_SHIFT_S 23
#define AR_PHY_CCA_NOM_VAL_2GHZ    -118

#define AR_RTC_9300_PLL_DIV      0x000003ff
#define AR_RTC_9300_PLL_DIV_S    0
#define AR_RTC_9300_PLL_REFDIV   0x00003C00
#define AR_RTC_9300_PLL_REFDIV_S 10
#define AR_RTC_9300_PLL_CLKSEL   0x0000C000
#define AR_RTC_9300_PLL_CLKSEL_S 14
#define AR_RTC_9300_PLL_BYPASS   0x00010000

#define AR_RTC_BASE          0x00020000
#define AR_RTC_RC            (0x7000)
#define AR_RTC_RC_M          0x00000003
#define AR_RTC_RC_MAC_WARM   0x00000001
#define AR_RTC_RC_MAC_COLD   0x00000002
#define AR_RTC_RC_COLD_RESET 0x00000004
#define AR_RTC_RC_WARM_RESET 0x00000008

/* Crystal Control */
#define AR_RTC_XTAL_CONTROL 0x7004

/* Reg Control 0 */
#define AR_RTC_REG_CONTROL0 0x7008

/* Reg Control 1 */
#define AR_RTC_REG_CONTROL1               0x700c
#define AR_RTC_REG_CONTROL1_SWREG_PROGRAM 0x00000001

#define AR_RTC_PLL_CONTROL (0x7014)

#define AR_RTC_PLL_CONTROL2 0x703c

#define AR_RTC_PLL_DIV      0x0000001f
#define AR_RTC_PLL_DIV_S    0
#define AR_RTC_PLL_DIV2     0x00000020
#define AR_RTC_PLL_REFDIV_5 0x000000c0
#define AR_RTC_PLL_CLKSEL   0x00000300
#define AR_RTC_PLL_CLKSEL_S 8
#define AR_RTC_PLL_BYPASS   0x00010000
#define AR_RTC_PLL_NOPWD    0x00040000
#define AR_RTC_PLL_NOPWD_S  18

#define PLL3              0x16188
#define PLL3_DO_MEAS_MASK 0x40000000
#define PLL4              0x1618c
#define PLL4_MEAS_DONE    0x8
#define SQSUM_DVC_MASK    0x007ffff8

#define AR_RTC_RESET    (0x7040)
#define AR_RTC_RESET_EN (0x00000001)

#define AR_RTC_STATUS (0x7044)

#define AR_RTC_STATUS_M (0x0000000f)

#define AR_RTC_PM_STATUS_M 0x0000000f

#define AR_RTC_STATUS_SHUTDOWN 0x00000001
#define AR_RTC_STATUS_ON       0x00000002
#define AR_RTC_STATUS_SLEEP    0x00000004
#define AR_RTC_STATUS_WAKEUP   0x00000008

#define AR_RTC_SLEEP_CLK         (0x7048)
#define AR_RTC_FORCE_DERIVED_CLK 0x2
#define AR_RTC_FORCE_SWREG_PRD   0x00000004

#define AR_RTC_FORCE_WAKE        (0x704c)
#define AR_RTC_FORCE_WAKE_EN     0x00000001
#define AR_RTC_FORCE_WAKE_ON_INT 0x00000002

#define AR_RTC_INTR_CAUSE (0x7050)

#define AR_RTC_INTR_ENABLE (0x7054)

#define AR_RTC_INTR_MASK (0x7058)

#define AR_RTC_KEEP_AWAKE 0x7034

/* RTC_DERIVED_* - only for AR9100 */

#define AR_RTC_DERIVED_CLK          (0x7038)
#define AR_RTC_DERIVED_CLK_PERIOD   0x0000fffe
#define AR_RTC_DERIVED_CLK_PERIOD_S 1

#define AR_SEQ_MASK 0x8060

#define AR_AN_RF2G1_CH0      0x7810
#define AR_AN_RF2G1_CH0_OB   0x03800000
#define AR_AN_RF2G1_CH0_OB_S 23
#define AR_AN_RF2G1_CH0_DB   0x1C000000
#define AR_AN_RF2G1_CH0_DB_S 26

#define AR_AN_RF5G1_CH0       0x7818
#define AR_AN_RF5G1_CH0_OB5   0x00070000
#define AR_AN_RF5G1_CH0_OB5_S 16
#define AR_AN_RF5G1_CH0_DB5   0x00380000
#define AR_AN_RF5G1_CH0_DB5_S 19

#define AR_AN_RF2G1_CH1      0x7834
#define AR_AN_RF2G1_CH1_OB   0x03800000
#define AR_AN_RF2G1_CH1_OB_S 23
#define AR_AN_RF2G1_CH1_DB   0x1C000000
#define AR_AN_RF2G1_CH1_DB_S 26

#define AR_AN_RF5G1_CH1       0x783C
#define AR_AN_RF5G1_CH1_OB5   0x00070000
#define AR_AN_RF5G1_CH1_OB5_S 16
#define AR_AN_RF5G1_CH1_DB5   0x00380000
#define AR_AN_RF5G1_CH1_DB5_S 19

#define AR_AN_TOP1             0x7890
#define AR_AN_TOP1_DACIPMODE   0x00040000
#define AR_AN_TOP1_DACIPMODE_S 18

#define AR_AN_TOP2               0x7894
#define AR_AN_TOP2_XPABIAS_LVL   0xC0000000
#define AR_AN_TOP2_XPABIAS_LVL_S 30
#define AR_AN_TOP2_LOCALBIAS     0x00200000
#define AR_AN_TOP2_LOCALBIAS_S   21
#define AR_AN_TOP2_PWDCLKIND     0x00400000
#define AR_AN_TOP2_PWDCLKIND_S   22

#define AR_AN_SYNTH9           0x7868
#define AR_AN_SYNTH9_REFDIVA   0xf8000000
#define AR_AN_SYNTH9_REFDIVA_S 27

#define AR_STA_ID1_STA_AP          0x00010000
#define AR_STA_ID1_ADHOC           0x00020000
#define AR_STA_ID1_PWR_SAV         0x00040000
#define AR_STA_ID1_KSRCHDIS        0x00080000
#define AR_STA_ID1_PCF             0x00100000
#define AR_STA_ID1_USE_DEFANT      0x00200000
#define AR_STA_ID1_DEFANT_UPDATE   0x00400000
#define AR_STA_ID1_RTS_USE_DEF     0x00800000
#define AR_STA_ID1_ACKCTS_6MB      0x01000000
#define AR_STA_ID1_BASE_RATE_11B   0x02000000
#define AR_STA_ID1_SECTOR_SELF_GEN 0x04000000
#define AR_STA_ID1_CRPT_MIC_ENABLE 0x08000000
#define AR_STA_ID1_KSRCH_MODE      0x10000000
#define AR_STA_ID1_PRESERVE_SEQNUM 0x20000000
#define AR_STA_ID1_CBCIV_ENDIAN    0x40000000
#define AR_STA_ID1_MCAST_KSRCH     0x80000000

#define AR_BSS_ID0       0x8008
#define AR_BSS_ID1       0x800C
#define AR_BSS_ID1_U16   0x0000FFFF
#define AR_BSS_ID1_AID   0x07FF0000
#define AR_BSS_ID1_AID_S 16

#define AR_BCN_RSSI_AVE      0x8010
#define AR_BCN_RSSI_AVE_MASK 0x00000FFF

#define AR_TIME_OUT       0x8014
#define AR_TIME_OUT_ACK   0x00003FFF
#define AR_TIME_OUT_ACK_S 0
#define AR_TIME_OUT_CTS   0x3FFF0000
#define AR_TIME_OUT_CTS_S 16

#define AR_RSSI_THR          0x8018
#define AR_RSSI_THR_MASK     0x000000FF
#define AR_RSSI_THR_BM_THR   0x0000FF00
#define AR_RSSI_THR_BM_THR_S 8
#define AR_RSSI_BCN_WEIGHT   0x1F000000
#define AR_RSSI_BCN_WEIGHT_S 24
#define AR_RSSI_BCN_RSSI_RST 0x20000000

#define AR_USEC            0x801c
#define AR_USEC_USEC       0x0000007F
#define AR_USEC_TX_LAT     0x007FC000
#define AR_USEC_TX_LAT_S   14
#define AR_USEC_RX_LAT     0x1F800000
#define AR_USEC_RX_LAT_S   23
#define AR_USEC_ASYNC_FIFO 0x12E00074

#define AR_RESET_TSF       0x8020
#define AR_RESET_TSF_ONCE  0x01000000
#define AR_RESET_TSF2_ONCE 0x02000000

#define AR_MAX_CFP_DUR 0x8038
#define AR_CFP_VAL     0x0000FFFF

#define AR_RX_FILTER 0x803C

#define AR_MCAST_FIL0 0x8040
#define AR_MCAST_FIL1 0x8044

/*
 * AR_DIAG_SW - Register which can be used for diagnostics and testing purposes.
 *
 * The force RX abort (AR_DIAG_RX_ABORT, bit 25) can be used in conjunction with
 * RX block (AR_DIAG_RX_DIS, bit 5) to help fast channel change to shut down
 * receive. The force RX abort bit will kill any frame which is currently being
 * transferred between the MAC and baseband. The RX block bit (AR_DIAG_RX_DIS)
 * will prevent any new frames from getting started.
 */
#define AR_DIAG_SW                 0x8048
#define AR_DIAG_CACHE_ACK          0x00000001
#define AR_DIAG_ACK_DIS            0x00000002
#define AR_DIAG_CTS_DIS            0x00000004
#define AR_DIAG_ENCRYPT_DIS        0x00000008
#define AR_DIAG_DECRYPT_DIS        0x00000010
#define AR_DIAG_RX_DIS             0x00000020 /* RX block */
#define AR_DIAG_LOOP_BACK          0x00000040
#define AR_DIAG_CORR_FCS           0x00000080
#define AR_DIAG_CHAN_INFO          0x00000100
#define AR_DIAG_SCRAM_SEED         0x0001FE00
#define AR_DIAG_SCRAM_SEED_S       8
#define AR_DIAG_FRAME_NV0          0x00020000
#define AR_DIAG_OBS_PT_SEL1        0x000C0000
#define AR_DIAG_OBS_PT_SEL1_S      18
#define AR_DIAG_OBS_PT_SEL2        0x08000000
#define AR_DIAG_OBS_PT_SEL2_S      27
#define AR_DIAG_FORCE_RX_CLEAR     0x00100000 /* force rx_clear high */
#define AR_DIAG_IGNORE_VIRT_CS     0x00200000
#define AR_DIAG_FORCE_CH_IDLE_HIGH 0x00400000
#define AR_DIAG_EIFS_CTRL_ENA      0x00800000
#define AR_DIAG_DUAL_CHAIN_INFO    0x01000000
#define AR_DIAG_RX_ABORT           0x02000000 /* Force RX abort */
#define AR_DIAG_SATURATE_CYCLE_CNT 0x04000000
#define AR_DIAG_OBS_PT_SEL2        0x08000000
#define AR_DIAG_RX_CLEAR_CTL_LOW   0x10000000
#define AR_DIAG_RX_CLEAR_EXT_LOW   0x20000000

#define AR_TSF_L32 0x804c
#define AR_TSF_U32 0x8050

#define AR_TST_ADDAC   0x8054
#define AR_DEF_ANTENNA 0x8058

#define AR_AES_MUTE_MASK0       0x805c
#define AR_AES_MUTE_MASK0_FC    0x0000FFFF
#define AR_AES_MUTE_MASK0_QOS   0xFFFF0000
#define AR_AES_MUTE_MASK0_QOS_S 16

#define AR_AES_MUTE_MASK1           0x8060
#define AR_AES_MUTE_MASK1_SEQ       0x0000FFFF
#define AR_AES_MUTE_MASK1_FC_MGMT   0xFFFF0000
#define AR_AES_MUTE_MASK1_FC_MGMT_S 16

#define AR_GATED_CLKS     0x8064
#define AR_GATED_CLKS_TX  0x00000002
#define AR_GATED_CLKS_RX  0x00000004
#define AR_GATED_CLKS_REG 0x00000008

#define AR_OBS_BUS_CTRL  0x8068
#define AR_OBS_BUS_SEL_1 0x00040000
#define AR_OBS_BUS_SEL_2 0x00080000
#define AR_OBS_BUS_SEL_3 0x000C0000
#define AR_OBS_BUS_SEL_4 0x08040000
#define AR_OBS_BUS_SEL_5 0x08080000

#define AR_OBS_BUS_1             0x806c
#define AR_OBS_BUS_1_PCU         0x00000001
#define AR_OBS_BUS_1_RX_END      0x00000002
#define AR_OBS_BUS_1_RX_WEP      0x00000004
#define AR_OBS_BUS_1_RX_BEACON   0x00000008
#define AR_OBS_BUS_1_RX_FILTER   0x00000010
#define AR_OBS_BUS_1_TX_HCF      0x00000020
#define AR_OBS_BUS_1_QUIET_TIME  0x00000040
#define AR_OBS_BUS_1_CHAN_IDLE   0x00000080
#define AR_OBS_BUS_1_TX_HOLD     0x00000100
#define AR_OBS_BUS_1_TX_FRAME    0x00000200
#define AR_OBS_BUS_1_RX_FRAME    0x00000400
#define AR_OBS_BUS_1_RX_CLEAR    0x00000800
#define AR_OBS_BUS_1_WEP_STATE   0x0003F000
#define AR_OBS_BUS_1_WEP_STATE_S 12
#define AR_OBS_BUS_1_RX_STATE    0x01F00000
#define AR_OBS_BUS_1_RX_STATE_S  20
#define AR_OBS_BUS_1_TX_STATE    0x7E000000
#define AR_OBS_BUS_1_TX_STATE_S  25

#define AR_LAST_TSTP  0x8080
#define AR_NAV        0x8084
#define AR_RTS_OK     0x8088
#define AR_RTS_FAIL   0x808c
#define AR_ACK_FAIL   0x8090
#define AR_FCS_FAIL   0x8094
#define AR_BEACON_CNT 0x8098

#define AR_SLEEP1               0x80d4
#define AR_SLEEP1_ASSUME_DTIM   0x00080000
#define AR_SLEEP1_CAB_TIMEOUT   0xFFE00000
#define AR_SLEEP1_CAB_TIMEOUT_S 21

#define AR_SLEEP2                  0x80d8
#define AR_SLEEP2_BEACON_TIMEOUT   0xFFE00000
#define AR_SLEEP2_BEACON_TIMEOUT_S 21

#define AR_TPC         0x80e8
#define AR_TPC_ACK     0x0000003f
#define AR_TPC_ACK_S   0
#define AR_TPC_CTS     0x00003f00
#define AR_TPC_CTS_S   8
#define AR_TPC_CHIRP   0x003f0000
#define AR_TPC_CHIRP_S 16
#define AR_TPC_RPT     0x3f000000
#define AR_TPC_RPT_S   24

#define AR_QUIET1                        0x80fc
#define AR_QUIET1_NEXT_QUIET_S           0
#define AR_QUIET1_NEXT_QUIET_M           0x0000ffff
#define AR_QUIET1_QUIET_ENABLE           0x00010000
#define AR_QUIET1_QUIET_ACK_CTS_ENABLE   0x00020000
#define AR_QUIET1_QUIET_ACK_CTS_ENABLE_S 17
#define AR_QUIET2                        0x8100
#define AR_QUIET2_QUIET_PERIOD_S         0
#define AR_QUIET2_QUIET_PERIOD_M         0x0000ffff
#define AR_QUIET2_QUIET_DUR_S            16
#define AR_QUIET2_QUIET_DUR              0xffff0000

#define AR_TSF_PARM        0x8104
#define AR_TSF_INCREMENT_M 0x000000ff
#define AR_TSF_INCREMENT_S 0x00

#define AR_QOS_NO_ACK            0x8108
#define AR_QOS_NO_ACK_TWO_BIT    0x0000000f
#define AR_QOS_NO_ACK_TWO_BIT_S  0
#define AR_QOS_NO_ACK_BIT_OFF    0x00000070
#define AR_QOS_NO_ACK_BIT_OFF_S  4
#define AR_QOS_NO_ACK_BYTE_OFF   0x00000180
#define AR_QOS_NO_ACK_BYTE_OFF_S 7

#define AR_PHY_ERR 0x810c

#define AR_PHY_ERR_DCHIRP      0x00000008
#define AR_PHY_ERR_RADAR       0x00000020
#define AR_PHY_ERR_OFDM_TIMING 0x00020000
#define AR_PHY_ERR_CCK_TIMING  0x02000000

#define AR_RXFIFO_CFG 0x8114

#define AR_MIC_QOS_CONTROL 0x8118
#define AR_MIC_QOS_SELECT  0x811c

#define AR_PCU_MISC                     0x8120
#define AR_PCU_FORCE_BSSID_MATCH        0x00000001
#define AR_PCU_MIC_NEW_LOC_ENA          0x00000004
#define AR_PCU_TX_ADD_TSF               0x00000008
#define AR_PCU_CCK_SIFS_MODE            0x00000010
#define AR_PCU_RX_ANT_UPDT              0x00000800
#define AR_PCU_TXOP_TBTT_LIMIT_ENA      0x00001000
#define AR_PCU_MISS_BCN_IN_SLEEP        0x00004000
#define AR_PCU_BUG_12306_FIX_ENA        0x00020000
#define AR_PCU_FORCE_QUIET_COLL         0x00040000
#define AR_PCU_TBTT_PROTECT             0x00200000
#define AR_PCU_CLEAR_VMF                0x01000000
#define AR_PCU_CLEAR_BA_VALID           0x04000000
#define AR_PCU_ALWAYS_PERFORM_KEYSEARCH 0x10000000

#define AR_PCU_BT_ANT_PREVENT_RX   0x00100000
#define AR_PCU_BT_ANT_PREVENT_RX_S 20

#define AR_FILT_OFDM       0x8124
#define AR_FILT_OFDM_COUNT 0x00FFFFFF

#define AR_FILT_CCK       0x8128
#define AR_FILT_CCK_COUNT 0x00FFFFFF

#define AR_PHY_ERR_1       0x812c
#define AR_PHY_ERR_1_COUNT 0x00FFFFFF
#define AR_PHY_ERR_MASK_1  0x8130

#define AR_PHY_ERR_2       0x8134
#define AR_PHY_ERR_2_COUNT 0x00FFFFFF
#define AR_PHY_ERR_MASK_2  0x8138

#define AR_PHY_COUNTMAX    (3 << 22)
#define AR_MIBCNT_INTRMASK (3 << 22)

#define AR_TSFOOR_THRESHOLD     0x813c
#define AR_TSFOOR_THRESHOLD_VAL 0x0000FFFF

#define AR_PHY_ERR_EIFS_MASK 0x8144

#define AR_PHY_ERR_3       0x8168
#define AR_PHY_ERR_3_COUNT 0x00FFFFFF
#define AR_PHY_ERR_MASK_3  0x816c

#define AR_BT_COEX_MODE           0x8170
#define AR_BT_TIME_EXTEND         0x000000ff
#define AR_BT_TIME_EXTEND_S       0
#define AR_BT_TXSTATE_EXTEND      0x00000100
#define AR_BT_TXSTATE_EXTEND_S    8
#define AR_BT_TX_FRAME_EXTEND     0x00000200
#define AR_BT_TX_FRAME_EXTEND_S   9
#define AR_BT_MODE                0x00000c00
#define AR_BT_MODE_S              10
#define AR_BT_QUIET               0x00001000
#define AR_BT_QUIET_S             12
#define AR_BT_QCU_THRESH          0x0001e000
#define AR_BT_QCU_THRESH_S        13
#define AR_BT_RX_CLEAR_POLARITY   0x00020000
#define AR_BT_RX_CLEAR_POLARITY_S 17
#define AR_BT_PRIORITY_TIME       0x00fc0000
#define AR_BT_PRIORITY_TIME_S     18
#define AR_BT_FIRST_SLOT_TIME     0xff000000
#define AR_BT_FIRST_SLOT_TIME_S   24

#define AR_BT_COEX_WEIGHT       0x8174
#define AR_BT_COEX_WGHT         0xff55
#define AR_STOMP_ALL_WLAN_WGHT  0xfcfc
#define AR_STOMP_LOW_WLAN_WGHT  0xa8a8
#define AR_STOMP_NONE_WLAN_WGHT 0x0000
#define AR_BTCOEX_BT_WGHT       0x0000ffff
#define AR_BTCOEX_BT_WGHT_S     0
#define AR_BTCOEX_WL_WGHT       0xffff0000
#define AR_BTCOEX_WL_WGHT_S     16

#define AR_BT_COEX_WL_WEIGHTS0     0x8174
#define AR_BT_COEX_WL_WEIGHTS1     0x81c4
#define AR_MCI_COEX_WL_WEIGHTS(_i) (0x18b0 + (_i << 2))
#define AR_BT_COEX_BT_WEIGHTS(_i)  (0x83ac + (_i << 2))

#define AR9300_BT_WGHT 0xcccc4444

#define AR_BT_COEX_MODE2                0x817c
#define AR_BT_BCN_MISS_THRESH           0x000000ff
#define AR_BT_BCN_MISS_THRESH_S         0
#define AR_BT_BCN_MISS_CNT              0x0000ff00
#define AR_BT_BCN_MISS_CNT_S            8
#define AR_BT_HOLD_RX_CLEAR             0x00010000
#define AR_BT_HOLD_RX_CLEAR_S           16
#define AR_BT_PROTECT_BT_AFTER_WAKEUP   0x00080000
#define AR_BT_PROTECT_BT_AFTER_WAKEUP_S 19
#define AR_BT_DISABLE_BT_ANT            0x00100000
#define AR_BT_DISABLE_BT_ANT_S          20
#define AR_BT_QUIET_2_WIRE              0x00200000
#define AR_BT_QUIET_2_WIRE_S            21
#define AR_BT_WL_ACTIVE_MODE            0x00c00000
#define AR_BT_WL_ACTIVE_MODE_S          22
#define AR_BT_WL_TXRX_SEPARATE          0x01000000
#define AR_BT_WL_TXRX_SEPARATE_S        24
#define AR_BT_RS_DISCARD_EXTEND         0x02000000
#define AR_BT_RS_DISCARD_EXTEND_S       25
#define AR_BT_TSF_BT_ACTIVE_CTRL        0x0c000000
#define AR_BT_TSF_BT_ACTIVE_CTRL_S      26
#define AR_BT_TSF_BT_PRIORITY_CTRL      0x30000000
#define AR_BT_TSF_BT_PRIORITY_CTRL_S    28
#define AR_BT_INTERRUPT_ENABLE          0x40000000
#define AR_BT_INTERRUPT_ENABLE_S        30
#define AR_BT_PHY_ERR_BT_COLL_ENABLE    0x80000000
#define AR_BT_PHY_ERR_BT_COLL_ENABLE_S  31

#define AR_TXSIFS              0x81d0
#define AR_TXSIFS_TIME         0x000000FF
#define AR_TXSIFS_TX_LATENCY   0x00000F00
#define AR_TXSIFS_TX_LATENCY_S 8
#define AR_TXSIFS_ACK_SHIFT    0x00007000
#define AR_TXSIFS_ACK_SHIFT_S  12

#define AR_BT_COEX_MODE3                  0x81d4
#define AR_BT_WL_ACTIVE_TIME              0x000000ff
#define AR_BT_WL_ACTIVE_TIME_S            0
#define AR_BT_WL_QC_TIME                  0x0000ff00
#define AR_BT_WL_QC_TIME_S                8
#define AR_BT_ALLOW_CONCURRENT_ACCESS     0x000f0000
#define AR_BT_ALLOW_CONCURRENT_ACCESS_S   16
#define AR_BT_AGC_SATURATION_CNT_ENABLE   0x00100000
#define AR_BT_AGC_SATURATION_CNT_ENABLE_S 20

#define AR_TXOP_X     0x81ec
#define AR_TXOP_X_VAL 0x000000FF

#define AR_TXOP_0_3   0x81f0
#define AR_TXOP_4_7   0x81f4
#define AR_TXOP_8_11  0x81f8
#define AR_TXOP_12_15 0x81fc

#define AR_NEXT_NDP2_TIMER              0x8180
#define AR_GEN_TIMER_BANK_1_LEN         8
#define AR_FIRST_NDP_TIMER              7
#define AR_NDP2_PERIOD                  0x81a0
#define AR_NDP2_TIMER_MODE              0x81c0
#define AR_GEN_TIMERS2_MODE_ENABLE_MASK 0x000000FF

#define AR_GEN_TIMERS(_i)        (0x8200 + ((_i) << 2))
#define AR_NEXT_TBTT_TIMER       AR_GEN_TIMERS(0)
#define AR_NEXT_DMA_BEACON_ALERT AR_GEN_TIMERS(1)
#define AR_NEXT_SWBA             AR_GEN_TIMERS(2)
#define AR_NEXT_CFP              AR_GEN_TIMERS(2)
#define AR_NEXT_HCF              AR_GEN_TIMERS(3)
#define AR_NEXT_TIM              AR_GEN_TIMERS(4)
#define AR_NEXT_DTIM             AR_GEN_TIMERS(5)
#define AR_NEXT_QUIET_TIMER      AR_GEN_TIMERS(6)
#define AR_NEXT_NDP_TIMER        AR_GEN_TIMERS(7)

#define AR_BEACON_PERIOD     AR_GEN_TIMERS(8)
#define AR_DMA_BEACON_PERIOD AR_GEN_TIMERS(9)
#define AR_SWBA_PERIOD       AR_GEN_TIMERS(10)
#define AR_HCF_PERIOD        AR_GEN_TIMERS(11)
#define AR_TIM_PERIOD        AR_GEN_TIMERS(12)
#define AR_DTIM_PERIOD       AR_GEN_TIMERS(13)
#define AR_QUIET_PERIOD      AR_GEN_TIMERS(14)
#define AR_NDP_PERIOD        AR_GEN_TIMERS(15)

#define AR_TIMER_MODE             0x8240
#define AR_TBTT_TIMER_EN          0x00000001
#define AR_DBA_TIMER_EN           0x00000002
#define AR_SWBA_TIMER_EN          0x00000004
#define AR_HCF_TIMER_EN           0x00000008
#define AR_TIM_TIMER_EN           0x00000010
#define AR_DTIM_TIMER_EN          0x00000020
#define AR_QUIET_TIMER_EN         0x00000040
#define AR_NDP_TIMER_EN           0x00000080
#define AR_TIMER_OVERFLOW_INDEX   0x00000700
#define AR_TIMER_OVERFLOW_INDEX_S 8
#define AR_TIMER_THRESH           0xFFFFF000
#define AR_TIMER_THRESH_S         12

#define AR_SLP32_MODE             0x8244
#define AR_SLP32_HALF_CLK_LATENCY 0x000FFFFF
#define AR_SLP32_ENA              0x00100000
#define AR_SLP32_TSF_WRITE_STATUS 0x00200000

#define AR_SLP32_WAKE          0x8248
#define AR_SLP32_WAKE_XTL_TIME 0x0000FFFF

#define AR_SLP32_INC     0x824c
#define AR_SLP32_TST_INC 0x000FFFFF

#define AR_SLP_CNT       0x8250
#define AR_SLP_CYCLE_CNT 0x8254

#define AR_SLP_MIB_CTRL    0x8258
#define AR_SLP_MIB_CLEAR   0x00000001
#define AR_SLP_MIB_PENDING 0x00000002

#define AR_MAC_PCU_LOGIC_ANALYZER             0x8264
#define AR_MAC_PCU_LOGIC_ANALYZER_DISBUG20768 0x20000000

#define AR_2040_MODE            0x8318
#define AR_2040_JOINED_RX_CLEAR 0x00000001

#define AR_EXTRCCNT 0x8328

#define AR_SELFGEN_MASK 0x832c

#define AR_PCU_TXBUF_CTRL             0x8340
#define AR_PCU_TXBUF_CTRL_SIZE_MASK   0x7FF
#define AR_PCU_TXBUF_CTRL_USABLE_SIZE 0x700

#define AR_PCU_MISC_MODE2                            0x8344
#define AR_PCU_MISC_MODE2_MGMT_CRYPTO_ENABLE         0x00000002
#define AR_PCU_MISC_MODE2_NO_CRYPTO_FOR_NON_DATA_PKT 0x00000004

#define AR_PCU_MISC_MODE2_RESERVED                        0x00000038
#define AR_PCU_MISC_MODE2_ADHOC_MCAST_KEYID_ENABLE        0x00000040
#define AR_PCU_MISC_MODE2_CFP_IGNORE                      0x00000080
#define AR_PCU_MISC_MODE2_MGMT_QOS                        0x0000FF00
#define AR_PCU_MISC_MODE2_MGMT_QOS_S                      8
#define AR_PCU_MISC_MODE2_ENABLE_LOAD_NAV_BEACON_DURATION 0x00010000
#define AR_PCU_MISC_MODE2_ENABLE_AGGWEP                   0x00020000
#define AR_PCU_MISC_MODE2_HWWAR1                          0x00100000
#define AR_PCU_MISC_MODE2_HWWAR2                          0x02000000
#define AR_PCU_MISC_MODE2_RESERVED2                       0xFFFE0000

#define AR_PCU_MISC_MODE3 0x83d0

#define AR_MAC_PCU_ASYNC_FIFO_REG3              0x8358
#define AR_MAC_PCU_ASYNC_FIFO_REG3_DATAPATH_SEL 0x00000400
#define AR_MAC_PCU_ASYNC_FIFO_REG3_SOFT_RESET   0x80000000
#define AR_MAC_PCU_GEN_TIMER_TSF_SEL            0x83d8

#define AR_DIRECT_CONNECT 0x83a0
#define AR_DC_AP_STA_EN   0x00000001
#define AR_DC_TSF2_ENABLE 0x00000001

#define AR_AES_MUTE_MASK0       0x805c
#define AR_AES_MUTE_MASK0_FC    0x0000FFFF
#define AR_AES_MUTE_MASK0_QOS   0xFFFF0000
#define AR_AES_MUTE_MASK0_QOS_S 16

#define AR_AES_MUTE_MASK1           0x8060
#define AR_AES_MUTE_MASK1_SEQ       0x0000FFFF
#define AR_AES_MUTE_MASK1_SEQ_S     0
#define AR_AES_MUTE_MASK1_FC_MGMT   0xFFFF0000
#define AR_AES_MUTE_MASK1_FC_MGMT_S 16

#define AR_RATE_DURATION_0   0x8700
#define AR_RATE_DURATION_31  0x87CC
#define AR_RATE_DURATION_32  0x8780
#define AR_RATE_DURATION(_n) (AR_RATE_DURATION_0 + ((_n) << 2))

/* WoW - Wake On Wireless */

#define AR_PMCTRL_AUX_PWR_DET 0x10000000 /* Puts Chip in L2 state */
#define AR_PMCTRL_D3COLD_VAUX 0x00800000
#define AR_PMCTRL_HOST_PME_EN                                              \
    0x00400000                                   /* Send OOB WAKE_L on WoW \
                                                             event */
#define AR_PMCTRL_WOW_PME_CLR         0x00200000 /* Clear WoW event */
#define AR_PMCTRL_PWR_STATE_MASK      0x0f000000 /* Power State Mask */
#define AR_PMCTRL_PWR_STATE_D1D3      0x0f000000 /* Activate D1 and D3 */
#define AR_PMCTRL_PWR_STATE_D1D3_REAL 0x0f000000 /* Activate D1 and D3 */
#define AR_PMCTRL_PWR_STATE_D0        0x08000000 /* Activate D0 */
#define AR_PMCTRL_PWR_PM_CTRL_ENA     0x00008000 /* Enable power mgmt */

#define AR_WOW_BEACON_TIMO_MAX 0xffffffff

#define AR_AGG_WEP_ENABLE_FIX 0x00000008 /* This allows the use of AR_AGG_WEP_ENABLE */
#define AR_ADHOC_MCAST_KEYID_ENABLE                     \
    0x00000040 /* This bit enables the Multicast search \
                * based on both MAC Address and Key ID. \
                * If bit is 0, then Multicast search is \
                * based on MAC address only.            \
                * For Merlin and above only.            \
                */
#define AR_AGG_WEP_ENABLE                                \
    0x00020000 /* This field enables AGG_WEP feature,    \
                * when it is enable, AGG_WEP would takes \
                * charge of the encryption interface of  \
                * pcu_txsm.                              \
                */

#define AR9300_SM_BASE                     0xa200
#define AR9003_PHY_AGC_CONTROL             AR9300_SM_BASE + 0xc4
#define AR_PHY_AGC_CONTROL                 (AR9003_PHY_AGC_CONTROL)
#define AR_PHY_AGC_CONTROL_CAL             0x00000001 /* do internal calibration */
#define AR_PHY_AGC_CONTROL_NF              0x00000002 /* do noise-floor calibration */
#define AR_PHY_AGC_CONTROL_OFFSET_CAL      0x00000800 /* allow offset calibration */
#define AR_PHY_AGC_CONTROL_ENABLE_NF       0x00008000 /* enable noise floor calibration to happen */
#define AR_PHY_AGC_CONTROL_FLTR_CAL        0x00010000 /* allow tx filter calibration */
#define AR_PHY_AGC_CONTROL_NO_UPDATE_NF    0x00020000 /* don't update noise floor automatically */
#define AR_PHY_AGC_CONTROL_EXT_NF_PWR_MEAS 0x00040000 /* extend noise floor power measurement */
#define AR_PHY_AGC_CONTROL_CLC_SUCCESS     0x00080000 /* carrier leak calibration done */
#define AR_PHY_AGC_CONTROL_PKDET_CAL       0x00100000
#define AR_PHY_AGC_CONTROL_YCOK_MAX        0x000003c0
#define AR_PHY_AGC_CONTROL_YCOK_MAX_S      6

/* The AR9003 descriptors' fields (ath9k/ar9003_mac.h). */

#define AR_DescId     0xffff0000
#define AR_DescId_S   16
#define AR_CtrlStat   0x00004000
#define AR_CtrlStat_S 14
#define AR_TxRxDesc   0x00008000
#define AR_TxRxDesc_S 15
#define AR_TxQcuNum   0x00000f00
#define AR_TxQcuNum_S 8

#define AR_BufLen   0x0fff0000
#define AR_BufLen_S 16

#define AR_TxDescId    0xffff0000
#define AR_TxDescId_S  16
#define AR_TxPtrChkSum 0x0000ffff

#define AR_LowRxChain 0x00004000

#define AR_Not_Sounding 0x20000000

/* ctl 12 */
#define AR_PAPRDChainMask   0x00000e00
#define AR_PAPRDChainMask_S 9

#define MAP_ISR_S2_CST         6
#define MAP_ISR_S2_GTT         6
#define MAP_ISR_S2_TIM         3
#define MAP_ISR_S2_CABEND      0
#define MAP_ISR_S2_DTIMSYNC    7
#define MAP_ISR_S2_DTIM        7
#define MAP_ISR_S2_TSFOOR      4
#define MAP_ISR_S2_BB_WATCHDOG 6

/* The descriptor fields the AR9003 shares with the AR5416 (ath9k/mac.h). */

#define AR_FrameLen     0x00000fff
#define AR_VirtMoreFrag 0x00001000
#define AR_TxCtlRsvd00  0x0000e000
#define AR_XmitPower0   0x003f0000
#define AR_XmitPower0_S 16
#define AR_XmitPower1   0x3f000000
#define AR_XmitPower1_S 24
#define AR_XmitPower2   0x3f000000
#define AR_XmitPower2_S 24
#define AR_XmitPower3   0x3f000000
#define AR_XmitPower3_S 24
#define AR_RTSEnable    0x00400000
#define AR_VEOL         0x00800000
#define AR_ClrDestMask  0x01000000
#define AR_TxCtlRsvd01  0x1e000000
#define AR_TxIntrReq    0x20000000
#define AR_DestIdxValid 0x40000000
#define AR_CTSEnable    0x80000000

#define AR_TxMore      0x00001000
#define AR_DestIdx     0x000fe000
#define AR_DestIdx_S   13
#define AR_FrameType   0x00f00000
#define AR_FrameType_S 20
#define AR_NoAck       0x01000000
#define AR_InsertTS    0x02000000
#define AR_CorruptFCS  0x04000000
#define AR_ExtOnly     0x08000000
#define AR_ExtAndCtl   0x10000000
#define AR_MoreAggr    0x20000000
#define AR_IsAggr      0x40000000

#define AR_BurstDur         0x00007fff
#define AR_BurstDur_S       0
#define AR_DurUpdateEna     0x00008000
#define AR_XmitDataTries0   0x000f0000
#define AR_XmitDataTries0_S 16
#define AR_XmitDataTries1   0x00f00000
#define AR_XmitDataTries1_S 20
#define AR_XmitDataTries2   0x0f000000
#define AR_XmitDataTries2_S 24
#define AR_XmitDataTries3   0xf0000000
#define AR_XmitDataTries3_S 28

#define AR_XmitRate0   0x000000ff
#define AR_XmitRate0_S 0
#define AR_XmitRate1   0x0000ff00
#define AR_XmitRate1_S 8
#define AR_XmitRate2   0x00ff0000
#define AR_XmitRate2_S 16
#define AR_XmitRate3   0xff000000
#define AR_XmitRate3_S 24

#define AR_PacketDur0   0x00007fff
#define AR_PacketDur0_S 0
#define AR_RTSCTSQual0  0x00008000
#define AR_PacketDur1   0x7fff0000
#define AR_PacketDur1_S 16
#define AR_RTSCTSQual1  0x80000000

#define AR_PacketDur2   0x00007fff
#define AR_PacketDur2_S 0
#define AR_RTSCTSQual2  0x00008000
#define AR_PacketDur3   0x7fff0000
#define AR_PacketDur3_S 16
#define AR_RTSCTSQual3  0x80000000

#define AR_AggrLen     0x0000ffff
#define AR_AggrLen_S   0
#define AR_TxCtlRsvd60 0x00030000
#define AR_PadDelim    0x03fc0000
#define AR_PadDelim_S  18
#define AR_EncrType    0x0c000000
#define AR_EncrType_S  26
#define AR_TxCtlRsvd61 0xf0000000
#define AR_LDPC        0x80000000

#define AR_2040_0       0x00000001
#define AR_GI0          0x00000002
#define AR_ChainSel0    0x0000001c
#define AR_ChainSel0_S  2
#define AR_2040_1       0x00000020
#define AR_GI1          0x00000040
#define AR_ChainSel1    0x00000380
#define AR_ChainSel1_S  7
#define AR_2040_2       0x00000400
#define AR_GI2          0x00000800
#define AR_ChainSel2    0x00007000
#define AR_ChainSel2_S  12
#define AR_2040_3       0x00008000
#define AR_GI3          0x00010000
#define AR_ChainSel3    0x000e0000
#define AR_ChainSel3_S  17
#define AR_RTSCTSRate   0x0ff00000
#define AR_RTSCTSRate_S 20
#define AR_STBC0        0x10000000
#define AR_STBC1        0x20000000
#define AR_STBC2        0x40000000
#define AR_STBC3        0x80000000

#define AR_TxRSSIAnt00    0x000000ff
#define AR_TxRSSIAnt00_S  0
#define AR_TxRSSIAnt01    0x0000ff00
#define AR_TxRSSIAnt01_S  8
#define AR_TxRSSIAnt02    0x00ff0000
#define AR_TxRSSIAnt02_S  16
#define AR_TxStatusRsvd00 0x3f000000
#define AR_TxBaStatus     0x40000000
#define AR_TxStatusRsvd01 0x80000000

/*
 * AR_FrmXmitOK - Frame transmission success flag. If set, the frame was
 * transmitted successfully. If clear, no ACK or BA was received to indicate
 * successful transmission when we were expecting an ACK or BA.
 */
#define AR_FrmXmitOK        0x00000001
#define AR_ExcessiveRetries 0x00000002
#define AR_FIFOUnderrun     0x00000004
#define AR_Filtered         0x00000008
#define AR_RTSFailCnt       0x000000f0
#define AR_RTSFailCnt_S     4
#define AR_DataFailCnt      0x00000f00
#define AR_DataFailCnt_S    8
#define AR_VirtRetryCnt     0x0000f000
#define AR_VirtRetryCnt_S   12
#define AR_TxDelimUnderrun  0x00010000
#define AR_TxDataUnderrun   0x00020000
#define AR_DescCfgErr       0x00040000
#define AR_TxTimerExpired   0x00080000
#define AR_TxStatusRsvd10   0xfff00000

#define AR_TxRSSIAnt10      0x000000ff
#define AR_TxRSSIAnt10_S    0
#define AR_TxRSSIAnt11      0x0000ff00
#define AR_TxRSSIAnt11_S    8
#define AR_TxRSSIAnt12      0x00ff0000
#define AR_TxRSSIAnt12_S    16
#define AR_TxRSSICombined   0xff000000
#define AR_TxRSSICombined_S 24

#define AR_TxTid   0xf0000000
#define AR_TxTid_S 28

#define AR_TxDone         0x00000001
#define AR_SeqNum         0x00001ffe
#define AR_SeqNum_S       1
#define AR_TxStatusRsvd80 0x0001e000
#define AR_TxOpExceeded   0x00020000
#define AR_TxStatusRsvd81 0x001c0000
#define AR_FinalTxIdx     0x00600000
#define AR_FinalTxIdx_S   21
#define AR_TxStatusRsvd82 0x01800000
#define AR_PowerMgmt      0x02000000
#define AR_TxStatusRsvd83 0xfc000000

#define AR_RxCTLRsvd00 0xffffffff

#define AR_RxCtlRsvd00 0x00001000
#define AR_RxIntrReq   0x00002000
#define AR_RxCtlRsvd01 0xffffc000

#define AR_RxRSSIAnt00    0x000000ff
#define AR_RxRSSIAnt00_S  0
#define AR_RxRSSIAnt01    0x0000ff00
#define AR_RxRSSIAnt01_S  8
#define AR_RxRSSIAnt02    0x00ff0000
#define AR_RxRSSIAnt02_S  16
#define AR_RxRate         0xff000000
#define AR_RxRate_S       24
#define AR_RxStatusRsvd00 0xff000000

#define AR_DataLen        0x00000fff
#define AR_RxMore         0x00001000
#define AR_NumDelim       0x003fc000
#define AR_NumDelim_S     14
#define AR_RxStatusRsvd10 0xff800000

#define AR_GI             0x00000001
#define AR_2040           0x00000002
#define AR_Parallel40     0x00000004
#define AR_Parallel40_S   2
#define AR_STBC           0x00000008 /* on ar9280 and later */
#define AR_RxStatusRsvd30 0x000000f0
#define AR_RxAntenna      0xffffff00
#define AR_RxAntenna_S    8

#define AR_RxRSSIAnt10      0x000000ff
#define AR_RxRSSIAnt10_S    0
#define AR_RxRSSIAnt11      0x0000ff00
#define AR_RxRSSIAnt11_S    8
#define AR_RxRSSIAnt12      0x00ff0000
#define AR_RxRSSIAnt12_S    16
#define AR_RxRSSICombined   0xff000000
#define AR_RxRSSICombined_S 24

#define AR_RxDone          0x00000001
#define AR_RxFrameOK       0x00000002
#define AR_CRCErr          0x00000004
#define AR_DecryptCRCErr   0x00000008
#define AR_PHYErr          0x00000010
#define AR_MichaelErr      0x00000020
#define AR_PreDelimCRCErr  0x00000040
#define AR_RxStatusRsvd70  0x00000080
#define AR_RxKeyIdxValid   0x00000100
#define AR_KeyIdx          0x0000fe00
#define AR_KeyIdx_S        9
#define AR_PHYErrCode      0x0000ff00
#define AR_PHYErrCode_S    8
#define AR_RxMoreAggr      0x00010000
#define AR_RxAggr          0x00020000
#define AR_PostDelimCRCErr 0x00040000
#define AR_RxStatusRsvd71  0x3ff80000
#define AR_RxFirstAggr     0x20000000
#define AR_DecryptBusyErr  0x40000000
#define AR_KeyMiss         0x80000000

/* The AR9003 baseband and radio (ath9k/ar9003_phy.h). */

/*
 * Channel Register Map
 */
#define AR_CHAN_BASE 0x9800

#define AR_PHY_TIMING1                                 (AR_CHAN_BASE + 0x0)
#define AR_PHY_TIMING2                                 (AR_CHAN_BASE + 0x4)
#define AR_PHY_TIMING3                                 (AR_CHAN_BASE + 0x8)
#define AR_PHY_TIMING4                                 (AR_CHAN_BASE + 0xc)
#define AR_PHY_TIMING5                                 (AR_CHAN_BASE + 0x10)
#define AR_PHY_TIMING6                                 (AR_CHAN_BASE + 0x14)
#define AR_PHY_TIMING11                                (AR_CHAN_BASE + 0x18)
#define AR_PHY_SPUR_REG                                (AR_CHAN_BASE + 0x1c)
#define AR_PHY_RX_IQCAL_CORR_B0                        (AR_CHAN_BASE + 0xdc)
#define AR_PHY_TX_IQCAL_CONTROL_3                      (AR_CHAN_BASE + 0xb0)
#define AR_PHY_TIMING_CONTROL4_DO_GAIN_DC_IQ_CAL_SHIFT 16

#define AR_PHY_TIMING11_SPUR_FREQ_SD   0x3FF00000
#define AR_PHY_TIMING11_SPUR_FREQ_SD_S 20

#define AR_PHY_TIMING11_SPUR_DELTA_PHASE   0x000FFFFF
#define AR_PHY_TIMING11_SPUR_DELTA_PHASE_S 0

#define AR_PHY_TIMING11_USE_SPUR_FILTER_IN_AGC   0x40000000
#define AR_PHY_TIMING11_USE_SPUR_FILTER_IN_AGC_S 30

#define AR_PHY_TIMING11_USE_SPUR_FILTER_IN_SELFCOR   0x80000000
#define AR_PHY_TIMING11_USE_SPUR_FILTER_IN_SELFCOR_S 31

#define AR_PHY_SPUR_REG_ENABLE_NF_RSSI_SPUR_MIT   0x4000000
#define AR_PHY_SPUR_REG_ENABLE_NF_RSSI_SPUR_MIT_S 26

#define AR_PHY_SPUR_REG_ENABLE_MASK_PPM    0x20000 /* bins move with freq offset */
#define AR_PHY_SPUR_REG_ENABLE_MASK_PPM_S  17
#define AR_PHY_SPUR_REG_SPUR_RSSI_THRESH   0x000000FF
#define AR_PHY_SPUR_REG_SPUR_RSSI_THRESH_S 0
#define AR_PHY_SPUR_REG_EN_VIT_SPUR_RSSI   0x00000100
#define AR_PHY_SPUR_REG_EN_VIT_SPUR_RSSI_S 8
#define AR_PHY_SPUR_REG_MASK_RATE_CNTL     0x03FC0000
#define AR_PHY_SPUR_REG_MASK_RATE_CNTL_S   18

#define AR_PHY_RX_IQCAL_CORR_B0_LOOPBACK_IQCORR_EN   0x20000000
#define AR_PHY_RX_IQCAL_CORR_B0_LOOPBACK_IQCORR_EN_S 29

#define AR_PHY_TX_IQCAL_CONTROL_3_IQCORR_EN   0x80000000
#define AR_PHY_TX_IQCAL_CONTROL_3_IQCORR_EN_S 31

#define AR_PHY_FIND_SIG_LOW (AR_CHAN_BASE + 0x20)

#define AR_PHY_SFCORR     (AR_CHAN_BASE + 0x24)
#define AR_PHY_SFCORR_LOW (AR_CHAN_BASE + 0x28)
#define AR_PHY_SFCORR_EXT (AR_CHAN_BASE + 0x2c)

#define AR_PHY_EXT_CCA         (AR_CHAN_BASE + 0x30)
#define AR_PHY_RADAR_0         (AR_CHAN_BASE + 0x34)
#define AR_PHY_RADAR_1         (AR_CHAN_BASE + 0x38)
#define AR_PHY_RADAR_EXT       (AR_CHAN_BASE + 0x3c)
#define AR_PHY_MULTICHAIN_CTRL (AR_CHAN_BASE + 0x80)
#define AR_PHY_PERCHAIN_CSD    (AR_CHAN_BASE + 0x84)

#define AR_PHY_TX_PHASE_RAMP_0    (AR_CHAN_BASE + 0xd0)
#define AR_PHY_ADC_GAIN_DC_CORR_0 (AR_CHAN_BASE + 0xd4)
#define AR_PHY_IQ_ADC_MEAS_0_B0   (AR_CHAN_BASE + 0xc0)
#define AR_PHY_IQ_ADC_MEAS_1_B0   (AR_CHAN_BASE + 0xc4)
#define AR_PHY_IQ_ADC_MEAS_2_B0   (AR_CHAN_BASE + 0xc8)
#define AR_PHY_IQ_ADC_MEAS_3_B0   (AR_CHAN_BASE + 0xcc)

#define AR_PHY_TX_CRC          (AR_CHAN_BASE + 0xa0)
#define AR_PHY_TST_DAC_CONST   (AR_CHAN_BASE + 0xa4)
#define AR_PHY_SPUR_REPORT_0   (AR_CHAN_BASE + 0xa8)
#define AR_PHY_CHAN_INFO_TAB_0 (AR_CHAN_BASE + 0x300)

/*
 * Channel Field Definitions
 */
#define AR_PHY_TIMING2_USE_FORCE_PPM         0x00001000
#define AR_PHY_TIMING2_FORCE_PPM_VAL         0x00000fff
#define AR_PHY_TIMING3_DSC_MAN               0xFFFE0000
#define AR_PHY_TIMING3_DSC_MAN_S             17
#define AR_PHY_TIMING3_DSC_EXP               0x0001E000
#define AR_PHY_TIMING3_DSC_EXP_S             13
#define AR_PHY_TIMING4_IQCAL_LOG_COUNT_MAX   0xF000
#define AR_PHY_TIMING4_IQCAL_LOG_COUNT_MAX_S 12
#define AR_PHY_TIMING4_DO_CAL                0x10000

#define AR_PHY_TIMING4_ENABLE_PILOT_MASK   0x10000000
#define AR_PHY_TIMING4_ENABLE_PILOT_MASK_S 28
#define AR_PHY_TIMING4_ENABLE_CHAN_MASK    0x20000000
#define AR_PHY_TIMING4_ENABLE_CHAN_MASK_S  29

#define AR_PHY_TIMING4_ENABLE_SPUR_FILTER   0x40000000
#define AR_PHY_TIMING4_ENABLE_SPUR_FILTER_S 30
#define AR_PHY_TIMING4_ENABLE_SPUR_RSSI     0x80000000
#define AR_PHY_TIMING4_ENABLE_SPUR_RSSI_S   31

#define AR_PHY_NEW_ADC_GAIN_CORR_ENABLE                    0x40000000
#define AR_PHY_NEW_ADC_DC_OFFSET_CORR_ENABLE               0x80000000
#define AR_PHY_SFCORR_LOW_USE_SELF_CORR_LOW                0x00000001
#define AR_PHY_SFCORR_LOW_M2COUNT_THR_LOW                  0x00003F00
#define AR_PHY_SFCORR_LOW_M2COUNT_THR_LOW_S                8
#define AR_PHY_SFCORR_LOW_M1_THRESH_LOW                    0x001FC000
#define AR_PHY_SFCORR_LOW_M1_THRESH_LOW_S                  14
#define AR_PHY_SFCORR_LOW_M2_THRESH_LOW                    0x0FE00000
#define AR_PHY_SFCORR_LOW_M2_THRESH_LOW_S                  21
#define AR_PHY_SFCORR_M2COUNT_THR                          0x0000001F
#define AR_PHY_SFCORR_M2COUNT_THR_S                        0
#define AR_PHY_SFCORR_M1_THRESH                            0x00FE0000
#define AR_PHY_SFCORR_M1_THRESH_S                          17
#define AR_PHY_SFCORR_M2_THRESH                            0x7F000000
#define AR_PHY_SFCORR_M2_THRESH_S                          24
#define AR_PHY_SFCORR_EXT_M1_THRESH                        0x0000007F
#define AR_PHY_SFCORR_EXT_M1_THRESH_S                      0
#define AR_PHY_SFCORR_EXT_M2_THRESH                        0x00003F80
#define AR_PHY_SFCORR_EXT_M2_THRESH_S                      7
#define AR_PHY_SFCORR_EXT_M1_THRESH_LOW                    0x001FC000
#define AR_PHY_SFCORR_EXT_M1_THRESH_LOW_S                  14
#define AR_PHY_SFCORR_EXT_M2_THRESH_LOW                    0x0FE00000
#define AR_PHY_SFCORR_EXT_M2_THRESH_LOW_S                  21
#define AR_PHY_SFCORR_EXT_SPUR_SUBCHANNEL_SD               0x10000000
#define AR_PHY_SFCORR_EXT_SPUR_SUBCHANNEL_SD_S             28
#define AR_PHY_SFCORR_SPUR_SUBCHNL_SD_S                    28
#define AR_PHY_EXT_CCA_THRESH62                            0x007F0000
#define AR_PHY_EXT_CCA_THRESH62_S                          16
#define AR_PHY_EXTCHN_PWRTHR1_ANT_DIV_ALT_ANT_MINGAINIDX   0x0000FF00
#define AR_PHY_EXTCHN_PWRTHR1_ANT_DIV_ALT_ANT_MINGAINIDX_S 8
#define AR_PHY_EXT_MINCCA_PWR                              0x01FF0000
#define AR_PHY_EXT_MINCCA_PWR_S                            16
#define AR_PHY_EXT_CYCPWR_THR1                             0x0000FE00L
#define AR_PHY_EXT_CYCPWR_THR1_S                           9
#define AR_PHY_TIMING5_CYCPWR_THR1                         0x000000FE
#define AR_PHY_TIMING5_CYCPWR_THR1_S                       1
#define AR_PHY_TIMING5_CYCPWR_THR1_ENABLE                  0x00000001
#define AR_PHY_TIMING5_CYCPWR_THR1_ENABLE_S                0
#define AR_PHY_TIMING5_CYCPWR_THR1A                        0x007F0000
#define AR_PHY_TIMING5_CYCPWR_THR1A_S                      16
#define AR_PHY_TIMING5_RSSI_THR1A                          (0x7F << 16)
#define AR_PHY_TIMING5_RSSI_THR1A_S                        16
#define AR_PHY_TIMING5_RSSI_THR1A_ENA                      (0x1 << 15)
#define AR_PHY_RADAR_0_ENA                                 0x00000001
#define AR_PHY_RADAR_0_FFT_ENA                             0x80000000
#define AR_PHY_RADAR_0_INBAND                              0x0000003e
#define AR_PHY_RADAR_0_INBAND_S                            1
#define AR_PHY_RADAR_0_PRSSI                               0x00000FC0
#define AR_PHY_RADAR_0_PRSSI_S                             6
#define AR_PHY_RADAR_0_HEIGHT                              0x0003F000
#define AR_PHY_RADAR_0_HEIGHT_S                            12
#define AR_PHY_RADAR_0_RRSSI                               0x00FC0000
#define AR_PHY_RADAR_0_RRSSI_S                             18
#define AR_PHY_RADAR_0_FIRPWR                              0x7F000000
#define AR_PHY_RADAR_0_FIRPWR_S                            24
#define AR_PHY_RADAR_1_RELPWR_ENA                          0x00800000
#define AR_PHY_RADAR_1_USE_FIR128                          0x00400000
#define AR_PHY_RADAR_1_RELPWR_THRESH                       0x003F0000
#define AR_PHY_RADAR_1_RELPWR_THRESH_S                     16
#define AR_PHY_RADAR_1_BLOCK_CHECK                         0x00008000
#define AR_PHY_RADAR_1_MAX_RRSSI                           0x00004000
#define AR_PHY_RADAR_1_RELSTEP_CHECK                       0x00002000
#define AR_PHY_RADAR_1_RELSTEP_THRESH                      0x00001F00
#define AR_PHY_RADAR_1_RELSTEP_THRESH_S                    8
#define AR_PHY_RADAR_1_MAXLEN                              0x000000FF
#define AR_PHY_RADAR_1_MAXLEN_S                            0
#define AR_PHY_RADAR_EXT_ENA                               0x00004000
#define AR_PHY_RADAR_DC_PWR_THRESH                         0x007f8000
#define AR_PHY_RADAR_DC_PWR_THRESH_S                       15
#define AR_PHY_RADAR_LB_DC_CAP                             0x7f800000
#define AR_PHY_RADAR_LB_DC_CAP_S                           23
#define AR_PHY_FIND_SIG_LOW_FIRSTEP_LOW                    (0x3f << 6)
#define AR_PHY_FIND_SIG_LOW_FIRSTEP_LOW_S                  6
#define AR_PHY_FIND_SIG_LOW_FIRPWR                         (0x7f << 12)
#define AR_PHY_FIND_SIG_LOW_FIRPWR_S                       12
#define AR_PHY_FIND_SIG_LOW_FIRPWR_SIGN_BIT                19
#define AR_PHY_FIND_SIG_LOW_RELSTEP                        0x1f
#define AR_PHY_FIND_SIG_LOW_RELSTEP_S                      0
#define AR_PHY_FIND_SIG_LOW_RELSTEP_SIGN_BIT               5
#define AR_PHY_CHAN_INFO_TAB_S2_READ                       0x00000008
#define AR_PHY_CHAN_INFO_TAB_S2_READ_S                     3
#define AR_PHY_RX_IQCAL_CORR_IQCORR_Q_Q_COFF               0x0000007F
#define AR_PHY_RX_IQCAL_CORR_IQCORR_Q_Q_COFF_S             0
#define AR_PHY_RX_IQCAL_CORR_IQCORR_Q_I_COFF               0x00003F80
#define AR_PHY_RX_IQCAL_CORR_IQCORR_Q_I_COFF_S             7
#define AR_PHY_RX_IQCAL_CORR_IQCORR_ENABLE                 0x00004000
#define AR_PHY_RX_IQCAL_CORR_LOOPBACK_IQCORR_Q_Q_COFF      0x003f8000
#define AR_PHY_RX_IQCAL_CORR_LOOPBACK_IQCORR_Q_Q_COFF_S    15
#define AR_PHY_RX_IQCAL_CORR_LOOPBACK_IQCORR_Q_I_COFF      0x1fc00000
#define AR_PHY_RX_IQCAL_CORR_LOOPBACK_IQCORR_Q_I_COFF_S    22

/*
 * MRC Register Map
 */
#define AR_MRC_BASE 0x9c00

#define AR_PHY_TIMING_3A       (AR_MRC_BASE + 0x0)
#define AR_PHY_LDPC_CNTL1      (AR_MRC_BASE + 0x4)
#define AR_PHY_LDPC_CNTL2      (AR_MRC_BASE + 0x8)
#define AR_PHY_PILOT_SPUR_MASK (AR_MRC_BASE + 0xc)
#define AR_PHY_CHAN_SPUR_MASK  (AR_MRC_BASE + 0x10)
#define AR_PHY_SGI_DELTA       (AR_MRC_BASE + 0x14)
#define AR_PHY_ML_CNTL_1       (AR_MRC_BASE + 0x18)
#define AR_PHY_ML_CNTL_2       (AR_MRC_BASE + 0x1c)
#define AR_PHY_TST_ADC         (AR_MRC_BASE + 0x20)

#define AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_A   0x00000FE0
#define AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_A_S 5
#define AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_A       0x1F
#define AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_A_S     0
#define AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_B   0x00FE0000
#define AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_IDX_B_S 17
#define AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_B       0x0001F000
#define AR_PHY_PILOT_SPUR_MASK_CF_PILOT_MASK_B_S     12

#define AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_IDX_A   0x00000FE0
#define AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_IDX_A_S 5
#define AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_A       0x1F
#define AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_A_S     0
#define AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_IDX_B   0x00FE0000
#define AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_IDX_B_S 17
#define AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_B       0x0001F000
#define AR_PHY_CHAN_SPUR_MASK_CF_CHAN_MASK_B_S     12

/*
 * MRC Field Definitions
 */
#define AR_PHY_SGI_DSC_MAN   0x0007FFF0
#define AR_PHY_SGI_DSC_MAN_S 4
#define AR_PHY_SGI_DSC_EXP   0x0000000F
#define AR_PHY_SGI_DSC_EXP_S 0
/*
 * BBB Register Map
 */
#define AR_BBB_BASE          0x9d00

/*
 * AGC Register Map
 */
#define AR_AGC_BASE 0x9e00

#define AR_PHY_SETTLING         (AR_AGC_BASE + 0x0)
#define AR_PHY_FORCEMAX_GAINS_0 (AR_AGC_BASE + 0x4)
#define AR_PHY_GAINS_MINOFF0    (AR_AGC_BASE + 0x8)
#define AR_PHY_DESIRED_SZ       (AR_AGC_BASE + 0xc)
#define AR_PHY_FIND_SIG         (AR_AGC_BASE + 0x10)
#define AR_PHY_AGC              (AR_AGC_BASE + 0x14)
#define AR_PHY_EXT_ATTEN_CTL_0  (AR_AGC_BASE + 0x18)
#define AR_PHY_CCA_0            (AR_AGC_BASE + 0x1c)
#define AR_PHY_CCA_CTRL_0       (AR_AGC_BASE + 0x20)
#define AR_PHY_RESTART          (AR_AGC_BASE + 0x24)

/*
 * Antenna Diversity  settings
 */
#define AR_PHY_MC_GAIN_CTRL   (AR_AGC_BASE + 0x28)
#define AR_ANT_DIV_CTRL_ALL   0x7e000000
#define AR_ANT_DIV_CTRL_ALL_S 25
#define AR_ANT_DIV_ENABLE     0x1000000
#define AR_ANT_DIV_ENABLE_S   24

#define AR_PHY_ANT_FAST_DIV_BIAS      0x00007e00
#define AR_PHY_ANT_FAST_DIV_BIAS_S    9
#define AR_PHY_ANT_SW_RX_PROT         0x00800000
#define AR_PHY_ANT_SW_RX_PROT_S       23
#define AR_PHY_ANT_DIV_LNADIV         0x01000000
#define AR_PHY_ANT_DIV_LNADIV_S       24
#define AR_PHY_ANT_DIV_ALT_LNACONF    0x06000000
#define AR_PHY_ANT_DIV_ALT_LNACONF_S  25
#define AR_PHY_ANT_DIV_MAIN_LNACONF   0x18000000
#define AR_PHY_ANT_DIV_MAIN_LNACONF_S 27
#define AR_PHY_ANT_DIV_ALT_GAINTB     0x20000000
#define AR_PHY_ANT_DIV_ALT_GAINTB_S   29
#define AR_PHY_ANT_DIV_MAIN_GAINTB    0x40000000
#define AR_PHY_ANT_DIV_MAIN_GAINTB_S  30

#define AR_PHY_EXTCHN_PWRTHR1   (AR_AGC_BASE + 0x2c)
#define AR_PHY_EXT_CHN_WIN      (AR_AGC_BASE + 0x30)
#define AR_PHY_20_40_DET_THR    (AR_AGC_BASE + 0x34)
#define AR_PHY_RIFS_SRCH        (AR_AGC_BASE + 0x38)
#define AR_PHY_PEAK_DET_CTRL_1  (AR_AGC_BASE + 0x3c)
#define AR_PHY_PEAK_DET_CTRL_2  (AR_AGC_BASE + 0x40)
#define AR_PHY_RX_GAIN_BOUNDS_1 (AR_AGC_BASE + 0x44)
#define AR_PHY_RX_GAIN_BOUNDS_2 (AR_AGC_BASE + 0x48)
#define AR_PHY_RSSI_0           (AR_AGC_BASE + 0x180)
#define AR_PHY_SPUR_CCK_REP0    (AR_AGC_BASE + 0x184)

#define AR_PHY_CCK_DETECT    (AR_AGC_BASE + 0x1c0)
#define AR_FAST_DIV_ENABLE   0x2000
#define AR_FAST_DIV_ENABLE_S 13

#define AR_PHY_DAG_CTRLCCK     (AR_AGC_BASE + 0x1c4)
#define AR_PHY_IQCORR_CTRL_CCK (AR_AGC_BASE + 0x1c8)

#define AR_PHY_CCK_SPUR_MIT                    (AR_AGC_BASE + 0x1cc)
#define AR_PHY_CCK_SPUR_MIT_SPUR_RSSI_THR      0x000001fe
#define AR_PHY_CCK_SPUR_MIT_SPUR_RSSI_THR_S    1
#define AR_PHY_CCK_SPUR_MIT_SPUR_FILTER_TYPE   0x60000000
#define AR_PHY_CCK_SPUR_MIT_SPUR_FILTER_TYPE_S 29
#define AR_PHY_CCK_SPUR_MIT_USE_CCK_SPUR_MIT   0x00000001
#define AR_PHY_CCK_SPUR_MIT_USE_CCK_SPUR_MIT_S 0
#define AR_PHY_CCK_SPUR_MIT_CCK_SPUR_FREQ      0x1ffffe00
#define AR_PHY_CCK_SPUR_MIT_CCK_SPUR_FREQ_S    9

#define AR_PHY_MRC_CCK_CTRL      (AR_AGC_BASE + 0x1d0)
#define AR_PHY_MRC_CCK_ENABLE    0x00000001
#define AR_PHY_MRC_CCK_ENABLE_S  0
#define AR_PHY_MRC_CCK_MUX_REG   0x00000002
#define AR_PHY_MRC_CCK_MUX_REG_S 1

#define AR_PHY_RX_OCGAIN (AR_AGC_BASE + 0x200)

#define AR_PHY_CCA_NOM_VAL_9300_2GHZ          -110
#define AR_PHY_CCA_NOM_VAL_9300_5GHZ          -115
#define AR_PHY_CCA_MIN_GOOD_VAL_9300_2GHZ     -125
#define AR_PHY_CCA_MIN_GOOD_VAL_9300_5GHZ     -125
#define AR_PHY_CCA_MAX_GOOD_VAL_9300_2GHZ     -60
#define AR_PHY_CCA_MAX_GOOD_VAL_9300_5GHZ     -60
#define AR_PHY_CCA_MAX_GOOD_VAL_9300_FCC_2GHZ -95
#define AR_PHY_CCA_MAX_GOOD_VAL_9300_FCC_5GHZ -100

/*
 * AGC Field Definitions
 */
#define AR_PHY_EXT_ATTEN_CTL_RXTX_MARGIN         0x00FC0000
#define AR_PHY_EXT_ATTEN_CTL_RXTX_MARGIN_S       18
#define AR_PHY_EXT_ATTEN_CTL_BSW_MARGIN          0x00003C00
#define AR_PHY_EXT_ATTEN_CTL_BSW_MARGIN_S        10
#define AR_PHY_EXT_ATTEN_CTL_BSW_ATTEN           0x0000001F
#define AR_PHY_EXT_ATTEN_CTL_BSW_ATTEN_S         0
#define AR_PHY_EXT_ATTEN_CTL_XATTEN2_MARGIN      0x003E0000
#define AR_PHY_EXT_ATTEN_CTL_XATTEN2_MARGIN_S    17
#define AR_PHY_EXT_ATTEN_CTL_XATTEN1_MARGIN      0x0001F000
#define AR_PHY_EXT_ATTEN_CTL_XATTEN1_MARGIN_S    12
#define AR_PHY_EXT_ATTEN_CTL_XATTEN2_DB          0x00000FC0
#define AR_PHY_EXT_ATTEN_CTL_XATTEN2_DB_S        6
#define AR_PHY_EXT_ATTEN_CTL_XATTEN1_DB          0x0000003F
#define AR_PHY_EXT_ATTEN_CTL_XATTEN1_DB_S        0
#define AR_PHY_RXGAIN_TXRX_ATTEN                 0x0003F000
#define AR_PHY_RXGAIN_TXRX_ATTEN_S               12
#define AR_PHY_RXGAIN_TXRX_RF_MAX                0x007C0000
#define AR_PHY_RXGAIN_TXRX_RF_MAX_S              18
#define AR_PHY_SETTLING_SWITCH                   0x00003F80
#define AR_PHY_SETTLING_SWITCH_S                 7
#define AR_PHY_DESIRED_SZ_ADC                    0x000000FF
#define AR_PHY_DESIRED_SZ_ADC_S                  0
#define AR_PHY_DESIRED_SZ_PGA                    0x0000FF00
#define AR_PHY_DESIRED_SZ_PGA_S                  8
#define AR_PHY_DESIRED_SZ_TOT_DES                0x0FF00000
#define AR_PHY_DESIRED_SZ_TOT_DES_S              20
#define AR_PHY_MINCCA_PWR                        0x1FF00000
#define AR_PHY_MINCCA_PWR_S                      20
#define AR_PHY_CCA_THRESH62                      0x0007F000
#define AR_PHY_CCA_THRESH62_S                    12
#define AR_PHY_EXT_CCA0_THRESH62                 0x000000FF
#define AR_PHY_EXT_CCA0_THRESH62_S               0
#define AR_PHY_EXT_CCA0_THRESH62_1               0x000001FF
#define AR_PHY_EXT_CCA0_THRESH62_1_S             0
#define AR_PHY_CCK_DETECT_WEAK_SIG_THR_CCK       0x0000003F
#define AR_PHY_CCK_DETECT_WEAK_SIG_THR_CCK_S     0
#define AR_PHY_CCK_DETECT_ANT_SWITCH_TIME        0x00001FC0
#define AR_PHY_CCK_DETECT_ANT_SWITCH_TIME_S      6
#define AR_PHY_CCK_DETECT_BB_ENABLE_ANT_FAST_DIV 0x2000

#define AR_PHY_DAG_CTRLCCK_EN_RSSI_THR   0x00000200
#define AR_PHY_DAG_CTRLCCK_EN_RSSI_THR_S 9
#define AR_PHY_DAG_CTRLCCK_RSSI_THR      0x0001FC00
#define AR_PHY_DAG_CTRLCCK_RSSI_THR_S    10

#define AR_PHY_RIFS_INIT_DELAY             0x3ff0000
#define AR_PHY_AGC_QUICK_DROP              0x03c00000
#define AR_PHY_AGC_QUICK_DROP_S            22
#define AR_PHY_AGC_COARSE_LOW              0x00007F80
#define AR_PHY_AGC_COARSE_LOW_S            7
#define AR_PHY_AGC_COARSE_HIGH             0x003F8000
#define AR_PHY_AGC_COARSE_HIGH_S           15
#define AR_PHY_AGC_COARSE_PWR_CONST        0x0000007F
#define AR_PHY_AGC_COARSE_PWR_CONST_S      0
#define AR_PHY_FIND_SIG_FIRSTEP            0x0003F000
#define AR_PHY_FIND_SIG_FIRSTEP_S          12
#define AR_PHY_FIND_SIG_FIRPWR             0x03FC0000
#define AR_PHY_FIND_SIG_FIRPWR_S           18
#define AR_PHY_FIND_SIG_FIRPWR_SIGN_BIT    25
#define AR_PHY_FIND_SIG_RELPWR             (0x1f << 6)
#define AR_PHY_FIND_SIG_RELPWR_S           6
#define AR_PHY_FIND_SIG_RELPWR_SIGN_BIT    11
#define AR_PHY_FIND_SIG_RELSTEP            0x1f
#define AR_PHY_FIND_SIG_RELSTEP_S          0
#define AR_PHY_FIND_SIG_RELSTEP_SIGN_BIT   5
#define AR_PHY_RESTART_ENABLE_DIV_M2FLAG   0x00200000
#define AR_PHY_RESTART_ENABLE_DIV_M2FLAG_S 21
#define AR_PHY_RESTART_DIV_GC              0x001C0000
#define AR_PHY_RESTART_DIV_GC_S            18
#define AR_PHY_RESTART_ENA                 0x01
#define AR_PHY_DC_RESTART_DIS              0x40000000

#define AR_PHY_TPC_OLPC_GAIN_DELTA_PAL_ON   0xFF000000
#define AR_PHY_TPC_OLPC_GAIN_DELTA_PAL_ON_S 24
#define AR_PHY_TPC_OLPC_GAIN_DELTA          0x00FF0000
#define AR_PHY_TPC_OLPC_GAIN_DELTA_S        16

#define AR_PHY_TPC_6_ERROR_EST_MODE   0x03000000
#define AR_PHY_TPC_6_ERROR_EST_MODE_S 24

/*
 * SM Register Map
 */
#define AR_SM_BASE 0xa200

#define AR_PHY_D2_CHIP_ID         (AR_SM_BASE + 0x0)
#define AR_PHY_GEN_CTRL           (AR_SM_BASE + 0x4)
#define AR_PHY_MODE               (AR_SM_BASE + 0x8)
#define AR_PHY_ACTIVE             (AR_SM_BASE + 0xc)
#define AR_PHY_SPUR_MASK_A        (AR_SM_BASE + (0x20))
#define AR_PHY_SPUR_MASK_B        (AR_SM_BASE + (0x24))
#define AR_PHY_SPECTRAL_SCAN      (AR_SM_BASE + 0x28)
#define AR_PHY_RADAR_BW_FILTER    (AR_SM_BASE + 0x2c)
#define AR_PHY_SEARCH_START_DELAY (AR_SM_BASE + 0x30)
#define AR_PHY_MAX_RX_LEN         (AR_SM_BASE + 0x34)
#define AR_PHY_FRAME_CTL          (AR_SM_BASE + 0x38)
#define AR_PHY_RFBUS_REQ          (AR_SM_BASE + 0x3c)
#define AR_PHY_RFBUS_GRANT        (AR_SM_BASE + 0x40)
#define AR_PHY_RIFS               (AR_SM_BASE + 0x44)
#define AR_PHY_RX_CLR_DELAY       (AR_SM_BASE + 0x50)
#define AR_PHY_RX_DELAY           (AR_SM_BASE + 0x54)

#define AR_PHY_XPA_TIMING_CTL (AR_SM_BASE + 0x64)
#define AR_PHY_MISC_PA_CTL    (AR_SM_BASE + 0x80)
#define AR_PHY_SWITCH_CHAIN_0 (AR_SM_BASE + 0x84)
#define AR_PHY_SWITCH_COM     (AR_SM_BASE + 0x88)
#define AR_PHY_SWITCH_COM_2   (AR_SM_BASE + 0x8c)
#define AR_PHY_RX_CHAINMASK   (AR_SM_BASE + 0xa0)
#define AR_PHY_CAL_CHAINMASK  (AR_SM_BASE + 0xc0)
#define AR_PHY_CALMODE        (AR_SM_BASE + 0xc8)
#define AR_PHY_FCAL_1         (AR_SM_BASE + 0xcc)
#define AR_PHY_FCAL_2_0       (AR_SM_BASE + 0xd0)
#define AR_PHY_DFT_TONE_CTL_0 (AR_SM_BASE + 0xd4)
#define AR_PHY_CL_CAL_CTL     (AR_SM_BASE + 0xd8)
#define AR_PHY_CL_TAB_0       (AR_SM_BASE + 0x100)
#define AR_PHY_SYNTH_CONTROL  (AR_SM_BASE + 0x140)
#define AR_PHY_ADDAC_CLK_SEL  (AR_SM_BASE + 0x144)
#define AR_PHY_PLL_CTL        (AR_SM_BASE + 0x148)
#define AR_PHY_ANALOG_SWAP    (AR_SM_BASE + 0x14c)
#define AR_PHY_ADDAC_PARA_CTL (AR_SM_BASE + 0x150)
#define AR_PHY_XPA_CFG        (AR_SM_BASE + 0x158)

#define AR_PHY_FLC_PWR_THRESH   7
#define AR_PHY_FLC_PWR_THRESH_S 0

#define AR_PHY_FRAME_CTL_CF_OVERLAP_WINDOW   3
#define AR_PHY_FRAME_CTL_CF_OVERLAP_WINDOW_S 0

#define AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_IDX_A   0x0001FC00
#define AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_IDX_A_S 10
#define AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_A       0x3FF
#define AR_PHY_SPUR_MASK_A_CF_PUNC_MASK_A_S     0

#define AR_PHY_TEST (AR_SM_BASE + (0x160))

#define AR_PHY_TEST_BBB_OBS_SEL   0x780000
#define AR_PHY_TEST_BBB_OBS_SEL_S 19

#define AR_PHY_TEST_RX_OBS_SEL_BIT5_S 23
#define AR_PHY_TEST_RX_OBS_SEL_BIT5   (1 << AR_PHY_TEST_RX_OBS_SEL_BIT5_S)

#define AR_PHY_TEST_CHAIN_SEL   0xC0000000
#define AR_PHY_TEST_CHAIN_SEL_S 30

#define AR_PHY_TEST_CTL_STATUS           (AR_SM_BASE + (0x164))
#define AR_PHY_TEST_CTL_TSTDAC_EN        0x1
#define AR_PHY_TEST_CTL_TSTDAC_EN_S      0
#define AR_PHY_TEST_CTL_TX_OBS_SEL       0x1C
#define AR_PHY_TEST_CTL_TX_OBS_SEL_S     2
#define AR_PHY_TEST_CTL_TX_OBS_MUX_SEL   0x60
#define AR_PHY_TEST_CTL_TX_OBS_MUX_SEL_S 5
#define AR_PHY_TEST_CTL_TSTADC_EN        0x100
#define AR_PHY_TEST_CTL_TSTADC_EN_S      8
#define AR_PHY_TEST_CTL_RX_OBS_SEL       0x3C00
#define AR_PHY_TEST_CTL_RX_OBS_SEL_S     10
#define AR_PHY_TEST_CTL_DEBUGPORT_SEL    0xe0000000
#define AR_PHY_TEST_CTL_DEBUGPORT_SEL_S  29

#define AR_PHY_TSTDAC (AR_SM_BASE + (0x168))

#define AR_PHY_CHAN_STATUS (AR_SM_BASE + (0x16c))

#define AR_PHY_CHAN_INFO_MEMORY                       (AR_SM_BASE + (0x170))
#define AR_PHY_CHAN_INFO_MEMORY_CHANINFOMEM_S2_READ   0x00000008
#define AR_PHY_CHAN_INFO_MEMORY_CHANINFOMEM_S2_READ_S 3

#define AR_PHY_CHNINFO_NOISEPWR (AR_SM_BASE + (0x174))
#define AR_PHY_CHNINFO_GAINDIFF (AR_SM_BASE + (0x178))
#define AR_PHY_CHNINFO_FINETIM  (AR_SM_BASE + (0x17c))
#define AR_PHY_CHAN_INFO_GAIN_0 (AR_SM_BASE + (0x180))
#define AR_PHY_SCRAMBLER_SEED   (AR_SM_BASE + (0x190))
#define AR_PHY_CCK_TX_CTRL      (AR_SM_BASE + (0x194))

#define AR_PHY_HEAVYCLIP_CTL  (AR_SM_BASE + (0x1a4))
#define AR_PHY_HEAVYCLIP_20   (AR_SM_BASE + 0x1a8)
#define AR_PHY_HEAVYCLIP_40   (AR_SM_BASE + 0x1ac)
#define AR_PHY_HEAVYCLIP_1    (AR_SM_BASE + 0x19c)
#define AR_PHY_HEAVYCLIP_2    (AR_SM_BASE + 0x1a0)
#define AR_PHY_HEAVYCLIP_3    (AR_SM_BASE + 0x1a4)
#define AR_PHY_HEAVYCLIP_4    (AR_SM_BASE + 0x1a8)
#define AR_PHY_HEAVYCLIP_5    (AR_SM_BASE + 0x1ac)
#define AR_PHY_ILLEGAL_TXRATE (AR_SM_BASE + 0x1b0)

#define AR_PHY_POWER_TX_RATE(_d) (AR_SM_BASE + 0x1c0 + ((_d) << 2))

#define AR_PHY_PWRTX_MAX    (AR_SM_BASE + 0x1f0)
#define AR_PHY_POWER_TX_SUB (AR_SM_BASE + 0x1f4)

#define AR_PHY_TPC_1                   (AR_SM_BASE + 0x1f8)
#define AR_PHY_TPC_1_FORCED_DAC_GAIN   0x0000003e
#define AR_PHY_TPC_1_FORCED_DAC_GAIN_S 1
#define AR_PHY_TPC_1_FORCE_DAC_GAIN    0x00000001
#define AR_PHY_TPC_1_FORCE_DAC_GAIN_S  0

#define AR_PHY_TPC_4_B0 (AR_SM_BASE + 0x204)
#define AR_PHY_TPC_5_B0 (AR_SM_BASE + 0x208)
#define AR_PHY_TPC_6_B0 (AR_SM_BASE + 0x20c)

#define AR_PHY_TPC_11_B0                (AR_SM_BASE + 0x220)
#define AR_PHY_TPC_11_B1                (AR_SM1_BASE + 0x220)
#define AR_PHY_TPC_11_B2                (AR_SM2_BASE + 0x220)
#define AR_PHY_TPC_11_OLPC_GAIN_DELTA   0x00ff0000
#define AR_PHY_TPC_11_OLPC_GAIN_DELTA_S 16

#define AR_PHY_TPC_12                        (AR_SM_BASE + 0x224)
#define AR_PHY_TPC_12_DESIRED_SCALE_HT40_5   0x3e000000
#define AR_PHY_TPC_12_DESIRED_SCALE_HT40_5_S 25

#define AR_PHY_TPC_18                   (AR_SM_BASE + 0x23c)
#define AR_PHY_TPC_18_THERM_CAL_VALUE   0x000000ff
#define AR_PHY_TPC_18_THERM_CAL_VALUE_S 0
#define AR_PHY_TPC_18_VOLT_CAL_VALUE    0x0000ff00
#define AR_PHY_TPC_18_VOLT_CAL_VALUE_S  8

#define AR_PHY_TPC_19               (AR_SM_BASE + 0x240)
#define AR_PHY_TPC_19_ALPHA_VOLT    0x001f0000
#define AR_PHY_TPC_19_ALPHA_VOLT_S  16
#define AR_PHY_TPC_19_ALPHA_THERM   0xff
#define AR_PHY_TPC_19_ALPHA_THERM_S 0

#define AR_PHY_TX_FORCED_GAIN                      (AR_SM_BASE + 0x258)
#define AR_PHY_TX_FORCED_GAIN_FORCE_TX_GAIN        0x00000001
#define AR_PHY_TX_FORCED_GAIN_FORCE_TX_GAIN_S      0
#define AR_PHY_TX_FORCED_GAIN_FORCED_TXBB1DBGAIN   0x0000000e
#define AR_PHY_TX_FORCED_GAIN_FORCED_TXBB1DBGAIN_S 1
#define AR_PHY_TX_FORCED_GAIN_FORCED_TXBB6DBGAIN   0x00000030
#define AR_PHY_TX_FORCED_GAIN_FORCED_TXBB6DBGAIN_S 4
#define AR_PHY_TX_FORCED_GAIN_FORCED_TXMXRGAIN     0x000003c0
#define AR_PHY_TX_FORCED_GAIN_FORCED_TXMXRGAIN_S   6
#define AR_PHY_TX_FORCED_GAIN_FORCED_PADRVGNA      0x00003c00
#define AR_PHY_TX_FORCED_GAIN_FORCED_PADRVGNA_S    10
#define AR_PHY_TX_FORCED_GAIN_FORCED_PADRVGNB      0x0003c000
#define AR_PHY_TX_FORCED_GAIN_FORCED_PADRVGNB_S    14
#define AR_PHY_TX_FORCED_GAIN_FORCED_PADRVGNC      0x003c0000
#define AR_PHY_TX_FORCED_GAIN_FORCED_PADRVGNC_S    18
#define AR_PHY_TX_FORCED_GAIN_FORCED_PADRVGND      0x00c00000
#define AR_PHY_TX_FORCED_GAIN_FORCED_PADRVGND_S    22
#define AR_PHY_TX_FORCED_GAIN_FORCED_ENABLE_PAL    0x01000000
#define AR_PHY_TX_FORCED_GAIN_FORCED_ENABLE_PAL_S  24

#define AR_PHY_PDADC_TAB_0 (AR_SM_BASE + 0x280)

#define AR_PHY_TXGAIN_TABLE (AR_SM_BASE + 0x300)

#define AR_PHY_TX_IQCAL_CONTROL_0         (AR_SM_BASE + (0x444))
#define AR_PHY_TX_IQCAL_CONTROL_1         (AR_SM_BASE + (0x448))
#define AR_PHY_TX_IQCAL_START             (AR_SM_BASE + (0x440))
#define AR_PHY_TX_IQCAL_STATUS_B0         (AR_SM_BASE + (0x48c))
#define AR_PHY_TX_IQCAL_CORR_COEFF_B0(_i) (AR_SM_BASE + (0x450) + ((_i) << 2))
#define AR_PHY_RTT_CTRL                   (AR_SM_BASE + 0x380)

#define AR_PHY_WATCHDOG_STATUS (AR_SM_BASE + 0x5c0)
#define AR_PHY_WATCHDOG_CTL_1  (AR_SM_BASE + 0x5c4)
#define AR_PHY_WATCHDOG_CTL_2  (AR_SM_BASE + 0x5c8)
#define AR_PHY_WATCHDOG_CTL    (AR_SM_BASE + 0x5cc)
#define AR_PHY_ONLY_WARMRESET  (AR_SM_BASE + 0x5d0)
#define AR_PHY_ONLY_CTL        (AR_SM_BASE + 0x5d4)
#define AR_PHY_ECO_CTRL        (AR_SM_BASE + 0x5dc)

#define AR_PHY_BB_THERM_ADC_1              (AR_SM_BASE + 0x248)
#define AR_PHY_BB_THERM_ADC_1_INIT_THERM   0x000000ff
#define AR_PHY_BB_THERM_ADC_1_INIT_THERM_S 0

#define AR_PHY_BB_THERM_ADC_3                        (AR_SM_BASE + 0x250)
#define AR_PHY_BB_THERM_ADC_3_THERM_ADC_SCALE_GAIN   0x0001ff00
#define AR_PHY_BB_THERM_ADC_3_THERM_ADC_SCALE_GAIN_S 8
#define AR_PHY_BB_THERM_ADC_3_THERM_ADC_OFFSET       0x000000ff
#define AR_PHY_BB_THERM_ADC_3_THERM_ADC_OFFSET_S     0

#define AR_PHY_BB_THERM_ADC_4                      (AR_SM_BASE + 0x254)
#define AR_PHY_BB_THERM_ADC_4_LATEST_THERM_VALUE   0x000000ff
#define AR_PHY_BB_THERM_ADC_4_LATEST_THERM_VALUE_S 0
#define AR_PHY_BB_THERM_ADC_4_LATEST_VOLT_VALUE    0x0000ff00
#define AR_PHY_BB_THERM_ADC_4_LATEST_VOLT_VALUE_S  8

#define AR_PHY_65NM_CH0_TXRF3            0x16048
#define AR_PHY_65NM_CH0_TXRF3_CAPDIV2G   0x0000001e
#define AR_PHY_65NM_CH0_TXRF3_CAPDIV2G_S 1

#define AR_PHY_65NM_CH0_SYNTH4            0x1608c
#define AR_PHY_SYNTH4_LONG_SHIFT_SELECT   (0x00000001)
#define AR_PHY_SYNTH4_LONG_SHIFT_SELECT_S 0
#define AR_PHY_65NM_CH0_SYNTH7            0x16098
#define AR_PHY_65NM_CH0_SYNTH12           0x160ac
#define AR_PHY_65NM_CH0_BIAS1             0x160c0
#define AR_PHY_65NM_CH0_BIAS2             0x160c4
#define AR_PHY_65NM_CH0_BIAS4             0x160cc
#define AR_PHY_65NM_CH0_RXTX2             0x16104
#define AR_PHY_65NM_CH1_RXTX2             0x16504
#define AR_PHY_65NM_CH2_RXTX2             0x16904
#define AR_PHY_65NM_CH0_RXTX4             0x1610c
#define AR_PHY_65NM_CH1_RXTX4             0x1650c
#define AR_PHY_65NM_CH2_RXTX4             0x1690c

#define AR_PHY_65NM_CH0_BB1 0x16140
#define AR_PHY_65NM_CH0_BB2 0x16144
#define AR_PHY_65NM_CH0_BB3 0x16148
#define AR_PHY_65NM_CH1_BB1 0x16540
#define AR_PHY_65NM_CH1_BB2 0x16544
#define AR_PHY_65NM_CH1_BB3 0x16548
#define AR_PHY_65NM_CH2_BB1 0x16940
#define AR_PHY_65NM_CH2_BB2 0x16944
#define AR_PHY_65NM_CH2_BB3 0x16948

#define AR_PHY_65NM_CH0_SYNTH12_VREFMUL3      0x00780000
#define AR_PHY_65NM_CH0_SYNTH12_VREFMUL3_S    19
#define AR_PHY_65NM_CH0_RXTX2_SYNTHON_MASK    0x00000004
#define AR_PHY_65NM_CH0_RXTX2_SYNTHON_MASK_S  2
#define AR_PHY_65NM_CH0_RXTX2_SYNTHOVR_MASK   0x00000008
#define AR_PHY_65NM_CH0_RXTX2_SYNTHOVR_MASK_S 3

#define AR_CH0_TOP              (0x1628c)
#define AR_CH0_TOP_XPABIASLVL   (0x300)
#define AR_CH0_TOP_XPABIASLVL_S (8)

#define AR_SWITCH_TABLE_COM_ALL        (0xffffff)
#define AR_SWITCH_TABLE_COM_ALL_S      (0)
#define AR_SWITCH_TABLE_COM_SPDT       (0x00f00000)
#define AR_SWITCH_TABLE_COM_SPDT_ALL   (0x0000fff0)
#define AR_SWITCH_TABLE_COM_SPDT_ALL_S (4)

#define AR_SWITCH_TABLE_COM2_ALL   (0xffffff)
#define AR_SWITCH_TABLE_COM2_ALL_S (0)

/* ath9k/reg_mci.h: coexistence control and the WLAN/BT switch owner. */
#define AR_MCI_INTERRUPT_EN             0x182c
#define AR_MCI_INTERRUPT_RX_MSG_EN      0x183c
#define AR_BTCOEX_CTRL                  0x18ac
#define AR_BTCOEX_CTRL_WBTIMER_EN       0x00000002
#define AR_BTCOEX_CTRL_MCI_MODE_EN      0x00000004
#define AR_BTCOEX_CTRL_SPDT_ENABLE      0x00000001
#define AR_BTCOEX_CTRL_BT_OWN_SPDT_CTRL 0x00000002

#define AR_SWITCH_TABLE_ALL   (0xfff)
#define AR_SWITCH_TABLE_ALL_S (0)

#define AR_CH0_THERM                  (0x16294)
#define AR_CH0_THERM_XPABIASLVL_MSB   0x3
#define AR_CH0_THERM_XPABIASLVL_MSB_S 0
#define AR_CH0_THERM_XPASHORT2GND     0x4
#define AR_CH0_THERM_XPASHORT2GND_S   2

#define AR_CH0_THERM_LOCAL         0x80000000
#define AR_CH0_THERM_START         0x20000000
#define AR_CH0_THERM_SAR_ADC_OUT   0x0000ff00
#define AR_CH0_THERM_SAR_ADC_OUT_S 8

#define AR_CH0_TOP2              (0x16284)
#define AR_CH0_TOP2_XPABIASLVL   (0xf000)
#define AR_CH0_TOP2_XPABIASLVL_S (12)

#define AR_CH0_XTAL             (0x16298)
#define AR_CH0_XTAL_CAPINDAC    0x7f000000
#define AR_CH0_XTAL_CAPINDAC_S  24
#define AR_CH0_XTAL_CAPOUTDAC   0x00fe0000
#define AR_CH0_XTAL_CAPOUTDAC_S 17

#define AR_PHY_PMU1       (0x16340)
#define AR_PHY_PMU1_PWD   0x1
#define AR_PHY_PMU1_PWD_S 0

#define AR_PHY_PMU2       (0x16344)
#define AR_PHY_PMU2_PGM   0x00200000
#define AR_PHY_PMU2_PGM_S 21

#define AR_PHY_RX1DB_BIQUAD_LONG_SHIFT   0x00380000
#define AR_PHY_RX1DB_BIQUAD_LONG_SHIFT_S 19
#define AR_PHY_RX6DB_BIQUAD_LONG_SHIFT   0x00c00000
#define AR_PHY_RX6DB_BIQUAD_LONG_SHIFT_S 22
#define AR_PHY_LNAGAIN_LONG_SHIFT        0xe0000000
#define AR_PHY_LNAGAIN_LONG_SHIFT_S      29
#define AR_PHY_MXRGAIN_LONG_SHIFT        0x03000000
#define AR_PHY_MXRGAIN_LONG_SHIFT_S      24
#define AR_PHY_VGAGAIN_LONG_SHIFT        0x1c000000
#define AR_PHY_VGAGAIN_LONG_SHIFT_S      26
#define AR_PHY_SCFIR_GAIN_LONG_SHIFT     0x00000001
#define AR_PHY_SCFIR_GAIN_LONG_SHIFT_S   0
#define AR_PHY_MANRXGAIN_LONG_SHIFT      0x00000002
#define AR_PHY_MANRXGAIN_LONG_SHIFT_S    1

/*
 * SM Field Definitions
 */
#define AR_PHY_CL_CAL_ENABLE          0x00000002
#define AR_PHY_PARALLEL_CAL_ENABLE    0x00000001
#define AR_PHY_TPCRG1_PD_CAL_ENABLE   0x00400000
#define AR_PHY_TPCRG1_PD_CAL_ENABLE_S 22

#define AR_PHY_ADDAC_PARACTL_OFF_PWDADC 0x00008000

#define AR_PHY_FCAL20_CAP_STATUS_0   0x01f00000
#define AR_PHY_FCAL20_CAP_STATUS_0_S 20

#define AR_PHY_RFBUS_REQ_EN        0x00000001 /* request for RF bus */
#define AR_PHY_RFBUS_GRANT_EN      0x00000001 /* RF bus granted */
#define AR_PHY_GC_TURBO_MODE       0x00000001 /* set turbo mode bits */
#define AR_PHY_GC_TURBO_SHORT      0x00000002 /* set short symbols to turbo mode setting */
#define AR_PHY_GC_DYN2040_EN       0x00000004 /* enable dyn 20/40 mode */
#define AR_PHY_GC_DYN2040_PRI_ONLY 0x00000008 /* dyn 20/40 - primary only */
#define AR_PHY_GC_DYN2040_PRI_CH                           \
    0x00000010 /* dyn 20/40 - primary ch offset (0=+10MHz, \
                  1=-10MHz)*/
#define AR_PHY_GC_DYN2040_PRI_CH_S 4
#define AR_PHY_GC_DYN2040_EXT_CH   0x00000020 /* dyn 20/40 - ext ch spacing (0=20MHz/ 1=25MHz) */
#define AR_PHY_GC_HT_EN            0x00000040 /* ht enable */
#define AR_PHY_GC_SHORT_GI_40      0x00000080 /* allow short GI for HT 40 */
#define AR_PHY_GC_WALSH            0x00000100 /* walsh spatial spreading for 2 chains,2 streams TX */
#define AR_PHY_GC_SINGLE_HT_LTF1   0x00000200 /* single length (4us) 1st HT long training symbol */
#define AR_PHY_GC_GF_DETECT_EN \
    0x00000400 /* enable Green Field detection. Only affects rx, not tx */
#define AR_PHY_GC_ENABLE_DAC_FIFO 0x00000800 /* fifo between bb and dac */
#define AR_PHY_RX_DELAY_DELAY     0x00003FFF /* delay from wakeup to rx ena */

#define AR_PHY_CALMODE_IQ                       0x00000000
#define AR_PHY_CALMODE_ADC_GAIN                 0x00000001
#define AR_PHY_CALMODE_ADC_DC_PER               0x00000002
#define AR_PHY_CALMODE_ADC_DC_INIT              0x00000003
#define AR_PHY_SWAP_ALT_CHAIN                   0x00000040
#define AR_PHY_MODE_OFDM                        0x00000000
#define AR_PHY_MODE_CCK                         0x00000001
#define AR_PHY_MODE_DYNAMIC                     0x00000004
#define AR_PHY_MODE_DYNAMIC_S                   2
#define AR_PHY_MODE_HALF                        0x00000020
#define AR_PHY_MODE_QUARTER                     0x00000040
#define AR_PHY_MAC_CLK_MODE                     0x00000080
#define AR_PHY_MODE_DYN_CCK_DISABLE             0x00000100
#define AR_PHY_MODE_SVD_HALF                    0x00000200
#define AR_PHY_ACTIVE_EN                        0x00000001
#define AR_PHY_ACTIVE_DIS                       0x00000000
#define AR_PHY_FORCE_XPA_CFG                    0x000000001
#define AR_PHY_FORCE_XPA_CFG_S                  0
#define AR_PHY_XPA_TIMING_CTL_TX_END_XPAB_OFF   0xFF000000
#define AR_PHY_XPA_TIMING_CTL_TX_END_XPAB_OFF_S 24
#define AR_PHY_XPA_TIMING_CTL_TX_END_XPAA_OFF   0x00FF0000
#define AR_PHY_XPA_TIMING_CTL_TX_END_XPAA_OFF_S 16
#define AR_PHY_XPA_TIMING_CTL_FRAME_XPAB_ON     0x0000FF00
#define AR_PHY_XPA_TIMING_CTL_FRAME_XPAB_ON_S   8
#define AR_PHY_XPA_TIMING_CTL_FRAME_XPAA_ON     0x000000FF
#define AR_PHY_XPA_TIMING_CTL_FRAME_XPAA_ON_S   0
#define AR_PHY_TX_END_TO_A2_RX_ON               0x00FF0000
#define AR_PHY_TX_END_TO_A2_RX_ON_S             16
#define AR_PHY_TX_END_DATA_START                0x000000FF
#define AR_PHY_TX_END_DATA_START_S              0
#define AR_PHY_TX_END_PA_ON                     0x0000FF00
#define AR_PHY_TX_END_PA_ON_S                   8
#define AR_PHY_TPCRG5_PD_GAIN_OVERLAP           0x0000000F
#define AR_PHY_TPCRG5_PD_GAIN_OVERLAP_S         0
#define AR_PHY_TPCRG5_PD_GAIN_BOUNDARY_1        0x000003F0
#define AR_PHY_TPCRG5_PD_GAIN_BOUNDARY_1_S      4
#define AR_PHY_TPCRG5_PD_GAIN_BOUNDARY_2        0x0000FC00
#define AR_PHY_TPCRG5_PD_GAIN_BOUNDARY_2_S      10
#define AR_PHY_TPCRG5_PD_GAIN_BOUNDARY_3        0x003F0000
#define AR_PHY_TPCRG5_PD_GAIN_BOUNDARY_3_S      16
#define AR_PHY_TPCRG5_PD_GAIN_BOUNDARY_4        0x0FC00000
#define AR_PHY_TPCRG5_PD_GAIN_BOUNDARY_4_S      22
#define AR_PHY_TPCRG1_NUM_PD_GAIN               0x0000c000
#define AR_PHY_TPCRG1_NUM_PD_GAIN_S             14
#define AR_PHY_TPCRG1_PD_GAIN_1                 0x00030000
#define AR_PHY_TPCRG1_PD_GAIN_1_S               16
#define AR_PHY_TPCRG1_PD_GAIN_2                 0x000C0000
#define AR_PHY_TPCRG1_PD_GAIN_2_S               18
#define AR_PHY_TPCRG1_PD_GAIN_3                 0x00300000
#define AR_PHY_TPCRG1_PD_GAIN_3_S               20
#define AR_PHY_TPCGR1_FORCED_DAC_GAIN           0x0000003e
#define AR_PHY_TPCGR1_FORCED_DAC_GAIN_S         1
#define AR_PHY_TPCGR1_FORCE_DAC_GAIN            0x00000001
#define AR_PHY_TXGAIN_FORCE                     0x00000001
#define AR_PHY_TXGAIN_FORCE_S                   0
#define AR_PHY_TXGAIN_FORCED_PADVGNRA           0x00003c00
#define AR_PHY_TXGAIN_FORCED_PADVGNRA_S         10
#define AR_PHY_TXGAIN_FORCED_PADVGNRB           0x0003c000
#define AR_PHY_TXGAIN_FORCED_PADVGNRB_S         14
#define AR_PHY_TXGAIN_FORCED_PADVGNRD           0x00c00000
#define AR_PHY_TXGAIN_FORCED_PADVGNRD_S         22
#define AR_PHY_TXGAIN_FORCED_TXMXRGAIN          0x000003c0
#define AR_PHY_TXGAIN_FORCED_TXMXRGAIN_S        6
#define AR_PHY_TXGAIN_FORCED_TXBB1DBGAIN        0x0000000e
#define AR_PHY_TXGAIN_FORCED_TXBB1DBGAIN_S      1

#define AR_PHY_POWER_TX_RATE_MAX                  0x993c
#define AR_PHY_POWER_TX_RATE_MAX_TPC_ENABLE       0x00000040
#define PHY_AGC_CLR                               0x10000000
#define RFSILENT_BB                               0x00002000
#define AR_PHY_CHAN_INFO_GAIN_DIFF_PPM_MASK       0xFFF
#define AR_PHY_CHAN_INFO_GAIN_DIFF_PPM_SIGNED_BIT 0x800
#define AR_PHY_CHAN_INFO_GAIN_DIFF_UPPER_LIMIT    320
#define AR_PHY_CHAN_INFO_MEMORY_CAPTURE_MASK      0x0001
#define AR_PHY_RX_DELAY_DELAY                     0x00003FFF
#define AR_PHY_CCK_TX_CTRL_JAPAN                  0x00000010

#define AR_PHY_SPECTRAL_SCAN_ENABLE           0x00000001
#define AR_PHY_SPECTRAL_SCAN_ENABLE_S         0
#define AR_PHY_SPECTRAL_SCAN_ACTIVE           0x00000002
#define AR_PHY_SPECTRAL_SCAN_ACTIVE_S         1
#define AR_PHY_SPECTRAL_SCAN_FFT_PERIOD       0x000000F0
#define AR_PHY_SPECTRAL_SCAN_FFT_PERIOD_S     4
#define AR_PHY_SPECTRAL_SCAN_PERIOD           0x0000FF00
#define AR_PHY_SPECTRAL_SCAN_PERIOD_S         8
#define AR_PHY_SPECTRAL_SCAN_COUNT            0x0FFF0000
#define AR_PHY_SPECTRAL_SCAN_COUNT_S          16
#define AR_PHY_SPECTRAL_SCAN_SHORT_REPEAT     0x10000000
#define AR_PHY_SPECTRAL_SCAN_SHORT_REPEAT_S   28
#define AR_PHY_SPECTRAL_SCAN_PRIORITY         0x20000000
#define AR_PHY_SPECTRAL_SCAN_PRIORITY_S       29
#define AR_PHY_SPECTRAL_SCAN_USE_ERR5         0x40000000
#define AR_PHY_SPECTRAL_SCAN_USE_ERR5_S       30
#define AR_PHY_SPECTRAL_SCAN_COMPRESSED_RPT   0x80000000
#define AR_PHY_SPECTRAL_SCAN_COMPRESSED_RPT_S 31

#define AR_PHY_CHANNEL_STATUS_RX_CLEAR                    0x00000004
#define AR_PHY_RTT_CTRL_ENA_RADIO_RETENTION               0x00000001
#define AR_PHY_RTT_CTRL_ENA_RADIO_RETENTION_S             0
#define AR_PHY_RTT_CTRL_RESTORE_MASK                      0x0000007E
#define AR_PHY_RTT_CTRL_RESTORE_MASK_S                    1
#define AR_PHY_RTT_CTRL_FORCE_RADIO_RESTORE               0x00000080
#define AR_PHY_RTT_CTRL_FORCE_RADIO_RESTORE_S             7
#define AR_PHY_RTT_SW_RTT_TABLE_ACCESS                    0x00000001
#define AR_PHY_RTT_SW_RTT_TABLE_ACCESS_S                  0
#define AR_PHY_RTT_SW_RTT_TABLE_WRITE                     0x00000002
#define AR_PHY_RTT_SW_RTT_TABLE_WRITE_S                   1
#define AR_PHY_RTT_SW_RTT_TABLE_ADDR                      0x0000001C
#define AR_PHY_RTT_SW_RTT_TABLE_ADDR_S                    2
#define AR_PHY_RTT_SW_RTT_TABLE_DATA                      0xFFFFFFF0
#define AR_PHY_RTT_SW_RTT_TABLE_DATA_S                    4
#define AR_PHY_TX_IQCAL_CONTROL_0_ENABLE_TXIQ_CAL         0x80000000
#define AR_PHY_TX_IQCAL_CONTROL_0_ENABLE_TXIQ_CAL_S       31
#define AR_PHY_TX_IQCAL_CONTROL_1_IQCORR_I_Q_COFF_DELPT   0x01fc0000
#define AR_PHY_TX_IQCAL_CONTROL_1_IQCORR_I_Q_COFF_DELPT_S 18
#define AR_PHY_TX_IQCAL_START_DO_CAL                      0x00000001
#define AR_PHY_TX_IQCAL_START_DO_CAL_S                    0

#define AR_PHY_TX_IQCAL_STATUS_FAILED 0x00000001
#define AR_PHY_CALIBRATED_GAINS_0     0x3e
#define AR_PHY_CALIBRATED_GAINS_0_S   1

#define AR_PHY_TX_IQCAL_CORR_COEFF_00_COEFF_TABLE   0x00003fff
#define AR_PHY_TX_IQCAL_CORR_COEFF_00_COEFF_TABLE_S 0
#define AR_PHY_TX_IQCAL_CORR_COEFF_01_COEFF_TABLE   0x0fffc000
#define AR_PHY_TX_IQCAL_CORR_COEFF_01_COEFF_TABLE_S 14

#define AR_PHY_65NM_CH0_RXTX4_THERM_ON       0x10000000
#define AR_PHY_65NM_CH0_RXTX4_THERM_ON_S     28
#define AR_PHY_65NM_CH0_RXTX4_THERM_ON_OVR   0x20000000
#define AR_PHY_65NM_CH0_RXTX4_THERM_ON_OVR_S 29

#define AR_PHY_65NM_RXTX4_XLNA_BIAS   0xC0000000
#define AR_PHY_65NM_RXTX4_XLNA_BIAS_S 30

/*
 * Channel 1 Register Map
 */
#define AR_CHAN1_BASE 0xa800

#define AR_PHY_EXT_CCA_1          (AR_CHAN1_BASE + 0x30)
#define AR_PHY_TX_PHASE_RAMP_1    (AR_CHAN1_BASE + 0xd0)
#define AR_PHY_ADC_GAIN_DC_CORR_1 (AR_CHAN1_BASE + 0xd4)

#define AR_PHY_SPUR_REPORT_1    (AR_CHAN1_BASE + 0xa8)
#define AR_PHY_CHAN_INFO_TAB_1  (AR_CHAN1_BASE + 0x300)
#define AR_PHY_RX_IQCAL_CORR_B1 (AR_CHAN1_BASE + 0xdc)

/*
 * Channel 1 Field Definitions
 */
#define AR_PHY_CH1_EXT_MINCCA_PWR   0x01FF0000
#define AR_PHY_CH1_EXT_MINCCA_PWR_S 16

/*
 * AGC 1 Register Map
 */
#define AR_AGC1_BASE 0xae00

#define AR_PHY_FORCEMAX_GAINS_1 (AR_AGC1_BASE + 0x4)
#define AR_PHY_EXT_ATTEN_CTL_1  (AR_AGC1_BASE + 0x18)
#define AR_PHY_CCA_1            (AR_AGC1_BASE + 0x1c)
#define AR_PHY_CCA_CTRL_1       (AR_AGC1_BASE + 0x20)
#define AR_PHY_RSSI_1           (AR_AGC1_BASE + 0x180)
#define AR_PHY_SPUR_CCK_REP_1   (AR_AGC1_BASE + 0x184)
#define AR_PHY_RX_OCGAIN_2      (AR_AGC1_BASE + 0x200)

/*
 * AGC 1 Field Definitions
 */
#define AR_PHY_CH1_MINCCA_PWR   0x1FF00000
#define AR_PHY_CH1_MINCCA_PWR_S 20

/*
 * SM 1 Register Map
 */
#define AR_SM1_BASE 0xb200

#define AR_PHY_SWITCH_CHAIN_1             (AR_SM1_BASE + 0x84)
#define AR_PHY_FCAL_2_1                   (AR_SM1_BASE + 0xd0)
#define AR_PHY_DFT_TONE_CTL_1             (AR_SM1_BASE + 0xd4)
#define AR_PHY_CL_TAB_1                   (AR_SM1_BASE + 0x100)
#define AR_PHY_CHAN_INFO_GAIN_1           (AR_SM1_BASE + 0x180)
#define AR_PHY_TPC_4_B1                   (AR_SM1_BASE + 0x204)
#define AR_PHY_TPC_5_B1                   (AR_SM1_BASE + 0x208)
#define AR_PHY_TPC_6_B1                   (AR_SM1_BASE + 0x20c)
#define AR_PHY_TPC_11_B1                  (AR_SM1_BASE + 0x220)
#define AR_PHY_PDADC_TAB_1                (AR_SM1_BASE + (0x240))
#define AR_PHY_TPC_19_B1                  (AR_SM1_BASE + 0x240)
#define AR_PHY_TPC_19_B1_ALPHA_THERM      0xff
#define AR_PHY_TPC_19_B1_ALPHA_THERM_S    0
#define AR_PHY_TX_IQCAL_STATUS_B1         (AR_SM1_BASE + 0x48c)
#define AR_PHY_TX_IQCAL_CORR_COEFF_B1(_i) (AR_SM1_BASE + 0x450 + ((_i) << 2))

#define AR_PHY_RTT_TABLE_SW_INTF_B(i)   (0x384 + ((i) ? AR_SM1_BASE : AR_SM_BASE))
#define AR_PHY_RTT_TABLE_SW_INTF_1_B(i) (0x388 + ((i) ? AR_SM1_BASE : AR_SM_BASE))
/*
 * Channel 2 Register Map
 */
#define AR_CHAN2_BASE                   0xb800

#define AR_PHY_EXT_CCA_2          (AR_CHAN2_BASE + 0x30)
#define AR_PHY_TX_PHASE_RAMP_2    (AR_CHAN2_BASE + 0xd0)
#define AR_PHY_ADC_GAIN_DC_CORR_2 (AR_CHAN2_BASE + 0xd4)

#define AR_PHY_SPUR_REPORT_2    (AR_CHAN2_BASE + 0xa8)
#define AR_PHY_CHAN_INFO_TAB_2  (AR_CHAN2_BASE + 0x300)
#define AR_PHY_RX_IQCAL_CORR_B2 (AR_CHAN2_BASE + 0xdc)

/*
 * Channel 2 Field Definitions
 */
#define AR_PHY_CH2_EXT_MINCCA_PWR   0x01FF0000
#define AR_PHY_CH2_EXT_MINCCA_PWR_S 16
/*
 * AGC 2 Register Map
 */
#define AR_AGC2_BASE                0xbe00

#define AR_PHY_FORCEMAX_GAINS_2 (AR_AGC2_BASE + 0x4)
#define AR_PHY_EXT_ATTEN_CTL_2  (AR_AGC2_BASE + 0x18)
#define AR_PHY_CCA_2            (AR_AGC2_BASE + 0x1c)
#define AR_PHY_CCA_CTRL_2       (AR_AGC2_BASE + 0x20)
#define AR_PHY_RSSI_2           (AR_AGC2_BASE + 0x180)

/*
 * AGC 2 Field Definitions
 */
#define AR_PHY_CH2_MINCCA_PWR   0x1FF00000
#define AR_PHY_CH2_MINCCA_PWR_S 20

/*
 * SM 2 Register Map
 */
#define AR_SM2_BASE 0xc200

#define AR_PHY_SWITCH_CHAIN_2             (AR_SM2_BASE + 0x84)
#define AR_PHY_FCAL_2_2                   (AR_SM2_BASE + 0xd0)
#define AR_PHY_DFT_TONE_CTL_2             (AR_SM2_BASE + 0xd4)
#define AR_PHY_CL_TAB_2                   (AR_SM2_BASE + 0x100)
#define AR_PHY_CHAN_INFO_GAIN_2           (AR_SM2_BASE + 0x180)
#define AR_PHY_TPC_4_B2                   (AR_SM2_BASE + 0x204)
#define AR_PHY_TPC_5_B2                   (AR_SM2_BASE + 0x208)
#define AR_PHY_TPC_6_B2                   (AR_SM2_BASE + 0x20c)
#define AR_PHY_TPC_11_B2                  (AR_SM2_BASE + 0x220)
#define AR_PHY_TPC_19_B2                  (AR_SM2_BASE + 0x240)
#define AR_PHY_TX_IQCAL_STATUS_B2         (AR_SM2_BASE + 0x48c)
#define AR_PHY_TX_IQCAL_CORR_COEFF_B2(_i) (AR_SM2_BASE + 0x450 + ((_i) << 2))

#define AR_PHY_TX_IQCAL_STATUS_B2_FAILED 0x00000001

/* GLB Registers */
#define AR_GLB_BASE         0x20000
#define AR_GLB_GPIO_CONTROL (AR_GLB_BASE)
#define AR_PHY_GLB_CONTROL  (AR_GLB_BASE + 0x44)
#define AR_GLB_SCRATCH      (AR_GLB_BASE + (0x50))
#define AR_GLB_STATUS       (AR_GLB_BASE + 0x48)

/*
 * Misc helper defines
 */
#define AR_PHY_CHAIN_OFFSET (AR_CHAN1_BASE - AR_CHAN_BASE)

#define AR_PHY_NEW_ADC_DC_GAIN_CORR(_i) (AR_PHY_ADC_GAIN_DC_CORR_0 + (AR_PHY_CHAIN_OFFSET * (_i)))
#define AR_PHY_SWITCH_CHAIN(_i)         (AR_PHY_SWITCH_CHAIN_0 + (AR_PHY_CHAIN_OFFSET * (_i)))
#define AR_PHY_EXT_ATTEN_CTL(_i)        (AR_PHY_EXT_ATTEN_CTL_0 + (AR_PHY_CHAIN_OFFSET * (_i)))

#define AR_PHY_RXGAIN(_i)    (AR_PHY_FORCEMAX_GAINS_0 + (AR_PHY_CHAIN_OFFSET * (_i)))
#define AR_PHY_TPCRG5(_i)    (AR_PHY_TPC_5_B0 + (AR_PHY_CHAIN_OFFSET * (_i)))
#define AR_PHY_PDADC_TAB(_i) (AR_PHY_PDADC_TAB_0 + (AR_PHY_CHAIN_OFFSET * (_i)))

#define AR_PHY_CAL_MEAS_0(_i) (AR_PHY_IQ_ADC_MEAS_0_B0 + (AR_PHY_CHAIN_OFFSET * (_i)))
#define AR_PHY_CAL_MEAS_1(_i) (AR_PHY_IQ_ADC_MEAS_1_B0 + (AR_PHY_CHAIN_OFFSET * (_i)))
#define AR_PHY_CAL_MEAS_2(_i) (AR_PHY_IQ_ADC_MEAS_2_B0 + (AR_PHY_CHAIN_OFFSET * (_i)))
#define AR_PHY_CAL_MEAS_3(_i) (AR_PHY_IQ_ADC_MEAS_3_B0 + (AR_PHY_CHAIN_OFFSET * (_i)))

#define AR_PHY_WATCHDOG_NON_IDLE_ENABLE 0x00000001
#define AR_PHY_WATCHDOG_IDLE_ENABLE     0x00000002
#define AR_PHY_WATCHDOG_IDLE_MASK       0xFFFF0000
#define AR_PHY_WATCHDOG_NON_IDLE_MASK   0x0000FFFC

#define AR_PHY_WATCHDOG_RST_ENABLE 0x00000002
#define AR_PHY_WATCHDOG_IRQ_ENABLE 0x00000004
#define AR_PHY_WATCHDOG_CNTL2_MASK 0xFFFFFFF9

#define AR_PHY_WATCHDOG_INFO         0x00000007
#define AR_PHY_WATCHDOG_INFO_S       0
#define AR_PHY_WATCHDOG_DET_HANG     0x00000008
#define AR_PHY_WATCHDOG_DET_HANG_S   3
#define AR_PHY_WATCHDOG_RADAR_SM     0x000000F0
#define AR_PHY_WATCHDOG_RADAR_SM_S   4
#define AR_PHY_WATCHDOG_RX_OFDM_SM   0x00000F00
#define AR_PHY_WATCHDOG_RX_OFDM_SM_S 8
#define AR_PHY_WATCHDOG_RX_CCK_SM    0x0000F000
#define AR_PHY_WATCHDOG_RX_CCK_SM_S  12
#define AR_PHY_WATCHDOG_TX_OFDM_SM   0x000F0000
#define AR_PHY_WATCHDOG_TX_OFDM_SM_S 16
#define AR_PHY_WATCHDOG_TX_CCK_SM    0x00F00000
#define AR_PHY_WATCHDOG_TX_CCK_SM_S  20
#define AR_PHY_WATCHDOG_AGC_SM       0x0F000000
#define AR_PHY_WATCHDOG_AGC_SM_S     24
#define AR_PHY_WATCHDOG_SRCH_SM      0xF0000000
#define AR_PHY_WATCHDOG_SRCH_SM_S    28

#define AR_PHY_WATCHDOG_STATUS_CLR 0x00000008

/*
 * PAPRD registers
 */
#define AR_PHY_XPA_TIMING_CTL (AR_SM_BASE + 0x64)

#define AR_PHY_PAPRD_AM2AM        (AR_CHAN_BASE + 0xe4)
#define AR_PHY_PAPRD_AM2AM_MASK   0x01ffffff
#define AR_PHY_PAPRD_AM2AM_MASK_S 0

#define AR_PHY_PAPRD_AM2PM        (AR_CHAN_BASE + 0xe8)
#define AR_PHY_PAPRD_AM2PM_MASK   0x01ffffff
#define AR_PHY_PAPRD_AM2PM_MASK_S 0

#define AR_PHY_PAPRD_HT40        (AR_CHAN_BASE + 0xec)
#define AR_PHY_PAPRD_HT40_MASK   0x01ffffff
#define AR_PHY_PAPRD_HT40_MASK_S 0

#define AR_PHY_PAPRD_CTRL0_B0                      (AR_CHAN_BASE + 0xf0)
#define AR_PHY_PAPRD_CTRL0_B1                      (AR_CHAN1_BASE + 0xf0)
#define AR_PHY_PAPRD_CTRL0_B2                      (AR_CHAN2_BASE + 0xf0)
#define AR_PHY_PAPRD_CTRL0_PAPRD_ENABLE            0x00000001
#define AR_PHY_PAPRD_CTRL0_PAPRD_ENABLE_S          0
#define AR_PHY_PAPRD_CTRL0_USE_SINGLE_TABLE_MASK   0x00000002
#define AR_PHY_PAPRD_CTRL0_USE_SINGLE_TABLE_MASK_S 1
#define AR_PHY_PAPRD_CTRL0_PAPRD_MAG_THRSH         0xf8000000
#define AR_PHY_PAPRD_CTRL0_PAPRD_MAG_THRSH_S       27

#define AR_PHY_PAPRD_CTRL1_B0                         (AR_CHAN_BASE + 0xf4)
#define AR_PHY_PAPRD_CTRL1_B1                         (AR_CHAN1_BASE + 0xf4)
#define AR_PHY_PAPRD_CTRL1_B2                         (AR_CHAN2_BASE + 0xf4)
#define AR_PHY_PAPRD_CTRL1_ADAPTIVE_SCALING_ENA       0x00000001
#define AR_PHY_PAPRD_CTRL1_ADAPTIVE_SCALING_ENA_S     0
#define AR_PHY_PAPRD_CTRL1_ADAPTIVE_AM2AM_ENABLE      0x00000002
#define AR_PHY_PAPRD_CTRL1_ADAPTIVE_AM2AM_ENABLE_S    1
#define AR_PHY_PAPRD_CTRL1_ADAPTIVE_AM2PM_ENABLE      0x00000004
#define AR_PHY_PAPRD_CTRL1_ADAPTIVE_AM2PM_ENABLE_S    2
#define AR_PHY_PAPRD_CTRL1_PAPRD_POWER_AT_AM2AM_CAL   0x000001f8
#define AR_PHY_PAPRD_CTRL1_PAPRD_POWER_AT_AM2AM_CAL_S 3
#define AR_PHY_PAPRD_CTRL1_PA_GAIN_SCALE_FACT_MASK    0x0001fe00
#define AR_PHY_PAPRD_CTRL1_PA_GAIN_SCALE_FACT_MASK_S  9
#define AR_PHY_PAPRD_CTRL1_PAPRD_MAG_SCALE_FACT       0x0ffe0000
#define AR_PHY_PAPRD_CTRL1_PAPRD_MAG_SCALE_FACT_S     17

#define AR_PHY_PAPRD_TRAINER_CNTL1 (AR_SM_BASE + (0x490))

#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_CF_PAPRD_TRAIN_ENABLE    0x00000001
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_CF_PAPRD_TRAIN_ENABLE_S  0
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_AGC2_SETTLING      0x0000007e
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_AGC2_SETTLING_S    1
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_IQCORR_ENABLE      0x00000100
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_IQCORR_ENABLE_S    8
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_RX_BB_GAIN_FORCE   0x00000200
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_RX_BB_GAIN_FORCE_S 9
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_TX_GAIN_FORCE      0x00000400
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_TX_GAIN_FORCE_S    10
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_LB_ENABLE          0x00000800
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_LB_ENABLE_S        11
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_LB_SKIP            0x0003f000
#define AR_PHY_PAPRD_TRAINER_CNTL1_CF_PAPRD_LB_SKIP_S          12

#define AR_PHY_PAPRD_TRAINER_CNTL2 (AR_SM_BASE + (0x494))

#define AR_PHY_PAPRD_TRAINER_CNTL2_CF_PAPRD_INIT_RX_BB_GAIN   0xFFFFFFFF
#define AR_PHY_PAPRD_TRAINER_CNTL2_CF_PAPRD_INIT_RX_BB_GAIN_S 0

#define AR_PHY_PAPRD_TRAINER_CNTL3 (AR_SM_BASE + (0x498))

#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_ADC_DESIRED_SIZE   0x0000003f
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_ADC_DESIRED_SIZE_S 0
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_QUICK_DROP         0x00000fc0
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_QUICK_DROP_S       6
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_MIN_LOOPBACK_DEL   0x0001f000
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_MIN_LOOPBACK_DEL_S 12
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_NUM_CORR_STAGES    0x000e0000
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_NUM_CORR_STAGES_S  17
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_COARSE_CORR_LEN    0x00f00000
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_COARSE_CORR_LEN_S  20
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_FINE_CORR_LEN      0x0f000000
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_FINE_CORR_LEN_S    24
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_BBTXMIX_DISABLE    0x20000000
#define AR_PHY_PAPRD_TRAINER_CNTL3_CF_PAPRD_BBTXMIX_DISABLE_S  29

#define AR_PHY_PAPRD_TRAINER_CNTL4 (AR_SM_BASE + (0x49c))

#define AR_PHY_PAPRD_TRAINER_CNTL4_CF_PAPRD_NUM_TRAIN_SAMPLES   0x03ff0000
#define AR_PHY_PAPRD_TRAINER_CNTL4_CF_PAPRD_NUM_TRAIN_SAMPLES_S 16
#define AR_PHY_PAPRD_TRAINER_CNTL4_CF_PAPRD_SAFETY_DELTA        0x0000f000
#define AR_PHY_PAPRD_TRAINER_CNTL4_CF_PAPRD_SAFETY_DELTA_S      12
#define AR_PHY_PAPRD_TRAINER_CNTL4_CF_PAPRD_MIN_CORR            0x00000fff
#define AR_PHY_PAPRD_TRAINER_CNTL4_CF_PAPRD_MIN_CORR_S          0

#define AR_PHY_PAPRD_PRE_POST_SCALE_0_B0 (AR_CHAN_BASE + 0x100)
#define AR_PHY_PAPRD_PRE_POST_SCALE_1_B0 (AR_CHAN_BASE + 0x104)
#define AR_PHY_PAPRD_PRE_POST_SCALE_2_B0 (AR_CHAN_BASE + 0x108)
#define AR_PHY_PAPRD_PRE_POST_SCALE_3_B0 (AR_CHAN_BASE + 0x10c)
#define AR_PHY_PAPRD_PRE_POST_SCALE_4_B0 (AR_CHAN_BASE + 0x110)
#define AR_PHY_PAPRD_PRE_POST_SCALE_5_B0 (AR_CHAN_BASE + 0x114)
#define AR_PHY_PAPRD_PRE_POST_SCALE_6_B0 (AR_CHAN_BASE + 0x118)
#define AR_PHY_PAPRD_PRE_POST_SCALE_7_B0 (AR_CHAN_BASE + 0x11c)
#define AR_PHY_PAPRD_PRE_POST_SCALING    0x3FFFF
#define AR_PHY_PAPRD_PRE_POST_SCALING_S  0

#define AR_PHY_PAPRD_TRAINER_STAT1 (AR_SM_BASE + (0x4a0))

#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_TRAIN_DONE         0x00000001
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_TRAIN_DONE_S       0
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_TRAIN_INCOMPLETE   0x00000002
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_TRAIN_INCOMPLETE_S 1
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_CORR_ERR           0x00000004
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_CORR_ERR_S         2
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_TRAIN_ACTIVE       0x00000008
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_TRAIN_ACTIVE_S     3
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_RX_GAIN_IDX        0x000001f0
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_RX_GAIN_IDX_S      4
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_AGC2_PWR           0x0001fe00
#define AR_PHY_PAPRD_TRAINER_STAT1_PAPRD_AGC2_PWR_S         9

#define AR_PHY_PAPRD_TRAINER_STAT2 (AR_SM_BASE + (0x4a4))

#define AR_PHY_PAPRD_TRAINER_STAT2_PAPRD_FINE_VAL     0x0000ffff
#define AR_PHY_PAPRD_TRAINER_STAT2_PAPRD_FINE_VAL_S   0
#define AR_PHY_PAPRD_TRAINER_STAT2_PAPRD_COARSE_IDX   0x001f0000
#define AR_PHY_PAPRD_TRAINER_STAT2_PAPRD_COARSE_IDX_S 16
#define AR_PHY_PAPRD_TRAINER_STAT2_PAPRD_FINE_IDX     0x00600000
#define AR_PHY_PAPRD_TRAINER_STAT2_PAPRD_FINE_IDX_S   21

#define AR_PHY_PAPRD_TRAINER_STAT3 (AR_SM_BASE + (0x4a8))

#define AR_PHY_PAPRD_TRAINER_STAT3_PAPRD_TRAIN_SAMPLES_CNT   0x000fffff
#define AR_PHY_PAPRD_TRAINER_STAT3_PAPRD_TRAIN_SAMPLES_CNT_S 0

#define AR_PHY_PAPRD_MEM_TAB_B0 (AR_CHAN_BASE + 0x120)
#define AR_PHY_PAPRD_MEM_TAB_B1 (AR_CHAN1_BASE + 0x120)
#define AR_PHY_PAPRD_MEM_TAB_B2 (AR_CHAN2_BASE + 0x120)

#define AR_PHY_PA_GAIN123_B0         (AR_CHAN_BASE + 0xf8)
#define AR_PHY_PA_GAIN123_B1         (AR_CHAN1_BASE + 0xf8)
#define AR_PHY_PA_GAIN123_B2         (AR_CHAN2_BASE + 0xf8)
#define AR_PHY_PA_GAIN123_PA_GAIN1   0x3FF
#define AR_PHY_PA_GAIN123_PA_GAIN1_S 0

#define AR_PHY_POWERTX_RATE5                 (AR_SM_BASE + 0x1d0)
#define AR_PHY_POWERTX_RATE5_POWERTXHT20_0   0x3F
#define AR_PHY_POWERTX_RATE5_POWERTXHT20_0_S 0

#define AR_PHY_POWERTX_RATE6                 (AR_SM_BASE + 0x1d4)
#define AR_PHY_POWERTX_RATE6_POWERTXHT20_5   0x3F00
#define AR_PHY_POWERTX_RATE6_POWERTXHT20_5_S 8

#define AR_PHY_POWERTX_RATE8                 (AR_SM_BASE + 0x1dc)
#define AR_PHY_POWERTX_RATE8_POWERTXHT40_5   0x3F00
#define AR_PHY_POWERTX_RATE8_POWERTXHT40_5_S 8

#define AR_PHY_CL_TAB_CL_GAIN_MOD   0x1f
#define AR_PHY_CL_TAB_CL_GAIN_MOD_S 0

#define AR_BTCOEX_WL_LNADIV                                0x1a64
#define AR_BTCOEX_WL_LNADIV_PREDICTED_PERIOD               0x00003FFF
#define AR_BTCOEX_WL_LNADIV_PREDICTED_PERIOD_S             0
#define AR_BTCOEX_WL_LNADIV_DPDT_IGNORE_PRIORITY           0x00004000
#define AR_BTCOEX_WL_LNADIV_DPDT_IGNORE_PRIORITY_S         14
#define AR_BTCOEX_WL_LNADIV_FORCE_ON                       0x00008000
#define AR_BTCOEX_WL_LNADIV_FORCE_ON_S                     15
#define AR_BTCOEX_WL_LNADIV_MODE_OPTION                    0x00030000
#define AR_BTCOEX_WL_LNADIV_MODE_OPTION_S                  16
#define AR_BTCOEX_WL_LNADIV_MODE                           0x007c0000
#define AR_BTCOEX_WL_LNADIV_MODE_S                         18
#define AR_BTCOEX_WL_LNADIV_ALLOWED_TX_ANTDIV_WL_TX_REQ    0x00800000
#define AR_BTCOEX_WL_LNADIV_ALLOWED_TX_ANTDIV_WL_TX_REQ_S  23
#define AR_BTCOEX_WL_LNADIV_DISABLE_TX_ANTDIV_ENABLE       0x01000000
#define AR_BTCOEX_WL_LNADIV_DISABLE_TX_ANTDIV_ENABLE_S     24
#define AR_BTCOEX_WL_LNADIV_CONTINUOUS_BT_ACTIVE_PROTECT   0x02000000
#define AR_BTCOEX_WL_LNADIV_CONTINUOUS_BT_ACTIVE_PROTECT_S 25
#define AR_BTCOEX_WL_LNADIV_BT_INACTIVE_THRESHOLD          0xFC000000
#define AR_BTCOEX_WL_LNADIV_BT_INACTIVE_THRESHOLD_S        26

/* Manual Peak detector calibration */
#define AR_PHY_65NM_BASE                             0x16000
#define AR_PHY_65NM_RXRF_GAINSTAGES(i)               (AR_PHY_65NM_BASE + (i * 0x400) + 0x8)
#define AR_PHY_65NM_RXRF_GAINSTAGES_RX_OVERRIDE      0x80000000
#define AR_PHY_65NM_RXRF_GAINSTAGES_RX_OVERRIDE_S    31
#define AR_PHY_65NM_RXRF_GAINSTAGES_LNAON_CALDC      0x00000002
#define AR_PHY_65NM_RXRF_GAINSTAGES_LNAON_CALDC_S    1
#define AR_PHY_65NM_RXRF_GAINSTAGES_LNA2G_GAIN_OVR   0x70000000
#define AR_PHY_65NM_RXRF_GAINSTAGES_LNA2G_GAIN_OVR_S 28
#define AR_PHY_65NM_RXRF_GAINSTAGES_LNA5G_GAIN_OVR   0x03800000
#define AR_PHY_65NM_RXRF_GAINSTAGES_LNA5G_GAIN_OVR_S 23

#define AR_PHY_65NM_RXTX2(i)         (AR_PHY_65NM_BASE + (i * 0x400) + 0x104)
#define AR_PHY_65NM_RXTX2_RXON_OVR   0x00001000
#define AR_PHY_65NM_RXTX2_RXON_OVR_S 12
#define AR_PHY_65NM_RXTX2_RXON       0x00000800
#define AR_PHY_65NM_RXTX2_RXON_S     11

#define AR_PHY_65NM_RXRF_AGC(i)                 (AR_PHY_65NM_BASE + (i * 0x400) + 0xc)
#define AR_PHY_65NM_RXRF_AGC_AGC_OVERRIDE       0x80000000
#define AR_PHY_65NM_RXRF_AGC_AGC_OVERRIDE_S     31
#define AR_PHY_65NM_RXRF_AGC_AGC_ON_OVR         0x40000000
#define AR_PHY_65NM_RXRF_AGC_AGC_ON_OVR_S       30
#define AR_PHY_65NM_RXRF_AGC_AGC_CAL_OVR        0x20000000
#define AR_PHY_65NM_RXRF_AGC_AGC_CAL_OVR_S      29
#define AR_PHY_65NM_RXRF_AGC_AGC2G_DBDAC_OVR    0x1E000000
#define AR_PHY_65NM_RXRF_AGC_AGC2G_DBDAC_OVR_S  25
#define AR_PHY_65NM_RXRF_AGC_AGC5G_DBDAC_OVR    0x00078000
#define AR_PHY_65NM_RXRF_AGC_AGC5G_DBDAC_OVR_S  15
#define AR_PHY_65NM_RXRF_AGC_AGC2G_CALDAC_OVR   0x01F80000
#define AR_PHY_65NM_RXRF_AGC_AGC2G_CALDAC_OVR_S 19
#define AR_PHY_65NM_RXRF_AGC_AGC5G_CALDAC_OVR   0x00007e00
#define AR_PHY_65NM_RXRF_AGC_AGC5G_CALDAC_OVR_S 9
#define AR_PHY_65NM_RXRF_AGC_AGC_OUT            0x00000004
#define AR_PHY_65NM_RXRF_AGC_AGC_OUT_S          2

#define AR9300_DFS_FIRPWR -28

/* The EEPROM and OTP windows (ath9k/eeprom.h and ar9003_eeprom.h). */

/* A read of the EEPROM's 16-bit word N is a read of AR_EEPROM_WORD(N),
 * which starts it; the word is in AR_EEPROM_STATUS_DATA_VAL once
 * AR_EEPROM_STATUS_DATA_BUSY and _PROT_ACCESS are clear. */
#define AR5416_EEPROM_S      2
#define AR5416_EEPROM_OFFSET 0x2000
#define AR_EEPROM_WORD(n)    (AR5416_EEPROM_OFFSET + ((n) << AR5416_EEPROM_S))

/* The OTP is read a 32-bit word at a time at AR9300_OTP_BASE + 4 * N; the
 * word is in AR9300_OTP_READ_DATA once AR9300_OTP_STATUS's type field is
 * AR9300_OTP_STATUS_VALID. */
#define AR9300_OTP_BASE               0x14000
#define AR9300_OTP_STATUS             0x15f18
#define AR9300_OTP_STATUS_TYPE        0x7
#define AR9300_OTP_STATUS_VALID       0x4
#define AR9300_OTP_STATUS_ACCESS_BUSY 0x2
#define AR9300_OTP_STATUS_SM_BUSY     0x1
#define AR9300_OTP_READ_DATA          0x15f1c

/* The AR9003 descriptors (ath9k/ar9003_mac.h). The chip reads and writes
 * them by DMA, in little-endian 32-bit words, at 4-byte alignment. */

/* A receive buffer's status, which the chip writes at the buffer's head
 * before the frame; status11 holds AR_RxDone. */
struct ar9003_rxs {
    uint32_t ds_info;
    uint32_t status1;
    uint32_t status2;
    uint32_t status3;
    uint32_t status4;
    uint32_t status5;
    uint32_t status6;
    uint32_t status7;
    uint32_t status8;
    uint32_t status9;
    uint32_t status10;
    uint32_t status11;
};

/* A transmit control descriptor, 128 bytes so that one fills a cache line
 * of 32 words. */
struct ar9003_txc {
    uint32_t info;   /* descriptor information */
    uint32_t link;   /* link pointer */
    uint32_t data0;  /* data pointer to 1st buffer */
    uint32_t ctl3;   /* DMA control 3 */
    uint32_t data1;  /* data pointer to 2nd buffer */
    uint32_t ctl5;   /* DMA control 5 */
    uint32_t data2;  /* data pointer to 3rd buffer */
    uint32_t ctl7;   /* DMA control 7 */
    uint32_t data3;  /* data pointer to 4th buffer */
    uint32_t ctl9;   /* DMA control 9 */
    uint32_t ctl10;  /* DMA control 10 */
    uint32_t ctl11;  /* DMA control 11 */
    uint32_t ctl12;  /* DMA control 12 */
    uint32_t ctl13;  /* DMA control 13 */
    uint32_t ctl14;  /* DMA control 14 */
    uint32_t ctl15;  /* DMA control 15 */
    uint32_t ctl16;  /* DMA control 16 */
    uint32_t ctl17;  /* DMA control 17 */
    uint32_t ctl18;  /* DMA control 18 */
    uint32_t ctl19;  /* DMA control 19 */
    uint32_t ctl20;  /* DMA control 20 */
    uint32_t ctl21;  /* DMA control 21 */
    uint32_t ctl22;  /* DMA control 22 */
    uint32_t ctl23;  /* DMA control 23 */
    uint32_t pad[8]; /* pad to cache line (128 bytes/32 dwords) */
};

/* An entry of the transmit status ring; status8 holds AR_TxDone. */
struct ar9003_txs {
    uint32_t ds_info;
    uint32_t status1;
    uint32_t status2;
    uint32_t status3;
    uint32_t status4;
    uint32_t status5;
    uint32_t status6;
    uint32_t status7;
    uint32_t status8;
};

#endif
