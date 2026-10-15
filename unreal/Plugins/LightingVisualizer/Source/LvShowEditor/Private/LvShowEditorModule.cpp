#include "Editor.h"
#include "LevelEditorViewport.h"
#include "LvShowSubsystem.h"
#include "Modules/ModuleManager.h"

class FLvShowEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// When a show has been laid out in the level, look at it.
		Handle = ULvShowSubsystem::OnReadyToEdit().AddLambda([](const FBox& Rig)
		{
			if (GEditor)
			{
				// From the house and a little above, as the visualizer first shows it: the
				// audience is on the +Y side, looking toward -Y.
				for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
				{
					if (Client && Client->IsPerspective())
					{
						Client->SetViewRotation(FRotator(-8.0, -90.0, 0.0));
					}
				}
				GEditor->MoveViewportCamerasToBox(Rig, false);
			}
		});
	}

	virtual void ShutdownModule() override
	{
		ULvShowSubsystem::OnReadyToEdit().Remove(Handle);
	}

private:
	FDelegateHandle Handle;
};

IMPLEMENT_MODULE(FLvShowEditorModule, LvShowEditor);
