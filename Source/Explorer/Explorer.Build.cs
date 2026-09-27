// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Explorer : ModuleRules
{
	public Explorer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencies.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"Niagara",
			"ProceduralMeshComponent"
		});

		PrivateDependencies.AddRange(new string[] {
		});

		// Enable for procedural generation
		PublicIncludePaths.AddRange(new string[] {
			System.IO.Path.Combine(ModuleDirectory, "Public/Procedural"),
			System.IO.Path.Combine(ModuleDirectory, "Public/Flight")
		});
	}
}
