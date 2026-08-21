/*
 * netconsole_hosttest.c: the kernel log on the wire, driven on the host.
 *
 * host-deps:
 *
 * netconsole.c is compiled natively and driven through the same entry points
 * the kernel uses, with the Ethernet layer replaced by a recorder. What that
 * buys over the QEMU capture (`make netconsole-test`) is the cases a booting
 * machine will not produce on demand: a ring that fills and drops, a payload
 * that fills exactly, and above all a driver that prints an error from inside
 * the transmit it was asked to perform, which is the recursion the code
 * refuses and which a healthy QEMU boot never provokes.
 *
 * The file is included rather than linked so the test can reach its statics
 * back to their initial state between cases; the kernel has one boot per run
 * and needs no such thing.
 */

extern int printf(const char *fmt, ...);

/* The kernel headers this pulls in define uint32_t themselves, so nothing
   from the host's stdint may be included alongside them. */
#include "../../rach2/sys/network/netconsole.h"

/* ------------------------------------------------------------------ */
/* The stubs netconsole links against.                                 */
/* ------------------------------------------------------------------ */

#define MAX_FRAMES 64

static unsigned char frame_data[MAX_FRAMES][NETCONSOLE_HDR_LEN + NETCONSOLE_PAYLOAD_MAX];
static unsigned short frame_len[MAX_FRAMES];
static unsigned short frame_type[MAX_FRAMES];
static unsigned char frame_dst[MAX_FRAMES][6];
static unsigned frame_count;

/* When set, the recorder prints from inside the transmit, exactly as a driver
   reporting an error would, and that print re-enters netconsole_putc. */
static int tx_prints;
static unsigned tx_depth;
static unsigned tx_max_depth;

void serial_putc(char c);

void ethernet_tx(const unsigned char *dst, unsigned short type, const unsigned char *payload,
                 unsigned short plen)
{
    unsigned i;

    tx_depth++;
    if (tx_depth > tx_max_depth) {
        tx_max_depth = tx_depth;
    }
    if (frame_count < MAX_FRAMES) {
        frame_type[frame_count] = type;
        frame_len[frame_count] = plen;
        for (i = 0; i < 6; i++) {
            frame_dst[frame_count][i] = dst[i];
        }
        for (i = 0; i < plen && i < sizeof(frame_data[0]); i++) {
            frame_data[frame_count][i] = payload[i];
        }
    }
    frame_count++;
    if (tx_prints) {
        serial_putc('!');
        serial_putc('\n');
    }
    tx_depth--;
}

/* The kernel's interrupt bracket. On the host there are no interrupts, so the
   flag value is a token; what the test needs is that every lock is released,
   which the balance counter below checks. */
static int lock_depth;
static int lock_ever_negative;

unsigned int net_lock(void)
{
    lock_depth++;
    return 0;
}

void net_unlock(unsigned int flags)
{
    (void)flags;
    lock_depth--;
    if (lock_depth < 0) {
        lock_ever_negative = 1;
    }
}

/* The driver's own serial path, which is how a transmit error gets back in. */
void serial_putc(char c)
{
    netconsole_putc(c);
}

void serial_printf(const char *fmt, ...)
{
    (void)fmt;
}

static unsigned krandom_value = 0x1234abcdu;

unsigned int krandom_u32(void)
{
    return krandom_value;
}

/*
 * The byte-order writers and the checksum netconsole calls.
 *
 * The kernel takes these from net_core.c and ip.c, and neither can be compiled
 * here: net_core.c's interrupt bracket is x86 inline assembly, and ip.c brings
 * the whole datagram path with it. So this file supplies its own, written
 * independently, and what they prove is the frame LAYOUT: that the fields land
 * at the offsets the header documents, in network order, and that a receiver
 * summing the frame gets zero. That the kernel's own `ip_checksum` agrees with
 * an outside implementation is proved elsewhere, by `make netconsole-test`,
 * whose checker recomputes the sum in Python over a real capture.
 */
void write16be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFF);
}

void write32be(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)(value & 0xFF);
}

uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    unsigned long sum = 0;
    uint16_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((unsigned long)buf[i] << 8) | buf[i + 1];
    }
    if (len & 1) {
        sum += (unsigned long)buf[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFF);
}

#include "../../rach2/sys/network/netconsole.c"

/* ------------------------------------------------------------------ */
/* The test.                                                           */
/* ------------------------------------------------------------------ */

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** Put the module back to the state a fresh boot starts in. */
static void nc_reset(void)
{
    nc_head = 0;
    nc_tail = 0;
    nc_boot_id = 0;
    nc_seq = 0;
    nc_frames = 0;
    nc_dropped = 0;
    nc_enabled = 0;
    nc_sending = 0;
    frame_count = 0;
    tx_prints = 0;
    tx_depth = 0;
    tx_max_depth = 0;
    lock_depth = 0;
    lock_ever_negative = 0;
}

static void put_string(const char *s)
{
    while (*s) {
        netconsole_putc(*s++);
    }
}

/** The 16-bit one's complement sum a receiver computes; zero means intact. */
static unsigned short frame_sum(unsigned f)
{
    return ip_checksum(frame_data[f], (unsigned short)frame_len[f]);
}

static unsigned frame_be32(unsigned f, unsigned off)
{
    return ((unsigned)frame_data[f][off] << 24) | ((unsigned)frame_data[f][off + 1] << 16) |
           ((unsigned)frame_data[f][off + 2] << 8) | (unsigned)frame_data[f][off + 3];
}

static unsigned frame_be16(unsigned f, unsigned off)
{
    return ((unsigned)frame_data[f][off] << 8) | (unsigned)frame_data[f][off + 1];
}

int main(void)
{
    /* ---- nothing leaves before the device is up ---- */
    nc_reset();
    put_string("boot line one\n");
    put_string("boot line two\n");
    CHECK(frame_count == 0, "nothing is transmitted before the console is enabled");
    CHECK(nc_pending() == 28, "the characters are held instead");

    /* ---- enabling sends what was held ---- */
    netconsole_enable();
    CHECK(frame_count == 1, "enabling drains the buffer in one frame");
    CHECK(frame_type[0] == NETCONSOLE_ETHERTYPE, "the frame has the netconsole ethertype");
    CHECK(frame_dst[0][0] == 0xFF && frame_dst[0][5] == 0xFF, "the frame is broadcast");
    CHECK(frame_len[0] == NETCONSOLE_HDR_LEN + 28, "the frame is header plus the held bytes");
    CHECK(frame_data[0][0] == 'R' && frame_data[0][1] == 'C' && frame_data[0][2] == 'H' &&
              frame_data[0][3] == '2',
          "the frame has the magic");
    CHECK(frame_be32(0, 4) == krandom_value, "the boot id is the drawn value");
    CHECK(frame_be32(0, 8) == 0, "the first frame is sequence zero");
    CHECK(frame_be16(0, 12) == 28, "the length field is the payload length");
    CHECK(frame_sum(0) == 0, "the frame sums to zero, as its receiver checks");
    CHECK(frame_data[0][NETCONSOLE_HDR_LEN] == 'b', "the payload starts at the first held byte");
    CHECK(nc_pending() == 0, "the buffer is empty afterwards");
    CHECK(lock_depth == 0 && !lock_ever_negative, "every lock taken was released");

    /* ---- a zero draw never becomes a zero boot id ---- */
    nc_reset();
    krandom_value = 0;
    netconsole_enable();
    put_string("x\n");
    CHECK(frame_count == 1 && frame_be32(0, 4) != 0, "a zero draw does not become a zero boot id");
    krandom_value = 0x1234abcdu;

    /* ---- a line at a time, numbered in order ---- */
    nc_reset();
    netconsole_enable();
    put_string("one\n");
    put_string("two\n");
    put_string("three\n");
    CHECK(frame_count == 3, "each completed line is a frame");
    CHECK(frame_be32(0, 8) == 0 && frame_be32(1, 8) == 1 && frame_be32(2, 8) == 2,
          "the sequence numbers count up");
    CHECK(frame_be32(0, 4) == frame_be32(2, 4), "every frame has the same boot id");
    CHECK(frame_sum(0) == 0 && frame_sum(1) == 0 && frame_sum(2) == 0, "every frame sums to zero");

    /* ---- an unterminated line waits, and is not lost ---- */
    nc_reset();
    netconsole_enable();
    put_string("no newline yet");
    CHECK(frame_count == 0, "an unterminated line is not sent");
    netconsole_putc('\n');
    CHECK(frame_count == 1 && frame_len[0] == NETCONSOLE_HDR_LEN + 15,
          "the newline sends the whole line");

    /* ---- a line longer than one frame goes out in pieces ---- */
    {
        unsigned i;

        nc_reset();
        netconsole_enable();
        for (i = 0; i < NETCONSOLE_PAYLOAD_MAX; i++) {
            netconsole_putc('a');
        }
        CHECK(frame_count == 1, "a payload's worth is sent without waiting for a newline");
        CHECK(frame_len[0] == NETCONSOLE_HDR_LEN + NETCONSOLE_PAYLOAD_MAX,
              "the frame has a full payload and no more");
        CHECK(frame_sum(0) == 0, "the full frame sums to zero");
    }

    /* ---- the ring is bounded, and says how much it dropped ---- */
    {
        unsigned i;

        nc_reset();
        for (i = 0; i < NETCONSOLE_RING + 100; i++) {
            netconsole_putc('z');
        }
        CHECK(nc_dropped == 101, "the overflow is counted, not written past the ring");
        CHECK(nc_pending() == NETCONSOLE_RING - 1, "the ring holds its capacity and no more");
        CHECK(nc_head < NETCONSOLE_RING && nc_tail < NETCONSOLE_RING,
              "both indices stay inside the ring");
    }

    /* ---- a full ring drains in as many frames as it takes ---- */
    {
        unsigned i, seq;

        nc_reset();
        for (i = 0; i < NETCONSOLE_RING - 1; i++) {
            netconsole_putc('q');
        }
        netconsole_enable();
        CHECK(frame_count ==
                  (NETCONSOLE_RING - 1 + NETCONSOLE_PAYLOAD_MAX - 1) / NETCONSOLE_PAYLOAD_MAX,
              "a full ring drains in ceil(ring / payload) frames");
        CHECK(nc_pending() == 0, "the ring is empty after the drain");
        for (seq = 0; seq < frame_count && seq < MAX_FRAMES; seq++) {
            if (frame_be32(seq, 8) != seq || frame_sum(seq) != 0) {
                fails++;
                printf("  FAIL: frame %u of the drain is out of order or corrupt\n", seq);
                break;
            }
        }
        CHECK(lock_depth == 0 && !lock_ever_negative, "the drain released every lock");
    }

    /* ---- a driver that prints from inside a transmit does not recurse ---- */
    nc_reset();
    netconsole_enable();
    tx_prints = 1;
    put_string("trigger\n");
    CHECK(tx_max_depth == 1, "the transmit is never entered from inside itself");
    CHECK(frame_count == 1, "the driver's own message did not become a second frame");
    CHECK(nc_pending() == 2, "the driver's own message is held in the ring instead");
    tx_prints = 0;
    netconsole_flush();
    CHECK(frame_count == 2 && frame_be32(1, 8) == 1,
          "the held message goes out on the next flush, in sequence");

    /* ---- a disabled console never sends, however much is printed ---- */
    {
        unsigned i;

        nc_reset();
        for (i = 0; i < 100; i++) {
            put_string("still down\n");
        }
        CHECK(frame_count == 0, "a console that was never enabled sends nothing");
        CHECK(netconsole_is_enabled() == 0, "and reports itself disabled");
        netconsole_flush();
        CHECK(frame_count == 0, "an explicit flush on a disabled console sends nothing");
    }

    return fails;
}
