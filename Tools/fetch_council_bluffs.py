"""Download OpenStreetMap + elevation data for central Council Bluffs, Iowa and bake it into
Data/CouncilBluffs.json for the game to build as a real place.

Data: (c) OpenStreetMap contributors, ODbL. Elevation: Open-Meteo (Copernicus DEM, 90 m).
Coordinates in the output are metres east/north of ORIGIN (Bayliss Park downtown).

Usage: python Tools/fetch_council_bluffs.py
"""
import json, math, os, sys, time, urllib.parse, urllib.request

ORIGIN = (41.2592, -95.8517)          # lat, lon
SOUTH, NORTH = 41.195, 41.315
WEST, EAST = -95.935, -95.795
ELEV_STEP_M = 300.0                    # elevation grid spacing
HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "..", "SourceAssets", "CouncilBluffs")
OUT = os.path.join(HERE, "..", "Data", "CouncilBluffs.json")
os.makedirs(RAW, exist_ok=True)
os.makedirs(os.path.dirname(OUT), exist_ok=True)

M_PER_DEG_LAT = 111320.0
M_PER_DEG_LON = 111320.0 * math.cos(math.radians(ORIGIN[0]))


def to_local(lat, lon):
    return (round((lon - ORIGIN[1]) * M_PER_DEG_LON, 1), round((lat - ORIGIN[0]) * M_PER_DEG_LAT, 1))


def http(url, data=None, tries=6):
    for t in range(tries):
        try:
            req = urllib.request.Request(url, data=data, headers={"User-Agent": "explorer-game/0.1 (personal project)"})
            return urllib.request.urlopen(req, timeout=180).read()
        except Exception as e:
            print("retry", t, e, file=sys.stderr)
            time.sleep(20 * (t + 1))
    raise RuntimeError("failed " + url)


def overpass(query, name):
    cache = os.path.join(RAW, name + ".json")
    if os.path.exists(cache):
        return json.load(open(cache, encoding="utf-8"))
    body = urllib.parse.urlencode({"data": query}).encode()
    raw = http("https://overpass-api.de/api/interpreter", body)
    open(cache, "wb").write(raw)
    return json.loads(raw)


def fetch_osm():
    bbox = f"({SOUTH},{WEST},{NORTH},{EAST})"
    q = f"""[out:json][timeout:180];(
      way["highway"]{bbox};
      way["building"]{bbox};
      way["natural"~"water|wood"]{bbox};
      way["waterway"~"river|stream|canal"]{bbox};
      way["landuse"~"grass|forest|park|recreation_ground|meadow|farmland|residential|commercial|industrial|retail|cemetery"]{bbox};
      way["leisure"~"park|pitch|golf_course|playground|track|stadium"]{bbox};
      way["railway"="rail"]{bbox};
      way["amenity"~"school|place_of_worship|hospital|library|townhall"]{bbox};
      node["name"~"Golden Spike"]{bbox};
    );out geom tags;"""
    return overpass(q, "osm")


def fetch_elevation():
    rows = int((NORTH - SOUTH) * M_PER_DEG_LAT / ELEV_STEP_M) + 1
    cols = int((EAST - WEST) * M_PER_DEG_LON / ELEV_STEP_M) + 1
    cache = os.path.join(RAW, "elev.json")
    if os.path.exists(cache):
        return json.load(open(cache))
    pts = []
    for r in range(rows):
        for c in range(cols):
            pts.append((SOUTH + (NORTH - SOUTH) * r / (rows - 1), WEST + (EAST - WEST) * c / (cols - 1)))
    out = []
    for i in range(0, len(pts), 100):
        chunk = pts[i:i + 100]
        url = "https://api.open-meteo.com/v1/elevation?latitude=%s&longitude=%s" % (
            ",".join("%.5f" % p[0] for p in chunk), ",".join("%.5f" % p[1] for p in chunk))
        out += json.loads(http(url))["elevation"]
        time.sleep(4)
        if (i // 100) % 20 == 0:
            print("elevation", i, "/", len(pts))
    data = {"rows": rows, "cols": cols, "elev": out}
    json.dump(data, open(cache, "w"))
    return data


def height_of(tags):
    if "height" in tags:
        try:
            return float(str(tags["height"]).split()[0])
        except ValueError:
            pass
    if "building:levels" in tags:
        try:
            return float(tags["building:levels"]) * 3.3 + 1.5
        except ValueError:
            pass
    return 0.0


def bake():
    osm = fetch_osm()
    elev = fetch_elevation()
    roads, buildings, water, areas, rails, places = [], [], [], [], [], []
    for e in osm["elements"]:
        t = e.get("tags", {})
        if e["type"] == "node":
            places.append({"name": t.get("name", ""), "kind": "monument", "p": to_local(e["lat"], e["lon"])})
            continue
        g = [to_local(p["lat"], p["lon"]) for p in e.get("geometry", [])]
        if len(g) < 2:
            continue
        flat = [round(v) for p in g for v in p]
        if "highway" in t:
            roads.append({"c": t["highway"], "n": t.get("name", ""), "p": flat, "b": 1 if t.get("bridge") == "yes" else 0})
        elif "building" in t:
            buildings.append({"h": height_of(t), "t": t["building"], "n": t.get("name", ""), "p": [round(v, 1) for p in g for v in p],
                              "roof": t.get("roof:shape", ""), "a": (t.get("addr:housenumber", "") + " " + t.get("addr:street", "")).strip()})
        elif t.get("natural") == "water" or "waterway" in t:
            water.append({"river": 1 if "waterway" in t else 0, "p": flat})
        elif t.get("railway") == "rail":
            rails.append({"p": flat})
        else:
            kind = t.get("landuse") or t.get("leisure") or t.get("natural") or t.get("amenity")
            areas.append({"k": kind, "n": t.get("name", ""), "p": flat})
    data = {
        "origin": {"lat": ORIGIN[0], "lon": ORIGIN[1]},
        "bounds": {"west": to_local(ORIGIN[0], WEST)[0], "east": to_local(ORIGIN[0], EAST)[0],
                   "south": to_local(SOUTH, ORIGIN[1])[1], "north": to_local(NORTH, ORIGIN[1])[1]},
        "elevation": {"rows": elev["rows"], "cols": elev["cols"], "m": [round(v) for v in elev["elev"]]},
        "roads": roads, "buildings": buildings, "water": water, "areas": areas, "rails": rails, "places": places,
        "attribution": "(c) OpenStreetMap contributors (ODbL); elevation Copernicus DEM via Open-Meteo",
    }
    json.dump(data, open(OUT, "w"), separators=(",", ":"))
    print("wrote", OUT, os.path.getsize(OUT) // 1024, "KB;", len(roads), "roads", len(buildings), "buildings",
          len(water), "water", len(areas), "areas")


if __name__ == "__main__":
    bake()
