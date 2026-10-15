#pragma once

#include "CoreMinimal.h"

class FFrameGrabber;
struct FLvVideoShared;
namespace viz { class MovieWriter; }

/**
 * Writes what the visualizer's window shows into a movie: QuickTime, JPEG pictures
 * (vizcore/movie.h), which Openviz turns into H.264 afterwards where the system can.
 *
 * Live, the movie runs at its own steady rate and each of its frames is the picture last drawn
 * by its time: pictures drawn faster than that are passed over, and a picture drawn late is
 * preceded by the one before it for the frames it missed. Frame by frame, each picture drawn is
 * one frame of the movie, for rendering at a fixed time step, however long each takes to draw.
 *
 * Pictures are read back from the GPU by the engine's FFrameGrabber and made into JPEG on worker
 * threads; they are written in the order drawn. Stopping takes a frame or two more.
 */
class FLvVideo
{
public:
	FLvVideo();
	~FLvVideo();

	/** Only the visualizer's own window (-game) can be recorded. */
	bool Start(const FString& Path, int32 FramesPerSecond, bool bFrameByFrame, FString& OutError);
	/** Once a frame, after the world has ticked: asks for this frame's picture and writes what is ready. */
	void Tick();
	/** Finishes over the next few frames; IsDone() says when, and Result() how it went. */
	void RequestStop();
	/** Finishes now, waiting for what can be had: for when the visualizer closes. */
	void StopNow();

	bool IsActive() const { return State != EState::Idle; }    // recording, or finishing
	bool IsRecording() const { return State == EState::Recording; }
	bool IsDone() const { return State == EState::Idle && bFinished; }
	const FString& Result() const { return Outcome; }          // empty if all went well
	const FString& GetPath() const { return Path; }
	int32 FramesWritten() const;
	double Seconds() const;
	FIntPoint Size() const { return FrameSize; }
	/** Pictures asked for and not yet written. */
	int32 Backlog() const;

private:
	enum class EState { Idle, Recording, Flushing };
	void TakeIn();
	void WriteReady();
	void Finish();

	EState State = EState::Idle;
	bool bFinished = false;
	bool bFrameByFrame = false;
	FString Path;
	FString Outcome;
	int32 Rate = 30;
	FIntPoint FrameSize = FIntPoint::ZeroValue;
	double StartTime = 0.0;
	int64 Asked = 0;             // pictures asked of the grabber
	int64 NextToWrite = 0;       // the next picture, in the order asked, to go into the movie
	int64 NextSlot = 0;          // live: the next frame of the movie a picture is wanted for
	int64 MovieFrames = 0;       // live: frames of the movie written so far
	int32 FlushFrames = 0;       // frames still to draw while stopping
	double FlushStarted = 0.0;
	TArray64<uint8> LastPicture; // live: for the frames a late picture missed
	TSharedPtr<FFrameGrabber> Grabber;
	TSharedPtr<FLvVideoShared, ESPMode::ThreadSafe> Shared;
	TUniquePtr<viz::MovieWriter> Movie;
};
