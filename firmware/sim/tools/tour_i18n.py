#!/usr/bin/env python3
"""Walk every Doba screen in one language, save a screenshot of each and
(Ukrainian) list the strings drawn with English still in them.

    python3 sim/tools/tour_i18n.py uk OUT_DIR [BOOK_DIR]
"""
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tests"))
sys.path.insert(0, os.path.join(HERE, "..", "client"))
os.environ.setdefault("DOBA_START", "home")
os.environ.setdefault("DOBA_MESH_REGION", "EU_868")
from otpsim import SimDevice  # noqa: E402

lang, out = sys.argv[1], sys.argv[2]
book = sys.argv[3] if len(sys.argv) > 3 else None
os.makedirs(out, exist_ok=True)
TOP, W, H = 64, 540, 960

dev = SimDevice(args=["--gps-baud", "115200", "--lora"])
d = os.path.join(dev.sd, "doba")
os.makedirs(os.path.join(d, "lib"), exist_ok=True)
open(os.path.join(d, "points.txt"), "w").write("CAMP;52.0610;6.6440\nBRIDGE;52.0300;6.5900\nWELL;52.0500;6.6000\n")
if book:
    shutil.copytree(book, os.path.join(d, "lib", "field"))
try:
    from test_doba import write_pois
    os.makedirs(os.path.join(d, "map"), exist_ok=True)
    write_pois(os.path.join(d, "map", "pois.dgp"), [(52.045, 6.62, 1, "Ziekenhuis"), (52.04, 6.61, 4, ""),
                                                    (52.05, 6.63, 5, "Spar"), (52.03, 6.60, 8, "")])
except Exception as e:
    print("no pois:", e)
dev.launch(30)
dev.wait_status(lambda s: s["paints"] > 0)
dev.console("lang " + lang, 500)
dev.gps(52.0421, 6.6170)
time.sleep(2)
n = [0]


def shot(name, wait=1.2):
    time.sleep(wait)
    n[0] += 1
    dev.screenshot(os.path.join(out, f"{n[0]:02d}_{name}.png"))


def screen(s, wait=0.8):
    dev.console("screen " + s, 800)
    time.sleep(wait)


screen("home"); shot("home")
screen("points"); shot("position")
dev.tap(270, TOP + 436 + 40); shot("point")
screen("points"); dev.tap(410, TOP + 306 + 30); shot("point_new")
screen("points"); dev.tap(270, TOP + 436 + 4 * 80 + 30); shot("points_more")
screen("mapopts"); shot("map_options")
dev.tap(270, TOP + 20 + 360 + 30 + 32); shot("legend")
screen("setup"); shot("settings")
dev.tap(270, TOP + 4 + 34 + 3 * 78 + 39); shot("callsign_keyboard")
screen("more"); shot("more")
screen("sun"); shot("sun")
screen("sos"); shot("sos")
screen("comms"); shot("comms_off")
dev.tap(270, TOP + 260 + 32); time.sleep(2); shot("comms_chats")
dev.tap(300, TOP + 96 + 23); shot("comms_nodes")
dev.tap(120, TOP + 96 + 23); time.sleep(0.5)
dev.tap(270, TOP + 150 + 48); shot("chat")
dev.tap(92 + 85, H - 60); shot("quick")
screen("comms"); dev.tap(270, TOP + 150 + 48); time.sleep(0.8); dev.tap(274 + 87, H - 60); shot("compose")
screen("library"); shot("library_books")
dev.tap(150, TOP + 96 - 8); shot("library_topics")
dev.tap(250, TOP + 96 - 8); shot("library_recent")
dev.tap(40, TOP + 96 - 8); time.sleep(0.6)
dev.tap(24 + 75, TOP + 204 + 75); shot("book")
dev.tap(270, TOP + 104 + 28); shot("search")
screen("library"); time.sleep(0.4)
if book:
    dev.console("libopen Knots", 1500); shot("article")
    dev.tap(W - 47, TOP + 27); shot("article_size")
screen("home"); dev.tap(400, 700); shot("power_sheet")
screen("map"); shot("map", 2.5)
dev.console("screen topo", 500)
time.sleep(1.5)
shot("map_topo", 1.5)
lines = [l for l in dev.log() if "[i18n]" in l]
open(os.path.join(out, "i18n.txt"), "w").write("\n".join(sorted(set(re.sub(r"^.*?\[i18n\]", "[i18n]", l) for l in lines))))
dev.stop()
print(n[0], "screens,", len(lines), "i18n lines")
