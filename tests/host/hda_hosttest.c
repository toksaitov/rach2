/**
 * HDA enumeration and stream programming against a model codec and MMIO.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "audio/hda.c"
struct vpath;
struct fd_entry;
static int procfs_resolve_pid(int pid);
static int procfs_open(const struct vpath *vp, int flags);
static int procfs_readlink_fd(int pid, int fd, char *buf, int cap);
static int procfs_nth_pid(int n);
static uint32_t procfs_ino_dir(void);
static uint32_t procfs_ino_cmdline(void);
static uint32_t procfs_ino_netdir(void);
static uint32_t procfs_ino_netdev(void);
static uint32_t procfs_ino_disk(void);
static uint32_t procfs_ino_uptime(void);
static uint32_t procfs_ino_statsys(void);
static uint32_t procfs_ino_meminfo(void);
static uint32_t procfs_ino_mounts(void);
static uint32_t procfs_ino_usb(void);
static uint32_t procfs_ino_audio(void);
static uint32_t procfs_ino_loop(void);
static uint32_t procfs_ino_interrupts(void);
static uint32_t procfs_ino_console(void);
static uint32_t procfs_ino_filesystems(void);
static uint32_t procfs_ino_netroute(void);
static uint32_t procfs_ino_netarp(void);
static uint32_t procfs_ino_nettcp(void);
static uint32_t procfs_fd_ino(struct fd_entry *e);
static uint32_t procfs_ino_pid(int pid);
#include "fs/procfs.c"

#include "fs/synthfs.c"

extern int printf(const char *format, ...);
extern char *strstr(const char *text, const char *needle);
static int failures;
static const char *row;
static uint8_t mmio[HDA_MMIO_SIZE];
static uint32_t params[128][32];
static uint16_t connections[128][127];
static uint8_t selected[128];
static uint16_t converter_format[128];
static uint32_t configurations[128];
static unsigned conn_reads, verbs, bad_offsets;
static int refuse_alloc;
extern void *malloc(unsigned long size);
extern void free(void *ptr);
static uint32_t clock_value;

/** Report the row and assertion that failed. */
static void check(int ok, const char *message)
{
    if (!ok) {
        printf("hda-test: fail: %s: %s\n", row, message);
        failures++;
    }
}

/** Answer verbs from an independently packed codec connection list. */
static uint32_t model_verb(uint32_t command)
{
    unsigned nid = (command >> 20) & 0x7F;
    unsigned verb = command & 0xFFFFF;
    unsigned offset = verb & 255;
    uint32_t result = 0;
    unsigned i, width, count;

    verbs++;
    check(!(command & (1u << 27)), "no indirect or truncated node verb");
    if ((verb & 0xFFF00) == HDA_VERB_GET_PARAM)
        return params[nid][offset & 31];
    if ((verb & 0xFFF00) == HDA_VERB_GET_CONN_LIST) {
        width = params[nid][HDA_PARAM_CONN_LIST] & 128 ? 16 : 8;
        count = 32 / width;
        conn_reads++;
        if (offset % count != 0)
            bad_offsets++;
        for (i = 0; i < count && offset + i < (params[nid][HDA_PARAM_CONN_LIST] & 127); i++)
            result |= (uint32_t)connections[nid][offset + i] << (width * i);
        return result;
    }
    if (verb == HDA_VERB_GET_CONFIG_DEF)
        return configurations[nid];
    if (verb == HDA_VERB_GET_CONN_SEL)
        return selected[nid];
    if ((verb & 0xFFF00) == HDA_VERB_SET_CONN_SEL)
        selected[nid] = offset;
    if ((verb & 0xF0000) == HDA_VERB_SET_CONV_FMT)
        converter_format[nid] = (uint16_t)verb;
    if (verb == 0xA0000)
        return converter_format[nid];
    return 0;
}

/** Supply byte-addressed MMIO and an advancing controller clock. */
uint32_t hda_test_read(uint32_t offset, int bits)
{
    uint32_t value = 0;
    int i;

    if (offset == HDA_WALCLK)
        return clock_value += 24000;
    for (i = 0; i < bits / 8; i++)
        value |= (uint32_t)mmio[offset + i] << (i * 8);
    return value;
}

/** Complete immediate commands when the driver sets ICB. */
void hda_test_write(uint32_t offset, uint32_t value, int bits)
{
    int i;

    if (offset == HDA_ICS) {
        if (value & HDA_ICS_IRV)
            mmio[HDA_ICS] &= ~HDA_ICS_IRV;
        if (value & HDA_ICS_ICB) {
            uint32_t reply = model_verb(hda_test_read(HDA_ICW, 32));
            hda_test_write(HDA_IRR, reply, 32);
            mmio[HDA_ICS] = HDA_ICS_IRV;
        }
        return;
    }
    for (i = 0; i < bits / 8; i++)
        mmio[offset + i] = (uint8_t)(value >> (i * 8));
}

/** Set up an AFG, one speaker pin and one stereo DAC. */
static void reset_model(void)
{
    k_memset(mmio, 0, sizeof(mmio));
    k_memset(params, 0, sizeof(params));
    k_memset(connections, 0, sizeof(connections));
    k_memset(selected, 0, sizeof(selected));
    k_memset(converter_format, 0, sizeof(converter_format));
    k_memset(configurations, 0, sizeof(configurations));
    conn_reads = verbs = bad_offsets = 0;
    playing = stream_started = false;
    user_callback = NULL;
    hda_base = 1;
    dma_posbuf_enabled = false;
    output_stream_index = 0;
    mmio[HDA_STATESTS] = 1;
    params[0][HDA_PARAM_VENDOR_ID] = 0x14F1506E;
    params[0][HDA_PARAM_NODE_COUNT] = 0x10001;
    params[1][HDA_PARAM_FUNC_GROUP] = 1;
    params[1][HDA_PARAM_NODE_COUNT] = 0x2007E;
    params[1][0x0A] = 0x30000 | 0xFFF;
    params[1][0x0B] = 1;
    params[2][HDA_PARAM_AUDIO_CAPS] = 1;
    params[10][HDA_PARAM_AUDIO_CAPS] = HDA_WIDGET_PIN << 20;
    params[10][HDA_PARAM_PIN_CAPS] = 0x10;
    params[10][HDA_PARAM_CONN_LIST] = 1;
    connections[10][0] = 2;
    configurations[10] = HDA_DEF_DEV_SPEAKER << 20;
}

/** Decode the controller word using its base, multiplier and divisor fields. */
static uint32_t decoded_rate(uint16_t word)
{
    return ((word & 0x4000) ? 44100u : 48000u) * (((word >> 11) & 7) + 1) / (((word >> 8) & 7) + 1);
}

/** Supply samples for exact-rate callback setup. */
static void callback(void *buffer, size_t size)
{
    k_memset(buffer, 0, size);
}

/** Every capability bit must produce that rate in both stream and converter. */
static void row_rates(void)
{
    static const uint32_t rates[] = {8000,  11025, 16000, 22050,  32000,  44100,
                                     48000, 88200, 96000, 176400, 192000, 384000};
    unsigned i, mode;

    row = "rates";
    for (i = 0; i < 12; i++) {
        for (mode = 0; mode < 2; mode++) {
            reset_model();
            params[1][0x0A] = 0x30000 | (1u << i);
            check(hda_find_output(), "enumeration succeeds");
            user_callback = mode ? callback : NULL;
            check(hda_start(rates[i], AUDIO_S16LE, 2) == 0, "supported start");
            check(decoded_rate((uint16_t)hda_test_read(0x92, 16)) == rates[i],
                  "stream rate equals request");
            check(decoded_rate(converter_format[2]) == rates[i], "converter rate equals request");
            check(current_hw_sample_rate == rates[i], "recorded rate equals programmed rate");
            check(hda_get_report()->programmed_rate == rates[i], "report rate");
            hda_stop();
            check(!hda_get_report()->playing && hda_get_report()->programmed_rate == rates[i],
                  "report survives stop");
            user_callback = NULL;
            check(hda_start(12345, AUDIO_U8, 1) == 0, "single advertised fallback rate");
            check(current_hw_sample_rate == rates[i], "fallback includes every rate bit");
        }
    }
    check(hda_rate_step(176400, 192000) == 60211, "high-rate ratio does not overflow");
}

/** Inherited, overridden, absent and disjoint capabilities govern setup. */
static void row_caps(void)
{
    unsigned before;

    row = "capabilities";
    reset_model();
    params[1][0x0A] = 0x30040;
    check(hda_find_output(), "inherit enumeration");
    check(hda_start(32000, AUDIO_S16LE, 2) == 0 && current_hw_sample_rate == 48000,
          "write resamples unsupported rate");
    hda_stop();
    user_callback = callback;
    before = verbs;
    check(hda_start(32000, AUDIO_S16LE, 2) == -1 && !playing && verbs == before,
          "unsupported callback refused before programming");
    reset_model();
    params[2][HDA_PARAM_AUDIO_CAPS] |= 1u << 4;
    params[2][0x0A] = 0x20010;
    params[2][0x0B] = 1;
    check(hda_find_output(), "override enumeration");
    check(hda_start(32000, AUDIO_S16LE, 2) == 0 && current_hw_sample_rate == 32000,
          "widget overrides afg");
    hda_stop();
    user_callback = callback;
    check(hda_start(32000, AUDIO_U8, 1) == -1, "unsupported width refused");
    params[2][0x0A] = 0;
    hda_capture_report();
    user_callback = NULL;
    check(hda_start(48000, AUDIO_S16LE, 2) == -1, "empty mask refused");
    params[2][0x0A] = 0x30040;
    params[2][0x0B] = 0;
    hda_capture_report();
    check(hda_start(48000, AUDIO_S16LE, 2) == -1, "non-pcm refused");
    params[2][0x0B] = 1;
    hp_dac_nid = 3;
    params[3][HDA_PARAM_AUDIO_CAPS] = 0x11;
    params[3][0x0A] = 0x30100;
    params[3][0x0B] = 1;
    hda_capture_report();
    check(hda_start(48000, AUDIO_S16LE, 2) == -1, "disjoint dacs refused");
    params[3][0x0A] |= 0x40;
    hda_capture_report();
    check(hda_start(96000, AUDIO_S16LE, 2) == 0 && current_hw_sample_rate == 48000,
          "both dacs use intersection");
    check(converter_format[2] == converter_format[3], "both converters match");
}

/** Read second words, full long NIDs and ranges across response boundaries. */
static void row_connections(void)
{
    uint16_t out[HDA_MAX_CONNECTIONS];
    int i, n;

    row = "short-offset";
    reset_model();
    params[10][HDA_PARAM_CONN_LIST] = 9;
    for (i = 0; i < 9; i++)
        connections[10][i] = (uint16_t)(20 + i);
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 9 && out[4] == 24 && out[8] == 28 && conn_reads == 3 && !bad_offsets,
          "entry offset advances by four");
    row = "long-offset";
    params[10][HDA_PARAM_CONN_LIST] = 0x85;
    conn_reads = bad_offsets = 0;
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 5 && out[2] == 22 && out[4] == 24 && conn_reads == 3 && !bad_offsets,
          "entry offset advances by two");
    row = "long-width";
    params[10][HDA_PARAM_CONN_LIST] = 0x82;
    connections[10][0] = 0x102;
    connections[10][1] = 0x1234;
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 2 && out[0] == 0x102 && out[1] == 0x1234, "fifteen-bit nids retained");
    check(!hda_find_output(), "indirect nid cannot alias the first dac");
    row = "short-range";
    params[10][HDA_PARAM_CONN_LIST] = 5;
    connections[10][0] = 2;
    connections[10][1] = 0x85;
    connections[10][2] = 8;
    connections[10][3] = 9;
    connections[10][4] = 0x8C;
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 9 && out[3] == 5 && out[6] == 10 && out[8] == 12, "expanded selector order");
    row = "long-range";
    params[10][HDA_PARAM_CONN_LIST] = 0x83;
    connections[10][0] = 0x101;
    connections[10][1] = 0x8104;
    connections[10][2] = 0x1234;
    n = hda_get_connections(10, out, HDA_MAX_CONNECTIONS);
    check(n == 5 && out[3] == 0x104 && out[4] == 0x1234, "long range bit and endpoint");
    row = "invalid-list";
    check(hda_get_connections(10, out, 3) == -1, "capacity rejects a partial list");
    connections[10][0] = 0x8101;
    check(hda_get_connections(10, out, HDA_MAX_CONNECTIONS) == -1, "leading range refused");
    connections[10][0] = 0x105;
    check(hda_get_connections(10, out, HDA_MAX_CONNECTIONS) == -1, "descending range refused");
}

/** Discovery and setup use the expanded index, including on Conexant codecs. */
static void row_route(void)
{
    row = "route";
    reset_model();
    params[10][HDA_PARAM_CONN_LIST] = 0x83;
    connections[10][0] = 20;
    connections[10][1] = 0x8018;
    connections[10][2] = 25;
    params[24][HDA_PARAM_AUDIO_CAPS] = (HDA_WIDGET_AUDIO_SEL << 20);
    params[24][HDA_PARAM_CONN_LIST] = 2;
    connections[24][0] = 3;
    connections[24][1] = 0x85;
    params[3][HDA_PARAM_AUDIO_CAPS] = 0xF00000;
    params[4][HDA_PARAM_AUDIO_CAPS] = 1;
    params[5][HDA_PARAM_AUDIO_CAPS] = 0xF00000;
    params[20][HDA_PARAM_AUDIO_CAPS] = params[21][HDA_PARAM_AUDIO_CAPS] = 0xF00000;
    params[22][HDA_PARAM_AUDIO_CAPS] = params[23][HDA_PARAM_AUDIO_CAPS] = 0xF00000;
    selected[24] = 1;
    check(hda_find_output(), "path found through range");
    check(pin_conn_sel_idx == 4 && mixer_nid == 24 && dac_nid == 4, "expanded route selected");
    check(hda_start(96000, AUDIO_S16LE, 2) == 0, "routed start");
    check(selected[10] == 4 && selected[24] == 1, "expanded indices programmed");
    check(hda_report.connections[0].raw[1] == 0x8018 && hda_report.connections[0].nodes[4] == 24,
          "raw and expanded path retained");
    reset_model();
    params[10][HDA_PARAM_CONN_LIST] = 1;
    connections[10][0] = 0x82;
    check(!hda_find_output(), "invalid list cannot fall back to an unrelated dac");
}

/** Snapshot the real driver report through the procfs formatter. */
static void row_report(void)
{
    struct procfs_text text;
    struct vpath path;
    unsigned before;
    int len;

    row = "proc-audio";
    reset_model();
    check(hda_find_output(), "report enumeration");
    check(hda_start(32000, AUDIO_S16LE, 2) == 0, "report stream start");
    before = verbs;
    procfs_text_init(&text);
    len = procfs_gen_audio(&text);
    if (procfs_text_room(&text, 1) == 0)
        text.buf[text.len] = 0;
    check(len > 0 && !text.nomem && verbs == before, "report sends no verbs");
    check(strstr(text.buf, "stream requested 32000 programmed 32000 format 0x0A11") != NULL,
          "report exposes programmed rate and register");
    check(strstr(text.buf, "caps dac 2 source 1 pcm 0x00030FFF formats 0x00000001") != NULL,
          "report exposes inherited capabilities");
    check(strstr(text.buf, "connections nid 10 long 0 raw 0x02\nexpanded 2\n") != NULL,
          "report exposes codec list");
    hda_stop();
    check(strstr(text.buf, "playing 1") != NULL, "open snapshot survives stop");
    kfree(text.buf);
    procfs_text_init(&text);
    refuse_alloc = 1;
    check(procfs_gen_audio(&text) < 0 && text.nomem, "allocation failure rejects report");
    refuse_alloc = 0;
    check(synth_classify("proc/audio", &path) == VNODE_PROC_AUDIO, "audio path classified");
    check(synth_classify("proc/audio/child", &path) == VNODE_ENOTDIR, "audio is a file");
    check(procfs_ino_audio() != procfs_ino_usb(), "audio inode is distinct");
}

/** Allocate a procfs snapshot, or inject allocation failure. */
void *kmalloc(size_t size)
{
    return refuse_alloc ? NULL : malloc(size);
}
/** Release a procfs snapshot. */
void kfree(void *ptr)
{
    free(ptr);
}
/** Name the model's selected driver. */
const char *audio_get_driver_name(void)
{
    return "Intel HD Audio";
}

/** Run codec and controller rows without executing host I/O instructions. */
int main(void)
{
    row_rates();
    row_caps();
    row_connections();
    row_route();
    row_report();
    printf("hda-test: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}

/** Ignore logging; the rows inspect commands and cached state. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}
/** The model's delay port has no side effects. */
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0;
}
/** No console is attached to the model. */
void console_putc(char c)
{
    (void)c;
}
/** No serial port is attached to the model. */
void serial_putc(char c)
{
    (void)c;
}
/** No formatter output is needed by the model. */
void k_vprintf(void (*putc_fn)(char), const char *format, va_list ap)
{
    (void)putc_fn;
    (void)format;
    (void)ap;
}
/** PCI discovery is outside these codec rows. */
bool pci_find_device(uint8_t c, uint8_t s, pci_device_t *out)
{
    (void)c;
    (void)s;
    (void)out;
    return false;
}
/** No second controller exists in the model. */
bool pci_find_device_skip(uint8_t c, uint8_t s, pci_device_t *skip, pci_device_t *out)
{
    (void)c;
    (void)s;
    (void)skip;
    (void)out;
    return false;
}
/** The model has no PCI capabilities. */
uint8_t pci_find_capability(uint8_t b, uint8_t d, uint8_t f, uint8_t c)
{
    (void)b;
    (void)d;
    (void)f;
    (void)c;
    return 0;
}
/** PCI configuration is outside these rows. */
uint16_t pci_read_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0;
}
/** PCI configuration is outside these rows. */
uint8_t pci_read_config8(uint8_t b, uint8_t d, uint8_t f, uint8_t o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0;
}
/** PCI configuration is outside these rows. */
void pci_write_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint16_t v)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    (void)v;
}
/** No interrupt allocation is used. */
int isr_alloc_vector(void)
{
    return -1;
}
/** The model has no APIC. */
int lapic_ready(void)
{
    return 0;
}
/** The model has no MSI capability. */
int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    (void)vector;
    return -1;
}
/** Interrupt service is outside these rows. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}
/** MMIO accesses use the test hooks. */
uintptr_t map_mmio(uintptr_t addr, size_t size)
{
    (void)size;
    return addr;
}
/** PCI mapping is outside these rows. */
uint32_t pci_bar_address(uint32_t bar)
{
    return bar;
}
/** PCI bus mastering is outside these rows. */
void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}
/** PCI mapping is outside these rows. */
void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
}
/** PCI interrupts are outside these rows. */
void pci_enable_interrupts(pci_device_t *dev)
{
    (void)dev;
}
/** The host does not execute cache instructions. */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    (void)leaf;
    *a = *b = *c = *d = 0;
}
/** Boot banner formatting is outside these rows. */
size_t k_itoa(int n, char *out, size_t size, int radix, bool sign)
{
    (void)n;
    (void)radix;
    (void)sign;
    if (size)
        out[0] = 0;
    return 0;
}
