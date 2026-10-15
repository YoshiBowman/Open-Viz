#include "LvGate.h"

#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "LvFixtureActor.h"
#include "Materials/MaterialInstanceDynamic.h"

DEFINE_LOG_CATEGORY_STATIC(LogLvGate, Log, All);

namespace
{
	const FName& RowName(int32 Row)
	{
		static TArray<FName> Names;
		if (Names.Num() == 0)
		{
			Names = {TEXT("Layer0A"), TEXT("Layer0B"), TEXT("Layer1A"), TEXT("Layer1B"), TEXT("Shape"), TEXT("Shape2"), TEXT("Turn"),
			         TEXT("Blade0"), TEXT("Blade1"), TEXT("Blade2"), TEXT("Blade3")};
			for (int32 Facet = 0; Facet < 16; ++Facet)
			{
				Names.Add(FName(*FString::Printf(TEXT("Facet%02d"), Facet)));
			}
		}
		return Names[Row];
	}

	const FName TextureNames[2] = {TEXT("Gobo0"), TEXT("Gobo1")};

	void SetConsoleVariable(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Variable->Set(Value, ECVF_SetByCode);
		}
	}
}

FLvGateLook::FLvGateLook()
{
	FMemory::Memzero(this, sizeof(FLvGateLook));
	// As the materials' own defaults: nothing in the beam.
	Set(Layer0A, 1.0, 0.0, 0.0, 1.0);
	Set(Layer1A, 1.0, 0.0, 0.0, 1.0);
	Set(Shape, 4.0, 0.02, 0.0, 1.0);
	Set(Shape2, 0.6, 1.0, 0.0, 0.0);
	Set(Turn, 1.0, 0.0, 0.0, 0.0);
	for (int32 Blade = 0; Blade < 4; ++Blade)
	{
		Set(Blade0 + Blade, 0.0, 1.0, 2.0, 0.0);
	}
	for (int32 Facet = 0; Facet < 16; ++Facet)
	{
		Set(Facet0 + Facet, 0.0, 0.0, 1.0, 1.0);
	}
}

void FLvGateLook::Set(int32 Row, double X, double Y, double Z, double W)
{
	const double In[4] = {X, Y, Z, W};
	for (int32 Index = 0; Index < 4; ++Index)
	{
		// Adding zero turns a negative zero into a plain one: the two compare equal as numbers but not as bytes.
		Values[Row][Index] = static_cast<float>(FMath::RoundToDouble(In[Index] * 4096.0) / 4096.0) + 0.0f;
	}
}

bool ULvGateSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::Editor;
}

TStatId ULvGateSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(ULvGateSubsystem, STATGROUP_Tickables);
}

void ULvGateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// The engine's atlas of light functions, at its largest: 256 looks of 256 pixels, in colour
	// (its default is 16 looks of 128 pixels in grey). It is only filled as far as it is used.
	SetConsoleVariable(TEXT("r.LightFunctionAtlas.Size"), 16);
	SetConsoleVariable(TEXT("r.LightFunctionAtlas.SlotResolution"), 256);
	SetConsoleVariable(TEXT("r.LightFunctionAtlas.Format"), 1);
	// Without MegaLights too, so that both ways of lighting show the same pattern.
	SetConsoleVariable(TEXT("r.Deferred.UsesLightFunctionAtlas"), 1);
}

int32 ULvGateSubsystem::GetLookCount() const
{
	return Entries.Num() - Idle.Num();
}

void ULvGateSubsystem::Request(ALvFixtureActor* Fixture, int32 Beam, const FLvGateLook& Look, bool bOpen)
{
	FRequest& Added = Requests.AddDefaulted_GetRef();
	Added.Fixture = Fixture;
	Added.Beam = Beam;
	Added.Serial = Fixture ? Fixture->GetGateSerial() : 0;
	Added.bOpen = bOpen;
	Added.Look = Look;
}

void ULvGateSubsystem::Release(int32 Handle)
{
	if (!Entries.IsValidIndex(Handle) || Entries[Handle].Users <= 0)
	{
		return;
	}
	if (--Entries[Handle].Users == 0)
	{
		// It stays filed under its look: whoever asks for the same again gets it back as it is.
		Idle.Add(Handle);
	}
}

int32 ULvGateSubsystem::Acquire(const FLvGateLook& Look)
{
	if (!bBasesTried)
	{
		bBasesTried = true;
		LightBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/LightingVisualizer/Materials/M_LvGate.M_LvGate"));
		BeamBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/LightingVisualizer/Materials/M_LvBeamGate.M_LvBeamGate"));
		if (!LightBase || !BeamBase)
		{
			UE_LOG(LogLvGate, Warning, TEXT("The gate materials are missing (tools/build_materials.py builds them); gobos, prisms, irises and blades will not show"));
		}
	}
	if (!LightBase || !BeamBase)
	{
		return INDEX_NONE;
	}

	int32 Handle = INDEX_NONE;
	if (const int32* Known = ByLook.Find(Look))
	{
		Handle = *Known;
		if (Entries[Handle].Users == 0)
		{
			Idle.RemoveSingleSwap(Handle, EAllowShrinking::No);
		}
	}
	else if (Idle.Num() > 0)
	{
		// Nobody shows this yet: an idle pair of materials is given the new look. The one freed
		// last is taken, which for a look that changes every frame is the one it just had.
		Handle = Idle.Pop(EAllowShrinking::No);
		ByLook.Remove(Entries[Handle].Look);
		Entries[Handle].Look = Look;
		ByLook.Add(Look, Handle);
	}
	else
	{
		Handle = Entries.AddDefaulted();
		FEntry& Made = Entries[Handle];
		Made.Look = Look;
		Made.Material = Materials.Num();
		Materials.Add(UMaterialInstanceDynamic::Create(LightBase, this));
		Materials.Add(UMaterialInstanceDynamic::Create(BeamBase, this));
		ByLook.Add(Look, Handle);
	}

	FEntry& Entry = Entries[Handle];
	++Entry.Users;
	if (!Entry.bApplied || Entry.Applied != Entry.Look)
	{
		UMaterialInstanceDynamic* Pair[2] = {Materials[Entry.Material], Materials[Entry.Material + 1]};
		for (UMaterialInstanceDynamic* Material : Pair)
		{
			for (int32 Index = 0; Index < 2; ++Index)
			{
				if (!Entry.bApplied || Entry.Applied.Texture[Index] != Entry.Look.Texture[Index])
				{
					// No picture: the material's own default, which a layer that is switched off never looks at.
					if (Entry.Look.Texture[Index])
					{
						Material->SetTextureParameterValue(TextureNames[Index], Entry.Look.Texture[Index]);
					}
				}
			}
			for (int32 Row = 0; Row < FLvGateLook::RowCount; ++Row)
			{
				const float* V = Entry.Look.Values[Row];
				if (!Entry.bApplied || FMemory::Memcmp(V, Entry.Applied.Values[Row], sizeof(float) * 4) != 0)
				{
					Material->SetVectorParameterValue(RowName(Row), FLinearColor(V[0], V[1], V[2], V[3]));
				}
			}
		}
		Entry.Applied = Entry.Look;
		Entry.bApplied = true;
	}
	return Handle;
}

void ULvGateSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Requests.Num() == 0)
	{
		return;
	}
	// Everything given back first, then everything asked for: fixtures that change together then
	// find each other's looks, whatever order they ticked in.
	for (FRequest& Request : Requests)
	{
		ALvFixtureActor* Fixture = Request.Fixture.Get();
		if (Fixture && Fixture->GetGateSerial() == Request.Serial)
		{
			Release(Fixture->TakeGateHandle(Request.Beam));
		}
	}
	for (FRequest& Request : Requests)
	{
		ALvFixtureActor* Fixture = Request.Fixture.Get();
		if (!Fixture || Fixture->GetGateSerial() != Request.Serial)
		{
			continue;
		}
		// Should one beam have asked twice, the later answer replaces the earlier.
		Release(Fixture->TakeGateHandle(Request.Beam));
		const int32 Handle = Request.bOpen ? INDEX_NONE : Acquire(Request.Look);
		const bool bValid = Entries.IsValidIndex(Handle);
		if (!Fixture->ApplyGate(Request.Beam, Handle, bValid ? Materials[Entries[Handle].Material].Get() : nullptr,
			bValid ? Materials[Entries[Handle].Material + 1].Get() : nullptr))
		{
			Release(Handle);    // the beam is no longer there
		}
	}
	Requests.Reset();
}
