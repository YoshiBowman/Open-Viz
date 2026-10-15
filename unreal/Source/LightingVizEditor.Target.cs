using UnrealBuildTool;

public class LightingVizEditorTarget : TargetRules
{
	public LightingVizEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("LightingViz");
	}
}
