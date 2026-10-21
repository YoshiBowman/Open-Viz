using UnrealBuildTool;

// Opens a show: an MVR rig, or one fixture type to look at, with live DMX. This is what the
// launcher starts. Runtime only: no editor modules, so it runs the same in -game and packaged.
public class LvShow : ModuleRules
{
	public LvShow(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new string[] { "LvFixtures", "VizCore", "InputCore", "Json", "Slate", "SlateCore",
			"MovieSceneCapture", "ImageWrapper", "RenderCore", "RHI",    // video: the engine's frame grabber, and JPEG
			"Sockets" });    // PosiStageNet, as Openviz passes it on
	}
}
