#include "LvVideo.h"

#include "Async/Async.h"
#include "Engine/GameEngine.h"
#include "FrameGrabber.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "Slate/SceneViewport.h"

#include "LvCoordinates.h"
#include "vizcore/movie.h"

namespace
{
	const int32 kQuality = 92;          // JPEG quality: little to see, and what avconvert makes of it is smaller anyway
	const int32 kMostInFlight = 8;      // pictures being made into JPEG at once, beyond which live recording passes frames over
	const int32 kFlushFrames = 2;       // frames drawn after stopping, for the grabber to hand over the last pictures

	struct FLvFramePayload : IFramePayload
	{
		int64 Sequence = 0;
		double Time = 0.0;
		bool bDiscard = false;    // drawn only to flush the grabber
	};
}

struct FLvVideoShared
{
	FCriticalSection Lock;
	TMap<int64, TPair<double, TArray64<uint8>>> Ready;    // made into JPEG, waiting their turn
	TSet<int64> Skipped;                                   // asked for, but there is nothing to write
	FThreadSafeCounter InFlight;
	IImageWrapperModule* Images = nullptr;
};

FLvVideo::FLvVideo() = default;

FLvVideo::~FLvVideo()
{
	StopNow();
}

bool FLvVideo::Start(const FString& InPath, int32 FramesPerSecond, bool bInFrameByFrame, FString& OutError)
{
	if (State != EState::Idle)
	{
		OutError = TEXT("a video is being recorded already");
		return false;
	}
	UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
	TSharedPtr<FSceneViewport> Viewport = GameEngine ? GameEngine->SceneViewport : nullptr;
	if (!Viewport.IsValid())
	{
		OutError = TEXT("only the visualizer's own window can be recorded");
		return false;
	}
	// Even sizes: what makes H.264 of it afterwards wants them.
	const FIntPoint Window = Viewport->GetSizeXY();
	FrameSize = FIntPoint(Window.X & ~1, Window.Y & ~1);
	if (FrameSize.X < 16 || FrameSize.Y < 16)
	{
		OutError = TEXT("the window is too small to record");
		return false;
	}
	Movie = MakeUnique<viz::MovieWriter>();
	std::string Error;
	if (!Movie->open(LvToStd(InPath), FrameSize.X, FrameSize.Y, FramesPerSecond, Error))
	{
		OutError = LvToFString(Error);
		Movie.Reset();
		return false;
	}
	Shared = MakeShared<FLvVideoShared, ESPMode::ThreadSafe>();
	Shared->Images = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	// The engine's framegrabber.framelatency is left at 0: each picture is read back in the frame it
	// is drawn, which makes the render thread wait for the GPU while recording. At 1 the last picture
	// asked for is only read back when another is, and the grabber's surfaces then wait for it for
	// ever when they are let go; changed while a grabber exists, it flushes from the render thread.
	Grabber = MakeShared<FFrameGrabber>(Viewport.ToSharedRef(), FrameSize, PF_B8G8R8A8, 4);
	Grabber->StartCapturingFrames();
	Path = InPath;
	Rate = FramesPerSecond;
	bFrameByFrame = bInFrameByFrame;
	StartTime = FPlatformTime::Seconds();
	Asked = NextToWrite = NextSlot = MovieFrames = 0;
	LastPicture.Reset();
	Outcome.Reset();
	bFinished = false;
	State = EState::Recording;
	return true;
}

int32 FLvVideo::FramesWritten() const
{
	return Movie ? static_cast<int32>(Movie->frames()) : 0;
}

double FLvVideo::Seconds() const
{
	return Movie ? Movie->seconds() : 0.0;
}

int32 FLvVideo::Backlog() const
{
	return static_cast<int32>(Asked - NextToWrite);
}

void FLvVideo::Tick()
{
	if (State == EState::Idle || !Grabber)
	{
		return;
	}
	if (State == EState::Recording)
	{
		TSharedPtr<FLvFramePayload, ESPMode::ThreadSafe> Payload = MakeShared<FLvFramePayload, ESPMode::ThreadSafe>();
		Payload->Time = FPlatformTime::Seconds() - StartTime;
		bool bWanted = true;
		if (bFrameByFrame)
		{
			// Every frame counts: wait for the JPEG makers rather than pass one over.
			while (Shared->InFlight.GetValue() >= kMostInFlight)
			{
				FPlatformProcess::Sleep(0.002f);
				TakeIn();
			}
		}
		else
		{
			// Only when the movie wants another frame, and not while the JPEG makers are behind.
			const int64 Slot = FMath::FloorToInt64(Payload->Time * Rate);
			bWanted = Slot >= NextSlot && Shared->InFlight.GetValue() < kMostInFlight;
			if (bWanted)
			{
				NextSlot = Slot + 1;
			}
		}
		if (bWanted)
		{
			Payload->Sequence = Asked++;
			Grabber->CaptureThisFrame(Payload);
		}
	}
	else if (State == EState::Flushing)
	{
		// A few frames more, so that the grabber reads back the last pictures asked for; then it stops.
		if (FlushFrames > 0)
		{
			--FlushFrames;
			TSharedPtr<FLvFramePayload, ESPMode::ThreadSafe> Payload = MakeShared<FLvFramePayload, ESPMode::ThreadSafe>();
			Payload->bDiscard = true;
			Grabber->CaptureThisFrame(Payload);
			if (FlushFrames == 0)
			{
				Grabber->StopCapturingFrames();
			}
		}
	}
	TakeIn();
	WriteReady();
	if (State == EState::Flushing && FlushFrames == 0 &&
		((NextToWrite >= Asked && Shared->InFlight.GetValue() == 0) || FPlatformTime::Seconds() - FlushStarted > 10.0))
	{
		Finish();
	}
}

void FLvVideo::TakeIn()
{
	if (!Grabber)
	{
		return;
	}
	TArray<FCapturedFrameData> Frames = Grabber->GetCapturedFrames();
	for (FCapturedFrameData& Frame : Frames)
	{
		const FLvFramePayload* Payload = Frame.GetPayload<FLvFramePayload>();
		if (!Payload || Payload->bDiscard)
		{
			continue;
		}
		const int64 Sequence = Payload->Sequence;
		const double Time = Payload->Time;
		if (Frame.ColorBuffer.Num() < Frame.BufferSize.X * Frame.BufferSize.Y || Frame.BufferSize.X <= 0)
		{
			FScopeLock Lock(&Shared->Lock);
			Shared->Skipped.Add(Sequence);
			continue;
		}
		Shared->InFlight.Increment();
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
			[Shared = Shared, Sequence, Time, Size = Frame.BufferSize, Pixels = MoveTemp(Frame.ColorBuffer)]()
			{
				TArray64<uint8> Jpeg;
				TSharedPtr<IImageWrapper> Wrapper = Shared->Images->CreateImageWrapper(EImageFormat::JPEG);
				if (Wrapper.IsValid() && Wrapper->SetRaw(Pixels.GetData(), static_cast<int64>(Pixels.Num()) * sizeof(FColor), Size.X, Size.Y, ERGBFormat::BGRA, 8))
				{
					Jpeg = Wrapper->GetCompressed(kQuality);
				}
				{
					FScopeLock Lock(&Shared->Lock);
					if (Jpeg.Num() > 0)
					{
						Shared->Ready.Add(Sequence, TPair<double, TArray64<uint8>>(Time, MoveTemp(Jpeg)));
					}
					else
					{
						Shared->Skipped.Add(Sequence);
					}
				}
				Shared->InFlight.Decrement();
			});
	}
}

void FLvVideo::WriteReady()
{
	if (!Shared || !Movie)
	{
		return;
	}
	while (NextToWrite < Asked)
	{
		TPair<double, TArray64<uint8>> Picture;
		{
			FScopeLock Lock(&Shared->Lock);
			if (Shared->Skipped.Remove(NextToWrite) > 0)
			{
				++NextToWrite;
				continue;
			}
			TPair<double, TArray64<uint8>>* Found = Shared->Ready.Find(NextToWrite);
			if (!Found)
			{
				return;    // its turn has come, but it is not made yet
			}
			Picture = MoveTemp(*Found);
			Shared->Ready.Remove(NextToWrite);
		}
		++NextToWrite;
		const uint8* Bytes = Picture.Value.GetData();
		const size_t Count = static_cast<size_t>(Picture.Value.Num());
		if (bFrameByFrame)
		{
			Movie->add(Bytes, Count);
			continue;
		}
		// Live: the frames of the movie up to this picture's time; those it came too late for show the one before.
		const int64 Slot = FMath::FloorToInt64(Picture.Key * Rate);
		if (Slot < MovieFrames)
		{
			continue;
		}
		while (MovieFrames < Slot)
		{
			const TArray64<uint8>& Fill = LastPicture.Num() > 0 ? LastPicture : Picture.Value;
			Movie->add(Fill.GetData(), static_cast<size_t>(Fill.Num()));
			++MovieFrames;
		}
		Movie->add(Bytes, Count);
		++MovieFrames;
		LastPicture = MoveTemp(Picture.Value);
	}
}

void FLvVideo::RequestStop()
{
	if (State != EState::Recording)
	{
		return;
	}
	State = EState::Flushing;
	FlushFrames = kFlushFrames;
	FlushStarted = FPlatformTime::Seconds();
}

void FLvVideo::StopNow()
{
	if (State == EState::Idle)
	{
		return;
	}
	if (Grabber)
	{
		if (State == EState::Recording)
		{
			Grabber->StopCapturingFrames();
		}
		FlushRenderingCommands();
		TakeIn();
	}
	const double Until = FPlatformTime::Seconds() + 5.0;
	while (Shared && Shared->InFlight.GetValue() > 0 && FPlatformTime::Seconds() < Until)
	{
		FPlatformProcess::Sleep(0.005f);
	}
	WriteReady();
	Finish();
}

void FLvVideo::Finish()
{
	if (Grabber)
	{
		Grabber->Shutdown();
		Grabber.Reset();
	}
	if (Movie)
	{
		std::string Error;
		if (Movie->frames() == 0)
		{
			Outcome = TEXT("no picture was recorded");
			Movie->close(Error);
			IFileManager::Get().Delete(*Path);
		}
		else if (!Movie->close(Error))
		{
			Outcome = LvToFString(Error);
		}
	}
	State = EState::Idle;
	bFinished = true;
}
