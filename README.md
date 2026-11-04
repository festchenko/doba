# Доба

Автономное офлайн-устройство на e-ink: топокарты Европы и Украины с GPS, библиотека (Википедия, медицина, бушкрафт, химия, ремёсла), связь LoRa без сети.

Железо: **LilyGo T5 E-Paper S3 Pro** (ESP32-S3, 4,7" e-ink 960×540, 16 оттенков серого, тач GT911, GPS MIA-M10Q, LoRa SX1262 868 МГц) + microSD в FAT32.

```
maps/       конвейер карт: OSM + рельеф → тайлы 4 бита для e-ink
firmware/   прошивка: форк OpenTrailPaper (Apache 2.0) + слой топокарты
release/    готовые образы прошивки
tools/      прошивка и подготовка SD-карты с Mac
library/    библиотека: ZIM → лёгкий формат для ESP32 (дальше)
```

## День прошивки (когда приедет устройство)

1. **Прошить.** Подключить по USB-C и выполнить:
   ```bash
   tools/flash-mac.sh
   ```
   Скрипт сначала сохраняет заводскую прошивку в `backup/` (≈4 мин), потом стирает флеш и пишет `release/doba-full.bin`. Если порт не находится: зажать BOOT, нажать и отпустить RST, отпустить BOOT.
2. **Карта.** Отформатировать microSD в FAT32 и скопировать тайлы:
   ```bash
   tools/prepare-sd-mac.sh list
   tools/prepare-sd-mac.sh format disk4        # номер диска из list!
   DOBA_BUILD=/Volumes/SSD/doba maps/make-region.sh groenlo
   tools/prepare-sd-mac.sh copy /Volumes/SSD/doba/tiles/groenlo
   ```
3. **Проверить.** Вставить карту, включить, выйти на улицу за GPS, кнопка Home → карта. Проверить GPS, сон, LoRa (Meshtastic) — это работает из OpenTrailPaper без изменений.
4. **Калибровка серого.** По USB в консоли (`screen /dev/cu.usbmodem* 115200` или Serial Monitor) ввести `greytest` — на экране 16 полос. Сфотографировать: по фото подстроим стиль карты под реальную панель.

Что делает прошивка сейчас — в `firmware/DOBA.md`.

## Карты: как это работает

1. **Planetiler** превращает OpenStreetMap в векторные тайлы (схема OpenMapTiles, до z14).
2. **Copernicus GLO-30** (рельеф 30 м) даёт горизонтали и отмывку.
3. **`render.py`** рисует растровые тайлы 256×256 сразу в 16 уровнях серого под экран, метатайлами 8×8 (без обрезанных подписей и швов на стыках).
4. Тайлы пакуются в файлы **`.dtp`**, по одному на клетку z8, каждый намного меньше лимита FAT32 в 4 ГБ. Формат описан в начале `maps/tilepack.py`; прошивка читает его так же: заголовок → двоичный поиск по индексу → один zlib-блок.
5. **`preview.py`** показывает, что именно будет на экране 960×540.

Стиль (уровни серого, толщины линий, шрифты, интервалы горизонталей) лежит в `maps/style.py`. Цветов нет: каждое значение — уровень 0–15, который экран покажет как есть.

## Запуск на Mac (тестовый регион Гронло)

Один раз:

```bash
brew install openjdk@21 python
cd maps
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

Данные и рендер (`DOBA_BUILD` — папка на внешнем SSD):

```bash
export DOBA_BUILD=/Volumes/SSD/doba
./fetch.sh groenlo
python3 render.py --mbtiles $DOBA_BUILD/groenlo.mbtiles --dem $DOBA_BUILD/dem \
    --bbox 6.50,51.97,6.75,52.12 --zoom 8-16 --out $DOBA_BUILD/tiles/groenlo
python3 preview.py $DOBA_BUILD/tiles/groenlo --at 52.042,6.617 --zoom 10,12,14,16 \
    --out $DOBA_BUILD/preview
```

При первом запуске `fetch.sh` дополнительно качает ~2 ГБ источников Planetiler (береговая линия, озёра), это один раз.

## Проверка без интернета

```bash
cd maps
python3 test/test_tilepack.py          # формат .dtp
python3 test/make_synthetic.py         # выдуманные данные вокруг Гронло
python3 render.py --mbtiles test/build/synthetic.mbtiles --dem test/build/dem \
    --bbox 6.50,51.97,6.75,52.12 --zoom 8-16 --out test/build/tiles
python3 preview.py test/build/tiles --at 52.042,6.617 --out test/build/preview
```

## Объёмы (оценка, Европа + Украина)

| До зума | Объём |
|---|---|
| z13 | ~7 ГБ |
| z14 | ~33 ГБ |
| z15 | ~140 ГБ |
| z16 | ~560 ГБ |

Вся Европа рендерится на арендованном сервере (`regions/europe.env`), не на ноутбуке.

## Известные ограничения

- Copernicus — модель **поверхности** (с лесом и домами), поэтому на равнинах горизонтали сглаживаются, а мелкие замкнутые кольца отбрасываются. Если на реальных данных будет шумно, увеличить `DEM_SMOOTH_M` в `style.py` или перейти на FABDEM (рельеф без леса).
- Названия улиц вдоль линий пока не рисуются.
- Шрифт DejaVu Sans Condensed (латиница, кириллица, греческий), лицензия в `maps/fonts/`.
