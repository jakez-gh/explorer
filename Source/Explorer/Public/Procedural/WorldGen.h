#pragma once

#include "CoreMinimal.h"

/** Broad climate zones. Neighbouring zones blend; this is the dominant one at a point. */
enum class EBiome : uint8
{
	Ocean,
	Beach,
	Desert,
	Savanna,
	Grassland,
	Forest,
	Jungle,
	Taiga,
	Tundra,
	Snow,
};

/** Everything the world generator knows about one point on the ground. */
struct FWorldSample
{
	// World units; sea level is 0.
	float Height = 0.0f;
	// 0..1 after altitude cooling.
	float Temperature = 0.0f;
	// 0..1.
	float Moisture = 0.0f;
	// 0 = open ocean, 1 = well inland.
	float Land = 0.0f;
	// 0..1 strength of mountain ranges here.
	float Mountains = 0.0f;
	EBiome Biome = EBiome::Ocean;
	// Linear ground colour before slope/rock shading.
	FLinearColor Color = FLinearColor::Black;
	// 0 = dry, 1 = glossy.
	float Wetness = 0.0f;
	// Chance per ~16 m cell of a tree (or shrub) growing here.
	float TreeDensity = 0.0f;

	// Terrain material layers, 0..1 each (grass is the base layer).
	float Sand = 0.0f;
	float Forest = 0.0f;
	float Snow = 0.0f;
	// Exposed rock regardless of slope (volcanoes, high ridges).
	float Rock = 0.0f;
	// 0 = lush, 1 = parched: yellows grass and turns rock to sandstone.
	float Dryness = 0.0f;

	// 0..1 inside a volcano's cone; 1 at the rim.
	float Volcano = 0.0f;
};

/**
 * Deterministic, seamless world generation. Every function is a pure function of world XY,
 * so any chunk can be built independently and neighbours always match.
 */
namespace WorldGen
{
	FWorldSample Sample(double X, double Y);

	inline float Height(double X, double Y) { return Sample(X, Y).Height; }

	// Colour of exposed rock and snow, applied on slopes and in the cold.
	FLinearColor RockColor();
	FLinearColor SnowColor();

	// Rare volcanoes: returns false if this ~40 km cell has none. Crater centre at ground level of the rim.
	bool FindVolcano(int32 CellX, int32 CellY, FVector2D& OutCenter, float& OutRadius, float& OutHeight);
	double VolcanoCellSize();

	uint32 Hash(int32 X, int32 Y, uint32 Seed);
	// Uniform in [0, 1).
	float HashFloat(int32 X, int32 Y, uint32 Seed);

	// Linear colour from an sRGB hex like 0x5A8C3B.
	FLinearColor Srgb(uint32 Hex);
}
