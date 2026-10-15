#include "LvScreenFeeds.h"

#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "Misc/Paths.h"

bool ULvScreenFeeds::Take(const FString& Kind, const FString& Value, UMediaPlayer*& OutPlayer, UMediaTexture*& OutTexture)
{
	const FString Key = Kind + TEXT("|") + Value;
	if (FLvScreenFeed* Open = Feeds.Find(Key))
	{
		++Open->Users;
		OutPlayer = Open->Player;
		OutTexture = Open->Texture;
		return true;
	}
	const bool bStream = Kind == TEXT("ndi");
	if (Value.IsEmpty() || (!bStream && !FPaths::FileExists(Value)))
	{
		return false;
	}
	FLvScreenFeed Feed;
	Feed.Player = NewObject<UMediaPlayer>(this);
	Feed.Player->PlayOnOpen = true;
	Feed.Player->SetLooping(!bStream);
	Feed.Texture = NewObject<UMediaTexture>(this);
	Feed.Texture->AutoClear = true;
	Feed.Texture->ClearColor = FLinearColor::Black;
	Feed.Texture->NewStyleOutput = true;
	Feed.Texture->SetMediaPlayer(Feed.Player);
	Feed.Texture->UpdateResource();
	if (!(bStream ? Feed.Player->OpenUrl(TEXT("ndi://") + Value) : Feed.Player->OpenFile(Value)))
	{
		Feed.Player->Close();
		return false;
	}
	Feed.Users = 1;
	OutPlayer = Feed.Player;
	OutTexture = Feed.Texture;
	Feeds.Add(Key, MoveTemp(Feed));
	return true;
}

void ULvScreenFeeds::GiveBack(const FString& Kind, const FString& Value)
{
	const FString Key = Kind + TEXT("|") + Value;
	FLvScreenFeed* Open = Feeds.Find(Key);
	if (Open && --Open->Users <= 0)
	{
		if (Open->Player)
		{
			Open->Player->Close();
		}
		Feeds.Remove(Key);
	}
}

void ULvScreenFeeds::Deinitialize()
{
	for (TPair<FString, FLvScreenFeed>& Feed : Feeds)
	{
		if (Feed.Value.Player)
		{
			Feed.Value.Player->Close();
		}
	}
	Feeds.Reset();
	Super::Deinitialize();
}
