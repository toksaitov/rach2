/*
 * audio_debug.c: the shell's `audio` command.
 *
 * Everything here is printed because a person asked for it at the prompt, so
 * it goes through the console calls to the screen and the serial port, and
 * not to the kernel log.
 */

#include "../arch/arch.h"
#include "audio_debug.h"
#include "hda.h"
#include "../klog.h"
#include "../lib/kstring.h"
#include "../proc/sched.h"
#include "../timer.h"

/** `ms` milliseconds as whole ticks, rounded down. */
static uint32_t ms_ticks(uint32_t ms)
{
    return ms / 1000u * TICK_HZ + ms % 1000u * TICK_HZ / 1000u;
}

/** The low word of the tick count, which the tests' intervals are measured
 * against. */
static uint32_t now_ticks(void)
{
    return (uint32_t)kernel_ticks;
}

/*
 * State for the kernel-side square-wave test tone. The mixer callback runs in
 * interrupt context and reads these while the shell command writes them, so
 * they are volatile. _tone_half_period is the count of frames in each half of
 * the wave; _tone_phase counts frames within one full period.
 */
static volatile uint32_t _tone_phase = 0;
static volatile uint32_t _tone_half_period = 0;
static volatile int16_t _tone_amp = 8000;
static volatile int _tone_channels = 2;

/*
 * State of the test scale, whose callback makes each frame from the count of
 * frames before it, so that the notes do not depend on when the device asks
 * for them. Each note lasts _scale_note_frames and a rest of
 * _scale_rest_frames follows it: three periods of the driver, the frames its
 * first call asks for. A refill that comes late leaves the device to play
 * what its buffer already has, the last two periods written and the rest of
 * the one being overwritten, so the buffer never has two notes at once and
 * they are heard in their order however late the refills come. The callback
 * runs in interrupt context while the shell command reads the counts, so they
 * are volatile; _scale_rest_frames is 0 until the first call.
 */
static const uint32_t _scale_freqs[] = {262, 294, 330, 349, 392};
#define SCALE_NOTES (sizeof(_scale_freqs) / sizeof(_scale_freqs[0]))
static uint32_t _scale_rate;
static uint32_t _scale_note_frames;
static volatile uint32_t _scale_rest_frames;
static volatile uint32_t _scale_frames;

/** Whether the strings `a` and `b` are equal. */
static bool audio_str_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }

    return *a == *b;
}

/** Whether `str` begins with `prefix`. */
static bool audio_str_starts_with(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix) {
            return false;
        }
        ++str;
        ++prefix;
    }

    return true;
}

/**
 * Split `buffer` into space- or tab-delimited tokens in place.
 *
 * Each separator run is overwritten with a NUL and the first character of each
 * token is recorded in `argv`. At most `max_args` tokens are stored; any tail
 * beyond that is left untouched in the buffer.
 *
 * @return the number of tokens stored in `argv`.
 */
static int audio_tokenize(char *buffer, char **argv, int max_args)
{
    int argc = 0;
    char *p;
    int in_arg = 0;

    for (p = buffer; *p != '\0' && argc < max_args; ++p) {
        if (*p == ' ' || *p == '\t') {
            *p = '\0';
            in_arg = 0;
        } else if (!in_arg) {
            argv[argc++] = p;
            in_arg = 1;
        }
    }

    return argc;
}

/**
 * Parse a non-negative decimal integer with no sign or whitespace.
 *
 * The whole string must be digits; anything else (including an empty string or
 * a null `str` or `out`) fails and leaves `*out` unchanged. Overflow past 32
 * bits wraps instead of being rejected.
 *
 * @return true on success, false on a malformed input.
 */
static bool audio_parse_uint(const char *str, uint32_t *out)
{
    uint32_t value = 0;

    if (!str || *str == '\0' || !out) {
        return false;
    }

    while (*str) {
        char c = *str++;
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10u + (uint32_t)(c - '0');
    }

    *out = value;
    return true;
}

/**
 * Mixer callback that fills `buffer` with the current square-wave tone.
 *
 * Runs in interrupt context from the driver's playback path. It emits signed
 * 16-bit samples driven by the file-scope _tone_* state, duplicating each
 * sample across both channels when stereo. `size` is in bytes, treated as
 * `size / 2` samples (a trailing odd byte is dropped). In stereo, the
 * `i + 1 < samples16` guard writes a final lone sample to one channel only, so
 * the buffer is never overrun.
 */
static void audio_test_callback(void *buffer, size_t size)
{
    size_t i;
    int16_t *out = buffer;
    size_t samples16 = size / 2;

    for (i = 0; i < samples16;) {
        int16_t s = (_tone_phase < _tone_half_period) ? _tone_amp : (int16_t)(-_tone_amp);
        _tone_phase++;
        if (_tone_phase >= (_tone_half_period * 2u)) {
            _tone_phase = 0;
        }

        if (_tone_channels == 2) {
            if (i + 1 < samples16) {
                out[i++] = s;
                out[i++] = s;
            } else {
                out[i++] = s;
            }
        } else {
            out[i++] = s;
        }
    }
}

/**
 * The scale's frame `frame`: a square wave of amplitude 12000 at its note's
 * frequency, starting high at the note's first frame, or 0 in a rest and after
 * the last note.
 */
static int16_t audio_scale_sample(uint32_t frame)
{
    uint32_t cycle = _scale_note_frames + _scale_rest_frames;
    uint32_t note = frame / cycle;
    uint32_t at = frame % cycle;
    uint32_t half;

    if (note >= SCALE_NOTES || at >= _scale_note_frames) {
        return 0;
    }
    half = _scale_rate / _scale_freqs[note] / 2;
    if (half == 0) {
        half = 1;
    }
    return (at / half) % 2 == 0 ? (int16_t)12000 : (int16_t)-12000;
}

/**
 * Mixer callback that fills `buffer` with the scale's next frames, signed
 * 16-bit stereo with both channels alike, and counts them. A trailing byte
 * short of a whole frame is silence.
 */
static void audio_scale_callback(void *buffer, size_t size)
{
    int16_t *out = buffer;
    uint32_t frames = (uint32_t)(size / 4);
    uint32_t first = _scale_frames;
    uint32_t i;

    if (_scale_rest_frames == 0) {
        _scale_rest_frames = 3 * frames;
    }
    for (i = 0; i < frames; i++) {
        out[2 * i] = out[2 * i + 1] = audio_scale_sample(first + i);
    }
    for (i = frames * 4; i < size; i++) {
        ((uint8_t *)buffer)[i] = 0;
    }
    _scale_frames = first + frames;
}

/**
 * Stop any active playback and detach the test callback.
 *
 * Safe to call when no driver is bound or nothing is playing.
 */
static void audio_stop_playback(void)
{
    if (!audio) {
        return;
    }

    if (audio->is_playing && audio->is_playing()) {
        audio->stop();
    }

    if (audio->set_callback) {
        audio->set_callback((audio_callback_t)0);
    }
}

/**
 * Play a five-note C-major scale through the callback path.
 *
 * Uses stereo signed-16 samples at 48 kHz, or 44.1 kHz on SB16. The
 * interrupt-time callback generates every frame, and each note is a fifth of
 * `duration_ms` in frames, which is clamped to 100 through 600000 ms; a rest
 * follows each note. The command waits until the callback has generated the
 * whole scale with its last rest, by which time the device has read the last
 * note, and prints "Done.". A device that stops asking for frames leaves the
 * scale unfinished after four times its length and a second, measured in
 * timer ticks, and the command prints how far it got. Always stops playback
 * on the way out.
 */
static void audio_run_scale_test(uint32_t duration_ms)
{
    uint32_t sample_rate = 48000;
    uint32_t total, limit;

    if (!audio) {
        console_puts("No audio driver.");
        return;
    }

    if (audio->type == AUDIO_DRIVER_SB16) {
        sample_rate = 44100;
    }

    if (duration_ms < 100) {
        duration_ms = 100;
    }
    if (duration_ms > 600000) {
        duration_ms = 600000;
    }

    audio_stop_playback();

    _scale_rate = sample_rate;
    _scale_note_frames = duration_ms / SCALE_NOTES * (sample_rate / 10) / 100;
    _scale_rest_frames = 0;
    _scale_frames = 0;

    if (audio->set_callback) {
        audio->set_callback(audio_scale_callback);
    }

    if (audio->start(sample_rate, AUDIO_S16LE, 2) != 0) {
        console_puts("AUDIO_START failed.");
        audio_stop_playback();
        return;
    }

    if (audio->set_volume) {
        audio->set_volume(80);
    }

    console_printf("Playing scale test (%d ms) using %s...\n", duration_ms,
                   audio_get_driver_name());

    total = (uint32_t)SCALE_NOTES * (_scale_note_frames + _scale_rest_frames);
    limit = now_ticks() + 4u * (total / (sample_rate / TICK_HZ)) + TICK_HZ;
    while (_scale_frames < total && (int32_t)(now_ticks() - limit) < 0) {
        idle_wait_irq_on();
    }

    audio_stop_playback();
    if (_scale_frames < total) {
        console_printf("audio: stalled: %u of %u frames\n", _scale_frames, total);
        return;
    }
    console_puts("Done.");
}

/**
 * Play the same scale through the driver's write path instead of the callback.
 *
 * Uses 11025 Hz unsigned-8 mono and synthesizes samples in this thread, pushing
 * them with audio->write(). Each timer tick it computes how many frames are due
 * (sample_acc keeps the fractional remainder across ticks so the rate stays
 * exact over time) and writes them in <= sizeof(buf) chunks, halting until the
 * next interrupt while a write takes nothing. Exercises the write side that the
 * callback test bypasses.
 */
static void audio_run_write_test(uint32_t duration_ms)
{
    uint32_t start_ticks;
    uint32_t total_ticks;
    uint32_t sample_rate = 11025;
    uint32_t tick_last;
    uint32_t sample_acc = 0;
    uint32_t phase = 0;
    uint32_t half_period = 0;
    size_t n;
    uint8_t buf[512];
    const uint8_t amp = 64;

    static const uint32_t freqs[] = {262, 294, 330, 349, 392};
    const size_t num_notes = sizeof(freqs) / sizeof(freqs[0]);

    if (!audio) {
        console_puts("No audio driver.");
        return;
    }

    if (duration_ms < 100) {
        duration_ms = 100;
    }

    audio_stop_playback();

    if (audio->start(sample_rate, AUDIO_U8, 1) != 0) {
        console_puts("AUDIO_START failed.");
        return;
    }

    if (audio->set_volume) {
        audio->set_volume(80);
    }

    console_printf("Playing write-based scale test (%d ms) using %s...\n", duration_ms,
                   audio_get_driver_name());

    start_ticks = now_ticks();
    total_ticks = ms_ticks(duration_ms);
    if (total_ticks == 0) {
        total_ticks = 1;
    }

    tick_last = now_ticks();

    for (n = 0; n < num_notes; ++n) {
        uint32_t period = sample_rate / freqs[n];
        uint32_t note_end_ticks;

        if (period < 2) {
            period = 2;
        }

        half_period = period / 2;
        if (half_period == 0) {
            half_period = 1;
        }
        phase = 0;

        note_end_ticks = start_ticks + (uint32_t)(((n + 1u) * total_ticks) / num_notes);
        while ((int32_t)(now_ticks() - note_end_ticks) < 0) {
            uint32_t to_write;

            while (now_ticks() == tick_last) {
                idle_wait_irq_on();
            }
            tick_last = now_ticks();

            sample_acc += sample_rate;
            to_write = sample_acc / TICK_HZ;
            sample_acc %= TICK_HZ;

            while (to_write > 0) {
                uint32_t chunk =
                    (to_write > (uint32_t)sizeof(buf)) ? (uint32_t)sizeof(buf) : to_write;
                uint32_t i;
                uint32_t off = 0;

                for (i = 0; i < chunk; ++i) {
                    uint8_t v = (phase < half_period) ? (uint8_t)(128 + amp) : (uint8_t)(128 - amp);
                    buf[i] = v;
                    phase++;
                    if (phase >= half_period * 2u) {
                        phase = 0;
                    }
                }

                while (off < chunk) {
                    int written = audio->write(buf + off, (size_t)(chunk - off));

                    if (written < 0) {
                        console_puts("AUDIO_WRITE failed.");
                        audio_stop_playback();
                        return;
                    }
                    if (written == 0) {
                        idle_wait_irq_on();
                        continue;
                    }

                    off += (uint32_t)written;
                }

                to_write -= chunk;
            }
        }
    }

    audio_stop_playback();
    console_puts("Done.");
}

/**
 * Play a steady tone while periodically dumping the HDA stream position.
 *
 * Only valid when the active driver is HDA. It plays `freq_hz` and, every
 * `interval_ms`, reads the link position-in-buffer (LPIB) and stream registers
 * via the debug snapshot, printing the LPIB delta since the last sample. The
 * delta wraps modulo the cyclic buffer length (CBL), so a DMA that has looped
 * past the buffer end still reports forward progress. Useful for confirming the
 * stream's DMA engine is actually advancing.
 */
static void audio_run_hda_watch(uint32_t freq_hz, uint32_t duration_ms, uint32_t interval_ms)
{
    uint32_t start_ticks;
    uint32_t end_ticks;
    uint32_t next_ticks;
    uint32_t sample_rate = 48000;
    uint32_t last_lpib = 0;
    uint32_t period;
    hda_stream_debug_snapshot_t snapshot;
    int channels = 2;

    if (!audio) {
        console_puts("No audio driver.");
        return;
    }
    if (audio->type != AUDIO_DRIVER_HDA) {
        console_puts("HDA not active.");
        return;
    }
    if (freq_hz == 0) {
        freq_hz = 440;
    }
    if (interval_ms == 0) {
        interval_ms = 100;
    }

    audio_stop_playback();

    period = sample_rate / freq_hz;
    if (period < 2) {
        period = 2;
    }

    _tone_channels = channels;
    _tone_half_period = period / 2;
    if (_tone_half_period == 0) {
        _tone_half_period = 1;
    }
    _tone_phase = 0;
    _tone_amp = 12000;

    if (audio->set_callback) {
        audio->set_callback(audio_test_callback);
    }

    if (audio->start(sample_rate, AUDIO_S16LE, channels) != 0) {
        console_puts("AUDIO_START failed.");
        audio_stop_playback();
        return;
    }

    if (audio->set_volume) {
        audio->set_volume(80);
    }

    if (!hda_debug_get_stream_snapshot(&snapshot)) {
        console_puts("HDA snapshot unavailable.");
        audio_stop_playback();
        return;
    }

    last_lpib = snapshot.lpib;

    console_printf("HDA watch: %d Hz, %d ms, interval %d ms\n", freq_hz, duration_ms, interval_ms);

    start_ticks = now_ticks();
    end_ticks = start_ticks + ms_ticks(duration_ms);
    next_ticks = start_ticks;

    while ((int32_t)(now_ticks() - end_ticks) < 0) {
        if ((int32_t)(now_ticks() - next_ticks) >= 0) {
            if (hda_debug_get_stream_snapshot(&snapshot)) {
                if (snapshot.cbl != 0) {
                    uint32_t delta = (snapshot.lpib >= last_lpib)
                                         ? (snapshot.lpib - last_lpib)
                                         : (snapshot.lpib + snapshot.cbl - last_lpib);
                    console_printf("t=%dms lpib=%d (+%d) ctl=0x%x sts=0x%x fmt=0x%x\n",
                                   (now_ticks() - start_ticks) * (1000u / TICK_HZ), snapshot.lpib,
                                   delta, snapshot.ctl, (uint32_t)snapshot.sts,
                                   (uint32_t)snapshot.fmt);
                } else {
                    console_printf("t=%dms lpib=%d ctl=0x%x sts=0x%x fmt=0x%x\n",
                                   (now_ticks() - start_ticks) * (1000u / TICK_HZ), snapshot.lpib,
                                   snapshot.ctl, (uint32_t)snapshot.sts, (uint32_t)snapshot.fmt);
                }
                last_lpib = snapshot.lpib;
            }

            next_ticks += ms_ticks(interval_ms);
            if (ms_ticks(interval_ms) == 0) {
                next_ticks += 1;
            }
        }

        idle_wait_irq_on();
    }

    audio_stop_playback();
    console_puts("Done.");
}

void audio_debug_print_help(void)
{
    console_puts("Audio commands:");
    console_puts("  audio info");
    console_puts("  audio test scale [duration_ms]");
    console_puts("  audio test write [duration_ms]");
    console_puts("  audio test watch [freq_hz] [duration_ms] [interval_ms]");
    console_puts("  audio hda dump");
    console_puts("  audio hda codec");
    console_puts("  audio hda pins");
    console_puts("  audio hda stats");
    console_puts("  audio hda out [auto|primary|hp|both]");
}

/** Print the active driver's name and whether it plays samples. */
static void audio_print_summary(void)
{
    console_printf("Audio driver: %s\n", audio_get_driver_name());
    console_printf("Real output:  %s\n", audio_has_real_output() ? "yes" : "no");
    console_puts("Use 'audio help' for commands.");
}

/**
 * The first word must be `audio` and nothing longer, so the command does not
 * take `audioctl` or the like. The line is copied into a 128-byte buffer and
 * split there; a bare `audio` prints the summary, and an unknown form the
 * help.
 */
bool audio_debug_handle_command(const char *cmd)
{
    char buffer[128];
    char *argv[8];
    int argc;
    if (!cmd || !audio_str_starts_with(cmd, "audio")) {
        return false;
    }
    if (cmd[5] != '\0' && cmd[5] != ' ' && cmd[5] != '\t') {
        return false;
    }

    k_strncpy(buffer, cmd, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    argc = audio_tokenize(buffer, argv, 8);

    if (argc <= 1) {
        audio_print_summary();
        return true;
    }

    if (audio_str_equal(argv[1], "help")) {
        audio_debug_print_help();
        return true;
    }

    if (audio_str_equal(argv[1], "info")) {
        audio_print_summary();
        return true;
    }

    if (audio_str_equal(argv[1], "test")) {
        uint32_t duration_ms = 2000;

        if (argc < 3) {
            audio_debug_print_help();
            return true;
        }

        if (audio_str_equal(argv[2], "scale")) {
            if (argc >= 4 && !audio_parse_uint(argv[3], &duration_ms)) {
                console_puts("Usage: audio test scale [duration_ms]");
                return true;
            }
            audio_run_scale_test(duration_ms);
            return true;
        }

        if (audio_str_equal(argv[2], "write")) {
            if (argc >= 4 && !audio_parse_uint(argv[3], &duration_ms)) {
                console_puts("Usage: audio test write [duration_ms]");
                return true;
            }
            audio_run_write_test(duration_ms);
            return true;
        }

        if (audio_str_equal(argv[2], "watch")) {
            uint32_t freq_hz = 440;
            uint32_t interval_ms = 100;

            if (argc >= 4 && !audio_parse_uint(argv[3], &freq_hz)) {
                console_puts("Usage: audio test watch [freq_hz] [duration_ms] [interval_ms]");
                return true;
            }
            if (argc >= 5 && !audio_parse_uint(argv[4], &duration_ms)) {
                console_puts("Usage: audio test watch [freq_hz] [duration_ms] [interval_ms]");
                return true;
            }
            if (argc >= 6 && !audio_parse_uint(argv[5], &interval_ms)) {
                console_puts("Usage: audio test watch [freq_hz] [duration_ms] [interval_ms]");
                return true;
            }

            audio_run_hda_watch(freq_hz, duration_ms, interval_ms);
            return true;
        }

        audio_debug_print_help();
        return true;
    }

    if (audio_str_equal(argv[1], "hda")) {
        if (argc < 3) {
            audio_debug_print_help();
            return true;
        }

        if (audio_str_equal(argv[2], "dump")) {
            hda_debug_dump();
            return true;
        }
        if (audio_str_equal(argv[2], "codec")) {
            hda_debug_dump_codec();
            return true;
        }
        if (audio_str_equal(argv[2], "pins")) {
            hda_debug_dump_pins();
            return true;
        }
        if (audio_str_equal(argv[2], "stats")) {
            hda_debug_dump_stats();
            return true;
        }
        if (audio_str_equal(argv[2], "out")) {
            if (argc < 4) {
                console_printf("HDA out mode=%d\n", hda_get_output_mode());
                console_puts("Usage: audio hda out [auto|primary|hp|both]");
                return true;
            }

            if (audio_str_equal(argv[3], "auto")) {
                hda_set_output_mode(0);
            } else if (audio_str_equal(argv[3], "primary")) {
                hda_set_output_mode(1);
            } else if (audio_str_equal(argv[3], "hp")) {
                hda_set_output_mode(2);
            } else if (audio_str_equal(argv[3], "both")) {
                hda_set_output_mode(3);
            } else {
                console_puts("Usage: audio hda out [auto|primary|hp|both]");
                return true;
            }

            hda_debug_dump_pins();
            return true;
        }

        audio_debug_print_help();
        return true;
    }

    audio_debug_print_help();
    return true;
}
