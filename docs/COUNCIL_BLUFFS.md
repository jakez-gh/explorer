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

## 1733 Avenue E (hero house)
Public listing data (RealtyTrac/Zillow via web search): single-storey, built 1920, 3 bed / 1 bath, gable asphalt roof, fireplace, lot 6,534 sq ft, ~880 sq ft main floor (some listings say 1,540 sq ft total). `HouseGen::GenerateBungalow1733` is a hand-planned layout (front room + dining, parents' room with crib, boys' bunk room, girls' room, kitchen, bath) furnished for a family of seven and a cat. Photos/blueprints weren't retrievable (Zillow returns 403); real photos from the player would improve it.

Update: the family lived at 1719 Avenue E (address since changed), so the hero house sits on the 10th lot west of 17th Street (`-StartAt="1719 Avenue E"`); 1733 is an ordinary lot again. Listing data for 1719 wasn't found (1701 Avenue E next door: 3 bed/1 bath, 1,040 sq ft, built 1950), so the hero plan still uses the 1733 listing's 1920 bungalow shape until you say otherwise.

## 1719 Avenue E as remembered (replaces the 1920 bungalow guess)
Two storeys, blue vinyl siding, brown trim, concrete front steps, enclosed front porch, blue-and-brown garage with an asphalt pad, chain-link fence, one tree in the back, full cinderblock basement with black-and-orange tile and a bar, earth-tone interior, 220 V window AC in the living room. Family: couple, two elementary boys, a young girl, a teen girl, a baby boy, one cat. Built by `HouseGen::GenerateFamilyHouse1719` (layout is a plausible plan, not a survey).
