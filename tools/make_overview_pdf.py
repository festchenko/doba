#!/usr/bin/env python3
"""docs/Doba.pdf: a printable overview — what Doba is, every screen, the
hardware and where to get it. Screens come from docs/screens (made with
firmware/sim/tools/tour_showcase.py).

    python3 tools/make_overview_pdf.py
"""
import os

from reportlab.lib.colors import HexColor
from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DOCS = os.path.join(ROOT, "docs")
FONTS = "/usr/share/fonts/truetype/dejavu"
pdfmetrics.registerFont(TTFont("Sans", os.path.join(FONTS, "DejaVuSans.ttf")))
pdfmetrics.registerFont(TTFont("Sans-B", os.path.join(FONTS, "DejaVuSans-Bold.ttf")))
pdfmetrics.registerFont(TTFont("Cond", os.path.join(FONTS, "DejaVuSansCondensed.ttf")))

PW, PH = A4
M = 46
INK, GREY, PALE = HexColor("#111111"), HexColor("#666666"), HexColor("#efefef")
c = canvas.Canvas(os.path.join(DOCS, "Doba.pdf"), pagesize=A4)
c.setTitle("Doba — Durable Offline Backcountry Assistant")
c.setAuthor("Doba project")
page_no = [0]


def footer():
    page_no[0] += 1
    c.setFont("Sans", 8)
    c.setFillColor(GREY)
    c.drawString(M, 24, "D.O.B.A · github.com/festchenko/doba")
    c.drawRightString(PW - M, 24, str(page_no[0]))
    c.setFillColor(INK)


def wrap(text, font, size, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if pdfmetrics.stringWidth(t, font, size) <= width:
            cur = t
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def para(x, y, text, width, font="Sans", size=10, lead=14, color=INK):
    c.setFont(font, size)
    c.setFillColor(color)
    for ln in wrap(text, font, size, width):
        c.drawString(x, y, ln)
        y -= lead
    c.setFillColor(INK)
    return y


def device(path, x, y, w):
    """A screen in a dark rounded bezel; (x, y) is the bottom-left."""
    h = w * 960 / 540
    b = w * 0.05
    c.setFillColor(HexColor("#1e1e1e"))
    c.roundRect(x - b, y - b, w + 2 * b, h + 2 * b, b * 1.6, stroke=0, fill=1)
    c.drawImage(path, x, y, w, h)
    c.setFillColor(INK)
    return h + 2 * b


def scr(lang, name):
    return os.path.join(DOCS, "screens", lang, name + ".png")


# --- cover ------------------------------------------------------------------------
c.setFillColor(PALE)
c.rect(0, 0, PW, PH, stroke=0, fill=1)
c.setFillColor(INK)
c.setLineWidth(3)
c.rect(M, PH - 150, 210, 70, stroke=1, fill=0)
c.setFont("Sans-B", 34)
c.drawCentredString(M + 105, PH - 128, "D.O.B.A")
c.setFont("Sans-B", 22)
c.drawString(M, PH - 190, "Durable Offline Backcountry Assistant")
para(M, PH - 218, "An e-ink pocket device that keeps working when the internet, the phone network and "
     "the power grid don't: topographic maps with a military grid, an offline library, "
     "text messaging over LoRa, and a paper card when it is switched off.", PW - 2 * M, size=12, lead=17)
w = 128
xs = [M + 8, M + 8 + (w + 40), M + 8 + 2 * (w + 40)]
for x, n in zip(xs, ["home", "map", "off"]):
    device(scr("en", n), x, 120, w)
c.setFont("Sans", 9)
c.setFillColor(GREY)
c.drawString(M, 70, "Firmware + data pipeline for the LilyGo T5 E-Paper S3 Pro · English / Українська")
c.drawString(M, 56, "github.com/festchenko/doba")
c.setFillColor(INK)
c.showPage()

# --- features ----------------------------------------------------------------------
FEATURES = [
    ("Maps", "Vector topographic tiles drawn for 4-grey e-ink: roads, buildings, forests, water, contour "
             "lines and heights. North-up or course-up, from street level to a whole country. Most of Europe "
             "plus Ukraine, built region by region from OpenStreetMap."),
    ("Navigation", "MGRS position to 1 m, latitude/longitude, magnetic declination, azimuth in mils (6000 or "
                   "6400) and degrees. A measuring cross gives distance and compass azimuth from you or from "
                   "point A, and the ground height or water depth under it."),
    ("Points & places", "Save points from GPS, from the map or typed as MGRS, then 'go here' with distance and "
                        "azimuth. Nearby: hospitals, doctors, pharmacies, water, food shops, farms, shelters, "
                        "bunkers and underground parking, offline."),
    ("Library", "Kiwix archives converted to a light format: Wikipedia (EN, UK) with pictures, WikiMed, iFixit, "
                "Appropedia, Stack Exchange, Wikisource and Gutenberg classics, Wikivoyage, Wiktionary, field "
                "manuals and your own PDF/EPUB. Topic pages and three emergency buttons."),
    ("Reader", "Serif text with Cyrillic hyphenation, pictures and captions, contents, links, text size. Long "
               "books reopen where you stopped, even after a restart."),
    ("Comms", "LoRa SX1262, Meshtastic-compatible channels and direct messages, nodes with distance and "
              "bearing, quick phrases, an EN/UA keyboard. SOS repeats your MGRS position every 5 minutes."),
    ("Stealth", "One switch: radio silent or listen-only, light off, screen dim. Bluetooth and Wi-Fi are off "
                "by default."),
    ("Sun & battery", "Sunrise, sunset, twilight, daylight left, moon phase. Battery: measured current, "
                           "time left at this pace, and where the charge goes."),
    ("Power-off card", "Switched off, the battery is disconnected (ship mode) and the glass keeps: where you "
                       "were, when, the charge, tomorrow's light, an 'if found' card and an emergency crib."),
]
y = PH - 70
c.setFont("Sans-B", 20)
c.drawString(M, y, "What it does")
y -= 34
for title, text in FEATURES:
    c.setFont("Sans-B", 11)
    c.drawString(M, y, title)
    y = para(M + 120, y, text, PW - 2 * M - 120, size=9.5, lead=13) - 10
footer()
c.showPage()

# --- screens ------------------------------------------------------------------------
GALLERY = [
    ("en", "home", "Home"),
    ("en", "map", "Topo map + MGRS"),
    ("en", "position", "Position & points"),
    ("en", "point", "Go to a point"),
    ("en", "nearby", "Places nearby"),
    ("en", "map_options", "Map options"),
    ("en", "library", "The library shelf"),
    ("en", "library_topics", "Library topics"),
    ("en", "book", "A book's own page"),
    ("en", "article_p2", "Reading"),
    ("en", "comms", "Comms (LoRa)"),
    ("en", "sos", "SOS"),
    ("en", "sun", "Sun and moon"),
    ("en", "battery", "Battery"),
    ("en", "settings", "Settings"),
    ("en", "more", "Device"),
    ("en", "off", "The power-off card"),
    ("uk", "home", "Українською"),
    ("uk", "map", "Мапа"),
    ("uk", "library", "Бібліотека"),
    ("uk", "off", "Екран вимкнення"),
]
per_row, w = 4, 100
gap_x = (PW - 2 * M - per_row * w) / (per_row - 1)
row_h = w * 960 / 540 + 48
i = 0
while i < len(GALLERY):
    c.setFont("Sans-B", 20)
    c.drawString(M, PH - 70, "Screens")
    c.setFont("Sans", 9)
    c.setFillColor(GREY)
    c.drawString(M, PH - 86, "Rendered by the firmware itself on the desktop simulator, with a real map of Groenlo (NL).")
    c.setFillColor(INK)
    top = PH - 110
    for r in range(3):
        for k in range(per_row):
            if i >= len(GALLERY):
                break
            lang, name, cap = GALLERY[i]
            x = M + k * (w + gap_x)
            yb = top - (r + 1) * row_h + 34
            device(scr(lang, name), x, yb, w)
            c.setFont("Sans", 8)
            c.drawCentredString(x + w / 2, yb - 20, cap)
            i += 1
    footer()
    c.showPage()

# --- hardware, start, credits ----------------------------------------------------------
y = PH - 70
c.setFont("Sans-B", 20)
c.drawString(M, y, "Hardware & where to get it")
y -= 30
HW = [
    ("LilyGo T5 E-Paper S3 Pro (H752-01)",
     "ESP32-S3, 16 MB flash, 8 MB PSRAM, 4.7-inch 960x540 touch e-ink, GPS, SX1262 LoRa, 1500 mAh battery, "
     "RTC. Choose the version with GPS and the LoRa band of your region (868 MHz in Europe and Ukraine).",
     ["lilygo.cc/products/t5-e-paper-s3-pro", "tindie.com/products/lilygo/lilygo-t5-e-paper-s3-pro",
      "LilyGo's official AliExpress store"]),
    ("microSD card, 256 GB to 1 TB",
     "Formatted FAT32 by the scripts. Any A1/A2 card works: the device reads over SPI, so the speed class "
     "only shortens the copy from the computer.", []),
    ("Optional: a second Meshtastic node",
     "To message with Doba or as a relay, for example a Heltec V3 at 868 MHz.", []),
]
for t, text, links in HW:
    c.setFont("Sans-B", 11)
    c.drawString(M, y, t)
    y = para(M, y - 16, text, PW - 2 * M, size=9.5, lead=13)
    for ln in links:
        c.setFont("Sans", 9.5)
        c.drawString(M + 12, y, "→ " + ln)
        y -= 13
    y -= 12

c.setFont("Sans-B", 20)
c.drawString(M, y - 6, "Getting started")
y -= 36
STEPS = [
    "Flash: python3 -m esptool --chip esp32s3 write-flash 0x0 release/doba-full.bin",
    "Build maps per region from OpenStreetMap: maps/make-region.sh <region>",
    "Build the library from Kiwix archives: library/fetch_zims.sh, then 'convert'",
    "Add your own books: library/docs2doba.py ~/Books .../lib/my_books",
    "Format the card and copy everything: tools/prepare-sd-mac.sh format diskN, then 'all'",
    "Buttons: BOOT short = home, hold = power menu; side IO48 = light; POWER = switch on; RST = restart",
]
for n, s in enumerate(STEPS, 1):
    y = para(M, y, f"{n}. {s}", PW - 2 * M, font="Cond", size=9.5, lead=13) - 4

c.setFont("Sans-B", 20)
c.drawString(M, y - 14, "Credits & support")
y -= 42
y = para(M, y, "Firmware is a fork of OpenTrailPaper by Raemond Bergstrom-Wood (Apache-2.0). Map data © "
         "OpenStreetMap contributors (ODbL). Library content comes from Kiwix archives under their own "
         "licenses and is not part of the repository. Meshtastic is a trademark of Meshtastic LLC; Doba speaks "
         "its open protocol and is not affiliated. Doba is not a certified safety or medical device.",
         PW - 2 * M, size=9.5, lead=13) - 10
y = para(M, y, "If Doba is useful to you, support its development: github.com/sponsors/festchenko",
         PW - 2 * M, font="Sans-B", size=10.5, lead=14)
footer()
c.showPage()
c.save()
print("docs/Doba.pdf")
