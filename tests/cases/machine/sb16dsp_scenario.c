/*
 * sb16dsp_scenario.c: /dev/dsp on an SB16, in signed stereo and at OSS's
 * defaults, judged by the program's exit and both channels QEMU records.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/** End the machine's recording and judge the scale in each output channel. */
static void judge(struct lc *l, struct etude_case_machine *m, int defaults)
{
    char name[] = "wavcheck", scale[] = "scale", slash[] = "/", path[1024], detail[160];
    char stereo[][4] = {"262", "294", "330", "349", "392", "523", "587", "659", "698", "784"};
    char mono[][4] = {"80", "100", "125", "160", "200"};
    char *argv[15] = {name, scale, path};
    struct etude_termination end;
    const char *wav;
    enum etude_status st;
    int i;

    if (!lc_quit(l, m, NULL) || !lc_step(l, "judge"))
        return;
    wav = etude_audio(m);
    if (!wav || strlen(wav) >= sizeof(path)) {
        lc_halt(l, ETUDE_REFUSED, "the machine's recording");
        return;
    }
    snprintf(path, sizeof(path), "%s", wav);
    for (i = 0; i < 5; i++) {
        argv[3 + i] = defaults ? mono[i] : stereo[i];
        argv[9 + i] = defaults ? mono[i] : stereo[5 + i];
    }
    argv[8] = slash;
    argv[14] = NULL;
    etude_progress(l->c, "wavcheck", 0);
    st = etude_host(l->c, argv, "wavcheck", &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "wavcheck");
        return;
    }
    etude_progress(l->c, "wavcheck", 1);
    snprintf(detail, sizeof(detail), "wavcheck ended %d with status %d; its evidence says why",
             (int)end.end, end.value);
    lc_assert(l, "notes", end.end == ETUDE_END_EXITED && end.value == 0, detail);
}

/**
 * Play a scale through /dev/dsp and require the granted format and a drain.
 * The defaults case sends no parameter changes; the other asks for stereo
 * signed 16-bit samples at the SB16's highest rate, 44100 Hz.
 */
int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"played", "grant", "drained", "notes", NULL};
    int defaults = strstr(etude_case_id(c), "defaults") != NULL;
    struct etude_case_machine *m;
    struct lc l;
    uint64_t at = 0;
    char *window;

    lc_begin(&l, c, ids);
    m = lc_shell(&l, "play", "fixture", &at);
    if (!m)
        return lc_finish(&l);
    window = lc_run(&l, m, "fixture",
                    defaults ? "exec /bin/audiotest -d\r" : "exec /bin/audiotest -d 44100 16 2\r",
                    &at, "played");
    if (window) {
        const char *grant = defaults ? "audiotest: dsp: granted 8000 hz, 8-bit, 1 channels"
                                     : "audiotest: dsp: granted 44100 hz, 16-bit, 2 channels";

        lc_assert(&l, "grant", strstr(window, grant) != NULL,
                  "the granted rate, format and channels");
        lc_assert(&l, "drained",
                  strstr(window,
                         "audiotest: result: complete; the scale played through /dev/dsp") != NULL,
                  "audiotest completed SNDCTL_DSP_SYNC");
        free(window);
    }
    if (lc_ok(&l))
        judge(&l, m, defaults);
    return lc_finish(&l);
}
