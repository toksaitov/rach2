/*
 * ath9k_model.c: the software chip of ath9k_model.h, which the ath9k
 * driver's host tests link through their host-deps lines.
 */

#include "ath9k_model.h"

/* The ranges a reset leaves alone: the host interface and the RTC. */
#define HOST_IF_FIRST 0x4000
#define HOST_IF_LAST  0x4fff
#define RTC_FIRST     0x7000
#define RTC_LAST      0x7fff

/* The ds_info of a status the chip writes (ar9003_mac.c): the vendor ID,
 * and for transmit the descriptor and control-status flags and the queue. */
#define DS_INFO_VENDOR ((uint32_t)0x168c << AR_DescId_S)
#define DS_INFO_TX     (DS_INFO_VENDOR | AR_TxRxDesc | AR_CtrlStat)

/** Append an access to the log, or count it as lost when the log is full. */
static void log_access(struct ath9k_model *m, unsigned op, uint32_t reg, uint32_t value)
{
    struct ath9k_model_access *a;

    if (m->log_count == ATH9K_MODEL_LOG) {
        m->log_lost++;
        return;
    }
    a = &m->log[m->log_count++];
    a->op = (uint8_t)op;
    a->reg = reg;
    a->value = value;
}

/** Whether `reg` is a word of the window. */
static int in_window(uint32_t reg)
{
    return reg < ATH9K_MODEL_WINDOW && (reg & 3) == 0;
}

/** Empty `f`, which holds at most `depth` addresses. */
static void fifo_reset(struct ath9k_model_fifo *f, unsigned depth)
{
    f->depth = depth;
    f->head = 0;
    f->count = 0;
}

/** Push `bus` on `f`, counting an overrun when it is full. */
static void fifo_push(struct ath9k_model *m, struct ath9k_model_fifo *f, uint32_t bus)
{
    if (f->count == f->depth) {
        m->overruns++;
        return;
    }
    f->bus[(f->head + f->count) % f->depth] = bus;
    f->count++;
}

/** Pop the oldest address of `f`, or return 0 when it is empty. */
static uint32_t fifo_pop(struct ath9k_model_fifo *f)
{
    uint32_t bus;

    if (f->count == 0)
        return 0;
    bus = f->bus[f->head];
    f->head = (f->head + 1) % f->depth;
    f->count--;
    return bus;
}

/** Forget every pushed buffer and chain, as a reset of the chip does. */
static void fifos_reset(struct ath9k_model *m)
{
    unsigned q;

    fifo_reset(&m->rx[0], ATH9K_RX_HP_DEPTH);
    fifo_reset(&m->rx[1], ATH9K_RX_LP_DEPTH);
    for (q = 0; q < AR_NUM_QCU; q++)
        fifo_reset(&m->tx[q], ATH9K_TX_FIFO_DEPTH);
    m->rxe = 0;
}

/** Reset the chip by `kind`: clear the registers outside the host
 * interface and the RTC, forget the FIFOs and call the reset hook. */
static void chip_reset(struct ath9k_model *m, unsigned kind)
{
    uint32_t off;

    for (off = 0; off < ATH9K_MODEL_WINDOW; off += 4)
        if ((off < HOST_IF_FIRST || off > HOST_IF_LAST) && (off < RTC_FIRST || off > RTC_LAST))
            m->reg[off / 4] = 0;
    fifos_reset(m);
    if (m->hooks.reset)
        m->hooks.reset(m, kind);
}

/** The RTC's state from AR_RTC_RESET and AR_RTC_FORCE_WAKE. */
static uint32_t rtc_status(const struct ath9k_model *m)
{
    if (!(m->reg[AR_RTC_RESET / 4] & AR_RTC_RESET_EN))
        return AR_RTC_STATUS_SHUTDOWN;
    if ((m->reg[AR_RTC_FORCE_WAKE / 4] & AR_RTC_FORCE_WAKE_EN) && !m->stuck_rtc)
        return AR_RTC_STATUS_ON;
    return AR_RTC_STATUS_SLEEP;
}

/** The queues that hold a chain, as AR_Q_TXE reads. */
static uint32_t tx_enabled(const struct ath9k_model *m)
{
    uint32_t bits = 0;
    unsigned q;

    for (q = 0; q < AR_NUM_QCU; q++)
        if (m->tx[q].count)
            bits |= 1u << q;
    return bits;
}

/** Answer a read the model gives behaviour to; return 0 for a plain one. */
static int special_read(struct ath9k_model *m, uint32_t reg, uint32_t *value)
{
    uint32_t isr = m->reg[AR_ISR / 4];

    if (reg >= AR9300_OTP_BASE && reg < AR9300_OTP_BASE + ATH9K_MODEL_NVRAM) {
        uint32_t at = reg - AR9300_OTP_BASE, word = 0;
        unsigned i;

        for (i = 0; i < 4; i++)
            if (at + i < m->otp_len)
                word |= (uint32_t)m->otp[at + i] << (8 * i);
        m->reg[AR9300_OTP_READ_DATA / 4] = word;
        *value = word;
        return 1;
    }
    if (reg >= AR5416_EEPROM_OFFSET && reg < AR_EEPROM_WORD(ATH9K_MODEL_NVRAM / 2)) {
        uint32_t n = (reg - AR5416_EEPROM_OFFSET) >> AR5416_EEPROM_S;
        uint32_t word = n < m->eeprom_words ? m->eeprom[n] : 0xffff;

        m->reg[AR_EEPROM_STATUS_DATA / 4] = word;
        *value = word;
        return 1;
    }
    switch (reg) {
    case AR_SREV:
        *value = m->srev;
        return 1;
    case AR_RTC_STATUS:
        *value = rtc_status(m);
        return 1;
    case AR_EEPROM_STATUS_DATA:
        *value = m->reg[reg / 4] & AR_EEPROM_STATUS_DATA_VAL;
        if (m->stuck_eeprom)
            *value |= AR_EEPROM_STATUS_DATA_BUSY;
        return 1;
    case AR9300_OTP_STATUS:
        *value = m->otp_len ? AR9300_OTP_STATUS_VALID : 0;
        return 1;
    case AR_ISR:
        *value = isr;
        return 1;
    case AR_ISR_RAC:
        *value = isr;
        m->reg[AR_ISR / 4] = 0;
        return 1;
    case AR_INTR_ASYNC_CAUSE:
        *value = isr ? AR_INTR_MAC_IRQ : 0;
        return 1;
    case AR_CR:
        *value = (m->reg[reg / 4] & ~(AR_CR_RXE | AR_CR_RXD)) | (m->rxe ? AR_CR_RXE : 0);
        return 1;
    case AR_Q_TXE:
        *value = tx_enabled(m);
        return 1;
    case AR_TSF_L32:
        *value = (uint32_t)m->usec;
        return 1;
    case AR_TSF_U32:
        *value = (uint32_t)(m->usec >> 32);
        return 1;
    default:
        break;
    }
    if (reg >= AR_QSTS(0) && reg < AR_QSTS(AR_NUM_QCU)) {
        unsigned q = (reg - AR_QSTS(0)) / 4;

        *value = (m->reg[reg / 4] & ~AR_Q_STS_PEND_FR_CNT) |
                 (m->tx[q].count < AR_Q_STS_PEND_FR_CNT ? m->tx[q].count : AR_Q_STS_PEND_FR_CNT);
        return 1;
    }
    return 0;
}

/** Apply a write the model gives behaviour to, after it is stored. */
static void special_write(struct ath9k_model *m, uint32_t reg, uint32_t value, uint32_t old)
{
    unsigned q;

    switch (reg) {
    case AR_RTC_RESET:
        if ((old & AR_RTC_RESET_EN) && !(value & AR_RTC_RESET_EN))
            chip_reset(m, ATH9K_MODEL_RESET_POWER_ON);
        return;
    case AR_RTC_RC:
        if (value & AR_RTC_RC_MAC_COLD & ~old)
            chip_reset(m, ATH9K_MODEL_RESET_COLD);
        else if (value & AR_RTC_RC_MAC_WARM & ~old)
            chip_reset(m, ATH9K_MODEL_RESET_WARM);
        return;
    case AR_ISR:
        m->reg[reg / 4] = old & ~value;
        return;
    case AR_CR:
        if (value & AR_CR_RXE)
            m->rxe = 1;
        if ((value & AR_CR_RXD) && !m->stuck_rx_dma)
            m->rxe = 0;
        return;
    case AR_HP_RXDP:
        fifo_push(m, &m->rx[0], value);
        return;
    case AR_LP_RXDP:
        fifo_push(m, &m->rx[1], value);
        return;
    case AR_Q_TXD:
        if (!m->stuck_tx_dma)
            for (q = 0; q < AR_NUM_QCU; q++)
                if (value & (1u << q))
                    fifo_reset(&m->tx[q], ATH9K_TX_FIFO_DEPTH);
        return;
    case AR_Q_STATUS_RING_START:
        m->txs_start = value;
        m->txs_next = value;
        return;
    case AR_Q_STATUS_RING_END:
        m->txs_end = value;
        return;
    default:
        break;
    }
    if (reg >= AR_QTXDP(0) && reg < AR_QTXDP(AR_NUM_QCU))
        fifo_push(m, &m->tx[(reg - AR_QTXDP(0)) / 4], value);
}

/** io.h's read: the hook, the model's behaviour, or the register file. */
static uint32_t model_read(void *ctx, uint32_t reg)
{
    struct ath9k_model *m = ctx;
    uint32_t value;

    if (m->dead)
        value = 0xffffffffu;
    else if (!in_window(reg)) {
        m->unmapped++;
        value = ATH9K_MODEL_UNMAPPED;
    } else if (!(m->hooks.read && m->hooks.read(m, reg, &value)) && !special_read(m, reg, &value))
        value = m->reg[reg / 4];
    if (m->log_reads)
        log_access(m, ATH9K_MODEL_READ, reg, value);
    return value;
}

/** io.h's write: store, apply the model's behaviour, then the hook. */
static void model_write(void *ctx, uint32_t reg, uint32_t value)
{
    struct ath9k_model *m = ctx;
    uint32_t old;

    log_access(m, ATH9K_MODEL_WRITE, reg, value);
    if (m->dead)
        return;
    if (!in_window(reg)) {
        m->unmapped++;
        return;
    }
    old = m->reg[reg / 4];
    m->reg[reg / 4] = value;
    special_write(m, reg, value, old);
    if (m->hooks.write)
        m->hooks.write(m, reg, value);
}

/** io.h's wait: count the time, which the TSF reads. */
static void model_udelay(void *ctx, uint32_t usec)
{
    struct ath9k_model *m = ctx;

    m->usec += usec;
    log_access(m, ATH9K_MODEL_DELAY, 0, usec);
}

const struct ath9k_io ath9k_model_io = {model_read, model_write, model_udelay};

/** Make `m` a powered-off `chip` of revision `mac_rev`. */
void ath9k_model_init(struct ath9k_model *m, const struct ath9k_chip *chip, uint32_t mac_rev)
{
    unsigned char *p = (unsigned char *)m;
    unsigned long i;

    for (i = 0; i < sizeof(*m); i++)
        p[i] = 0;
    m->srev = ((chip->mac_version << AR_SREV_TYPE2_S) & AR_SREV_VERSION2) |
              ((mac_rev << AR_SREV_REVISION2_S) & AR_SREV_REVISION2) | AR_SREV_ID;
    fifos_reset(m);
}

/** Point `ah`'s register access at `m`. */
void ath9k_model_attach(struct ath9k_model *m, struct ath9k *ah)
{
    ah->io = &ath9k_model_io;
    ah->io_ctx = m;
}

/** Give the model an EEPROM of `n` words; 0 removes it. */
void ath9k_model_set_eeprom(struct ath9k_model *m, const uint16_t *words, unsigned n)
{
    unsigned i;

    if (n > ATH9K_MODEL_NVRAM / 2)
        n = ATH9K_MODEL_NVRAM / 2;
    for (i = 0; i < n; i++)
        m->eeprom[i] = words[i];
    m->eeprom_words = n;
}

/** Give the model an OTP of `n` bytes; 0 removes it. */
void ath9k_model_set_otp(struct ath9k_model *m, const uint8_t *bytes, unsigned n)
{
    unsigned i;

    if (n > ATH9K_MODEL_NVRAM)
        n = ATH9K_MODEL_NVRAM;
    for (i = 0; i < n; i++)
        m->otp[i] = bytes[i];
    m->otp_len = n;
}

/** A register's stored value, without a log entry or a hook. */
uint32_t ath9k_model_peek(const struct ath9k_model *m, uint32_t reg)
{
    return in_window(reg) ? m->reg[reg / 4] : ATH9K_MODEL_UNMAPPED;
}

/** Set a register's stored value, without a log entry or a hook. */
void ath9k_model_poke(struct ath9k_model *m, uint32_t reg, uint32_t value)
{
    if (in_window(reg))
        m->reg[reg / 4] = value;
}

/** Raise interrupt causes `isr`. */
void ath9k_model_raise(struct ath9k_model *m, uint32_t isr)
{
    m->reg[AR_ISR / 4] |= isr;
}

/** Empty the log. */
void ath9k_model_log_clear(struct ath9k_model *m)
{
    m->log_count = 0;
    m->log_lost = 0;
}

/** The first log entry at or after `from` of kind `op` on `reg`, or -1. */
int ath9k_model_find(const struct ath9k_model *m, unsigned from, unsigned op, uint32_t reg)
{
    unsigned i;

    for (i = from; i < m->log_count; i++)
        if (m->log[i].op == op && m->log[i].reg == reg)
            return (int)i;
    return -1;
}

/** The log entries of kind `op` on `reg`. */
unsigned ath9k_model_count(const struct ath9k_model *m, unsigned op, uint32_t reg)
{
    unsigned i, n = 0;

    for (i = 0; i < m->log_count; i++)
        if (m->log[i].op == op && m->log[i].reg == reg)
            n++;
    return n;
}

/** The value of the last logged write to `reg`, or `otherwise`. */
uint32_t ath9k_model_last_write(const struct ath9k_model *m, uint32_t reg, uint32_t otherwise)
{
    unsigned i = m->log_count;

    while (i-- > 0)
        if (m->log[i].op == ATH9K_MODEL_WRITE && m->log[i].reg == reg)
            return m->log[i].value;
    return otherwise;
}

/** Receive `frame` into the oldest buffer of FIFO `hp`. */
int ath9k_model_rx(struct ath9k_model *m, int hp, const uint8_t *frame, unsigned len,
                   const struct ar9003_rxs *status)
{
    struct ath9k_model_fifo *f = &m->rx[hp ? 0 : 1];
    struct ar9003_rxs rxs = {0};
    unsigned char *buf, *src;
    unsigned i;
    uint32_t bus;

    if (f->count == 0)
        return -1;
    if (sizeof(rxs) + len > ATH9K_RX_BUF_LEN || len > AR_DataLen)
        return -2;
    bus = fifo_pop(f);
    if (!m->hooks.dma || !(buf = m->hooks.dma(m, bus, ATH9K_RX_BUF_LEN))) {
        m->dma_faults++;
        return -2;
    }
    if (status)
        rxs = *status;
    rxs.ds_info = (rxs.ds_info & ~(AR_DescId | AR_TxRxDesc | AR_CtrlStat)) | DS_INFO_VENDOR;
    rxs.status2 = (rxs.status2 & ~AR_DataLen) | len;
    rxs.status11 |= AR_RxDone;
    src = (unsigned char *)&rxs;
    for (i = 0; i < sizeof(rxs); i++)
        buf[i] = src[i];
    for (i = 0; i < len; i++)
        buf[sizeof(rxs) + i] = frame[i];
    ath9k_model_raise(m, hp ? AR_ISR_HP_RXOK : AR_ISR_LP_RXOK);
    return 0;
}

/** Finish the oldest descriptor chain of queue `q`. */
uint32_t ath9k_model_tx_complete(struct ath9k_model *m, unsigned q, const struct ar9003_txs *status)
{
    struct ar9003_txs txs = {0};
    unsigned char *ring, *src;
    unsigned i;
    uint32_t bus;

    if (q >= AR_NUM_QCU || m->tx[q].count == 0)
        return 0;
    if (!m->hooks.dma || !(ring = m->hooks.dma(m, m->txs_next, sizeof(txs)))) {
        m->dma_faults++;
        return 0;
    }
    bus = fifo_pop(&m->tx[q]);
    if (status)
        txs = *status;
    txs.ds_info = (txs.ds_info & ~(AR_DescId | AR_TxRxDesc | AR_CtrlStat | AR_TxQcuNum)) |
                  DS_INFO_TX | SM(q, AR_TxQcuNum);
    txs.status8 |= AR_TxDone;
    src = (unsigned char *)&txs;
    for (i = 0; i < sizeof(txs); i++)
        ring[i] = src[i];
    m->txs_next += sizeof(txs);
    if (m->txs_next >= m->txs_end)
        m->txs_next = m->txs_start;
    ath9k_model_raise(m, (txs.status3 & (AR_ExcessiveRetries | AR_Filtered)) ? AR_ISR_TXERR
                                                                             : AR_ISR_TXOK);
    return bus;
}
