/* toneprobe: play two seconds of a 440 Hz tone through Wine's waveOut. */
#include "probe.h"
#include <math.h>
#include <mmsystem.h>

/* The tone: 16-bit mono at 48000 Hz, the rate the loopback reader takes. */
#define RATE    48000
#define SECONDS 2
#define HZ      440.0
#define LEVEL   13000.0

/**
 * Play the tone on the wave mapper, waveOut's default device, which Wine's
 * winepulse.drv gives PulseAudio's default sink, and wait until the device
 * returns the buffer. Prints the device's name and "TONE OK". Returns 0, or 1
 * when a waveOut call fails.
 */
int main(void)
{
    static short samples[RATE * SECONDS];
    WAVEFORMATEX fmt;
    WAVEOUTCAPSA caps;
    WAVEHDR hdr;
    HWAVEOUT out;
    MMRESULT rc;
    int i;

    probe_output();
    for (i = 0; i < RATE * SECONDS; i++)
        samples[i] = (short)(LEVEL * sin(2.0 * 3.14159265358979 * HZ * i / RATE));
    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = RATE * 2;
    if (waveOutGetDevCapsA(WAVE_MAPPER, &caps, sizeof caps) == MMSYSERR_NOERROR)
        printf("tone: device %s, %u devices\n", caps.szPname, waveOutGetNumDevs());
    rc = waveOutOpen(&out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (rc != MMSYSERR_NOERROR) {
        printf("tone: waveOutOpen: %u\n", rc);
        return 1;
    }
    memset(&hdr, 0, sizeof hdr);
    hdr.lpData = (LPSTR)samples;
    hdr.dwBufferLength = sizeof samples;
    if (waveOutPrepareHeader(out, &hdr, sizeof hdr) != MMSYSERR_NOERROR ||
        waveOutWrite(out, &hdr, sizeof hdr) != MMSYSERR_NOERROR) {
        printf("tone: waveOutWrite failed\n");
        return 1;
    }
    while (!(hdr.dwFlags & WHDR_DONE))
        Sleep(10);
    waveOutUnprepareHeader(out, &hdr, sizeof hdr);
    waveOutClose(out);
    printf("tone: played %d frames at %d Hz\n", RATE * SECONDS, RATE);
    puts("TONE OK");
    return 0;
}
