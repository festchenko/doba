#!/usr/bin/env python3
"""Checks src/milgeo.cpp against the `mgrs` and `pyproj` libraries."""
import os, subprocess, sys, random
import mgrs as M
from pyproj import Proj
HERE = os.path.dirname(os.path.abspath(__file__))
exe = os.path.join(HERE, "milgeo_test")
subprocess.check_call(["c++", "-std=c++17", "-O1", "-o", exe,
                       os.path.join(HERE, "main.cpp"), os.path.join(HERE, "../../src/milgeo.cpp")])
random.seed(1)
pts = [(52.042, 6.617), (50.4501, 30.5234), (48.3794, 31.1656), (60.39, 5.32),
       (78.22, 15.65), (36.14, -5.35), (45.0, 0.0), (64.1, -21.9), (-33.9, 18.4)]
pts += [(random.uniform(35, 71), random.uniform(-10, 40)) for _ in range(500)]
inp = "".join(f"{a} {b}\n" for a, b in pts)
out = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True).stdout.splitlines()
m = M.MGRS()
bad = 0
worst_rt = 0
for (lat, lon), line in zip(pts, out):
    ours, zone, e, n, la, lo = line.split("|")
    ref = m.toMGRS(lat, lon, MGRSPrecision=5)
    ref_fmt = f"{ref[:-15]} {ref[-15:-13]} {ref[-10:-5]} {ref[-5:]}" if False else None
    # reference "32ULC1234567890" -> compare without spaces, allow 1 m rounding
    o = ours.replace(" ", "")
    if o[:-10] != ref[:-10]:
        bad += 1; print("square differs", lat, lon, ours, ref)
    else:
        de = abs(int(o[-10:-5]) - int(ref[-10:-5])); dn = abs(int(o[-5:]) - int(ref[-5:]))
        if de > 1 or dn > 1:
            bad += 1; print("digits differ", lat, lon, ours, ref)
    p = Proj(proj="utm", zone=int(zone), ellps="WGS84", south=lat < 0)
    E, N = p(lon, lat)
    if lat < 0: N += 0
    if abs(E - float(e)) > 0.01 or abs((N if lat >= 0 else N) - float(n)) > 0.01:
        bad += 1; print("utm differs", lat, lon, e, n, E, N)
    worst_rt = max(worst_rt, abs(float(la) - lat), abs(float(lo) - lon))
print("mils:", " | ".join(out[len(pts):]))
print(f"{len(pts)} points, {bad} mismatches, worst round trip {worst_rt * 111000:.4f} m")
sys.exit(1 if bad or worst_rt * 111000 > 0.5 else 0)
