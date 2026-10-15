using UnrealBuildTool;

// Conveniences for editing a show in the Unreal Editor that need the editor's own code, such as
// pointing the viewport at the rig. Editor only: nothing in the visualizer depends on this, and
// the runtime modules do not depend on any editor module.
public class LvShowEditor : ModuleRules
{
	public LvShowEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "UnrealEd", "LvShow" });
	}
}
