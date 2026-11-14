"""The dashboard's pages and layout (src/dash_layout.cpp, src/dash_config.cpp,
the link's GetDash / SetDash / ShowPage, ui_dashboard's carousel): what the
app sends, what the device keeps and shows, how the rider moves between
pages, and the config file on the card that is the one copy of it all."""
import os
import re
import time

import pytest


from test_regressions_ui import glass

FIELDS = ["speed", "avgspeed", "power3s", "power", "hr", "cadence", "distance", "ridetime",
          "movingtime", "climb", "grade", "altitude", "battery", "sats", "clock", "routeleft", "radar",
          "mgrs", "latlon", "course"]   # DOBA
CFG = "/config/dashboard.cfg"


def set_dash(link, text):
    r = link.request(set_dash=pb.DashConfig(text=text))
    assert r.WhichOneof("body") == "dash", r
    return r.dash.text


def parse(text):
    """The device's layout text as [(kind, [item lines])], the map strip apart:
    (pages, map_fields). A page's items are their whitespace-normalised lines."""
    pages, cur, strip = [], ("fields", []), None
    for line in text.split("\n"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        tok = line.split()
        if tok[0] == "page":
            if cur[0] != "fields" or cur[1]:
                pages.append(cur)
            cur = (" ".join(tok[1:]) or "fields", [])
        elif tok[0] == "map":
            strip = tok[1:]
        else:
            cur[1].append(" ".join(tok))
    if cur[0] != "fields" or cur[1]:
        pages.append(cur)
    return pages, strip


def wait_pages(device, n):
    return device.wait_status(lambda s: s["page_count"] == n, timeout=5)


# -- what the device keeps ----------------------------------------------------------------


# -- the file on the card -------------------------------------------------------------------


def test_a_first_boot_writes_the_default_layout_out_to_be_edited(device):
    path = os.path.join(device.sd, CFG.lstrip("/"))
    text = open(path).read()
    assert text.startswith("# OpenTrailPaper dashboard layout")
    assert parse(text)[0][0] == ("fields", ["mgrs large", "latlon small", "course medium",   # DOBA
                                             "altitude medium half", "distance medium half",
                                             "ridetime small"])


def test_an_unusable_file_falls_back_to_the_default_and_says_so(device):
    path = os.path.join(device.sd, CFG.lstrip("/"))
    with open(path, "w") as f:
        f.write("this is not a layout\n")
    device.reboot()
    assert parse(device.status()["dash"])[0][0][1][0] == "mgrs large"   # DOBA default
    device.wait_log(r"dash: /config/dashboard.cfg has no usable fields", since=0, timeout=2)


@pytest.mark.xfail(strict=True, reason=(
    "BUG (or a missing feature — dash_config.h says the file is there to be edited over USB and that begin() "
    "should run 'again after a rescan', but nothing calls it): a layout edited in /config/dashboard.cfg while "
    "a computer holds the card is not read when the card is ejected (ui_dashboard.cpp task() rescans the maps "
    "and applies firmware.bin, not the layout); the rider sees the old dashboard until the next reboot."))
def test_a_layout_edited_over_usb_applies_when_the_card_is_ejected(device):
    device.usb_host(True)
    with open(os.path.join(device.sd, CFG.lstrip("/")), "w") as f:
        f.write("speed hero\nclock large\n")
    device.usb_host(False)
    device.wait_status(lambda s: parse(s["dash"])[0][0] == ("fields", ["speed hero", "clock large"]), timeout=5)


# -- moving between pages -------------------------------------------------------------------


