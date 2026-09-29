# Council Bluffs, Iowa — a real place in Explorer

Goal: a faithful, photoreal recreation of Council Bluffs the way the player remembers it (late 1970s-80s), enterable buildings included.

## Data (Tools/fetch_council_bluffs.py -> Data/CouncilBluffs.json)
- OpenStreetMap (ODbL): roads, ~5,800 buildings, water, land cover, parks, named schools. Current, not historical.
- Elevation: Copernicus DEM via Open-Meteo, 300 m grid (Loess Hills bluffs 292-402 m ASL).
- World placement: origin (Bayliss Park) at world X=2,500,000 Y=1,500,000 (X north, Y east). Launch with `-CouncilBluffs` or `-StartAt="Big Lake Park"` (any landmark name substring).

## Status
- [x] Terrain from DEM, lakes/river carved, land-cover driven ground + tree density (woods around Big Lake)
- [x] Real street network as ribbon paths
- [ ] Buildings from footprints (HouseGen at true footprint size for houses; massing + interiors for large ones)
- [ ] Schools: Abraham Lincoln HS, Thomas Jefferson HS, Wilson MS, Roosevelt Elementary
- [ ] Downtown: 8th St & Broadway, Bayliss Park; 18th & Avenue E; 1733 Avenue E
- [ ] Big Lake Park woods, trails (motorcycle trails), campsites
- [ ] Historical accuracy (1978-88): needs period sources (Sanborn maps, aerials, photos)

## Limits
No historical imagery is available offline; current OSM is the baseline and period details must come from photos/records the player provides or public archives.
