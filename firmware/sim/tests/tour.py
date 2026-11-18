"""Not a test: walks every page in both languages and saves screenshots (QA)."""
import os, sys, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(__file__))
os.environ["DOBA_START"] = "home"
os.environ["DOBA_MESH_REGION"] = ""
from otpsim.device import SimDevice
from test_doba import LIB, TOP, write_pois
from test_topo import LAT, LON, Z, tile_xy, write_packs
from test_map_canvas import pattern

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/tour"
os.makedirs(OUT, exist_ok=True)
tmp = "/tmp/tourbook"
if not os.path.exists(tmp + "/field/index.dti"):
    os.makedirs(tmp, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(LIB, "test", "make_test_zim.py"), tmp + "/test.zim"], check=True)
    subprocess.run([sys.executable, os.path.join(LIB, "zim2doba.py"), tmp + "/test.zim", tmp + "/field", "--name", "Field",
                    "--source", "Test book"], check=True, capture_output=True)
dev = SimDevice(args=["--gps-baud", "115200", "--lora"])
sd = dev.sd
lib = os.path.join(sd, "doba", "lib")
os.makedirs(lib, exist_ok=True)
shutil.copytree(tmp + "/field", os.path.join(lib, "field"), dirs_exist_ok=True)
with open(os.path.join(lib, "sections.txt"), "w") as f:
    f.write("X|KNOTS|knots|field|Knots\nS|k|KNOTS|knots|field\n")
fx, fy = tile_xy(LAT, LON, Z)
tx, ty = int(fx), int(fy)
write_packs(sd, {(Z, tx + dx, ty + dy): pattern(tx + dx, ty + dy) for dy in range(-3, 4) for dx in range(-2, 3)})
write_pois(os.path.join(sd, "doba", "map", "pois.dgp"), [(LAT + 0.001, LON + 0.001, 1, "Hospital"), (LAT - 0.001, LON, 5, "Shop")])
with open(os.path.join(sd, "doba", "points.txt"), "w") as f:
    f.write("CAR;%.5f;%.5f\nCAMP;%.5f;%.5f\n" % (LAT + 0.01, LON + 0.01, LAT - 0.02, LON + 0.005))
dev.launch()
time.sleep(2)
dev.gps(LAT, LON)
dev.battery(64)
time.sleep(2)
dev.console("mesh on", 800)
time.sleep(1)
for lang in ("en", "uk"):
    dev.console("lang " + lang, 600)
    for name in ["home", "map", "mapopts", "points", "library", "comms", "compose", "sos", "sun", "setup", "more", "battery", "gps"]:
        dev.console("screen " + name, 1200)
        time.sleep(1.2)
        dev.screenshot(os.path.join(OUT, "%s-%s.png" % (lang, name)))
    # a few taps deeper: points list item, library topics, an article
    dev.console("screen library", 800); time.sleep(0.8)
    dev.tap(150, TOP + 96); time.sleep(0.8)
    dev.screenshot(os.path.join(OUT, "%s-library-topics.png" % lang))
    dev.tap(96, TOP + 194 + 28 + 54); time.sleep(1.2)
    dev.screenshot(os.path.join(OUT, "%s-article.png" % lang))
    dev.console("screen comms", 800); time.sleep(0.8)
    dev.tap(150, 260); time.sleep(0.8)
    dev.screenshot(os.path.join(OUT, "%s-chat.png" % lang))
    dev.tap(177, 908); time.sleep(0.8)
    dev.screenshot(os.path.join(OUT, "%s-quick.png" % lang))
    dev.console("screen home", 800); time.sleep(0.5)
    dev.tap(270, 900); time.sleep(0.8)   # stealth bar
    dev.screenshot(os.path.join(OUT, "%s-stealth.png" % lang))
    dev.tap(270, 900, ms=5200); time.sleep(0.8)
print("\n".join(l for l in dev.log(0) if "lib]" in l or "error" in l.lower() or "fail" in l.lower())[-3000:])
dev.stop()
