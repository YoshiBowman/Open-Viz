using System.IO;
using UnrealBuildTool;

// Compiles the standalone core (core/ at the repository root) into the plugin. The core has no
// Unreal dependencies; this module only gives its sources a place in the Unreal build, with the
// same definitions core/CMakeLists.txt sets.
public class VizCore : ModuleRules
{
	public VizCore(ReadOnlyTargetRules Target) : base(Target)
	{
		// The core's sources must not see Unreal's headers or macros, and each keeps file-local
		// helpers that would collide in a unity build.
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;

		string CoreDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", "..", "..", "core"));
		PublicIncludePaths.Add(Path.Combine(CoreDir, "include"));
		PrivateIncludePaths.AddRange(new string[]
		{
			Path.Combine(CoreDir, "src"),
			Path.Combine(CoreDir, "third_party", "pugixml"),
			Path.Combine(CoreDir, "third_party", "miniz"),
		});
		PrivateDefinitions.AddRange(new string[]
		{
			"PUGIXML_NO_EXCEPTIONS",
			"PUGIXML_NO_XPATH",
			"MINIZ_NO_STDIO",
			"MINIZ_NO_ZLIB_COMPATIBLE_NAMES",
		});

		PublicDependencyModuleNames.Add("Core");
	}
}
