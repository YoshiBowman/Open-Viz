#include "LvSceneryActor.h"

#include "Components/SceneComponent.h"

ALvSceneryActor::ALvSceneryActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Movable);
}
