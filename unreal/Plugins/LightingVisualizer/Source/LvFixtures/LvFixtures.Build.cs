using UnrealBuildTool;

// Roadmap phase 2: fixtures built at runtime from GDTF data, driven by DMX.
// Runtime only: no editor modules.
public class LvFixtures : ModuleRules
{
	public LvFixtures(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// VizCore is public because this module's headers expose the core's types.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "VizCore" });
		PrivateDependencyModuleNames.AddRange(new string[] { "DMXProtocol", "MeshDescription", "StaticMeshDescription", "MediaAssets" });
	}
}
