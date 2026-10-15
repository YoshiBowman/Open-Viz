using UnrealBuildTool;

// The project module is deliberately empty: everything lives in the LightingVisualizer plugin,
// so it can be moved to another project or packaged on its own.
public class LightingViz : ModuleRules
{
	public LightingViz(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
	}
}
