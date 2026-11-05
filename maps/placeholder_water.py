#!/usr/bin/env python3
"""A stand-in water-polygons-split-3857.zip for inland regions.

Planetiler's OpenMapTiles profile insists on the ocean polygons from
osmdata.openstreetmap.de. When that server is unreachable this writes a valid
shapefile with one tiny polygon in the mid-Atlantic, so inland regions build.
Coastal regions need the real file (delete this one and rerun fetch.sh).

    python3 placeholder_water.py build/sources/water-polygons-split-3857.zip
"""
import io
import sys
import zipfile

import shapefile  # pyshp

PRJ = ('PROJCS["WGS_1984_Web_Mercator_Auxiliary_Sphere",GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",'
       'SPHEROID["WGS_1984",6378137.0,298.257223563]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]],'
       'PROJECTION["Mercator_Auxiliary_Sphere"],PARAMETER["False_Easting",0.0],PARAMETER["False_Northing",0.0],'
       'PARAMETER["Central_Meridian",0.0],PARAMETER["Standard_Parallel_1",0.0],'
       'PARAMETER["Auxiliary_Sphere_Type",0.0],UNIT["Meter",1.0]]')


def main(out):
    shp, shx, dbf = io.BytesIO(), io.BytesIO(), io.BytesIO()
    w = shapefile.Writer(shp=shp, shx=shx, dbf=dbf, shapeType=shapefile.POLYGON)
    w.field("x", "N", 10)
    w.field("y", "N", 10)
    x, y = -3_500_000, 3_000_000          # mid-Atlantic, ~100 m square
    w.poly([[[x, y], [x, y + 100], [x + 100, y + 100], [x + 100, y], [x, y]]])
    w.record(0, 0)
    w.close()
    with zipfile.ZipFile(out, "w") as z:
        d = "water-polygons-split-3857/"
        z.writestr(d + "water_polygons.shp", shp.getvalue())
        z.writestr(d + "water_polygons.shx", shx.getvalue())
        z.writestr(d + "water_polygons.dbf", dbf.getvalue())
        z.writestr(d + "water_polygons.prj", PRJ)
        z.writestr(d + "water_polygons.cpg", "UTF-8")
    print("placeholder ocean polygons ->", out)


if __name__ == "__main__":
    main(sys.argv[1])
