"""E-ink map style for Doba.

Levels are 0 (black) .. 15 (white), but the panel driver (EPD_Painter, fast
mode) shows FOUR greys: 0-2 black, 3-7 dark, 8-12 light, 13-15 white. So the
style uses only BLACK, DARK, LIGHT and WHITE below, and tells areas apart by
pattern (dots, hatch, dashes) rather than by tone, like a military sheet
printed in one colour. Widths and font sizes are in screen pixels at 1x.

Data schema: OpenMapTiles (what Planetiler produces by default).
"""

WHITE = 15
LIGHT = 10
DARK = 5
BLACK = 0
PANEL_LEVELS = (BLACK, DARK, LIGHT, WHITE)   # what the glass can show

BACKGROUND = WHITE

# --- area fills -----------------------------------------------------------
# (layer, class) -> (min_zoom, level). Solid tones only where nothing else
# would read; textured areas are in AREA_PATTERNS.
AREA_FILLS = [
    ("landcover", "farmland", 0, WHITE),
    ("landcover", "grass", 0, WHITE),
    ("landcover", "ice", 0, WHITE),
    ("landuse", "quarry", 12, LIGHT),
]

# (layer, class) -> (min_zoom, pattern, level). Patterns (render.py):
#   dots  — 2x2 dots on a staggered grid (forest)
#   hatch — 45 degree lines (built-up areas)
#   dash  — short horizontal dashes (wetland)
#   grain — sparse fine dots (sand, rock)
AREA_PATTERNS = [
    ("landcover", "wood", 7, "dots", DARK),
    ("landcover", "wetland", 10, "dash", DARK),
    ("landcover", "sand", 10, "grain", DARK),
    ("landcover", "rock", 10, "grain", BLACK),
    ("landuse", "residential", 10, "hatch", LIGHT),
    ("landuse", "suburb", 10, "hatch", LIGHT),
    ("landuse", "neighbourhood", 10, "hatch", LIGHT),
    ("landuse", "commercial", 12, "hatch", LIGHT),
    ("landuse", "retail", 12, "hatch", LIGHT),
    ("landuse", "industrial", 12, "hatch", DARK),
    ("landuse", "railway", 13, "hatch", LIGHT),
    ("landuse", "cemetery", 13, "grain", DARK),
    ("landuse", "military", 11, "hatch", DARK),
]
PATTERN_SPACING = {"dots": 7, "hatch": 6, "dash": 6, "grain": 5}
# Outline drawn round patterned areas, so the edge reads even where the
# pattern is sparse (forest edge, town edge). None to skip.
PATTERN_EDGE = {"dots": DARK, "hatch": None, "dash": None, "grain": None}

WATER_FILL = LIGHT
WATER_EDGE = DARK
WATER_MIN_ZOOM = 0

BUILDING_FILL = DARK
BUILDING_EDGE = BLACK
BUILDING_MIN_ZOOM = 14

# --- relief ----------------------------------------------------------------
HILLSHADE = {  # strength = max darkening fraction on steep shadowed slopes
    "min_zoom": 6,
    # Off: four panel greys turn a smooth shade into blotches. Contours carry
    # the relief instead, as on a military sheet.
    "strength_low": 0.0,    # z <= 12
    "strength_high": 0.0,   # z >= 13
}

# zoom -> (interval m, index interval m)
CONTOUR_INTERVALS = {11: (100, 500), 12: (50, 250), 13: (20, 100),
                     14: (10, 50), 15: (10, 50), 16: (10, 50)}
CONTOUR = {"level": LIGHT, "width": 1.0}
CONTOUR_INDEX = {"level": DARK, "width": 1.4}
DEM_SMOOTH_M = 45  # smoothing radius; Copernicus is a surface model (trees!)
CONTOUR_MIN_LOOP_PX = 60  # drop closed contour rings shorter than this

# --- linear water ----------------------------------------------------------
# class -> (min_zoom, width)
WATERWAYS = {
    "river": (8, 2.2),
    "canal": (11, 1.8),
    "stream": (13, 1.0),
    "ditch": (14, 0.7),
    "drain": (14, 0.7),
}
WATERWAY_LEVEL = DARK

# --- roads -----------------------------------------------------------------
# class -> (min_zoom, width at z14, fill level when cased)
ROADS = {
    "motorway": (5, 6.0, LIGHT),
    "trunk": (6, 5.5, LIGHT),
    "primary": (8, 5.0, WHITE),
    "secondary": (9, 4.5, WHITE),
    "tertiary": (11, 4.0, WHITE),
    "minor": (12, 3.0, WHITE),
    "service": (14, 2.0, WHITE),
}
# below this zoom roads are drawn as single dark lines
ROAD_CASED_FROM = 12
# thin-line widths for low zooms
ROAD_THIN = {"motorway": 1.6, "trunk": 1.4, "primary": 1.2,
             "secondary": 1.0, "tertiary": 0.8, "minor": 0.6}
ROAD_CASING = BLACK

# class -> (min_zoom, width, level, dash pattern or None)
TRAILS = {
    "track": (12, 1.0, BLACK, (6, 3)),
    "path": (13, 1.0, BLACK, (2, 3)),
}
RAIL = {"min_zoom": 10, "width": 2.0, "level": BLACK, "tie_dash": (5, 5)}

# admin_level -> (min_zoom, width, level, dash)
BOUNDARIES = {2: (0, 2.0, BLACK, (8, 3, 2, 3)), 4: (8, 1.0, DARK, (6, 4))}

# --- labels ----------------------------------------------------------------
# place class -> (min_zoom, {zoom: font px}, bold)
PLACES = {
    "city": (5, {5: 11, 8: 14, 11: 18, 14: 20}, True),
    "town": (8, {8: 11, 11: 15, 14: 18}, True),
    "village": (11, {11: 11, 13: 13, 15: 15}, False),
    "suburb": (13, {13: 12, 15: 14}, False),
    "hamlet": (13, {13: 10, 15: 12}, False),
    "neighbourhood": (15, {15: 11}, False),
    "isolated_dwelling": (15, {15: 10}, False),
}
PLACE_ORDER = ["city", "town", "village", "suburb", "hamlet",
               "neighbourhood", "isolated_dwelling"]
PEAK = {"min_zoom": 12, "font": 11, "marker": 5}
WATER_NAME = {"min_zoom": 12, "font": 11, "level": DARK}
LABEL_HALO = 2  # px, white outline around text

FONT_REGULAR = "DejaVuSansCondensed.ttf"
FONT_BOLD = "DejaVuSansCondensed-Bold.ttf"
FONT_ITALIC = "DejaVuSansCondensed-Oblique.ttf"


def size_for_zoom(table, z):
    """Pick the value for the highest key <= z."""
    best = None
    for k in sorted(table):
        if k <= z:
            best = table[k]
    return best


def road_width(cls, z):
    _, w14, _ = ROADS[cls]
    return w14 * 2 ** ((z - 14) * 0.5)
