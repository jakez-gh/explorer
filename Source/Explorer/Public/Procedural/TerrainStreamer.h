#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Containers/Queue.h"
#include "Tasks/Task.h"
#include "TerrainStreamer.generated.h"

class UProceduralMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;

/**
 * Endless procedural world. Keeps a disc of square chunks around the player: full detail and
 * collision up close, coarser terrain in the distance, vegetation and small settlements nearby,
 * and large landmarks (cities, lighthouses, volcanoes, floating islands) everywhere in view.
 * Everything is a pure function of world position (see WorldGen), so chunks line up seamlessly
 * and every place is the same each time you return.
 */
UCLASS()
class EXPLORER_API ATerrainStreamer : public AActor
{
	GENERATED_BODY()

public:
	ATerrainStreamer();

	// Quads per chunk side at full detail.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain", meta = (ClampMin = "8"))
	int32 ChunkResolution = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float GridSpacing = 500.0f;

	// Radius, in chunks, of the disc kept loaded around the player.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	int32 ViewRadius = 17;

	// Chunks within this radius get vegetation, rocks and small settlements.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	int32 DetailRadius = 4;

	// Chunks within this radius get scanned trees (beyond the detail radius, trees only).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	int32 TreeRadius = 15;

	// Chunks within this radius get collision.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	int32 CollisionRadius = 2;

	// Milliseconds per frame spent uploading finished chunks to the renderer (generation runs on worker threads).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	float BuildBudgetMs = 6.0f;

	// Beyond the chunk disc, coarse 4x4-chunk tiles carry the land out to this radius (in chunks).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	int32 FarRadius = 48;

	// Blades of grass are planted within this distance of the viewer.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Grass")
	float GrassRadius = 8000.0f;

	// No grass is built when the viewer is higher than this above the ground (it couldn't be seen).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Grass")
	float GrassMaxViewHeight = 25000.0f;

	// Generation jobs allowed on worker threads at once.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	int32 MaxJobsInFlight = 10;

	// How far ahead (seconds of travel) streaming prioritises along the direction of flight.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	float LookAheadSeconds = 2.5f;

	// Near chunks still waiting to be built, 0..1 (1 = badly behind). The flight pawn eases off when high.
	float GetStreamingBacklog() const { return Backlog; }

	// Terrain surface height in world space at a world XY position.
	UFUNCTION(BlueprintCallable, Category = "Terrain")
	float GetHeightAtLocation(FVector2D WorldXY) const;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaTime) override;

	// Landmark placement, shared by generation and the -BiomeReport finder. Each returns false if the
	// region cell has none; otherwise the landmark's centre (islands: the top surface, in the air).
	static bool FindVillage(int32 CellX, int32 CellY, FVector& OutCenter);
	static bool FindCity(int32 CellX, int32 CellY, FVector& OutCenter);
	static bool FindFloatingIsland(int32 CellX, int32 CellY, FVector& OutTop, float& OutRadius);
	static bool FindLighthouse(int32 CellX, int32 CellY, FVector& OutBase);
	static bool FindStoneCircle(int32 CellX, int32 CellY, FVector& OutCenter);
	static bool FindRuin(int32 CellX, int32 CellY, FVector& OutCenter);
	static bool FindObelisks(int32 CellX, int32 CellY, FVector& OutCenter);
	static bool FindWorldTree(int32 CellX, int32 CellY, FVector& OutBase);
	static bool FindSkyGate(int32 CellX, int32 CellY, FVector& OutCenter);
	static bool FindWindFarm(int32 CellX, int32 CellY, FVector& OutCenter);
	static double RuinCellSize();
	static double ObeliskCellSize();
	static double WorldTreeCellSize();
	static double SkyGateCellSize();
	static double WindFarmCellSize();
	static double VillageCellSize();
	static double CityCellSize();
	static double IslandCellSize();
	static double LighthouseCellSize();
	static double StoneCircleCellSize();

private:
	enum EPropPart : uint8
	{
		Cylinder,
		Cone,
		Sphere,
		Cube,
		Rock,
		Bush,
		Trunk, // invisible collider around tree trunks
		IslandBush, // foliage on floating islands, which never fades with distance
		BuildSurf0, // house walls, floors, roofs... one per HouseGen::ESurface, photoscanned materials
		Furniture0 = BuildSurf0 + 13, // one per HouseGen::EFurniture, CC0 scanned furniture
		Tree0 = Furniture0 + 17, Tree1, Tree2, Tree3, Tree4, Tree5, Tree6, Tree7, // runtime-built tree wood meshes
		IslandTree, // broadleaf wood that never fades, for floating islands
		Island0, Island1, Island2, Island3, // runtime-built floating island meshes
		Scanned0, // photoscanned Megascans trees (Nanite), loaded if the packs are installed
		NumParts = Scanned0 + 18
	};
	static constexpr int32 NumScannedTrees = NumParts - Scanned0;

	// Surface selector for M_Prop (custom data 4).
	enum ESurface : uint8
	{
		SurfRock,
		SurfConcrete,
		SurfBrick,
		SurfSlate,
		SurfGlass,
		SurfGrass,
	};

	enum ETreeVariant : int32
	{
		TreeBroadleafA,
		TreeBroadleafB,
		TreeBroadleafC,
		TreeConiferA,
		TreeConiferB,
		TreeAcacia,
		TreeJungle,
		TreeBirch,
		NumTreeVariants
	};

	/** A generated tree: where its branch tips are (foliage goes there) and its trunk size (for collision). */
	struct FTreeTemplate
	{
		struct FTip
		{
			FVector Position;
			float Size;
		};
		TArray<FTip> Tips;
		float TrunkRadius = 30.0f;
		float TrunkHeight = 500.0f;
	};

	UStaticMesh* CreateTreeMesh(ETreeVariant Variant, FTreeTemplate& OutTemplate);

	// Floating islands are unit meshes (radius 100) scaled per instance. Top surface height at a
	// local position (radius-100 units), for planting trees on it.
	static constexpr int32 NumIslandVariants = 4;
	static float IslandTopHeight(int32 Variant, float LocalX, float LocalY);
	UStaticMesh* CreateIslandMesh(int32 Variant);

	enum class EProps : uint8
	{
		None,
		Landmarks,
		Trees, // landmarks plus scanned trees (cheap with Nanite), for the middle distance
		Full,  // plus trees, villages and house exteriors (real-place houses as empty shells)
		Shell, // within two chunks: real-place houses get walls, windows and roofs (no rooms)
		Near,  // plus furnished interiors: only the chunk the viewer is in
	};

	// Loaded scanned trees: height (cm) per slot; 0 if not available.
	float ScannedTreeHeight[NumScannedTrees] = {};
	int32 NumLoadedScanned = 0;
	// Loaded slots split into tall mature trees (the canopy) and young understory trees.
	TArray<int32> MatureSlots;
	TArray<int32> YoungSlots;

	struct FChunk
	{
		UProceduralMeshComponent* Mesh = nullptr;
		// Roads and trails laid on the terrain (full-detail chunks only).
		UProceduralMeshComponent* PathMesh = nullptr;
		UInstancedStaticMeshComponent* Parts[NumParts] = {};
		int32 Step = 0;
		bool bHasCollision = false;
		// Whether this chunk's solid props (trunks, rocks, buildings) currently collide.
		bool bPropCollision = false;
		EProps Props = EProps::None;
		// Chunk the viewer was in when this chunk's terrain was built (for geomorph refreshes).
		FIntPoint BuiltCenter = FIntPoint(MAX_int32, MAX_int32);
	};

	/** Terrain surface arrays, built off the game thread. */
	struct FSurfaceData
	{
		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0;
		TArray<FVector2D> UV1;
		TArray<FColor> Colors;
		TArray<int32> Triangles;
	};

	enum class EJobKind : uint8
	{
		Chunk,
		Far,
		Grass,
	};

	/** A finished background job, waiting to be uploaded on the game thread. */
	struct FJobResult
	{
		EJobKind Kind = EJobKind::Chunk;
		FIntPoint Coord;
		int32 Step = 0;
		bool bCollision = false;
		EProps Props = EProps::None;
		FIntPoint BuiltCenter;
		bool bSurface = false;
		bool bProps = false;
		FSurfaceData Surface;
		FSurfaceData Paths;
		TArray<FTransform> Transforms[NumParts];
		TArray<float> CustomData[NumParts];
	};

	/** Instances collected for one chunk before they're pushed to its components. */
	struct FPropBatch
	{
		struct FInstance
		{
			FVector Center;
			FRotator Rotation;
			FVector Size;
		};
		TArray<FInstance> Instances[NumParts];
		TArray<float> CustomData[NumParts];

		// Size is the full extent in cm; the part's mesh is scaled to fit and centred on Center.
		void Add(EPropPart Part, const FVector& Center, const FRotator& Rotation, const FVector& SizeCm, const FLinearColor& Color, float Glow = 0.0f, ESurface Surface = SurfRock);
	};

	float ChunkWorldSize() const { return ChunkResolution * GridSpacing; }
	FIntPoint WorldToChunk(const FVector& Location) const;
	int32 StepForDistance(float DistanceInChunks) const;

	// Computes an N x N quad terrain patch with skirts at Origin (its Z offsets the whole patch). Vertices
	// between MorphStart and MorphEnd (world distance from MorphCenter) blend towards the next-coarser
	// grid, so each detail ring meets the next without a visible step. Thread safe.
	void ComputeSurface(FSurfaceData& Out, const FVector& Origin, int32 N, float Spacing, const FVector2D& MorphCenter = FVector2D::ZeroVector, float MorphStart = 0.0f, float MorphEnd = 0.0f) const;
	void ApplySurface(UProceduralMeshComponent* Mesh, const FVector& Origin, FSurfaceData& Data, bool bWithCollision, UMaterialInterface* Material) const;
	void ComputePaths(const FIntPoint& Coord, FSurfaceData& Out) const;
	void ComputeProps(const FIntPoint& Coord, EProps Level, FJobResult& Out) const;
	void ComputeGrassTile(const FIntPoint& Tile, TArray<FTransform>& Transforms, TArray<float>& Data) const;
	void ApplyChunk(FJobResult& Result, const FIntPoint& Center);
	// Ring edge (in chunks) where a chunk of this step hands over to the next-coarser one.
	float RingEnd(int32 Step) const;
	void LaunchJob(TFunction<void(FJobResult&)>&& Work, EJobKind Kind, const FIntPoint& Coord);
	void UpdateFarTerrain(const FIntPoint& Center, double Deadline);
	// Height of the rendered full-detail terrain surface (matches its triangles exactly).
	float SurfaceHeight(double X, double Y) const;
	void UpdateGrass(const FVector& ViewLocation, double Deadline);
	UStaticMesh* CreateGrassClumpMesh();
	static void SetInstances(UInstancedStaticMeshComponent* Component, const TArray<FTransform>& Transforms, const TArray<float>& CustomData);
	void AddVegetation(const FIntPoint& Coord, FPropBatch& Batch, bool bTreesOnly) const;
	void AddVillages(const FIntPoint& Coord, FPropBatch& Batch) const;
	void AddStoneCircles(const FIntPoint& Coord, FPropBatch& Batch) const;
	void AddDiscoveries(const FIntPoint& Coord, FPropBatch& Batch) const;
	void AddRealBuildings(const FIntPoint& Coord, FPropBatch& Batch, EProps Level) const;
	void AddCities(const FIntPoint& Coord, FPropBatch& Batch) const;
	void AddLighthouses(const FIntPoint& Coord, FPropBatch& Batch) const;
	void AddVolcanoGlow(const FIntPoint& Coord, FPropBatch& Batch) const;
	void AddFloatingIslands(const FIntPoint& Coord, FPropBatch& Batch) const;
	bool ChunkContains(const FIntPoint& Coord, const FVector& Location) const;

	void ReleaseChunk(FChunk& Chunk);
	UProceduralMeshComponent* AcquireMesh();
	UInstancedStaticMeshComponent* AcquirePart(EPropPart Part);

	FTreeTemplate TreeTemplates[NumTreeVariants];

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> BarkMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PathMaterial;

	TMap<FIntPoint, FChunk> Chunks;

	// Background generation.
	TQueue<TSharedPtr<FJobResult, ESPMode::ThreadSafe>, EQueueMode::Mpsc> Results;
	TArray<UE::Tasks::FTask> Tasks;
	TSet<FIntPoint> ChunkJobs;
	TSet<FIntPoint> FarJobs;
	TSet<FIntPoint> GrassJobs;
	FVector LastViewLocation = FVector::ZeroVector;
	float Backlog = 0.0f;
	FVector ViewVelocity = FVector::ZeroVector;
	// Bounds of each part's mesh, cached so workers can size instances without touching UObjects.
	TArray<FBox> PartBounds;
	TMap<FIntPoint, UProceduralMeshComponent*> FarTiles;
	TMap<FIntPoint, UHierarchicalInstancedStaticMeshComponent*> GrassTiles;
	TArray<UHierarchicalInstancedStaticMeshComponent*> GrassPool;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> GrassMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> GrassMaterial;

	UPROPERTY(VisibleAnywhere, Category = "Terrain")
	TObjectPtr<UStaticMeshComponent> Ocean;

	UPROPERTY(EditAnywhere, Category = "Terrain|Assets")
	TObjectPtr<UMaterialInterface> TerrainMaterial;

	// Materials and meshes indexed by EPropPart.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> PartMaterials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> PartMeshes;

	// Recycled components, so streaming doesn't churn UObjects.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> MeshPool;

	TArray<UInstancedStaticMeshComponent*> PartPools[NumParts];

	// Every prop component ever created, pooled or live; keeps them referenced for GC.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> AllParts;
};
