#include "LvDmxSubsystem.h"

#include "DMXProtocolSettings.h"
#include "DMXProtocolTypes.h"
#include "IO/DMXInputPort.h"
#include "IO/DMXInputPortConfig.h"
#include "IO/DMXPortManager.h"

struct ULvDmxSubsystem::FCache
{
	uint64 Frame = 0;
	TMap<int32, FDMXSignalSharedPtr> Signals;
};

namespace
{
	// Universes given by Openviz itself (see Provide), each 512 slots.
	TMap<int32, TArray<uint8>> Provided;
	bool bProvidedOnly = false;
}

void ULvDmxSubsystem::Provide(int32 Universe, const uint8* Levels, int32 Count)
{
	TArray<uint8>& Slots = Provided.FindOrAdd(Universe);
	Slots.SetNumZeroed(512);
	FMemory::Memcpy(Slots.GetData(), Levels, FMath::Clamp(Count, 0, 512));
}

void ULvDmxSubsystem::ProvideOnly(bool bOnly)
{
	bProvidedOnly = bOnly;
}

void ULvDmxSubsystem::ClearProvided()
{
	Provided.Reset();
	bProvidedOnly = false;
}

const uint8* ULvDmxSubsystem::GetUniverse(int32 Universe)
{
	if (const TArray<uint8>* Given = Provided.Find(Universe))
	{
		return Given->GetData();
	}
	if (bProvidedOnly)
	{
		return nullptr;
	}
	if (!Cache)
	{
		Cache = MakeShared<FCache>();
	}
	// Every fixture on a universe asks for it each frame; look it up on the ports once.
	if (Cache->Frame != GFrameCounter)
	{
		Cache->Frame = GFrameCounter;
		Cache->Signals.Reset();
	}
	FDMXSignalSharedPtr* Known = Cache->Signals.Find(Universe);
	if (!Known)
	{
		FDMXSignalSharedPtr Signal;
		for (const FDMXInputPortSharedRef& Port : FDMXPortManager::Get().GetInputPorts())
		{
			if (Port->GameThreadGetDMXSignal(Universe, Signal) && Signal.IsValid())
			{
				break;
			}
		}
		Known = &Cache->Signals.Add(Universe, Signal);
	}
	return Known->IsValid() && (*Known)->ChannelData.Num() >= 512 ? (*Known)->ChannelData.GetData() : nullptr;
}

namespace
{
	// What the port added last receives, for adding to it.
	FName CoveredProtocol;
	FString CoveredAddress;
	int32 CoveredLast = 0;
	// The highest universe anything has asked for: remembered, in case it asks before there is a port.
	int32 WantedLast = 0;
}

void ULvDmxSubsystem::CoverUniverses(int32 Last)
{
	WantedLast = FMath::Max(WantedLast, FMath::Min(Last, 32767));
	if (CoveredLast > 0 && WantedLast > CoveredLast)
	{
		AddInputPort(CoveredProtocol, CoveredAddress, CoveredLast + 1, WantedLast - CoveredLast);
	}
}

void ULvDmxSubsystem::AddInputPort(FName Protocol, const FString& DeviceAddress, int32 FirstUniverse, int32 NumUniverses)
{
	CoveredProtocol = Protocol;
	CoveredAddress = DeviceAddress;
	CoveredLast = FMath::Max(CoveredLast, FirstUniverse + FMath::Max(NumUniverses, 1) - 1);
	FDMXInputPortConfigParams Params;
	Params.PortName = TEXT("Openviz");    // the name Unreal's own DMX monitors list it under
	Params.ProtocolName = Protocol;
	Params.CommunicationType = EDMXCommunicationType::InternalOnly;    // the only type an input port has
	Params.bAutoCompleteDeviceAddressEnabled = false;
	Params.AutoCompleteDeviceAddress = TEXT("");
	Params.DeviceAddress = DeviceAddress;
	Params.LocalUniverseStart = FirstUniverse;
	Params.NumUniverses = FMath::Max(NumUniverses, 1);
	Params.ExternUniverseStart = FirstUniverse;
	Params.PriorityStrategy = EDMXPortPriorityStrategy::None;
	Params.Priority = 0;

	UDMXProtocolSettings* Settings = GetMutableDefault<UDMXProtocolSettings>();
	Settings->InputPortConfigs.Add(FDMXInputPortConfig(FGuid::NewGuid(), Params));
	Settings->OverrideReceiveDMXEnabled(true);
	FDMXPortManager::Get().UpdateFromProtocolSettings();
	if (WantedLast > CoveredLast)
	{
		AddInputPort(Protocol, DeviceAddress, CoveredLast + 1, WantedLast - CoveredLast);    // what was asked for before this port was there
	}
}
