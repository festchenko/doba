#!/usr/bin/env python3
"""A tiny ZIM (English + Russian + Ukrainian articles, a redirect, an infobox
and a references section to be stripped) for testing zim2doba.py offline."""
import os
import sys

from libzim.writer import Creator, Hint, Item, StringProvider


class Page(Item):
    def __init__(self, path, title, html):
        super().__init__()
        self._p, self._t, self._h = path, title, html

    def get_path(self): return self._p
    def get_title(self): return self._t
    def get_mimetype(self): return "text/html"
    def get_contentprovider(self): return StringProvider(self._h)
    def get_hints(self): return {Hint.FRONT_ARTICLE: True}


class Picture(Item):
    def __init__(self, path, data):
        super().__init__()
        self._p, self._d = path, data

    def get_path(self): return self._p
    def get_title(self): return ""
    def get_mimetype(self): return "image/png"
    def get_contentprovider(self): return StringProvider(self._d)
    def get_hints(self): return {Hint.FRONT_ARTICLE: False}


def bowline_png():
    import io
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (300, 200), "white")
    d = ImageDraw.Draw(img)
    d.ellipse((60, 30, 240, 170), outline="black", width=8)
    d.line((20, 100, 280, 100), fill="black", width=8)
    b = io.BytesIO()
    img.save(b, "PNG")
    return b.getvalue()


LONG = " ".join(["Water must be made safe before drinking: boil it for at least one minute,"
                 " or filter and disinfect it."] * 12)
PAGES = [
    ("Water_purification", "Water purification", f"""<html><body><h1>Water purification</h1>
<table class="infobox"><tr><td>INFOBOX JUNK</td></tr></table>
<p><b>Water purification</b> is the process of removing contaminants; see also <a href="./Knots">knots</a> and <a href="Tourniquet">a tourniquet</a>.<sup>[1]</sup></p>
<h2>Methods</h2><p>{LONG}</p>
<ul><li>Boiling kills pathogens.</li><li>Chlorine tablets work in 30 minutes.</li></ul>
<h3>Improvised filters</h3><p>Sand, charcoal and gravel in layers.</p>
<h2>References</h2><ol><li>SHOULD NOT APPEAR</li></ol></body></html>"""),
    ("Tourniquet", "Tourniquet", """<html><body><p>A <a href="x">tourniquet</a> stops
severe bleeding from a limb.</p><h2>Use</h2><p>Apply 5–7 cm above the wound.</p></body></html>"""),
    ("Київ", "Київ", """<html><body><p><b>Київ</b> — столиця України, місто на Дніпрі.</p>
<h2>Історія</h2><p>Одне з найдавніших міст Східної Європи.</p></body></html>"""),
    ("Костёр", "Костёр", """<html><body><p><b>Костёр</b> — контролируемый огонь на открытом воздухе.
Для розжига нужна растопка: береста, сухие ветки.</p><h2>Виды</h2>
<ul><li>Шалаш</li><li>Звёздный</li><li>Нодья — горит всю ночь.</li></ul>
<h2>Примечания</h2><p>НЕ ДОЛЖНО ПОЯВИТЬСЯ</p></body></html>"""),
    # Wikisource-style chapters: long Cyrillic paths that share their first 55
    # bytes once folded; each must stay its own page, and links reach the right one.
    ("Твори_(Франко,_1956–1962)/13/Захар_Беркут", "Твори (Франко, 1956–1962)/13/Захар Беркут",
     """<html><body><p>Повість Івана Франка про громаду Тухольщини. Розділи: <a href="./Захар_Беркут/I">перший</a>,
<a href="./Захар_Беркут/II">другий</a>.</p></body></html>"""),
    ("Твори_(Франко,_1956–1962)/13/Захар_Беркут/I", "Твори (Франко, 1956–1962)/13/Захар Беркут/I",
     # long enough to be a book: several pages, for "opens where reading stopped"
     "<html><body><p>Розділ перший. Тухольщина в горах, громада вирішує, як зустріти ворога і що робити далі.</p>"
     + "".join("<p>Абзац %d. Над Тухлею стояли гори, вкриті смерековим лісом, і громада сходилася на раду, "
               "щоб разом вирішити, як жити далі й берегти свою землю від чужої сили.</p>" % i for i in range(1, 61))
     + "</body></html>"),
    ("Твори_(Франко,_1956–1962)/13/Захар_Беркут/II", "Твори (Франко, 1956–1962)/13/Захар Беркут/II",
     """<html><body><p>Розділ другий. Монголи йдуть через гори, і Захар Беркут збирає людей на раду біля скелі.</p></body></html>"""),
    ("Knots", "Knots", """<html class="vector-feature-toc-pinned-clientpref-0"><body class="skin-vector mediawiki"><p>A knot is a method of fastening cordage; the bowline makes a fixed loop
that will not slip under load and is easy to untie afterwards.</p>
<figure><img src="_assets_/bowline.png" width="300" height="200"><figcaption>The bowline, tied.</figcaption></figure>
<p>An icon <img src="_assets_/bowline.png" width="16" height="16"> stays out.</p></body></html>"""),
]

if __name__ == "__main__":
    out = sys.argv[1]
    if os.path.exists(out):
        os.remove(out)
    with Creator(out).config_indexing(False, "eng") as c:
        c.set_mainpath("Water_purification")
        for p in PAGES:
            c.add_item(Page(*p))
        try:
            c.add_item(Picture("_assets_/bowline.png", bowline_png()))
        except ImportError:
            pass
        c.add_redirection("Kyiv", "Kyiv", "Київ", {Hint.FRONT_ARTICLE: True})
        c.add_redirection("Boiling_water", "Boiling water", "Water_purification", {})
        c.add_metadata("Title", "Doba test book")
        c.add_metadata("Language", "eng")
    print("wrote", out)
