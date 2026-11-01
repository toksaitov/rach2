/*
 * ath9k_model.h: a software chip of the AR9003 family that the ath9k
 * driver's host tests run the driver's parts against.
 *
 * The model is a register file of the chip's window with the behaviour the
 * driver's sequences depend on built in, a log of every access in order,
 * and hooks through which a test adds what its part needs. It is the chip
 * of a row of the driver's table, which the test names. It is written from
 * ath9k (Linux v6.16), which is all that is known of the family; a chip
 * that behaves otherwise passes here and fails on the machine.
 *
 * What the model does by itself:
 *
 * - AR_SREV reads the row's MAC version and the revision given to
 *   ath9k_model_init, in the AR9003 form ath9k decodes.
 * - The RTC: AR_RTC_STATUS reads SHUTDOWN while AR_RTC_RESET's enable bit
 *   is clear, ON while it and AR_RTC_FORCE_WAKE's are set, and SLEEP
 *   otherwise. Clearing AR_RTC_RESET's enable bit is a power-on reset, and
 *   setting AR_RTC_RC's MAC_WARM or MAC_COLD bit a warm or cold one: each
 *   clears the register file but for the host interface (0x4000 to 0x4fff)
 *   and the RTC (0x7000 to 0x7fff), forgets the FIFOs, and calls the reset
 *   hook.
 * - The EEPROM: a read of AR_EEPROM_WORD(n) puts word n of the test's
 *   image, or 0xffff past its end, in AR_EEPROM_STATUS_DATA with the busy
 *   bits clear. The OTP: a read at AR9300_OTP_BASE + 4 * n puts the
 *   little-endian word of bytes 4n to 4n + 3 in AR9300_OTP_READ_DATA, and
 *   AR9300_OTP_STATUS reads VALID while an OTP image is set.
 * - Interrupts: AR_ISR holds the raised causes, and a write to it clears
 *   the bits written; a read of AR_ISR_RAC returns them and clears them.
 *   AR_INTR_ASYNC_CAUSE reads AR_INTR_MAC_IRQ while any is raised.
 * - Receive: AR_CR reads AR_CR_RXE after a write of it and until a write of
 *   AR_CR_RXD. A write of AR_HP_RXDP or AR_LP_RXDP pushes a buffer's bus
 *   address on that FIFO; ath9k_model_rx fills the oldest.
 * - Transmit: a write of AR_QTXDP(q) pushes a descriptor chain on queue
 *   q's FIFO and AR_QSTS(q) counts the chains there; AR_Q_TXE reads the
 *   queues that hold one. A write of AR_Q_TXD empties the queues it names.
 *   A write of AR_Q_STATUS_RING_START starts the status ring there;
 *   ath9k_model_tx_complete finishes the oldest chain of a queue.
 * - AR_TSF_L32 and AR_TSF_U32 read the microseconds the driver has waited.
 *
 * The receive and transmit calls reach the driver's memory only through
 * the dma hook, which maps a bus address the driver gave to host memory.
 *
 * Every function is for one thread, the test's.
 */
#ifndef _ATH9K_MODEL_H_
#define _ATH9K_MODEL_H_

#include "network/driver/ath9k/ath9k.h"

/* The bytes of the window the model keeps: every register of reg.h and
 * the OTP window lie below it. */
#define ATH9K_MODEL_WINDOW 0x20000

/* The largest EEPROM and OTP, 4 KiB, the most ath9k reads of either
 * (AR9300_BASE_ADDR_4K): the EEPROM's window is 0x2000 to 0x3fff and the
 * OTP's 0x14000 to 0x14fff, clear of every register. */
#define ATH9K_MODEL_NVRAM 4096

/* The accesses the log keeps; later ones are counted in log_lost. */
#define ATH9K_MODEL_LOG 16384

/* What a reading of an address outside the window returns. */
#define ATH9K_MODEL_UNMAPPED 0xdeadbeefu

/* The access kinds of the log, and the resets of the reset hook. */
enum ath9k_model_op { ATH9K_MODEL_READ, ATH9K_MODEL_WRITE, ATH9K_MODEL_DELAY };
enum ath9k_model_reset {
    ATH9K_MODEL_RESET_POWER_ON,
    ATH9K_MODEL_RESET_WARM,
    ATH9K_MODEL_RESET_COLD
};

/* One access: a read and the value it returned, a write and the value
 * written, or a wait and its microseconds in `value` (reg 0). */
struct ath9k_model_access {
    uint8_t op; /* enum ath9k_model_op */
    uint32_t reg;
    uint32_t value;
};

struct ath9k_model;

/* What a test adds. Each hook is optional. */
struct ath9k_model_hooks {
    /** Answer a read before the model does: return nonzero with *value set
     * to answer it, or 0 to let the model answer. */
    int (*read)(struct ath9k_model *m, uint32_t reg, uint32_t *value);
    /** See a write after the model has applied it. */
    void (*write)(struct ath9k_model *m, uint32_t reg, uint32_t value);
    /** See a reset (enum ath9k_model_reset) after the model has cleared
     * its registers, to give them the values the test's chip resets to. */
    void (*reset)(struct ath9k_model *m, unsigned kind);
    /** Map `len` bytes at bus address `bus` to host memory, or return NULL
     * for an address the driver was not given; the model counts that as a
     * DMA fault and touches nothing. */
    void *(*dma)(struct ath9k_model *m, uint32_t bus, unsigned len);
};

/* One FIFO of bus addresses the driver pushed, oldest first. */
struct ath9k_model_fifo {
    uint32_t bus[ATH9K_RX_LP_DEPTH];
    unsigned depth; /* the most it holds; a push past it is an overrun */
    unsigned head;
    unsigned count;
};

struct ath9k_model {
    uint32_t reg[ATH9K_MODEL_WINDOW / 4]; /* by offset / 4 */
    struct ath9k_model_hooks hooks;
    void *priv; /* the test's */

    uint32_t srev; /* what AR_SREV reads */

    uint16_t eeprom[ATH9K_MODEL_NVRAM / 2]; /* the EEPROM's words */
    unsigned eeprom_words;                  /* 0 when it has none */
    uint8_t otp[ATH9K_MODEL_NVRAM];
    unsigned otp_len; /* 0 when it has none */

    uint64_t usec;                 /* microseconds waited, which the TSF counts */
    uint8_t rxe;                   /* receive enabled through AR_CR */
    struct ath9k_model_fifo rx[2]; /* [0] high priority, [1] low */
    struct ath9k_model_fifo tx[AR_NUM_QCU];
    uint32_t txs_start, txs_end; /* the status ring's bus addresses */
    uint32_t txs_next;           /* where the next status goes */

    struct ath9k_model_access log[ATH9K_MODEL_LOG];
    unsigned log_count; /* entries kept */
    unsigned log_lost;  /* entries past ATH9K_MODEL_LOG */
    uint8_t log_reads;  /* log reads too; writes and waits always */

    /* Faults the test injects. */
    uint8_t dead;         /* every read returns 0xffffffff, as off the bus */
    uint8_t stuck_rtc;    /* AR_RTC_STATUS never reads ON */
    uint8_t stuck_eeprom; /* AR_EEPROM_STATUS_DATA stays busy */
    uint8_t stuck_rx_dma; /* AR_CR_RXE stays set after AR_CR_RXD */
    uint8_t stuck_tx_dma; /* AR_Q_TXD empties no queue */

    /* What went wrong on the driver's side. */
    unsigned unmapped;   /* accesses outside the window */
    unsigned dma_faults; /* bus addresses the dma hook refused */
    unsigned overruns;   /* pushes onto a full FIFO, which were dropped */
};

/** The model's register access, for `struct ath9k`'s io; its context is
 * the `struct ath9k_model`. */
extern const struct ath9k_io ath9k_model_io;

/** Make `m` a powered-off `chip` of revision `mac_rev`, one of its row's
 * revs, with no EEPROM, no OTP, no hooks and an empty log. */
void ath9k_model_init(struct ath9k_model *m, const struct ath9k_chip *chip, uint32_t mac_rev);

/** Point `ah`'s register access at `m`. */
void ath9k_model_attach(struct ath9k_model *m, struct ath9k *ah);

/** Give the model an EEPROM of `n` words, or an OTP of `n` bytes, of at
 * most ATH9K_MODEL_NVRAM bytes; 0 removes it. */
void ath9k_model_set_eeprom(struct ath9k_model *m, const uint16_t *words, unsigned n);
void ath9k_model_set_otp(struct ath9k_model *m, const uint8_t *bytes, unsigned n);

/** A register's value as the driver last left it, read without a log
 * entry or a hook; set one the same way. */
uint32_t ath9k_model_peek(const struct ath9k_model *m, uint32_t reg);
void ath9k_model_poke(struct ath9k_model *m, uint32_t reg, uint32_t value);

/** Raise interrupt causes `isr` (AR_ISR bits). */
void ath9k_model_raise(struct ath9k_model *m, uint32_t isr);

/** Empty the log. */
void ath9k_model_log_clear(struct ath9k_model *m);

/** The index of the first log entry at or after `from` of kind `op` on
 * `reg`, or -1. */
int ath9k_model_find(const struct ath9k_model *m, unsigned from, unsigned op, uint32_t reg);

/** The log entries of kind `op` on `reg`. */
unsigned ath9k_model_count(const struct ath9k_model *m, unsigned op, uint32_t reg);

/** The value of the last write to `reg` in the log, or `otherwise` when
 * the log has none. */
uint32_t ath9k_model_last_write(const struct ath9k_model *m, uint32_t reg, uint32_t otherwise);

/**
 * Receive `len` bytes of `frame` into the oldest buffer of FIFO `hp` (1
 * high, 0 low), as the chip does: `status`, or zeros when it is NULL, at
 * the buffer's head with AR_DescId 0x168c, AR_TxRxDesc and AR_CtrlStat
 * clear, the length in AR_DataLen and AR_RxDone set, the frame after it,
 * and AR_ISR_HP_RXOK or AR_ISR_LP_RXOK raised. Returns 0, -1 when the
 * FIFO is empty, or -2 when the frame does not fit or the buffer cannot be
 * mapped.
 */
int ath9k_model_rx(struct ath9k_model *m, int hp, const uint8_t *frame, unsigned len,
                   const struct ar9003_rxs *status);

/**
 * Finish the oldest descriptor chain of queue `q`, as the chip does: write
 * `status`, or zeros when it is NULL, to the status ring with ds_info's
 * AR_DescId 0x168c, AR_TxRxDesc, AR_CtrlStat and AR_TxQcuNum q set and
 * AR_TxDone set, step the ring, and raise AR_ISR_TXOK, or AR_ISR_TXERR
 * when the status has AR_ExcessiveRetries or AR_Filtered. Returns the
 * chain's bus address, or 0 when the queue is empty or the ring cannot be
 * mapped.
 */
uint32_t ath9k_model_tx_complete(struct ath9k_model *m, unsigned q,
                                 const struct ar9003_txs *status);

#endif
