"""frames.py RESULT.json: a play's frame timing from video-timing.html: the
gap between consecutive frames' expected display times, its histogram in
whole ms, the decoder's time and the jumps in media time where frames were
dropped."""
import json, sys, statistics as st
r = json.load(open(sys.argv[1])); fr = r['frames']
print(r['name'], r['total'], r['dropped'], len(fr))
d = [fr[i][3]-fr[i-1][3] for i in range(1, len(fr)) if fr[i][1]-fr[i-1][1] < 0.04]
print("expected-display interval between consecutive frames: median %.2f mean %.2f" % (st.median(d), st.mean(d)))
import collections
h = collections.Counter(round(x) for x in d); print(sorted(h.items())[:40])
pr = [f[4]*1000 for f in fr if f[4]]; print("decode ms median %.1f p90 %.1f" % (st.median(pr), sorted(pr)[int(len(pr)*.9)]))
jumps = [(i, round((fr[i][1]-fr[i-1][1])*1000)) for i in range(1, len(fr)) if fr[i][1]-fr[i-1][1] > 0.04]
print("media jumps", len(jumps), jumps[:15])
