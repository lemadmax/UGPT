
#include "GitSourceControlLocksWorker.h"

#include "Modules/ModuleManager.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlState.h"
//#include "GitSourceControlMenu.h"
//#include "Misc/MessageDialog.h"
#include "Engine/Engine.h"


FGitSourceControlLocksWorker* FGitSourceControlLocksWorker::Runnable = NULL;

FGitSourceControlLocksWorker::FGitSourceControlLocksWorker()
{
	Thread = FRunnableThread::Create(this, TEXT("LocksWorker"), 0, TPri_BelowNormal);

	//GitSourceControlUtils::GetSelfLocksFile(SelfLockedFile);
	//GitSourceControlUtils::GetOtherLocksFile(OtherLockedFile);

	//UE_LOG(LogSourceControl, Log, TEXT("lock file path: %s"), *OtherLockedFile);


	const FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>("GitSourceControl");
	PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
	//PathToRepositoryRoot = GitSourceControl.AccessSettings().GetRepositoryRootPath();
	PathToRepositoryRoot = GitSourceControl.GetProvider().GetPathToRepositoryRoot();
	LfsUserName = GitSourceControl.AccessSettings().GetLfsUserName();


	UE_LOG(LogSourceControl, Warning, TEXT("PathToGitBinary: %s"), *PathToGitBinary);
	UE_LOG(LogSourceControl, Warning, TEXT("PathToRepositoryRoot: %s"), *PathToRepositoryRoot);
	UE_LOG(LogSourceControl, Warning, TEXT("LfsUserName: %s"), *LfsUserName);
}

FGitSourceControlLocksWorker::~FGitSourceControlLocksWorker()
{
	delete Thread;
	Thread = NULL;
}

bool FGitSourceControlLocksWorker::Init()
{
	UE_LOG(LogSourceControl, Warning, TEXT("Thread started"));
	return true;
}

uint32 FGitSourceControlLocksWorker::Run()
{
	FPlatformProcess::Sleep(0.03);

	const FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>("GitSourceControl");

	while (StopTaskCounter.GetValue() == 0)
	{
		//UE_LOG(LogSourceControl, Error, TEXT("command queue size: %d"), CommandQueue.size());
		if (!CommandQueue.empty()) 
		{
			COMMAND command = CommandQueue.front();
			CommandQueue.pop();
			TArray<FString> Results;
			TArray<FString> ErrorMessage;
			UE_LOG(LogSourceControl, Warning, TEXT("lock operation: %s"), *command.Command);
			GitSourceControlUtils::RunCommand(command.Command, command.PathToGitBinary, command.RepositoryRoot, command.Parameters, command.Files, Results, ErrorMessage);
		}
		else if(CurIteration == 0)
		{
			CurIteration = (CurIteration + 1) % MaxIteration;
			TArray<FString> ChangedFiles;
			const bool changed = GitSourceControlUtils::UpdateLockCaches(ChangedFiles, PathToGitBinary, PathToRepositoryRoot, LfsUserName);
			if (changed) {
				//PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
				//PathToRepositoryRoot = GitSourceControl.AccessSettings().GetRepositoryRootPath();
				TArray<FGitSourceControlState> States;
				for (const auto& File : ChangedFiles) {
					TArray<FString> OneFile;
					TArray<FString> ErrorMessage;
					OneFile.Add(File);
					FString RepoRoot = PathToRepositoryRoot;
					GitSourceControlUtils::FindRepoRoot(File, RepoRoot);
					GitSourceControlUtils::RunUpdateStatus(PathToGitBinary, RepoRoot, true, OneFile, ErrorMessage, States);
					GitSourceControlUtils::UpdateCachedStates(States);
				}
			}
		}
		else if (CurIteration == -1) {
			CurIteration = (CurIteration + 1) % MaxIteration;
			GEngine->AddOnScreenDebugMessage(-1, 8.f, FColor::Green, TEXT("Updating Submodules to the latest version!"));
			
			TArray<FString> Results;
			TArray<FString> ErrorMessages;
			TArray<FString> Parameters;
			Parameters.Add(TEXT("--rebase"));
			Parameters.Add(TEXT("--autostash"));
			// TODO Configure origin
			Parameters.Add(TEXT("origin"));
			Parameters.Add(TEXT("HEAD"));
			GitSourceControlUtils::RunCommand(TEXT("pull"), PathToGitBinary, PathToRepositoryRoot, Parameters, TArray<FString>(), Results, ErrorMessages);

			TArray<FString> Parameters1;
			Parameters1.Add("--recursive");
			Parameters1.Add("--remote");
			GitSourceControlUtils::RunCommand(TEXT("submodule update"), PathToGitBinary, PathToRepositoryRoot, Parameters1, TArray<FString>(), Results, ErrorMessages);

			TArray<FString> AllProjects;
			GitSourceControlUtils::GetSubModulesRoots(AllProjects);
			AllProjects.Add(TEXT(""));
			// now update the status of our files
			TArray<FGitSourceControlState> States;
			for (const auto& Sub : AllProjects) {
				FString RepoRoot = PathToRepositoryRoot;
				if (Sub != TEXT("")) RepoRoot += TEXT("/") + Sub;
				TArray<FString> ProjectDirs;
				ProjectDirs.Add(RepoRoot + TEXT("/"));
				GitSourceControlUtils::RunUpdateStatus(PathToGitBinary, RepoRoot, true, ProjectDirs, ErrorMessages, States);
			}
		}
		else {
			CurIteration = (CurIteration + 1) % MaxIteration;
			FPlatformProcess::Sleep(0.03);
		}
	}
	UE_LOG(LogSourceControl, Warning, TEXT("Run finished"));
	return 0;
}

void FGitSourceControlLocksWorker::Stop()
{
	StopTaskCounter.Increment();
}

void FGitSourceControlLocksWorker::EnsureCompletion()
{
	Stop();
	Thread->WaitForCompletion();
}

FGitSourceControlLocksWorker * FGitSourceControlLocksWorker::JoyInit()
{
	if (!Runnable && FPlatformProcess::SupportsMultithreading()) 
	{
		UE_LOG(LogSourceControl, Warning, TEXT("lock worker joyinit"));
		Runnable = new FGitSourceControlLocksWorker();
	}
	return Runnable;
}

void FGitSourceControlLocksWorker::PushCommand(const FString & InCommand, const FString & InPathToGitBinary, const FString & InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles)
{
	COMMAND node(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles);
	if(Runnable == NULL) JoyInit();
	Runnable->CommandQueue.push(node);
}

void FGitSourceControlLocksWorker::PushUpdates(const int32 OpCode)
{
	if (Runnable == NULL) JoyInit();
	Runnable->CurIteration = OpCode;
}

void FGitSourceControlLocksWorker::Shutdown()
{
	if (Runnable)
	{
		Runnable->EnsureCompletion();
		delete Runnable;
		Runnable = NULL;
	}
}

bool FGitSourceControlLocksWorker::IsThreadFinished()
{
	if (Runnable) return Runnable->IsFinished();
	return true;
}

bool FGitSourceControlLocksWorker::IsWrittingCache()
{
	if (Runnable != NULL) return Runnable->mutex;
	else return false;
}

void FGitSourceControlLocksWorker::LockCache()
{
	if (Runnable != NULL) Runnable->mutex = true;
}

void FGitSourceControlLocksWorker::UnlockCache()
{
	if (Runnable != NULL) Runnable->mutex = false;
}
