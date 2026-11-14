#!/usr/bin/env python3
"""Screens for the README: the simulator on a small real card (a map around
Groenlo, heights, places, a few books), one language per run.

    python3 sim/tools/tour_showcase.py en OUT_DIR CARD_DIR
"""
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tests"))
sys.path.insert(0, os.path.join(HERE, "..", "client"))
os.environ.setdefault("DOBA_START", "home")
os.environ.setdefault("DOBA_MESH_REGION", "EU_868")
from otpsim import SimDevice  # noqa: E402

lang, out, card = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
TOP, W, H = 64, 540, 960
dev = SimDevice(args=["--gps-baud", "115200", "--lora"])
shutil.copytree(os.path.join(card, "doba"), os.path.join(dev.sd, "doba"), dirs_exist_ok=True)
open(os.path.join(dev.sd, "doba", "points.txt"), "w").write(
    "CAMP;52.0610;6.6440\nBRIDGE;52.0300;6.5900\nWELL;52.0500;6.6000\n")
dev.launch(30)
dev.wait_status(lambda s: s["paints"] > 0)
dev.console("lang " + lang, 500)
dev.gps(52.0421, 6.6170)
time.sleep(3)


def shot(name, wait=1.5):
    time.sleep(wait)
    dev.screenshot(os.path.join(out, f"{name}.png"))


def screen(s, wait=1.0):
    dev.console("screen " + s, 800)
    time.sleep(wait)


screen("home"); shot("home")
screen("map"); shot("map", 4)
dev.console("zoom in", 500); time.sleep(3); shot("map_close", 3)
screen("points"); shot("position")
dev.tap(270, TOP + 436 + 40); shot("point")
screen("points"); dev.tap(440, TOP + 436 - 24); shot("nearby")
screen("mapopts"); shot("map_options")
screen("library"); shot("library")
dev.tap(150, TOP + 96 - 8); shot("library_topics")
dev.tap(40, TOP + 96 - 8); time.sleep(0.6)
dev.tap(24 + 75, TOP + 204 + 75); shot("book")
dev.console("libopen First Aid", 1500); shot("article", 2.5)
dev.tap(450, 600); shot("article_p2", 2)
screen("comms"); shot("comms")
screen("sun"); shot("sun")
screen("sos"); shot("sos")
screen("setup"); shot("settings")
screen("more"); shot("more")
screen("battery"); shot("battery")
dev.console("owner Name Surname|01.01.1990|A(II)+|no allergies|+00 000 000 000" if lang == "en"
            else "owner Ім'я Прізвище|01.01.1990|A(II)+|алергій немає|+00 000 000 000", 500)
dev.console("farewell", 800); shot("off", 4)
dev.stop()
