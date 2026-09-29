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

## Revised 1719 Avenue E (from the family's memory)
Story-and-a-half, blue vinyl siding, brown trim. Ground floor: central hall with the stairs in the middle of the house; master bedroom, baby's room (the former den), kitchen with pantry and a whole bath beside it, living/dining at the front, enclosed front porch. Upstairs: boys' room open to the landing; the girl's room in the dormer. Basement (cinderblock, black-and-orange tile, bar): the teen's bedroom and the laundry; no workbench. Detached blue/brown garage, asphalt pad, chain-link fence, one back-yard tree, 220 V window AC in the living room.

## 343 East Graham Avenue
Public records: built 1920, ~1,539 sq ft, 2 bed / 3 bath, roof permit Dec 2024. Landmark `-StartAt="343 East Graham"` is placed on the mapped Graham Avenue East (unverified). No detailed model yet: needs the player's description or photos.

## The bones of 1733 E Ave (studied from all 52 Zillow photos, 2026-09-29)
Note: photos show it as renovated now (new floors, kitchen, paint); the shape is what matters.
- **Type:** story-and-a-half, narrow, front-gable (ridge front-to-back), grey vinyl siding, brown fascia/gutters, tan-brown asphalt shingles. Gable end faces the street with a pair of double-hung windows.
- **Front:** full-width enclosed porch (siding skirt, banks of windows, low roof), entry door centered on the porch, wooden steps centered, chain-link fence with a gate at the steps. Porch is a narrow room; an interior door leads into the living room.
- **Ground floor:** living room at the front (big front window + the porch door), opening straight back into an open kitchen/dining space with a peninsula bar with pendants; a carpeted bedroom off the living room (right of the porch door); a whole bath with tub beside the kitchen; a stair going up beside the kitchen (left side) and the cellar stair beneath it.
- **Upper floor (attic rooms):** carpeted, sloped ceilings, knee-wall closets, half-wall railing around the stair opening; a hall leads to a room with a window at the back and another at the side; a dormer-niche with a small window; a five-panel door to eave storage.
- **Basement:** cinderblock walls, unfinished, orange-and-black checker tile floor near the stairs, small high window.
- **Back:** double window and back door, wooden deck with stairs to the side, big tree in the yard, sheds with teal roofs, asphalt pad at the alley, chain-link fence.

Corrections from the family: the front step was concrete (the photos show newer wood; not what they remember), and the interior stairs did not move, so the central-stairs layout stands over the photos' kitchen-side stair.

More corrections: front steps were poured concrete from a form with a metal rail; no brick is visible anywhere on the house (basement walls are cinderblock) so the chimney is removed; the stairs belong where the photos show them, beside the kitchen (the current stair in the hall west of the kitchen is the closest; its exact run is unverified).
