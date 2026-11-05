// Host test driver: reads "lat lon" lines, prints MGRS, UTM and round trip.
#include <cstdio>
#include <initializer_list>
#include "../../src/milgeo.h"
int main() {
    double lat, lon;
    while (scanf("%lf %lf", &lat, &lon) == 2) {
        char m[32];
        milgeo::mgrs(lat, lon, m, sizeof m);
        milgeo::Utm u = milgeo::toUtm(lat, lon);
        double la, lo;
        milgeo::fromUtm(u.zone, u.north, u.easting, u.northing, la, lo);
        double pa = 0, po = 0;
        const bool ok = milgeo::parseMgrs(m, pa, po);
        double brg, dist;
        milgeo::bearingDistance(lat, lon, pa, po, brg, dist);
        printf("%s|%d|%.3f|%.3f|%.9f|%.9f|%d|%.3f\n", m, u.zone, u.easting, u.northing, la, lo, ok, dist);
    }
    char b[8];
    for (float d : {0.f, 30.f, 90.f, 180.f, 359.99f, -10.f}) {
        milgeo::mils(d, 6000, b, sizeof b); printf("%s ", b);
        milgeo::mils(d, 6400, b, sizeof b); printf("%s\n", b);
    }
}
