// The map screen: what it shows (buildMapScreenData), how a frame of it is made
// (the frame cache, the topo canvas), the idle work that keeps it ready, a drag,
// a long press and the farewell frame. Split out of ui_dashboard.cpp, whose task
// loop calls it; the state the two share is in ui_shared.h.
#include "ui_shared.h"
#include "doba_mem.h"

namespace uid {

float mapMpp = 2.0f;  // map zoom: 1/2/4/8/16/32 m per px (wide levels show tiles)
// DOBA: the last map frame was a topo one. Topo packs carry zooms 9-16, so the
// map may zoom out further than the vector maps' 32 m/px (z9 is ~190 m/px here).
bool mapTopoActive = false;
// ~z4 (6 km/px at 52 N): all of Europe on the screen, off the overview pack
// (base.dtp holds zooms 0-8; under z8 the cells give way to it).
float mapMaxMpp() { return 6500.0f; }
// Whether the next map frame may read tiles off the card. See renderMapScreen:
// the first pass after any change is drawn from RAM alone so it lands at once.
// Which zoom button is showing its pressed state (0 none, 1 in, 2 out). Held
// from the tap until a COMPLETE map frame has been drawn.
int zoomHeld = 0;
// DOBA: how far the map has been dragged off the position, metres east/north.
float mapPanE = 0, mapPanN = 0;
bool mapPanned() { return mapPanE != 0 || mapPanN != 0; }
float mapTestHeading = -1.0f;   // console `heading`: stands in for the real one

MapBaseCache mapCache;
bool mapCacheOn = true;
bool mapCacheAllocated = false;
MapBaseKey mapViewKey;              // the view on screen
uint32_t mapStillSince = 0;         // when it last moved
uint32_t mapPrerenderNotBefore = 0;
uint32_t mapCacheHits = 0, mapCacheMisses = 0, mapPrerenders = 0, mapPrerendersCut = 0;
// How long the view must hold before the zooms either side are worth rendering. While
// riding it never gets here, which is the point: those frames would go unused.
constexpr uint32_t kMapStillMs = 3000;
constexpr uint32_t kMapPrerenderRetryMs = 5000;
// A cached layer is the base layer's pixels as drawn: the topo tiles in their
// greys. (It was packed to one bit a pixel when the base was black vector roads,
// which turned a cached topo view into a thresholded copy of itself.)
const uint8_t* mapFrameBase = nullptr;   // this frame's cached layer, if it has one
uint8_t* mapPrerenderFb = nullptr;       // a full framebuffer to prerender into
uint8_t* mapLayerBeingKept = nullptr;


// The map the device leaves on the glass when it powers off. Logs what it got, because
// this frame is the one nobody can watch being built.
void renderFarewell(uint8_t* fb) {
    memset(fb, 0xFF, fbSize);
    mapMpp = 32.0f;   // a wide view: snapped to the topo zoom nearest it
    // Always north-up for the farewell. This image sits on the glass for
    // days; rotated to whatever heading the last fix happened to have, it is
    // unreadable as a map of where the bike is. (Deep sleep follows
    // immediately, so mutating the live setting here is harmless.)
    DeviceState s = g_state.snapshot();
    // Save the last known position on the way down, so the next boot's
    // GPS warm-start seed is as fresh as possible.
    if (s.everHadFix) settings::setLastPosition(s.latitude, s.longitude);
    MapScreenData map = {};
    const uint32_t t0 = millis();
    buildMapScreenData(s, map, /*finalFrame=*/true);
    const uint32_t t1 = millis();
    ui_render_map_features(map, s, fb);
    diag::log("farewell: %.1f m/px build=%lu draw=%lu%s at=%.5f,%.5f",
              mapMpp, (unsigned long)(t1 - t0), (unsigned long)(millis() - t1),
              map.topo ? "" : " NOMAP", map.centerLat, map.centerLon);
    doba_ui::renderOffCard(s, g_offByPowerKey, fb);   // DOBA: the paper card
}


// DOBA: a finger drawn across the map body moves the map with it. Released,
// not live: e-paper redraws once, where the finger let go.
bool mapDrag(int x0, int y0, int x1, int y1) {
    if (screen != SCREEN_MAP || powerOverlay) return false;
    const int dx = x1 - x0, dy = y1 - y0;
    if (dx * dx + dy * dy < 30 * 30) return false;
    if (y0 < ui::STATUS_H || y0 >= ui::MAP_STRIP_TOP) return false;
    mapPanE -= dx * mapMpp;
    mapPanN += dy * mapMpp;
    Serial.printf("[map] drag %d,%d -> %.0f m E %.0f m N\n", dx, dy, mapPanE, mapPanN);
    // The spinner is a panel refresh of its own: only when the new view has tiles
    // to come off the card. On the canvas (topo_layer.cpp) the map itself is next.
    MapScreenData map = {};
    buildMapScreenData(g_state.snapshot(), map);
    if (!(map.topo && topo::viewReady(map))) ackBusy();
    return true;
}

// DOBA: a finger held on the map body drops a waypoint there and opens it.
bool mapLongPress(int x, int y) {
    if (screen != SCREEN_MAP) return false;
    if (y < ui::STATUS_H + 10 || y > ui::MAP_STRIP_TOP - 10) return false;
    if (x >= kMapZoom.zoomX - 10) return false;          // the button column
    double lat, lon;
    if (!milspec::screenToLatLon(x, y, lat, lon)) return false;
    const int i = doba_points::add(nullptr, lat, lon);
    if (i < 0) return false;
    Serial.printf("[pts] long-press point %d at %.6f,%.6f\n", i, lat, lon);
    doba_ui::openPoint(i);
    screen = SCREEN_POINT;
    return true;
}


// The letters in a rider's map marker: the first two of their name, "Al" for
// Alex, so two riders sharing an initial still read apart. A name nobody set
// ("OpenTrail 1a2b", "Meshtastic 1a2b") would give everyone the same letters,
// so those fall back to the short name, which is unique-ish by default.
static void peerInitials(const mesh_service::Node& nd, char out[3]) {
    const char* name = nd.longName;
    if (!strncmp(name, "OpenTrail ", 10) || !strncmp(name, "Meshtastic ", 11))
        name = "";
    for (const char* src : {name, (const char*)nd.shortName}) {
        int n = 0;
        for (const char* c = src; *c && n < 2; ++c) {
            if (!isalnum((unsigned char)*c)) continue;
            out[n] = n ? (char)tolower((unsigned char)*c) : (char)toupper((unsigned char)*c);
            ++n;
        }
        out[n] = 0;
        if (n) return;
    }
    snprintf(out, 3, "?");
}

// A point on the screen, around (centerX, centerY) at the view's centre: a local
// flat projection, rotated by rotateDeg, clamped well inside int16.
void geoToScreen(double lat, double lon, double centerLat, double centerLon,
                 float metersPerPixel, int centerX, int centerY,
                 float rotateDeg, int16_t& sx, int16_t& sy) {
    const double kxl = 111320.0 * cos(centerLat * M_PI / 180.0);
    float x = centerX + (float)((lon - centerLon) * kxl / metersPerPixel);
    float y = centerY - (float)((lat - centerLat) * 110540.0 / metersPerPixel);
    if (rotateDeg != 0) {
        const float rc = cosf(rotateDeg * (float)M_PI / 180.0f);
        const float rs = sinf(rotateDeg * (float)M_PI / 180.0f);
        const float dx = x - centerX, dy = y - centerY;
        x = centerX + dx * rc - dy * rs;
        y = centerY + dx * rs + dy * rc;
    }
    x = x < -20000 ? -20000 : x > 20000 ? 20000 : x;
    y = y < -20000 ? -20000 : y > 20000 ? 20000 : y;
    sx = (int16_t)lroundf(x);
    sy = (int16_t)lroundf(y);
}

// Riders on our private mesh channels who have told us where they are (see
// MapPeer). Public-channel positions are left off: those are strangers, and a
// busy public mesh would cover the map in them.
void buildMapPeers(double lat, double lon, float rot, bool useMiles,
                   MapScreenData& map) {
    static MapPeer peers[16];
    int count = 0;
    const uint32_t now = millis();
    const int n = mesh_service::nodeCount();
    for (int i = 0; i < n && count < (int)(sizeof(peers) / sizeof(peers[0])); ++i) {
        mesh_service::Node nd;
        if (!mesh_service::nodeAt(i, nd)) break;
        if (!nd.hasPosition || nd.positionChannel == 0) continue;
        const uint32_t age = now - nd.positionMs;
        if (age > MESH_MAP_MAX_AGE_MS) continue;

        // Project at a scale that cannot overflow the int16 screen coordinates
        // — a rider 50 km off at 2 m/px is 25,000 px away. Only the direction
        // matters once they are off the screen, and that survives the rescale.
        const double dy = (nd.latitude - lat) * 111320.0;
        const double dx = (nd.longitude - lon) * 111320.0 * cos(lat * M_PI / 180.0);
        const float distM = (float)sqrt(dx * dx + dy * dy);
        const float mpp = distM / mapMpp > 8000.0f ? distM / 8000.0f : mapMpp;
        int16_t x, y;
        geoToScreen(nd.latitude, nd.longitude, lat, lon, mpp,
                    map.riderX, map.riderY, rot, x, y);

        // Inside the map with room for the marker, clear of the top band when
        // something is drawn in it; otherwise pinned further in, so the
        // pointer reaching out of it toward them stays on the glass too.
        // Under the status bar and the map's header band (52 px), or the popup.
        const int bandBottom = ui::STATUS_H + (map.navBannerVisible ? 138 : 52);
        MapPeer& p = peers[count];
        p.offscreen = x < 24 || x > 540 - 24 || y < bandBottom + 24 ||
                      y > ui::MAP_STRIP_TOP - 24;
        const int left = 84, right = 540 - 84;
        const int top = bandBottom + 84, bottom = ui::MAP_STRIP_TOP - 84;
        if (p.offscreen) {
            const float vx = x - map.riderX, vy = y - map.riderY;
            float t = 1.0f;
            if (vx > 0) t = fminf(t, (right - map.riderX) / vx);
            if (vx < 0) t = fminf(t, (left - map.riderX) / vx);
            if (vy > 0) t = fminf(t, (bottom - map.riderY) / vy);
            if (vy < 0) t = fminf(t, (top - map.riderY) / vy);
            // Slide in along the same line until it is clear of the chrome
            // drawn over the map edge (zoom column, scale bar), so the marker
            // is not buried.
            auto underChrome = [&](float px, float py) {
                if (px > kMapZoom.zoomX - 50 && py > kMapZoom.layersY - 50 &&
                    py < kMapZoom.zoomOutY + kMapZoom.size + 50) return true;
                if (px < 190 + 50 && py > ui::MAP_STRIP_TOP - 48 - 50) return true;
                return false;
            };
            // In 4 px steps along the line, no nearer the rider than 80 px.
            const float len = sqrtf(vx * vx + vy * vy);
            const float edge = t;
            for (float k = edge; k * len > 80.0f; k -= 4.0f / len) {
                t = k;
                // The marker and the tip of its pointer (78 px further out).
                const float cx = map.riderX + vx * k, cy = map.riderY + vy * k;
                if (!underChrome(cx, cy) &&
                    !underChrome(cx + vx / len * 78, cy + vy / len * 78)) break;
            }
            x = (int16_t)(map.riderX + vx * t);
            y = (int16_t)(map.riderY + vy * t);
        }
        p.x = x;
        p.y = y;
        p.stale = age > MESH_MAP_FRESH_MS;
        peerInitials(nd, p.initials);
        // Off the screen, how far: which way AND whether it is a wait at the
        // next corner or a ride to catch up.
        p.label[0] = 0;
        if (p.offscreen) {
            units::navDist(p.label, sizeof(p.label), distM, useMiles);
            for (char* c = p.label; *c; ++c) *c = (char)toupper((unsigned char)*c);
        }
        count++;
    }
    map.peers = peers;
    map.peerCount = count;
}

MapBaseKey mapKeyFor(const MapScreenData& map) {
    MapBaseKey k;
    k.lat = map.centerLat + mapPanN / 111320.0;
    k.lon = map.centerLon + mapPanE / (111320.0 * cos(map.centerLat * M_PI / 180.0));
    k.mpp = map.metersPerPixel;
    k.rotDeg = map.northDeg;
    k.tiles = topo::generation();
    k.hasMap = map.topo;
    return k;
}

void mapCacheAllocate() {
    if (mapCacheAllocated) return;
    mapCacheAllocated = true;
    const size_t bytes = fbSize;
    for (int i = 0; i < MapBaseCache::kSlots; ++i)
        mapCache.attach(i, (uint8_t*)doba_mem::big(bytes));
    mapPrerenderFb = (uint8_t*)doba_mem::big(fbSize);
}

void mapLayerKeep(const uint8_t* fb) { memcpy(mapLayerBeingKept, fb, fbSize); }

void buildMapScreenData(const DeviceState& s, MapScreenData& map,
                        bool finalFrame) {
    map.riderX = 270;
    map.riderY = 430;
    // Track-up: the world rotates so travel direction is up; the rider
    // arrow points up and the compass needle shows where north went.
    // Which way the device is pointing. Course-over-ground is a DIRECTION OF
    // TRAVEL: it is meaningless standing still and jittery at walking pace,
    // which is why a track-up map used to spin while the rider waited at a
    // light. The compass has no such problem and is used below 5 km/h, if one
    // is fitted and has finished calibrating.
    const bool useCompass = s.compassValid && s.speedKmh < 5.0f;
    const float heading = mapTestHeading >= 0.0f ? mapTestHeading
                                                 : useCompass ? s.compassDeg : s.courseDeg;
    // The topo tiles are a raster drawn 1:1: the map is always north-up, and the
    // position's arrow turns instead.
    float rot = 0.0f;
    map.headingDeg = heading;
    map.northDeg = 0.0f;
    map.trackUp = false;
    // Tell the map renderer to drop the compass below the turn banner when one
    // is drawn over the top of the viewport. The pause banner sits in the same
    // band, so it displaces the compass the same way.
    map.navBannerVisible = meshNoticeShown();
    map.metersPerPixel = mapMpp;

    // Position priority: the current GPS fix; else the last known position;
    // else the persisted/default.
    double lat = DEFAULT_MAP_LAT, lon = DEFAULT_MAP_LON;
    if (s.gpsFix) {
        lat = s.latitude;
        lon = s.longitude;
    } else if (s.everHadFix) {
        lat = s.latitude;
        lon = s.longitude;
    } else {
        settings::lastPosition(lat, lon);
    }
    map.centerLat = lat;
    map.centerLon = lon;
    // DOBA: the base layer is the topo pack under the view. Its tiles are drawn
    // 1:1, so the zoom snaps to the tile zoom nearest the one asked for (the
    // zoom buttons still halve/double; the snap keeps them on tile zooms), and
    // the map is north-up: a raster is not rotated.
    map.topo = topo::covers(lat, lon);
    mapTopoActive = map.topo;
    {
        // DOBA: the first topo view opens at z14 (~6 m/px), not at the closest
        // level, so + and - both do something from the start.
        static bool opened = false;
        if (!opened) {
            opened = true;
            if (mapMpp == 2.0f) mapMpp = topo::nativeMpp(14, lat);
        }
        // Down to ~0.4 m/px (z18): drawn from z17 detail tiles where the card
        // has them, else from z16, the last steps by enlarging the tiles.
        const int zmax = topo::maxZoomAt(lat, lon);
        // DOBA: no enlarging past the deepest tiles. A z17 tile blown up 2x
        // turns its fine anti-aliased lines into dotted ones on four greys
        // (seen on the device at 1:3 500), and costs a second per screen.
        const float nMax = topo::nativeMpp(zmax, lat);
        if (mapMpp < nMax * 0.75f) mapMpp = nMax;
        else mapMpp = topo::nativeMpp(std::min(topo::zoomFor(lat, mapMpp), zmax), lat);
        map.metersPerPixel = mapMpp;
        rot = 0.0f;
        map.northDeg = 0.0f;
        map.trackUp = false;
        map.headingDeg = heading;
    }
    // DOBA: dragged: the view stays north-up and the position moves on the
    // screen (it may leave it); everything is still drawn round it.
    map.zoomInEnd = mapMpp <= topo::nativeMpp(topo::maxZoomAt(lat, lon), lat) * 1.01f;
    map.zoomOutEnd = mapMpp * 2.0f > mapMaxMpp() * 1.01f;
    // DOBA: the place under the view (the position, or the middle when dragged)
    {
        const double vlat = lat + mapPanN / 111320.0;
        const double vlon = lon + mapPanE / (111320.0 * cos(lat * M_PI / 180.0));
        static double qLat = 999, qLon = 999;
        static char qName[32] = "", qSub[32] = "";
        double b0, moved;
        milgeo::bearingDistance(qLat, qLon, vlat, vlon, b0, moved);
        if (qLat > 90 || moved > 150) {
            qLat = vlat;
            qLon = vlon;
            double d, b;
            if (doba_places::nearest(vlat, vlon, qName, sizeof qName, d, b)) {
                static const char* const kDir[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
                // the spot relative to the place: from the place, towards here
                const char* dir = kDir[(int)lround(fmod(b + 180.0, 360.0) / 45.0) % 8];
                if (d < 800) snprintf(qSub, sizeof qSub, "%s", mapPanned() ? TR("map centre") : TR("you are here"));
                else if (d < 10000) snprintf(qSub, sizeof qSub, TR("%.1f km %s of it"), d / 1000.0, TR(dir));
                else snprintf(qSub, sizeof qSub, TR("%.0f km %s of it"), d / 1000.0, TR(dir));
            } else {
                qName[0] = qSub[0] = 0;
            }
        }
        snprintf(map.place, sizeof map.place, "%s", qName);
        snprintf(map.placeSub, sizeof map.placeSub, "%s", qSub);
    }
    if (mapPanned()) {
        map.panned = true;
        rot = 0.0f;
        map.northDeg = 0.0f;
        map.trackUp = false;
        map.headingDeg = heading;
        map.riderX = 270 - (int)lroundf(mapPanE / mapMpp);
        map.riderY = 430 + (int)lroundf(mapPanN / mapMpp);
    }
    // With this view's base layer already finished, the tiles have nothing left to
    // give: skip them.
    mapFrameBase = nullptr;
    if (mapCacheOn && !finalFrame) {
        mapCacheAllocate();
        mapFrameBase = mapCache.find(mapKeyFor(map), millis());
    }

    // Not on the farewell frame: that one stays on the glass for days, long
    // after anyone is still where it would show them.
    if (!finalFrame) buildMapPeers(lat, lon, rot, false, map);
}

// The map frame's base layer, from the cache if it can be, kept for next time if not.
void renderMapWithCache(const MapScreenData& map, const DeviceState& s, uint8_t* fb) {
    const uint32_t now = millis();
    const MapBaseKey key = mapKeyFor(map);
    if (!mapSameView(key, mapViewKey)) {
        mapViewKey = key;
        mapStillSince = now;
    }
    if (!mapCacheOn) {
        ui_render_map(map, s, fb);
        return;
    }
    if (mapFrameBase) {
        mapCacheHits++;
        memcpy(fb, mapFrameBase, fbSize);
        ui_render_map(map, s, fb, /*baseDrawn=*/true);
        return;
    }
    mapCacheMisses++;
    if (map.topo) {
        // The canvas holds this zoom round the view; the frame cache is for the
        // other zooms (one claimed a frame a GPS step while walking, and pushed
        // the prerendered + and - out).
        ui_render_map(map, s, fb);
        return;
    }
    uint8_t* save = mapCache.claim(now);
    mapLayerBeingKept = save;
    ui_render_map(map, s, fb, false, save ? mapLayerKeep : nullptr);
    if (save) mapCache.commit(save, key, now);
}

void renderMapScreen(const DeviceState& s, uint8_t* fb) {
    MapScreenData map = {};
    uint32_t m0 = millis();
    buildMapScreenData(s, map);
    uint32_t m1 = millis();

    // A finger arrived while this frame was being built. Don't spend the panel
    // on it — one paint is 130-400 ms, and that is 130-400 ms in which nothing
    // acknowledges the touch. Put the LAST PAINTED frame back and return, so
    // refresh() finds nothing changed, skips the panel, and the next thing to
    // reach the glass is the pressed zoom button.
    //
    // Restoring from shadowFb rather than just returning is the whole point: the
    // caller memsets the framebuffer to white before this runs, so returning
    // early leaves a BLANK screen — status bar, data fields and all — with only
    // the acknowledgement drawn on it. shadowFb is by definition what is
    // currently on the glass, so this is a no-op repaint rather than a wipe.
    if ((touchIrq || touchWasDown) && shadowFb) {
        memcpy(fb, shadowFb, fbSize);
        forceDraw = true;
        if (dbgTiming) diag::log("map: frame dropped for a touch");
        return;
    }
    renderMapWithCache(map, s, fb);
    zoomHeld = 0;   // the frame is complete: the pressed zoom button lets go
    if (dbgTiming)
        diag::log("map: build=%lu draw=%lu mpp=%.2f topo=%d at=%.5f,%.5f peers=%d",
                  (unsigned long)(m1 - m0), (unsigned long)(millis() - m1), mapMpp,
                  (int)map.topo, map.centerLat, map.centerLon, map.peerCount);
    drawMeshNotice(fb);
}

// The map's idle work, one small piece a call, in the UI task's spare time, giving
// way to a finger. First the canvas round the view (topo_layer.cpp: half a screen
// of map beyond each edge, so a drag or a walk lands on map already drawn), then,
// with the view holding still, one zoom either side, so a tap on + or - only has
// to put it on the panel. True when it did something (the loop comes straight
// back for the next piece instead of sleeping).
bool mapPrerenderStep() {
    if (screen != SCREEN_MAP || powerOverlay || zoomHeld) return false;
    if (touchIrq || touchWasDown) return false;
    {
        MapScreenData map = {};
        const DeviceState s = g_state.snapshot();
        buildMapScreenData(s, map);
        if (map.topo && topo::idleStep(map)) return true;
        // A topo view needs no zoom +-1 frames kept: a zoom shows the canvas
        // enlarged at once (drawZoomPreview), and topo frames skip the cache.
        // Drawing them went through the canvas too and moved it to the other
        // zoom, throwing away the margin the next drag needed.
        if (map.topo) return false;
    }
    if (!mapCacheOn) return false;
    const uint32_t now = millis();
    if (now - mapStillSince < kMapStillMs || (int32_t)(now - mapPrerenderNotBefore) < 0) return false;

    for (const float ask : {mapMpp / 2.0f, mapMpp * 2.0f}) {
        if (ask > mapMaxMpp() * 1.01f) continue;
        const float liveMpp = mapMpp;
        mapMpp = ask;
        MapScreenData map = {};
        const DeviceState s = g_state.snapshot();
        buildMapScreenData(s, map);   // snaps to a tile zoom the card has
        const float mpp = mapMpp;
        mapMpp = liveMpp;
        if (fabsf(mpp - liveMpp) < 0.01f) continue;   // already at the end of the range
        if (mapFrameBase) continue;                    // kept already
        if (!mapPrerenderFb) return false;
        uint8_t* px = mapCache.claim(millis());
        if (!px) return false;
        memset(mapPrerenderFb, 0xFF, fbSize);
        ui_render_map_base(map, mapPrerenderFb);
        if (touchIrq || touchWasDown) {   // a finger came: this frame may be stale
            mapPrerendersCut++;
            mapPrerenderNotBefore = millis() + kMapPrerenderRetryMs;
            return false;
        }
        memcpy(px, mapPrerenderFb, fbSize);
        mapCache.commit(px, mapKeyFor(map), millis());
        mapPrerenders++;
        return true;
    }
    return false;
}

// Console `farewell`: the power-off screen without the power-off, held for a look.
void farewellTest() {
    const float liveMpp = mapMpp;
    uint8_t* fb = epdc_framebuffer();
    renderFarewell(fb);
    if (shadowFb) memcpy(shadowFb, fb, fbSize);
    epdc_clear();
    epdc_paint();
    epdc_paint_wait();
    vTaskDelay(pdMS_TO_TICKS(30000));
    mapMpp = liveMpp;
    forceDraw = true;
}


// What the map would show, as a number: its next frame changes only if this
// does. The map used to be drawn every second whether or not anything on it
// had moved (330-390 ms of the UI task's CPU a second), which starved the
// idle work that draws the canvas margin before the next drag.
uint32_t mapTickKey(const DeviceState& s) {
    MapScreenData map = {};
    buildMapScreenData(s, map);
    uint32_t h = 2166136261u;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    };
    const int32_t v[] = {(int32_t)lround(map.centerLat * 1e6), (int32_t)lround(map.centerLon * 1e6),
                         map.riderX, map.riderY, (int32_t)lroundf(map.headingDeg),
                         (int32_t)lroundf(map.metersPerPixel * 100), (int32_t)lroundf(map.northDeg),
                         map.trackUp, map.navBannerVisible, map.panned, map.topo, map.peerCount,
                         s.gpsFix, s.satellites, s.batteryPercent, s.charging, s.sdCard,
                         (int32_t)(time(nullptr) / 60)};
    mix(v, sizeof v);
    mix(map.place, sizeof map.place);
    mix(map.placeSub, sizeof map.placeSub);
    for (int i = 0; i < map.peerCount && map.peers; ++i) mix(&map.peers[i], sizeof(MapPeer));
    return h;
}

// DOBA: the map follows a finger while it drags (released, the drag is kept by
// mapDrag). Each frame is the canvas round the view, copied (topo::drawQuick):
// nothing comes off the card while the finger moves, so frames go out as fast
// as the panel takes them, at most one in 120 ms. Off the canvas (dragged past
// its margin) the map waits for the finger to let go.
bool liveDragging = false;
uint32_t liveLastPaint = 0;

// A live frame in black and white. The panel lands a grey by darkening and
// lightening a pixel in turn, so every grey that moved under the finger (the
// buildings, the woods) blinked on every frame. While the finger moves the
// fills go to paper and the lines, roads and names to ink; the greys come back
// with the frame drawn after it lets go.
void inkOnly(uint8_t* fb) {
    static uint8_t lut[256];
    static bool built = false;
    if (!built) {
        for (int b = 0; b < 256; ++b) {
            const uint8_t lo = (b & 0x0F) < 8 ? 0x0 : 0xF, hi = (b >> 4) < 8 ? 0x0 : 0xF;
            lut[b] = (uint8_t)(hi << 4 | lo);
        }
        built = true;
    }
    for (size_t i = 0; i < fbSize; ++i) fb[i] = lut[fb[i]];
}

bool mapLiveDrag() {
    if (screen != SCREEN_MAP || powerOverlay || !touchWasDown) {
        liveDragging = false;
        return false;
    }
    const int x0 = touchStartX, y0 = touchStartY, x = touchNowX, y = touchNowY;
    if (y0 < ui::STATUS_H || y0 >= ui::MAP_STRIP_TOP) return false;
    if (x0 >= kMapZoom.zoomX - 10) return false;          // the button column
    const int dx = x - x0, dy = y - y0;
    if (!liveDragging && dx * dx + dy * dy < 30 * 30) return false;
    liveDragging = true;
    // One frame at a time: the next only once the panel has finished driving the
    // last. Sent sooner, it lands on greys still on their way and they read
    // darker until a later frame settles them (the drag looked dark, then the
    // map came back to its tone after the finger let go).
    if (millis() - liveLastPaint < 120 || !epdc_paint_idle()) return false;
    const float e = mapPanE, n = mapPanN;
    mapPanE -= dx * mapMpp;
    mapPanN += dy * mapMpp;
    MapScreenData map = {};
    const DeviceState s = g_state.snapshot();
    buildMapScreenData(s, map);
    uint8_t* fb = epdc_framebuffer();
    memset(fb, 0xFF, fbSize);
    const bool ok = map.topo && topo::drawQuick(map, fb);
    if (ok) { ui_render_map(map, s, fb, /*baseDrawn=*/true); inkOnly(fb); }
    mapPanE = e;
    mapPanN = n;
    if (!ok) {
        if (shadowFb) memcpy(fb, shadowFb, fbSize);
        return false;
    }
    liveLastPaint = millis();
    refresh(false, true, false, false, false, false);
    if (dbgTiming) diag::log("map: live drag %d,%d", dx, dy);
    return true;
}

}  // namespace uid
