/*
 * ath9k_lenovo.h: the Lenovo G50-70's AR9565 as Linux 6.18's ath9k read
 * it under mmiotrace (dev-log/70-wifi-chip/linux/), for the ath9k host
 * tests that run the driver against that card: its revision, the values
 * its resets leave, and its OTP, which holds the EEPROM image.
 *
 * Include it once, after ath9k_model.h, in a test whose chip model is
 * named `model`.
 */
#ifndef _ATH9K_LENOVO_H_
#define _ATH9K_LENOVO_H_

/* What the Lenovo's card read: AR_SREV, AR_WA before the driver wrote it,
 * and AR_STA_ID1 and the baseband's chip ID after a reset. */
#define LENOVO_SREV     0x002cc1ffu
#define LENOVO_WA       0x0102021bu
#define LENOVO_STA_ID1  0x20000000u
#define LENOVO_CHIP_ID  0xc7559163u
#define AR_PHY_CHIP_ID_ 0x9818

/* The card's OTP words 123 to 255, which hold its one block: a header at
 * byte 0x3ff naming template 4 (h116), 521 bytes of runs and a checksum.
 * ath9k reads no word below 123. Each word is bytes 4n to 4n + 3,
 * the lowest first. */
#define OTP_FIRST 123
static const uint32_t lenovo_otp[] = {
    0x00000000, 0x00504b00, 0x12000000, 0x1a181614, 0x00000509, 0x14120000, 0x091a1816, 0x00000005,
    0x16141200, 0x05091a18, 0x00000000, 0x18161412, 0x0005091a, 0x12000000, 0x1a181614, 0x00000509,
    0x14120000, 0x091a1816, 0x00000005, 0x16141200, 0x05091a18, 0x00000000, 0x18161412, 0x0005091a,
    0x12000000, 0x1a181614, 0x00040a1c, 0x12000000, 0x1a181614, 0x00040a1c, 0x12000000, 0x1a181614,
    0x00040a1c, 0x12000000, 0x1a181614, 0x00040a1c, 0x12000000, 0x1a181614, 0x00040a1c, 0x12000000,
    0x1a181614, 0x00040a1c, 0x12000000, 0x1a181614, 0x00040a1c, 0x12000000, 0x1a181614, 0x1a18101c,
    0x1a18101c, 0x1a18121c, 0x1a18121c, 0x1c1a141c, 0x1c1a141e, 0x1c1a141e, 0x1c1a141e, 0xbb2bc51e,
    0x08022d3c, 0x22029303, 0x90a0b0c8, 0x00370050, 0x00000000, 0x0f100000, 0x22220205, 0x50054000,
    0x20602020, 0x20612120, 0x20602021, 0x1f5f1f1e, 0x20602020, 0x20602020, 0x25642423, 0x24632420,
    0x575f1a17, 0x1a1e5f1b, 0x1c1e5f1c, 0x1f5f5f1f, 0x7a7f9da2, 0x7075a7ac, 0x7075a7ac, 0x0345a7ac,
    0xac7a7f9d, 0xac7075a7, 0x840809a7, 0x757aa27a, 0x087aa270, 0x00000016, 0x201e1a00, 0x0a202020,
    0x00000004, 0x201e1a00, 0x0a202020, 0x00000004, 0x201e1a00, 0x0a202020, 0x00000004, 0x061e1a00,
    0x00000008, 0x061e1a00, 0x00000008, 0x061e1a00, 0x22201c04, 0x22201c24, 0x22201c24, 0x98080c24,
    0x00993101, 0x980303e7, 0x0303e700, 0x03e60098, 0x373c3c05, 0x2d32323c, 0x030c082d, 0xff000853,
    0xff0008ff, 0xb41c0aff, 0x19008ca4, 0x00000000, 0x10000000, 0x50015001, 0x22220200, 0x50054000,
    0x76421e12, 0x10031e11, 0x48005d22, 0x002d0405, 0x03041102, 0x6c2e676a, 0x0731006f, 0x302d4403,
    0x352d3031, 0x57423333, 0x65328915, 0x0211b8ee, 0x64209404};

/** The OTP word `n` of the recorded card. */
static uint32_t otp_word_of(unsigned n)
{
    return n >= OTP_FIRST ? lenovo_otp[n - OTP_FIRST] : 0;
}

/** Give the registers a reset leaves the values the card had. */
static void lenovo_reset(struct ath9k_model *m, unsigned kind)
{
    (void)kind;
    ath9k_model_poke(m, AR_STA_ID1, LENOVO_STA_ID1);
    ath9k_model_poke(m, AR_PHY_CHIP_ID_, LENOVO_CHIP_ID);
}

/** Make the model the recorded card, of revision `mac_rev`, with its OTP. */
static void lenovo_model(uint32_t mac_rev)
{
    static uint8_t otp[1024];
    unsigned n, i;

    ath9k_model_init(&model, ath9k_chip_find(0x0036), mac_rev);
    if (mac_rev == 1)
        model.srev = LENOVO_SREV;
    ath9k_model_poke(&model, AR_WA, LENOVO_WA);
    model.hooks.reset = lenovo_reset;
    for (n = 0; n < 256; n++)
        for (i = 0; i < 4; i++)
            otp[4 * n + i] = (uint8_t)(otp_word_of(n) >> (8 * i));
    ath9k_model_set_otp(&model, otp, sizeof(otp));
}

#endif
