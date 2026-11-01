# Audio fixtures

Five short recordings of one tone, one in each format Rach2's players must decode: `tone.wav` (16-bit PCM), `tone.flac`, `tone.ogg` (Vorbis), `tone.opus` and `tone.mp3`. Each is two seconds of stereo at 48 kHz, a sine of 500 Hz in the left channel and of 750 Hz in the right, at half of full scale, so a check of the decoded sound proves both the tone and that the channels were not swapped or mixed. `SHA256SUMS` holds their digests.

## Use

Check the files, then decode one to PCM and judge it with `wavcheck`, built from `tests/system/checkers/wavcheck.c`:

    sha256sum -c SHA256SUMS
    ffmpeg -i tone.mp3 -c:a pcm_s16le out.wav
    wavcheck scale out.wav 500 / 750

Each file decodes by FFmpeg to 96,000 frames; `wavcheck` passes every one, and fails it with the frequencies swapped.

## Updating

The files were made once, on 2026-11-01, by FFmpeg 8.0.1-3ubuntu2 of Ubuntu 26.04 on x86-64, with its FLAC encoder, libvorbis, libopus and libmp3lame, from `aevalsrc` with metadata dropped and bitexact output:

    src='aevalsrc=0.5*sin(2*PI*500*t)|0.5*sin(2*PI*750*t):s=48000:d=2'
    for spec in 'wav -c:a pcm_s16le' 'flac -c:a flac' 'ogg -c:a libvorbis -q:a 4' \
        'opus -c:a libopus -b:a 96k' 'mp3 -c:a libmp3lame -b:a 128k'; do
        set -- $spec; ext=$1; shift
        ffmpeg -y -f lavfi -i "$src" -map_metadata -1 -fflags +bitexact -flags:a +bitexact "$@" tone.$ext
    done
    sha256sum tone.wav tone.flac tone.ogg tone.opus tone.mp3 >SHA256SUMS

Another FFmpeg or encoder library gives other bytes for the lossy files, so a file made again needs its digest made again with it. Change them only when a check needs another tone.
