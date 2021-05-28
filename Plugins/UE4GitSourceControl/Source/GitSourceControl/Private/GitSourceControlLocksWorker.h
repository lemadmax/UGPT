#pragma once

#include "HAL/Runnable.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/RunnableThread.h"

#include "CoreMinimal.h"

#include "GitSourceControlUtils.h"
#include "GitSourceControlCommand.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <queue>

// tonyxia changed
#include "GenericPlatform/GenericPlatformFile.h"

struct COMMAND 
{
	FString Command;
	FString PathToGitBinary;
	FString RepositoryRoot;
	TArray<FString> Parameters;
	TArray<FString> Files;
	COMMAND(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles)
		: Command(InCommand), PathToGitBinary(InPathToGitBinary), RepositoryRoot(InRepositoryRoot), Parameters(InParameters), Files(InFiles)
	{
	}
};

class FGitSourceControlLocksWorker : public FRunnable 
{
	static FGitSourceControlLocksWorker* Runnable;

	FRunnableThread* Thread;

	FThreadSafeCounter StopTaskCounter;

private:
	const int32 MaxDecayV = 2;

	const int32 MaxIteration = 300;
	int32 CurIteration = 1;
	std::queue<COMMAND> CommandQueue;

	/** Path to the Git binary */
	FString PathToGitBinary;

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	FString PathToRepositoryRoot;

	FString LfsUserName;

	bool mutex = false;

public:

	bool IsFinished() const
	{
		return false;
	}

	FGitSourceControlLocksWorker();
	virtual ~FGitSourceControlLocksWorker();

	virtual bool Init();
	virtual uint32 Run();
	virtual void Stop();

	void EnsureCompletion();

	static FGitSourceControlLocksWorker* JoyInit();

	static void PushCommand(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles);
	static void PushUpdates(const int32 OpCode);
	static void Shutdown();
	static bool IsThreadFinished();

	static bool IsWrittingCache();
	static void LockCache();
	static void UnlockCache();
};