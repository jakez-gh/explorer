// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Explorer : ModuleRules
{
	public Explorer(ReadOnlyTargetRules Target) : base(Target)
	{
		// UE 5.4 headers fail on MSVC 14.40+ (which ships sanitizer/asan_interface.h): ConcurrentLinearAllocator.h
		// then evaluates clang's __has_feature, tripping C4668/C4067. Disable that path for this module, and skip the
		// engine's shared PCH, which is compiled without this definition.
		PCHUsage = PCHUsageMode.NoPCHs;
		PrivateDefinitions.Add("PLATFORM_HAS_ASAN_INCLUDE=0");

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"Slate",
			"SlateCore",
			"ProceduralMeshComponent",
			"MeshDescription",
			"StaticMeshDescription"
		});
	}
}
