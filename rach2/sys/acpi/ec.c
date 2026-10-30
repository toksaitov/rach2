/*
 * ec.c: the ACPI embedded controller, PNP0C09, by the ACPI specification:
 * the EmbeddedControl address space of the firmware's methods over the EC's
 * two ports (ecio.c), and its queries, which run the methods _Qxx when the
 * EC raises SCI_EVT on its GPE.
 */

#include "../klog.h"
#include "../udelay.h"
#include "../arch/x86/portio.h"
#include "../lib/kirq.h"

#include "acpi.h"

#include "devices.h"
#include "ecio.h"

/* The most queries one SCI_EVT runs before the work gives the queue back;
 * an EC that keeps SCI_EVT set past them is asked again on its next GPE. */
#define EC_QUERIES 16

/* The longest path of the EC's device logged. */
#define EC_PATH_MAX 64

static uint8_t ec_in(uint16_t port)
{
    return inb(port);
}

static void ec_out(uint16_t port, uint8_t value)
{
    outb(port, value);
}

static void ec_delay(uint32_t us)
{
    k_udelay(us);
}

static struct {
    ACPI_HANDLE handle; /* NULL when no EC was found */
    struct ec_io io;
    int ports;    /* the I/O resources of _CRS seen: data, then command */
    int has_gpe;  /* _GPE gave a number */
    UINT32 gpe;   /* the GPE of the FADT's blocks that SCI_EVT raises */
    int queued;   /* a query's work is queued or running */
    int timeouts; /* the transactions that timed out, the first few logged */
    char path[EC_PATH_MAX];
} ec;

/* One transaction at a time: the address space's handler runs outside
 * ACPICA's interpreter lock, and a query beside it. A semaphore of one unit,
 * on which a process sleeps and the boot spins (AcpiOsWaitSemaphore): a
 * process that met a spin lock taken by kacpid would spin in its system
 * call, which nothing preempts, and kacpid would never run to give it
 * back. The GPE's handler only reads the status, so interrupts stay on
 * while a slow EC is waited for. */
static ACPI_SEMAPHORE ec_lock;

/** Take the EC for one transaction. */
static void ec_take(void)
{
    (void)AcpiOsWaitSemaphore(ec_lock, 1, ACPI_WAIT_FOREVER);
}

/** Give the EC back. */
static void ec_give(void)
{
    (void)AcpiOsSignalSemaphore(ec_lock, 1);
}

/** Log a transaction that timed out, the first four times. */
static void ec_timed_out(const char *what, unsigned address)
{
    if (ec.timeouts++ < 4) {
        klog(KLOG_ERR, "acpi", "ec: timeout: %s 0x%02x, status 0x%02x", what, address,
             (unsigned)ec_status(&ec.io));
    }
}

/**
 * The EmbeddedControl space: read or write `width` bits at byte `address`
 * of the EC's 256, a byte at a time, the lowest first, as Linux does for a
 * field wider than a byte. AE_TIME when the EC did not answer, AE_BAD_ADDRESS
 * past its space.
 */
static ACPI_STATUS ec_space(UINT32 function, ACPI_PHYSICAL_ADDRESS address, UINT32 width,
                            UINT64 *value, void *context, void *region)
{
    UINT32 i, bytes = (width + 7) / 8;
    uint8_t byte;
    int rc = 0;

    (void)context;
    (void)region;
    if (address > 0xFF || bytes == 0 || bytes > 8 || address + bytes > 0x100) {
        return AE_BAD_ADDRESS;
    }
    if (function == ACPI_READ) {
        *value = 0;
    }
    ec_take();
    for (i = 0; i < bytes && rc == 0; i++) {
        if (function == ACPI_READ) {
            rc = ec_read(&ec.io, (uint8_t)(address + i), &byte);
            if (rc == 0) {
                *value |= (UINT64)byte << (8 * i);
            }
        } else {
            rc = ec_write(&ec.io, (uint8_t)(address + i), (uint8_t)(*value >> (8 * i)));
        }
    }
    ec_give();
    if (rc != 0) {
        ec_timed_out(function == ACPI_READ ? "read" : "write", (unsigned)(address + i - 1));
        return AE_TIME;
    }
    return AE_OK;
}

static void ec_queries(void *context);

/**
 * Queue the queries when the EC has SCI_EVT set and none are queued, with
 * interrupts off, as the SCI has them, so that the GPE's handler and the
 * end of the work do not both queue it.
 */
static void ec_queue(void)
{
    if (!ec.queued && (ec_status(&ec.io) & EC_SCI_EVT)) {
        ec.queued = 1;
        if (ACPI_FAILURE(AcpiOsExecute(OSL_EC_POLL_HANDLER, ec_queries, NULL))) {
            ec.queued = 0;
        }
    }
}

/**
 * Ask the EC which events it has and run each one's _Qxx, while SCI_EVT
 * stays set, EC_QUERIES at most. 1 when the EC may have more, 0 when it
 * timed out or answered that it has none, which a look again would only
 * repeat.
 */
static int ec_run_queries(void)
{
    static char name[] = "_Q00";
    static const char hex[] = "0123456789ABCDEF";
    uint8_t query;
    ACPI_STATUS status;
    int n, rc;

    for (n = 0; n < EC_QUERIES; n++) {
        ec_take();
        if (!(ec_status(&ec.io) & EC_SCI_EVT)) {
            ec_give();
            return 1;
        }
        rc = ec_query(&ec.io, &query);
        ec_give();
        if (rc != 0) {
            ec_timed_out("query", EC_QUERY);
            return 0;
        }
        if (query == 0) {
            return 0;
        }
        name[2] = hex[query >> 4];
        name[3] = hex[query & 0xF];
        klog(KLOG_DEBUG, "acpi", "ec: query 0x%02x", (unsigned)query);
        status = AcpiEvaluateObject(ec.handle, name, NULL, NULL);
        if (ACPI_FAILURE(status)) {
            klog(KLOG_DEBUG, "acpi", "ec: query 0x%02x: %s: %s", (unsigned)query, name,
                 AcpiFormatException(status));
        }
    }
    return 1;
}

/**
 * Run the EC's queries, in ACPICA's thread. The flag `queued` is cleared
 * and SCI_EVT looked at once more, with interrupts off: an edge that came
 * after the queries' last look found the flag set and queued nothing, so
 * the work queues itself again, behind what else is queued. After a
 * timeout or a query of none it is not, and the EC is asked again on its
 * next GPE.
 */
static void ec_queries(void *context)
{
    uint32_t flags;
    int more;

    (void)context;
    more = ec_run_queries();
    flags = kirq_save();
    ec.queued = 0;
    if (more) {
        ec_queue();
    }
    kirq_restore(flags);
}

/**
 * The EC's GPE, in the SCI: queue the queries when the EC has SCI_EVT set.
 * The EC raises its GPE too for each byte of a transaction, which needs
 * nothing here. Edge-triggered, as Linux takes it, so ACPICA cleared it and
 * enables it again.
 */
static UINT32 ec_gpe(ACPI_HANDLE device, UINT32 number, void *context)
{
    (void)device;
    (void)number;
    (void)context;
    ec_queue();
    return ACPI_INTERRUPT_HANDLED | ACPI_REENABLE_GPE;
}

/** A resource of the EC's _CRS: the first I/O port is the data port and
 * the second the command port, as the specification orders them. */
static ACPI_STATUS ec_resource(ACPI_RESOURCE *res, void *context)
{
    UINT16 port;

    (void)context;
    if (res->Type == ACPI_RESOURCE_TYPE_IO) {
        port = res->Data.Io.Minimum;
    } else if (res->Type == ACPI_RESOURCE_TYPE_FIXED_IO) {
        port = res->Data.FixedIo.Address;
    } else {
        return AE_OK;
    }
    if (ec.ports == 0) {
        ec.io.data = port;
    } else if (ec.ports == 1) {
        ec.io.command = port;
    }
    ec.ports++;
    return AE_OK;
}

/** A device PNP0C09 whose _STA says it is present: the first is the EC. */
static ACPI_STATUS ec_found(ACPI_HANDLE handle, UINT32 depth, void *context, void **ret)
{
    (void)depth;
    (void)context;
    (void)ret;
    if (ec.handle == NULL) {
        ec.handle = handle;
    }
    return AE_CTRL_TERMINATE;
}

/** The integer `_GPE` of the EC, which a firmware may also give as a
 * package naming a GPE block device; that form is not taken. */
static void ec_read_gpe(void)
{
    static char gpe[] = "_GPE";
    ACPI_OBJECT obj;
    ACPI_BUFFER buf;

    buf.Length = sizeof(obj);
    buf.Pointer = &obj;
    if (ACPI_SUCCESS(AcpiEvaluateObjectTyped(ec.handle, gpe, NULL, &buf, ACPI_TYPE_INTEGER))) {
        ec.gpe = (UINT32)obj.Integer.Value;
        ec.has_gpe = 1;
    }
}

void acpi_ec_probe(void)
{
    static char hid[] = "PNP0C09", crs[] = "_CRS";
    ACPI_BUFFER name;
    ACPI_STATUS status;

    (void)AcpiGetDevices(hid, ec_found, NULL, NULL);
    if (ec.handle == NULL) {
        return;
    }
    name.Length = sizeof(ec.path);
    name.Pointer = ec.path;
    if (ACPI_FAILURE(AcpiGetName(ec.handle, ACPI_FULL_PATHNAME_NO_TRAILING, &name))) {
        ec.path[0] = '\0';
    }
    ec.io.in = ec_in;
    ec.io.out = ec_out;
    ec.io.delay = ec_delay;
    ec.io.timeout_us = EC_TIMEOUT_US;
    status = AcpiWalkResources(ec.handle, crs, ec_resource, NULL);
    if (ACPI_FAILURE(status) || ec.ports < 2) {
        klog(KLOG_ERR, "acpi", "ec: refused: %s: no two ports in _CRS", ec.path);
        ec.handle = NULL;
        return;
    }
    ec_read_gpe();
    if (ACPI_FAILURE(AcpiOsCreateSemaphore(1, 1, &ec_lock))) {
        klog(KLOG_ERR, "acpi", "ec: refused: %s: no lock", ec.path);
        ec.handle = NULL;
        return;
    }
    status =
        AcpiInstallAddressSpaceHandlerNo_Reg(ec.handle, ACPI_ADR_SPACE_EC, ec_space, NULL, NULL);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_ERR, "acpi", "ec: refused: %s: address space: %s", ec.path,
             AcpiFormatException(status));
        (void)AcpiOsDeleteSemaphore(ec_lock);
        ec.handle = NULL;
        return;
    }
    if (ec.has_gpe) {
        klog(KLOG_INFO, "acpi", "ec: %s at ports 0x%x and 0x%x, gpe 0x%x", ec.path,
             (unsigned)ec.io.data, (unsigned)ec.io.command, (unsigned)ec.gpe);
    } else {
        klog(KLOG_INFO, "acpi", "ec: %s at ports 0x%x and 0x%x, no gpe", ec.path,
             (unsigned)ec.io.data, (unsigned)ec.io.command);
    }
}

/**
 * Run _REG, then take the GPE and enable it, and run the queries once at
 * once when the EC already has SCI_EVT set, whose edge came before the
 * handler, as Linux does at its start.
 */
void acpi_ec_start(void)
{
    ACPI_STATUS status;
    uint32_t flags;

    if (ec.handle == NULL) {
        return;
    }
    status = AcpiExecuteRegMethods(ec.handle, ACPI_ADR_SPACE_EC);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "acpi", "ec: %s: _REG: %s", ec.path, AcpiFormatException(status));
    }
    if (!ec.has_gpe) {
        return;
    }
    status = AcpiInstallGpeHandler(NULL, ec.gpe, ACPI_GPE_EDGE_TRIGGERED, ec_gpe, NULL);
    if (ACPI_SUCCESS(status)) {
        status = AcpiEnableGpe(NULL, ec.gpe);
    }
    if (ACPI_FAILURE(status)) {
        klog(KLOG_ERR, "acpi", "ec: refused: gpe 0x%x: %s", (unsigned)ec.gpe,
             AcpiFormatException(status));
        return;
    }
    klog(KLOG_INFO, "acpi", "ec: gpe 0x%x enabled, status 0x%02x", (unsigned)ec.gpe,
         (unsigned)ec_status(&ec.io));
    flags = kirq_save();
    ec_queue();
    kirq_restore(flags);
}
