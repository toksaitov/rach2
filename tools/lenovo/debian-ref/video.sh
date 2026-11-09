#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# video.sh: Firefox ESR's H.264 and VP9 at 1080p30 with VA-API off, under cage
# fullscreen (kiosk), muted: run2 step 9's two 60 s clips played 58 s each by
# video.html, which posts the playback quality (frames, drops) every 10 s.
# media.ffmpeg.vaapi.enabled is false, and media.hardware-video-decoding.enabled
# too, so that the decoder is software whichever pref this release reads;
# Firefox's PlatformDecoderModule log shows which decoder ran. Output in
# $OUT/video.
set -u
. /root/ref80/lib.sh
need cage firefox-esr ffmpeg python3
O=$OUT/video; P=$S80/prof; WEB=$S80/web
mkdir -p $O $WEB; chown gfx:gfx $O
SECS=58; [ $REHEARSE = 1 ] && SECS=10
# The clips: step 9's when p5 still has them, otherwise made the same way.
for c in h264-1080p30.mp4 vp9-1080p30.webm; do
	if [ -s /srv/run2/step9/web/$c ]; then cp /srv/run2/step9/web/$c $WEB/
	elif [ ! -s $WEB/$c ]; then
		d=60; [ $REHEARSE = 1 ] && d=12
		SRC="testsrc2=size=1920x1080:rate=30:duration=$d,noise=alls=12:allf=t+u"
		case $c in
		*.mp4) ffmpeg -loglevel error -y -f lavfi -i "$SRC" -c:v libx264 -profile:v high -pix_fmt yuv420p \
			-b:v 8M -maxrate 10M -bufsize 16M -g 60 -movflags +faststart $WEB/$c ;;
		*) ffmpeg -loglevel error -y -f lavfi -i "$SRC" -c:v libvpx-vp9 -pix_fmt yuv420p -b:v 6M \
			-deadline realtime -cpu-used 8 -row-mt 1 -g 60 $WEB/$c ;;
		esac
	fi
	echo "$c $(stat -c %s $WEB/$c) bytes $(sha256sum $WEB/$c | cut -c1-16) $(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,width,height,r_frame_rate:format=duration -of csv=p=0 $WEB/$c | tr '\n' ' ')"
done > $O/clips.txt
cp $R80/video.html $WEB/
python3 -I $R80/probe-server.py $WEB $O 8000 > $O/server.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1
CE=$(cage_env)
for c in h264-1080p30.mp4 vp9-1080p30.webm; do
	n=firefox-${c%%-*}-novaapi
	rm -rf "${P:?}/$n" $O/$n.json; mkdir -p $P/$n
	cat > $P/$n/user.js <<J
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.aboutwelcome.enabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("app.update.auto", false);
user_pref("media.ffmpeg.vaapi.enabled", false);
user_pref("media.hardware-video-decoding.enabled", false);
J
	cp $P/$n/user.js $O/$n.user.js
	chown -R gfx:gfx $P/$n
	sampler_start $O/$n.samples
	say "video: start $n" | tee -a $O/runs.log
	# shellcheck disable=SC2086
	runuser -u gfx -- env HOME=/home/gfx XDG_RUNTIME_DIR=$RUN $CE cage -- env MOZ_ENABLE_WAYLAND=1 \
		MOZ_LOG=PlatformDecoderModule:5,Dmabuf:5 MOZ_LOG_FILE=$O/$n.moz \
		firefox-esr --new-instance --no-remote --profile $P/$n --kiosk \
		"http://127.0.0.1:8000/video.html?name=$n&clip=$c&secs=$SECS" > $O/$n.log 2>&1 < /dev/null &
	i=0; while [ ! -s $O/$n.json ] && [ $i -lt $((SECS + 60)) ]; do sleep 1; i=$((i + 1)); done
	say "video: $n result after $i s: $(test -s $O/$n.json && echo yes || echo no)" | tee -a $O/runs.log
	sampler_stop
	pkill -u gfx -f firefox-esr; sleep 3; pkill -u gfx -x cage; sleep 1; pkill -9 -u gfx -f firefox-esr
	rm -rf "${P:?}/$n"
	python3 -I -c '
import json, sys
r = json.load(open(sys.argv[1]))
print("  %s: %s frames, %s dropped, %s corrupted, %sx%s, %.1f s played, samples %s"
      % (r["name"], r.get("total"), r.get("dropped"), r.get("corrupted"), r.get("width"),
         r.get("height"), r.get("currentTime") or 0, [(round(s["t"]), s["dropped"]) for s in r.get("samples", [])]))
' $O/$n.json 2>&1 | tee -a $O/runs.log
	echo "  decoders in the log: $(grep -h -o -E 'FFmpeg(Video|Audio)Decoder|VA-API|VAAPI|libdav1d|FFVPX|ffvpx|Dav1d|libvpx' $O/$n.moz* 2>/dev/null | sort | uniq -c | tr '\n' ' ')" | tee -a $O/runs.log
done
