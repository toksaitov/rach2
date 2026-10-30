/*
 * acpi_ec_hosttest.c: the embedded controller's command protocol,
 * rach2/sys/acpi/ecio.c, against a model of an EC's two ports and its
 * 256 bytes, with a clock of its own that the protocol's waits move on.
 *
 * The host must write a byte only once the EC has taken the one before
 * (IBF clear) and read one only once the EC has put it out (OBF set). An EC
 * that never takes a byte, or never answers, must give -ETIMEDOUT after the
 * limit of the step, not hang the kernel, and the host must write nothing
 * more to it after the step that timed out.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/acpi/ecio.c"

/** The number of failed checks, which is the exit status. */
static int fails;

/** Count and print a failure when `cond` is false; `what` says what held. */
#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

#define DATA    0x62
#define COMMAND 0x66

/* What the model waits for next after a command. */
enum want { IDLE, READ_ADDRESS, WRITE_ADDRESS, WRITE_VALUE };

/* The model EC: its space, its status, the byte it last put out, the queries
 * it has for the host, and how slowly it takes a byte. */
static struct {
    uint8_t space[256];
    uint8_t status;
    uint8_t out;
    enum want want;
    uint8_t address;
    uint8_t queries[8];
    int nqueries;
    uint32_t busy_us;  /* the time it takes a byte in */
    uint64_t taken_at; /* the clock at which the byte in is taken */
    uint8_t pending;   /* that byte, and the port it came to */
    uint16_t pending_port;
    int stuck;    /* it never takes a byte: IBF stays set */
    int mute;     /* it takes bytes but never puts one out */
    uint64_t now; /* the clock, in microseconds */
    int overruns; /* bytes written while IBF was set */
    int writes;   /* bytes written to either port */
} ec;

/** Start the model afresh, with `busy_us` to take a byte. */
static void fresh(uint32_t busy_us)
{
    int i;

    for (i = 0; i < 256; i++) {
        ec.space[i] = (uint8_t)(i ^ 0x5A);
    }
    ec.status = 0;
    ec.want = IDLE;
    ec.nqueries = 0;
    ec.busy_us = busy_us;
    ec.stuck = 0;
    ec.mute = 0;
    ec.now = 0;
    ec.overruns = 0;
    ec.writes = 0;
}

/** Put `value` out for the host, unless the model is mute. */
static void put_out(uint8_t value)
{
    if (!ec.mute) {
        ec.out = value;
        ec.status |= EC_OBF;
    }
}

/** Take the byte in, when its time has come: a command or a data byte. */
static void take(void)
{
    uint8_t v = ec.pending;

    if (!(ec.status & EC_IBF) || ec.stuck || ec.now < ec.taken_at) {
        return;
    }
    ec.status &= (uint8_t)~(EC_IBF | EC_CMD);
    if (ec.pending_port == COMMAND) {
        if (v == EC_READ) {
            ec.want = READ_ADDRESS;
        } else if (v == EC_WRITE) {
            ec.want = WRITE_ADDRESS;
        } else if (v == EC_QUERY) {
            ec.want = IDLE;
            put_out(ec.nqueries > 0 ? ec.queries[0] : 0);
            if (ec.nqueries > 0) {
                int i;

                for (i = 1; i < ec.nqueries; i++) {
                    ec.queries[i - 1] = ec.queries[i];
                }
                ec.nqueries--;
            }
            if (ec.nqueries == 0) {
                ec.status &= (uint8_t)~EC_SCI_EVT;
            }
        }
        return;
    }
    if (ec.want == READ_ADDRESS) {
        put_out(ec.space[v]);
        ec.want = IDLE;
    } else if (ec.want == WRITE_ADDRESS) {
        ec.address = v;
        ec.want = WRITE_VALUE;
    } else if (ec.want == WRITE_VALUE) {
        ec.space[ec.address] = v;
        ec.want = IDLE;
    }
}

static uint8_t model_in(uint16_t port)
{
    take();
    if (port == COMMAND) {
        return ec.status;
    }
    ec.status &= (uint8_t)~EC_OBF;
    return ec.out;
}

static void model_out(uint16_t port, uint8_t value)
{
    take();
    ec.writes++;
    if (ec.status & EC_IBF) {
        ec.overruns++;
    }
    ec.pending = value;
    ec.pending_port = port;
    ec.status |= EC_IBF;
    if (port == COMMAND) {
        ec.status |= EC_CMD;
    }
    ec.taken_at = ec.now + ec.busy_us;
}

static void model_delay(uint32_t us)
{
    ec.now += us;
}

/** The ports of the model, with a limit of `timeout_us` on each step. */
static struct ec_io io_of(uint32_t timeout_us)
{
    struct ec_io io;

    io.data = DATA;
    io.command = COMMAND;
    io.in = model_in;
    io.out = model_out;
    io.delay = model_delay;
    io.timeout_us = timeout_us;
    return io;
}

/** Reads and writes of a quick EC and of a slow one. */
static void transfers(void)
{
    static const uint32_t busy[] = {0, 1, 300, 20000};
    struct ec_io io = io_of(EC_TIMEOUT_US);
    uint8_t v = 0;
    unsigned i;

    for (i = 0; i < sizeof(busy) / sizeof(busy[0]); i++) {
        fresh(busy[i]);
        CHECK(ec_read(&io, 0x10, &v) == 0 && v == (0x10 ^ 0x5A), "a read gives the byte");
        CHECK(ec_write(&io, 0x20, 0xC3) == 0 && ec.space[0x20] == 0xC3, "a write stores the byte");
        CHECK(ec_read(&io, 0x20, &v) == 0 && v == 0xC3, "a read after a write gives it back");
        CHECK(ec_read(&io, 0xFF, &v) == 0 && v == (0xFF ^ 0x5A), "the last byte is read");
        CHECK(ec.overruns == 0, "no byte is written before the EC took the one before");
        CHECK(!(ec.status & (EC_IBF | EC_OBF)), "a transfer leaves both buffers empty");
    }
}

/** The queries come in order, clear SCI_EVT with the last, and give 0 when
 * there are none. */
static void queries(void)
{
    struct ec_io io = io_of(EC_TIMEOUT_US);
    uint8_t q = 0xEE;

    fresh(50);
    ec.queries[0] = 0x11;
    ec.queries[1] = 0x32;
    ec.nqueries = 2;
    ec.status |= EC_SCI_EVT;
    CHECK((ec_status(&io) & EC_SCI_EVT) != 0, "the status shows SCI_EVT");
    CHECK(ec_query(&io, &q) == 0 && q == 0x11, "the first query is 0x11");
    CHECK((ec_status(&io) & EC_SCI_EVT) != 0, "SCI_EVT stays while a query is left");
    CHECK(ec_query(&io, &q) == 0 && q == 0x32, "the second query is 0x32");
    CHECK(!(ec_status(&io) & EC_SCI_EVT), "the last query clears SCI_EVT");
    CHECK(ec_query(&io, &q) == 0 && q == 0, "a query with none gives 0");
    CHECK(ec.overruns == 0, "no query's byte is written early");
}

/**
 * An EC that never takes a byte times out at the command, an EC that never
 * answers at its byte out; each within its step's limit on the model's
 * clock, with nothing written past the step that timed out and the value
 * left alone.
 */
static void timeouts(void)
{
    struct ec_io io = io_of(1000);
    uint8_t v = 0x77;
    int writes;

    fresh(0);
    ec.stuck = 1;
    ec.status |= EC_IBF;
    CHECK(ec_read(&io, 0x10, &v) == -ETIMEDOUT, "an EC that never takes a byte times out");
    CHECK(ec.writes == 0, "nothing is written to an EC whose input buffer stays full");
    CHECK(ec.now >= 1000 && ec.now <= 1000 + EC_POLL_US, "the wait lasts the step's limit");
    CHECK(v == 0x77, "a read that timed out leaves the value alone");

    fresh(0);
    ec.stuck = 1;
    CHECK(ec_write(&io, 0x20, 0x01) == -ETIMEDOUT, "a write to an EC that takes nothing times out");
    CHECK(ec.writes == 1 && ec.overruns == 0, "the command alone is written, and nothing after it");
    CHECK(ec.space[0x20] == (0x20 ^ 0x5A), "the byte is not written");

    fresh(0);
    ec.mute = 1;
    CHECK(ec_read(&io, 0x10, &v) == -ETIMEDOUT, "an EC that never answers a read times out");
    CHECK(v == 0x77 && ec.writes == 2, "the command and address went, and the value is left alone");
    CHECK(ec.now >= 1000 && ec.now <= 1000 + EC_POLL_US, "the read waits the step's limit once");

    fresh(0);
    ec.mute = 1;
    ec.status |= EC_SCI_EVT;
    CHECK(ec_query(&io, &v) == -ETIMEDOUT && v == 0x77, "a query the EC never answers times out");

    fresh(800);
    CHECK(ec_read(&io, 0x10, &v) == 0, "an EC slower than the poll but within the limit answers");
    fresh(2000);
    writes = ec.writes;
    CHECK(ec_read(&io, 0x10, &v) == -ETIMEDOUT, "an EC slower than the limit times out");
    CHECK(ec.writes == writes + 1, "a command not taken in time is followed by nothing");
}

/** A limit of 0 still reads the status once, and a ready EC answers. */
static void no_wait(void)
{
    struct ec_io io = io_of(0);
    uint8_t v = 0;

    fresh(0);
    CHECK(ec_read(&io, 0x01, &v) == 0 && v == (0x01 ^ 0x5A), "a ready EC answers with no wait");
    CHECK(ec.now == 0, "no time passes for an EC that is ready");
    fresh(1);
    CHECK(ec_read(&io, 0x01, &v) == -ETIMEDOUT, "a busy EC times out with no wait");
}

int main(void)
{
    transfers();
    queries();
    timeouts();
    no_wait();
    if (fails != 0) {
        printf("acpi_ec_hosttest: %d checks failed\n", fails);
        return fails > 255 ? 255 : fails;
    }
    printf("acpi_ec_hosttest: ok\n");
    return 0;
}
