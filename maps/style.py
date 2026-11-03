"""E-ink map style for Doba.

The screen shows 16 grey levels (0 = black, 15 = white). Every colour here
is a level number, so what you set is exactly what the panel shows.
Widths and font sizes are in screen pixels at 1x.

Data schema: OpenMapTiles (what Planetiler produces by default).
"""

WHITE = 15
BLACK = 0

BACKGROUND = WHITE

# --- area fills -----------------------------------------------------------
# (layer, class) -> (min_zoom, level)
AREA_FILLS = [
    ("landcover", "farmland", 0, WHITE),
    ("landcover", "grass", 0, WHITE),
    ("landcover", "sand", 10, 14),
    ("landcover", "rock", 10, 12),
    ("landcover", "ice", 0, WHITE),
    ("landcover", "wetland", 10, 14),
    ("landcover", "wood", 7, 13),
    ("landuse", "residential", 10, 14),
    ("landuse", "suburb", 10, 14),
    ("landuse", "neighbourhood", 10, 14),
    ("landuse", "commercial", 12, 14),
    ("landuse", "retail", 12, 14),
    ("landuse", "industrial", 12, 13),
    ("landuse", "railway", 13, 14),
    ("landuse", "cemetery", 13, 13),
    ("landuse", "military", 11, 14),
    ("landuse", "quarry", 12, 12),
]

# Forest gets a dot pattern on top of its fill so it stays distinct from
# built-up areas even on a dim e-ink panel.
FOREST_DOTS = {"min_zoom": 12, "level": 8, "spacing": 7}

WATER_FILL = 11
WATER_EDGE = 5
WATER_MIN_ZOOM = 0

BUILDING_FILL = 10
BUILDING_EDGE = 7
BUILDING_MIN_ZOOM = 14

# --- relief ----------------------------------------------------------------
HILLSHADE = {  # strength = max darkening fraction on steep shadowed slopes
    "min_zoom": 6,
    "strength_low": 0.45,   # z <= 12
    "strength_high": 0.30,  # z >= 13
}

# zoom -> (interval m, index interval m)
CONTOUR_INTERVALS = {11: (100, 500), 12: (50, 250), 13: (20, 100),
                     14: (10, 50), 15: (10, 50), 16: (10, 50)}
CONTOUR = {"level": 9, "width": 0.8}
CONTOUR_INDEX = {"level": 6, "width": 1.3}
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
WATERWAY_LEVEL = 5

# --- roads -----------------------------------------------------------------
# class -> (min_zoom, width at z14, fill level when cased)
ROADS = {
    "motorway": (5, 6.0, 11),
    "trunk": (6, 5.5, 12),
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
    "path": (13, 1.0, 2, (3, 3)),
}
RAIL = {"min_zoom": 10, "width": 2.0, "level": BLACK, "tie_dash": (5, 5)}

# admin_level -> (min_zoom, width, level, dash)
BOUNDARIES = {2: (0, 2.0, 3, (8, 4)), 4: (8, 1.0, 6, (6, 4))}

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
WATER_NAME = {"min_zoom": 12, "font": 11, "level": 3}
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
