// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ExplorerEditorTarget : TargetRules
{
	public ExplorerEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_4;
		ExtraModuleNames.Add("Explorer");
	}
}
