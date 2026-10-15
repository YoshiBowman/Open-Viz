#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LvSceneryActor.generated.h"

/**
 * Something on stage that is not a fixture, as a rig file (MVR) describes it: a stage deck, a
 * truss, a set piece. It is drawn with the model files the rig packs for it, takes light, and
 * stops beams where they land on it. It does nothing else: it is rebuilt from the file each time
 * the show is opened, and moving it in the editor is not kept. A set piece added in Openviz (a
 * deck, a drape, a truss) is one too, with a shape made by the core (vizcore/pieces.h).
 */
UCLASS()
class LVFIXTURES_API ALvSceneryActor : public AActor
{
	GENERATED_BODY()

public:
	ALvSceneryActor();

	/** What the rig file calls it, what kind of thing it is, and where the file files it. For reading. */
	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString SceneryName;

	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString Kind;

	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString Layer;

	UPROPERTY(VisibleAnywhere, Category = "Openviz")
	FString Class;

	/** For a set piece added in Openviz: the id it has in the show's pieces file. Empty for what the rig file brought. */
	FString PieceId;
};
