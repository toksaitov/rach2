/**
 * /dev/mixer (audio/mixer.c) over a model driver: the device, record and
 * stereo masks of a driver with and without input controls, VOLUME and PCM
 * as one master volume, a level set by another path, each source's gain
 * scaled by RECLEV, IGAIN on every source, the record source, and the
 * errors.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The test includes mixer.c whole. The model driver keeps the master
 * volume, the source recorded and each source's gain and boost as the mixer
 * last set them, so that a row reads what the hardware would be given.
 */
#include "audio/mixer.c"

extern int printf(const char *format, ...);
extern int strcmp(const char *a, const char *b);

/* OSS's numbers of the requests and the masks the rows use. */
#define READ(n)     MIXER_READ(n)
#define WRITE(n)    MIXER_WRITE(n)
#define M_VOLUME    (1 << CH_VOLUME)
#define M_PCM       (1 << CH_PCM)
#define M_LINE      (1 << CH_LINE)
#define M_MIC       (1 << CH_MIC)
#define M_RECLEV    (1 << CH_RECLEV)
#define M_IGAIN     (1 << CH_IGAIN)
#define M_MONITOR   (1 << CH_MONITOR)
#define DSP_GETFMTS 0x8004500Bu /* a request of another device */
#define LEVEL(l)    ((l) | (l) << 8)
#define SRC(s)      (1u << (s))
#define CTL(c)      (1u << (c))

static int failures;
static const char *row; /* the row under way, named in a failure */

/* The model driver's state: what the mixer gave it. */
static struct {
    int volume;
    unsigned sources;  /* the sources it records, 1 << audio_source_t */
    unsigned controls; /* its input controls, 1 << audio_input_ctl_t */
    int src;           /* the source recorded */
    int level[2][AUDIO_SOURCES];
} md;

/** Count and name a failure when `ok` is false. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("mixer-hosttest: fail: %s: %s\n", row, what);
        failures++;
    }
}

/** The model's master volume, kept between 0 and 100. */
static void m_set_volume(int v)
{
    md.volume = v < 0 ? 0 : v > 100 ? 100 : v;
}

static int m_get_volume(void)
{
    return md.volume;
}

static unsigned m_input_caps(unsigned *controls)
{
    *controls = md.controls;
    return md.sources;
}

static int m_input_select(int s)
{
    if (s >= 0 && s < AUDIO_SOURCES && (md.sources & SRC(s)))
        md.src = s;
    return md.src;
}

static int m_input_level(int ctl, int s, int level)
{
    if (s < 0 || s >= AUDIO_SOURCES || !(md.sources & SRC(s)) || ctl < 0 || ctl > 1 ||
        !(md.controls & CTL(ctl)))
        return -EINVAL;
    if (level >= 0)
        md.level[ctl][s] = level;
    return md.level[ctl][s];
}

/* A driver that records, as HDA over a codec with an ADC, and one that does
 * not, as the SB16. */
static audio_driver_t recording = {.name = "recording",
                                   .type = AUDIO_DRIVER_HDA,
                                   .set_volume = m_set_volume,
                                   .get_volume = m_get_volume,
                                   .input_caps = m_input_caps,
                                   .input_select = m_input_select,
                                   .input_level = m_input_level};
static audio_driver_t output = {.name = "output",
                                .type = AUDIO_DRIVER_SB16,
                                .set_volume = m_set_volume,
                                .get_volume = m_get_volume};

audio_driver_t *audio;

/** Copy from the caller; a NULL source is a bad pointer. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    if (!src)
        return -EFAULT;
    k_memcpy(dst, src, len);
    return 0;
}

/** Copy to the caller; a NULL destination is a bad pointer. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    if (!dst)
        return -EFAULT;
    k_memcpy(dst, src, len);
    return 0;
}

/** Begin row `name` over driver `d`: the mixer before its first request,
 * the model at 100 recording the internal mic, the jack mic beside it, each
 * gain at 93, the 0 dB step of the G50-70's ADC, and each boost at 0. */
static void begin(const char *name, audio_driver_t *d)
{
    int i;

    row = name;
    audio = d;
    k_memset(&mx, 0, sizeof(mx));
    k_memset(&md, 0, sizeof(md));
    md.volume = 100;
    md.sources = SRC(AUDIO_SRC_MIC) | SRC(AUDIO_SRC_INT_MIC);
    md.controls = CTL(AUDIO_IN_GAIN) | CTL(AUDIO_IN_BOOST);
    md.src = AUDIO_SRC_INT_MIC;
    for (i = 0; i < AUDIO_SOURCES; i++)
        md.level[AUDIO_IN_GAIN][i] = 93;
}

/** The ioctl `request` with the int `*v`: its return. */
static int32_t req(uint32_t request, int *v)
{
    return mixer_ioctl(NULL, request, (uintptr_t)v);
}

/** The int request `request` reads, or -1000 - the error. */
static int get(uint32_t request)
{
    int v = -1;
    int32_t rc = req(request, &v);

    return rc != 0 ? -1000 + rc : v;
}

/** Write `value` with `request`: the value given back, or -1000 - the
 * error. */
static int put(uint32_t request, int value)
{
    int32_t rc = req(request, &value);

    return rc != 0 ? -1000 + rc : value;
}

/**
 * A driver that records nothing has the volume and PCM alone, no record
 * source and no capabilities, and its channels read 100 at the start.
 */
static void row_output(void)
{
    begin("output-only", &output);
    check(get(READ(MIXER_DEVMASK)) == (M_VOLUME | M_PCM), "the volume and pcm alone");
    check(get(READ(MIXER_RECMASK)) == 0 && get(READ(MIXER_RECSRC)) == 0, "no source");
    check(get(READ(MIXER_CAPS)) == 0 && get(READ(MIXER_STEREODEVS)) == 0, "no caps, mono");
    check(get(READ(CH_VOLUME)) == LEVEL(100) && get(READ(CH_PCM)) == LEVEL(100), "both read 100");
    check(get(READ(CH_MIC)) == -1000 - EINVAL && put(WRITE(CH_RECLEV), 50) == -1000 - EINVAL,
          "the input channels are missing");
    check(put(WRITE(MIXER_RECSRC), M_MIC) == 0, "a record source is not taken");
}

/**
 * The master volume is VOLUME times PCM, each of which reads as set: a
 * write takes the louder side and stops at 100. A level the driver is
 * given by another path reads as VOLUME, with PCM back at 100.
 */
static void row_volume(void)
{
    begin("volume-pcm", &recording);
    check(put(WRITE(CH_VOLUME), LEVEL(80)) == LEVEL(80) && md.volume == 80, "volume 80");
    check(put(WRITE(CH_PCM), LEVEL(50)) == LEVEL(50) && md.volume == 40, "pcm 50 halves it");
    check(get(READ(CH_VOLUME)) == LEVEL(80) && get(READ(CH_PCM)) == LEVEL(50), "each as set");
    check(put(WRITE(CH_VOLUME), 0x2050) == LEVEL(80) && md.volume == 40, "the louder side");
    check(put(WRITE(CH_PCM), LEVEL(200)) == LEVEL(100) && md.volume == 80, "pcm stops at 100");
    put(WRITE(CH_PCM), LEVEL(50));
    md.volume = 70; /* the console's AUDIO_SET_VOLUME */
    check(get(READ(CH_VOLUME)) == LEVEL(70) && get(READ(CH_PCM)) == LEVEL(100),
          "a level set elsewhere is the volume's, and pcm 100");
    check(md.volume == 70, "and stays as set");
    check(put(WRITE(CH_VOLUME), 0) == 0 && md.volume == 0, "volume 0");
}

/**
 * With the G50-70's two mics the device mask has their channels, MIC for
 * the jack and MONITOR for the internal one, RECLEV and IGAIN, and the
 * record mask the two. RECLEV starts at the gain of the source recorded,
 * and each source's gain is its level times RECLEV; IGAIN is the boost of
 * both.
 */
static void row_input(void)
{
    begin("input-levels", &recording);
    check(get(READ(MIXER_DEVMASK)) == (M_VOLUME | M_PCM | M_MIC | M_RECLEV | M_IGAIN | M_MONITOR),
          "the mics, rec and igain beside the volume and pcm");
    check(get(READ(MIXER_RECMASK)) == (M_MIC | M_MONITOR), "both mics are sources");
    check(get(READ(MIXER_CAPS)) == CAP_EXCL_INPUT, "one source at a time");
    check(get(READ(CH_RECLEV)) == LEVEL(93) && get(READ(CH_IGAIN)) == 0,
          "rec and igain start at the driver's");
    check(get(READ(CH_MIC)) == LEVEL(100) && get(READ(CH_MONITOR)) == LEVEL(100),
          "the sources start at 100");
    check(get(READ(CH_LINE)) == -1000 - EINVAL, "no line in");
    check(put(WRITE(CH_MIC), LEVEL(50)) == LEVEL(50), "mic 50");
    check(md.level[AUDIO_IN_GAIN][AUDIO_SRC_MIC] == 47 &&
              md.level[AUDIO_IN_GAIN][AUDIO_SRC_INT_MIC] == 93,
          "the jack's gain is 50 times 93, the internal mic's unchanged");
    check(put(WRITE(CH_RECLEV), LEVEL(100)) == LEVEL(100), "rec 100");
    check(md.level[AUDIO_IN_GAIN][AUDIO_SRC_MIC] == 50 &&
              md.level[AUDIO_IN_GAIN][AUDIO_SRC_INT_MIC] == 100,
          "rec scales both");
    check(put(WRITE(CH_IGAIN), LEVEL(33)) == LEVEL(33) &&
              md.level[AUDIO_IN_BOOST][AUDIO_SRC_MIC] == 33 &&
              md.level[AUDIO_IN_BOOST][AUDIO_SRC_INT_MIC] == 33,
          "igain boosts both");
    check(md.volume == 100, "the volume is untouched");

    begin("input-no-boost", &recording);
    md.controls = CTL(AUDIO_IN_GAIN);
    check(!(get(READ(MIXER_DEVMASK)) & M_IGAIN), "no igain without a boost");
    md.controls = 0;
    check(get(READ(MIXER_DEVMASK)) == (M_VOLUME | M_PCM) &&
              get(READ(MIXER_RECMASK)) == (M_MIC | M_MONITOR),
          "sources with no gain have no channel and still record");
}

/**
 * The record source reads as the driver's; a write of one source takes
 * it, of two the one not recorded, of none or of a missing one nothing,
 * and gives back the source recorded.
 */
static void row_recsrc(void)
{
    begin("recsrc", &recording);
    check(get(READ(MIXER_RECSRC)) == M_MONITOR, "the internal mic is recorded");
    check(put(WRITE(MIXER_RECSRC), M_MIC) == M_MIC && md.src == AUDIO_SRC_MIC, "the jack");
    check(put(WRITE(MIXER_RECSRC), M_MIC | M_MONITOR) == M_MONITOR && md.src == AUDIO_SRC_INT_MIC,
          "of two, the one added");
    check(put(WRITE(MIXER_RECSRC), 0) == M_MONITOR, "none changes nothing");
    check(put(WRITE(MIXER_RECSRC), M_LINE) == M_MONITOR, "a missing source changes nothing");
    check(get(READ(MIXER_RECSRC)) == M_MONITOR, "and reads so");
}

/** The errors, the version, and SOUND_MIXER_INFO's count of writes. */
static void row_requests(void)
{
    struct mixer_info info;

    begin("requests", &recording);
    check(get(READ(CH_VOLUME)) == LEVEL(100), "a read");
    check(put(WRITE(CH_VOLUME), LEVEL(60)) == LEVEL(60), "a write");
    check(put(WRITE(MIXER_RECSRC), M_MIC) == M_MIC, "a source");
    check(get(READ(1)) == -1000 - EINVAL && put(WRITE(1), 0) == -1000 - EINVAL, "bass is missing");
    check(get(READ(30)) == -1000 - EINVAL, "a number past the channels");
    check(get(DSP_GETFMTS) == -1000 - ENOTTY, "another device's request");
    check(req(READ(CH_VOLUME), NULL) == -EFAULT && req(WRITE(CH_PCM), NULL) == -EFAULT,
          "a bad pointer");
    check(get(OSS_GETVERSION) == OSS_VERSION, "the version");
    check(mixer_ioctl(NULL, MIXER_INFO, (uintptr_t)&info) == 0 && info.modify_counter == 2 &&
              strcmp(info.id, "HDA") == 0 && strcmp(info.name, "recording") == 0,
          "info names the driver and counts two writes");
    audio = NULL;
    check(get(READ(CH_VOLUME)) == -1000 - ENODEV && !mixer_present(), "no driver");
}

int main(void)
{
    row_output();
    row_volume();
    row_input();
    row_recsrc();
    row_requests();
    printf("mixer-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
