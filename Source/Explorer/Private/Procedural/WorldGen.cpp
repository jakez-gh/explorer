#include "Procedural/WorldGen.h"
#include "Procedural/RealPlace.h"

namespace
{
	// Per-layer offsets decorrelate noise layers that share the same Perlin function.
	const FVector2D OffContinents(113.7, -71.3);
	const FVector2D OffHills(-311.1, 57.9);
	const FVector2D OffRanges(911.3, 419.2);
	const FVector2D OffRidges(-57.1, -923.4);
	const FVector2D OffTemperature(271.9, 613.3);
	const FVector2D OffMoisture(-733.3, 151.7);
	const FVector2D OffDunes(47.9, 388.1);

	float Fbm(double X, double Y, int32 Octaves, const FVector2D& Offset)
	{
		float Sum = 0.0f;
		float Amplitude = 1.0f;
		float Norm = 0.0f;
		double Frequency = 1.0;
		for (int32 i = 0; i < Octaves; ++i)
		{
			Sum += FMath::PerlinNoise2D(FVector2D(X * Frequency, Y * Frequency) + Offset * (i + 1)) * Amplitude;
			Norm += Amplitude;
			Amplitude *= 0.5f;
			Frequency *= 2.0;
		}
		return Sum / Norm;
	}

	// Sharp crests where the noise crosses zero; 0..1.
	float Ridged(double X, double Y, int32 Octaves, const FVector2D& Offset)
	{
		float Sum = 0.0f;
		float Amplitude = 1.0f;
		float Norm = 0.0f;
		double Frequency = 1.0;
		for (int32 i = 0; i < Octaves; ++i)
		{
			float N = 1.0f - FMath::Abs(FMath::PerlinNoise2D(FVector2D(X * Frequency, Y * Frequency) + Offset * (i + 1)));
			Sum += N * N * Amplitude;
			Norm += Amplitude;
			Amplitude *= 0.5f;
			Frequency *= 2.0;
		}
		return Sum / Norm;
	}

	float Smooth(float Edge0, float Edge1, float X)
	{
		const float T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	struct FBiomeInfo
	{
		EBiome Biome;
		FLinearColor Color;
		float TreeDensity;
		float Wetness;
	};
}

namespace WorldGen
{
	FLinearColor Srgb(uint32 Hex)
	{
		return FLinearColor(FColor((Hex >> 16) & 0xFF, (Hex >> 8) & 0xFF, Hex & 0xFF));
	}

	double RoadCellSize() { return 250000.0; }
	static constexpr double TrailCellSize = 70000.0;

	FVector2D RoadNode(int32 CX, int32 CY)
	{
		return FVector2D((CX + FMath::Lerp(0.36f, 0.64f, HashFloat(CX, CY, 21))) * RoadCellSize(), (CY + FMath::Lerp(0.36f, 0.64f, HashFloat(CX, CY, 22))) * RoadCellSize());
	}

	static FVector2D TrailNode(int32 CX, int32 CY)
	{
		return FVector2D((CX + FMath::Lerp(0.3f, 0.7f, HashFloat(CX, CY, 931))) * TrailCellSize, (CY + FMath::Lerp(0.3f, 0.7f, HashFloat(CX, CY, 932))) * TrailCellSize);
	}

	void PathsNear(const FVector2D& Min, const FVector2D& Max, double Margin, TArray<FPath>& Out)
	{
		auto Gather = [&](double Cell, bool bRoad, float Chance, float Width, uint32 Seed, FVector2D (*Node)(int32, int32))
		{
			const int32 X0 = FMath::FloorToInt((Min.X - Margin) / Cell) - 1;
			const int32 X1 = FMath::FloorToInt((Max.X + Margin) / Cell) + 1;
			const int32 Y0 = FMath::FloorToInt((Min.Y - Margin) / Cell) - 1;
			const int32 Y1 = FMath::FloorToInt((Max.Y + Margin) / Cell) + 1;
			for (int32 CY = Y0; CY <= Y1; ++CY)
			{
				for (int32 CX = X0; CX <= X1; ++CX)
				{
					const FVector2D A = Node(CX, CY);
					const FIntPoint Next[] = { FIntPoint(CX + 1, CY), FIntPoint(CX, CY + 1) };
					for (int32 k = 0; k < 2; ++k)
					{
						if (HashFloat(CX, CY, Seed + k) > Chance)
						{
							continue;
						}
						const FVector2D B = Node(Next[k].X, Next[k].Y);
						const double Reach = Margin + FVector2D::Distance(A, B) * 0.2;
						const FVector2D Lo(FMath::Min(A.X, B.X) - Reach, FMath::Min(A.Y, B.Y) - Reach);
						const FVector2D Hi(FMath::Max(A.X, B.X) + Reach, FMath::Max(A.Y, B.Y) + Reach);
						if (Hi.X >= Min.X && Lo.X <= Max.X && Hi.Y >= Min.Y && Lo.Y <= Max.Y)
						{
							Out.Add({ A, B, Width, bRoad, Hash(CX, CY, Seed + 7 + k) });
						}
					}
				}
			}
		};
		// Inside Council Bluffs the real mapped streets and trails replace the generated ones.
		if (!RealPlace::Covers(Min, Max, Margin))
		{
			Gather(RoadCellSize(), true, 0.9f, 420.0f, 940, &RoadNode);
			Gather(TrailCellSize, false, 0.45f, 150.0f, 950, &TrailNode);
		}
		RealPlace::AppendPaths(Min, Max, Margin, Out);
	}

	FVector2D PathPoint(const FPath& Path, float T)
	{
		if (Path.bStraight)
		{
			return FMath::Lerp(Path.A, Path.B, T);
		}
		const FVector2D Along = Path.B - Path.A;
		const float Length = Along.Size();
		const FVector2D Side(-Along.Y / Length, Along.X / Length);
		// Two scales of wander, pinned to zero at both ends so paths meet their nodes.
		const float Phase = (Path.Seed & 1023) * 0.37f;
		// Long, lazy curves: a bend takes kilometres, never turns more than ~20 degrees, and vanishes at
		// both ends so roads leave their junctions straight.
		const float Wander = FMath::PerlinNoise1D(T * Length / 180000.0f + Phase) * 0.03f + FMath::PerlinNoise1D(T * Length / 55000.0f + Phase * 2.0f) * 0.003f;
		return Path.A + Along * T + Side * Wander * Length * FMath::Sin(T * PI);
	}

	float DistanceToPath(const TArray<FPath>& Paths, const FVector2D& P, float& OutWidth)
	{
		float Best = TNumericLimits<float>::Max();
		OutWidth = 0.0f;
		for (const FPath& Path : Paths)
		{
			const FVector2D Along = Path.B - Path.A;
			const float Length = Along.Size();
			// Quick reject against the straight line plus the widest wander.
			const float T = FMath::Clamp(FVector2D::DotProduct(P - Path.A, Along) / (Length * Length), 0.0f, 1.0f);
			if (FVector2D::Distance(P, Path.A + Along * T) > Length * 0.15f + Best)
			{
				continue;
			}
			// Refine around the nearest parameter on the meandering curve.
			float Lo = FMath::Max(0.0f, T - 0.2f), Hi = FMath::Min(1.0f, T + 0.2f);
			for (int32 Pass = 0; Pass < 3; ++Pass)
			{
				float BestT = Lo, BestD = TNumericLimits<float>::Max();
				for (int32 i = 0; i <= 8; ++i)
				{
					const float S = FMath::Lerp(Lo, Hi, i / 8.0f);
					const float D = FVector2D::DistSquared(P, PathPoint(Path, S));
					if (D < BestD) { BestD = D; BestT = S; }
				}
				const float Span = (Hi - Lo) / 8.0f;
				Lo = FMath::Max(0.0f, BestT - Span);
				Hi = FMath::Min(1.0f, BestT + Span);
				if (Pass == 2 && FMath::Sqrt(BestD) < Best)
				{
					Best = FMath::Sqrt(BestD);
					OutWidth = Path.Width;
				}
			}
		}
		return Best;
	}

	double VolcanoCellSize() { return 4000000.0; }

	bool FindVolcano(int32 CX, int32 CY, FVector2D& OutCenter, float& OutRadius, float& OutHeight)
	{
		if (HashFloat(CX, CY, 900) > 0.35f)
		{
			return false;
		}
		// Keep the cone well inside its cell so only one cell ever needs checking.
		OutCenter = FVector2D((CX + FMath::Lerp(0.25f, 0.75f, HashFloat(CX, CY, 901))) * VolcanoCellSize(),
			(CY + FMath::Lerp(0.25f, 0.75f, HashFloat(CX, CY, 902))) * VolcanoCellSize());
		OutRadius = FMath::Lerp(350000.0f, 700000.0f, HashFloat(CX, CY, 903));
		OutHeight = FMath::Lerp(120000.0f, 250000.0f, HashFloat(CX, CY, 904));
		return true;
	}

	FLinearColor RockColor() { return Srgb(0x77706A); }
	FLinearColor SnowColor() { return Srgb(0xF2F4FA); }

	uint32 Hash(int32 X, int32 Y, uint32 Seed)
	{
		uint32 H = Seed * 0x9E3779B9u;
		H ^= static_cast<uint32>(X) * 0x85EBCA6Bu;
		H = (H << 13) | (H >> 19);
		H ^= static_cast<uint32>(Y) * 0xC2B2AE35u;
		H ^= H >> 16;
		H *= 0x7FEB352Du;
		H ^= H >> 15;
		H *= 0x846CA68Bu;
		H ^= H >> 16;
		return H;
	}

	float HashFloat(int32 X, int32 Y, uint32 Seed)
	{
		return (Hash(X, Y, Seed) >> 8) * (1.0f / 16777216.0f);
	}

	FWorldSample Sample(double WorldX, double WorldY)
	{
		// Work in metres.
		const double X = WorldX * 0.01;
		const double Y = WorldY * 0.01;

		FWorldSample Out;

		// Continents and oceans (~40 km features).
		const float Continent = Fbm(X / 42000.0, Y / 42000.0, 4, OffContinents) * 1.6f;
		const float Land = Smooth(-0.08f, 0.12f, Continent);
		const float Base = FMath::Lerp(-70.0f, 6.0f, Land) + FMath::Max(Continent, 0.0f) * 60.0f * Land;

		// Rolling hills.
		const float Hills = Fbm(X / 1500.0, Y / 1500.0, 5, OffHills) * FMath::Lerp(3.0f, 45.0f, Land);

		// Mountain ranges where a slow mask allows them.
		const float Ranges = Smooth(0.05f, 0.45f, Fbm(X / 22000.0, Y / 22000.0, 3, OffRanges) * 1.6f) * Land;
		const float Ridge = Ridged(X / 5000.0, Y / 5000.0, 5, OffRidges);
		const float Mountains = Ridge * Ridge * 1100.0f * Ranges;

		float H = Base + Hills + Mountains;

		// Volcanoes: a broad cone with a crater, rising from land or straight out of the sea.
		float VolcanoRim = 0.0f;
		{
			const int32 CX = FMath::FloorToInt(WorldX / VolcanoCellSize());
			const int32 CY = FMath::FloorToInt(WorldY / VolcanoCellSize());
			FVector2D Center;
			float Radius, Peak;
			if (FindVolcano(CX, CY, Center, Radius, Peak))
			{
				const float D = FVector2D::Distance(Center, FVector2D(WorldX, WorldY)) / Radius;
				if (D < 1.0f)
				{
					const float Cone = FMath::Pow(1.0f - D, 1.6f) * Peak * 0.01f;
					const float Crater = Smooth(0.14f, 0.05f, D) * Peak * 0.01f * 0.35f;
					// Lava-carved gullies running down the flanks, deepest mid-slope.
					const float Gullies = (Ridged(X / 450.0, Y / 450.0, 3, OffRidges) - 0.55f) * 70.0f * FMath::Sin(D * PI);
					H = FMath::Max(H, H * D + Cone - Crater + Gullies);
					VolcanoRim = Smooth(0.85f, 0.35f, D);
				}
			}
		}

		// Climate (~20-35 km zones), cooled by altitude.
		float Temperature = 0.5f + Fbm(X / 34000.0, Y / 34000.0, 3, OffTemperature) * 0.9f;
		const float Moisture = FMath::Clamp(0.5f + Fbm(X / 22000.0, Y / 22000.0, 3, OffMoisture) * 0.9f, 0.0f, 1.0f);
		Temperature = FMath::Clamp(Temperature - FMath::Max(H, 0.0f) / 2600.0f * 0.45f, 0.0f, 1.0f);

		const float Hot = Smooth(0.56f, 0.72f, Temperature);
		const float Cold = Smooth(0.36f, 0.22f, Temperature);
		const float Mild = FMath::Max(0.0f, 1.0f - Hot - Cold);
		const float Dry = Smooth(0.46f, 0.30f, Moisture);
		const float Wet = Smooth(0.56f, 0.72f, Moisture);
		const float Mid = FMath::Max(0.0f, 1.0f - Dry - Wet);

		// Deserts get dunes, and mesas where ranges would be.
		const float Desertness = Hot * Dry * Land;
		if (Desertness > 0.0f)
		{
			H += Ridged(X / 260.0, Y / 140.0, 2, OffDunes) * 14.0f * Desertness;
			const float Step = 35.0f;
			const float Terraced = FMath::FloorToFloat(H / Step) * Step + Smooth(0.8f, 1.0f, FMath::Frac(H / Step)) * Step;
			H = FMath::Lerp(H, Terraced, Desertness * Ranges * 0.9f);
		}

		const FBiomeInfo Biomes[] = {
			{ EBiome::Desert,    Srgb(0xD9B27A), 0.015f, 0.0f },
			{ EBiome::Savanna,   Srgb(0xA89A4E), 0.06f,  0.0f },
			{ EBiome::Jungle,    Srgb(0x2F5A22), 0.90f,  0.5f },
			{ EBiome::Grassland, Srgb(0x7E9A45), 0.04f,  0.1f },
			{ EBiome::Forest,    Srgb(0x4A6E2E), 0.80f,  0.2f },
			{ EBiome::Tundra,    Srgb(0x8C8A74), 0.01f,  0.1f },
			{ EBiome::Taiga,     Srgb(0x44583A), 0.75f,  0.2f },
		};
		const float Weights[] = {
			Hot * Dry,
			Hot * Mid,
			Hot * Wet,
			Mild * Dry + Mild * Mid * 0.5f,
			Mild * Mid * 0.5f + Mild * Wet,
			Cold * Dry,
			Cold * (Mid + Wet),
		};

		FLinearColor Color = FLinearColor::Black;
		float TotalWeight = 0.0f;
		float BestWeight = -1.0f;
		const FBiomeInfo* Best = &Biomes[0];
		for (int32 i = 0; i < UE_ARRAY_COUNT(Biomes); ++i)
		{
			Color += Biomes[i].Color * Weights[i];
			TotalWeight += Weights[i];
			Out.TreeDensity += Biomes[i].TreeDensity * Weights[i];
			Out.Wetness += Biomes[i].Wetness * Weights[i];
			if (Weights[i] > BestWeight)
			{
				BestWeight = Weights[i];
				Best = &Biomes[i];
			}
		}
		Color /= TotalWeight;
		Out.TreeDensity /= TotalWeight;
		Out.Wetness /= TotalWeight;
		Out.Biome = Best->Biome;

		// Snow in the cold and on high peaks.
		const float Snow = Smooth(0.16f, 0.06f, Temperature);
		Color = FMath::Lerp(Color, SnowColor(), Snow);
		Out.TreeDensity *= 1.0f - Smooth(0.3f, 0.9f, Snow);
		if (Snow > 0.5f)
		{
			Out.Biome = EBiome::Snow;
			Out.Wetness = 0.3f;
		}

		// Beaches along the shore, sea floor below it.
		const float Beach = Smooth(3.5f, 0.5f, H) * (1.0f - Ranges);
		Color = FMath::Lerp(Color, Srgb(0xE3D3A6), Beach);
		Out.TreeDensity *= 1.0f - Beach;
		if (Beach > 0.5f)
		{
			Out.Biome = EBiome::Beach;
			Out.Wetness = FMath::Max(Out.Wetness, Smooth(2.0f, 0.0f, H) * 0.7f);
		}
		if (H < 0.0f)
		{
			Color = FMath::Lerp(Srgb(0xB9AE86), Srgb(0x3F5A56), Smooth(0.0f, -25.0f, H));
			Out.Biome = EBiome::Ocean;
			Out.TreeDensity = 0.0f;
			Out.Wetness = 1.0f;
		}

		// Material layers for the photoreal terrain.
		Out.Dryness = FMath::Clamp(Smooth(0.55f, 0.25f, Moisture) * Smooth(0.35f, 0.65f, Temperature) + Hot * Mid * 0.4f, 0.0f, 1.0f);
		Out.Forest = FMath::Clamp((Weights[2] + Weights[4] + Weights[6]) / TotalWeight, 0.0f, 1.0f) * (1.0f - Beach) * (1.0f - Snow);
		Out.Sand = FMath::Max(FMath::Max(Desertness * (1.0f - Ranges), Beach), H < 0.0f ? 1.0f : 0.0f);
		Out.Snow = Snow;
		Out.Rock = FMath::Max(Smooth(0.55f, 0.85f, Ridge) * Ranges, VolcanoRim);
		Out.Volcano = VolcanoRim;
		if (VolcanoRim > 0.0f)
		{
			// Dark, barren volcanic rock: no sandstone, sparse growth, snow only right at the summit.
			Out.TreeDensity *= 1.0f - VolcanoRim;
			Out.Dryness *= 1.0f - VolcanoRim;
			Out.Snow *= Smooth(0.7f, 0.95f, VolcanoRim);
		}

		Out.Height = H * 100.0f;
		Out.Temperature = Temperature;
		Out.Moisture = Moisture;
		Out.Land = Land;
		Out.Mountains = Ranges;
		Out.Color = Color;
		RealPlace::Apply(WorldX, WorldY, Out);
		return Out;
	}
}
