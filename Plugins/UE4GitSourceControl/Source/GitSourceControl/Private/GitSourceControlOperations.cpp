// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlOperations.h"

#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SourceControlOperations.h"
#include "ISourceControlModule.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlCommand.h"
#include "GitSourceControlUtils.h"
#include "Logging/MessageLog.h"

#include "GitSourceControlLocksWorker.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

FName FGitPush::GetName() const
{
	return "Push";
}

FText FGitPush::GetInProgressString() const
{
	// TODO Configure origin
	return LOCTEXT("SourceControl_Push", "Pushing local commits to remote origin...");
}


FName FGitConnectWorker::GetName() const
{
	return "Connect";
}

bool FGitConnectWorker::Execute(FGitSourceControlCommand& InCommand)
{

	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FConnect, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FConnect>(InCommand.Operation);

	// Check Git Availability
	if((InCommand.PathToGitBinary.Len() > 0) && GitSourceControlUtils::CheckGitAvailability(InCommand.PathToGitBinary))
	{
		// Now update the status of assets in Content/ directory and also Config files
		TArray<FString> ProjectDirs;
		ProjectDirs.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
		ProjectDirs.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()));
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, ProjectDirs, InCommand.ErrorMessages, States);
		if(!InCommand.bCommandSuccessful || InCommand.ErrorMessages.Num() > 0)
		{
			Operation->SetErrorText(LOCTEXT("NotAGitRepository", "Failed to enable Git source control. You need to initialize the project as a Git repository first."));
			InCommand.bCommandSuccessful = false;
		}
		else
		{
			GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

			if(InCommand.bUsingGitLfsLocking)
			{
				// Check server connection by checking lock status (when using Git LFS file Locking workflow)
				InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("lfs locks"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, TArray<FString>(), TArray<FString>(), InCommand.InfoMessages, InCommand.ErrorMessages);
			}
		}
	}
	else
	{
		Operation->SetErrorText(LOCTEXT("GitNotFound", "Failed to enable Git source control. You need to install Git and specify a valid path to git executable."));
		InCommand.bCommandSuccessful = false;
	}
	if (InCommand.bCommandSuccessful) {
		UE_LOG(LogSourceControl, Warning, TEXT("Locks Worker initiated!"));
		FGitSourceControlLocksWorker::JoyInit();
		//FGitSourceControlLocksWorker::PushUpdates(-1);
	}
	return InCommand.bCommandSuccessful;
}

bool FGitConnectWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitCheckOutWorker::GetName() const
{
	return "CheckOut";
}

bool FGitCheckOutWorker::Execute(FGitSourceControlCommand& InCommand)
{

	check(InCommand.Operation->GetName() == GetName());
	InCommand.bCommandSuccessful = true;

	FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
	if (InCommand.Files.Num() > 0) {
		GitSourceControlUtils::FindRepoRoot(InCommand.Files[0], PathToRepositoryRoot);
	}
	//TArray<FString> Results;
	//TArray<FString> ErrorMessages;
	//TArray<FString> Parameters;
	//Parameters.Add(TEXT("--rebase"));
	//Parameters.Add(TEXT("--autostash"));
	//// TODO Configure origin
	//Parameters.Add(TEXT("origin"));
	//Parameters.Add(TEXT("HEAD"));
	//InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("pull"), InCommand.PathToGitBinary, PathToRepositoryRoot, Parameters, TArray<FString>(), Results, ErrorMessages);

	if(InCommand.bUsingGitLfsLocking)
	{
		// lock files: execute the LFS command on relative filenames
		const TArray<FString> RelativeFiles = GitSourceControlUtils::RelativeFilenames(InCommand.Files, PathToRepositoryRoot);
		for(const auto& File : RelativeFiles)
		{
			TArray<FString> OneFile;
			OneFile.Add(File);
			while (FGitSourceControlLocksWorker::IsWrittingCache()) {}
			FGitSourceControlLocksWorker::LockCache();
			GitSourceControlUtils::CacheLock(OneFile);
			FGitSourceControlLocksWorker::UnlockCache();
			//InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("lfs lock"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), OneFile, InCommand.InfoMessages, InCommand.ErrorMessages);
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), OneFile, InCommand.InfoMessages, InCommand.ErrorMessages);
			FGitSourceControlLocksWorker::PushCommand(TEXT("lfs lock"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), OneFile);
		}

		// now update the status of our files
		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ErrorMessages, States);
	}
	else
	{
		InCommand.bCommandSuccessful = false;
	}

	return InCommand.bCommandSuccessful;
}

bool FGitCheckOutWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

static FText ParseCommitResults(const TArray<FString>& InResults)
{
	if(InResults.Num() >= 1)
	{
		const FString& FirstLine = InResults[0];
		return FText::Format(LOCTEXT("CommitMessage", "Commited {0}."), FText::FromString(FirstLine));
	}
	return LOCTEXT("CommitMessageUnknown", "Submitted revision.");
}

// Get Locked Files (that is, CheckedOut files, not Added ones)
const TArray<FString> GetLockedFiles(const TArray<FString>& InFiles)
{
	TArray<FString> LockedFiles;

	FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>("GitSourceControl");
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(InFiles, LocalStates, EStateCacheUsage::Use);
	for(const auto& State : LocalStates)
	{
		if(State->IsCheckedOut())
		{
			LockedFiles.Add(State->GetFilename());
		}
	}

	return LockedFiles;
}

FName FGitCheckInWorker::GetName() const
{
	return "CheckIn";
}

bool FGitCheckInWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());


	TArray<FString> AllProjects;
	GitSourceControlUtils::GetSubModulesRoots(AllProjects);
	AllProjects.Add(TEXT(""));
	TArray<FString> InFiles;
	for (const auto& File : InCommand.Files) InFiles.Add(File);
	for (const auto& Sub : AllProjects) {
		FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
		if(Sub != TEXT("")) PathToRepositoryRoot += TEXT("/") + Sub;
		TArray<FString> Files;
		for (const auto& File : InFiles) {
			if (FPaths::IsRelative(File)) {
				FString AbsFileName = PathToRepositoryRoot + TEXT("/") + File;
				if (FPaths::FileExists(AbsFileName)) {
					Files.Add(File);
				}
			}
			else {
				if(File.StartsWith(PathToRepositoryRoot))
					Files.Add(File);
			}
		}
		if (Files.Num() == 0) continue;
		for (const auto& File : Files) {
			UE_LOG(LogSourceControl, Warning, TEXT("Checking in file: %s at dir: %s"), *File, *PathToRepositoryRoot);
			InFiles.RemoveSwap(File, true);
		}

		TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);

		// make a temp file to place our commit message in
		FGitScopedTempFile CommitMsgFile(Operation->GetDescription());
		if (CommitMsgFile.GetFilename().Len() > 0)
		{
			TArray<FString> Parameters;
			FString ParamCommitMsgFilename = TEXT("--file=\"");
			ParamCommitMsgFilename += FPaths::ConvertRelativePathToFull(CommitMsgFile.GetFilename());
			ParamCommitMsgFilename += TEXT("\"");
			Parameters.Add(ParamCommitMsgFilename);

			InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommit(InCommand.PathToGitBinary, PathToRepositoryRoot, Parameters, Files, InCommand.InfoMessages, InCommand.ErrorMessages);
			if (InCommand.bCommandSuccessful)
			{
				// Remove any deleted files from status cache
				FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>("GitSourceControl");
				FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

				TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
				Provider.GetState(Files, LocalStates, EStateCacheUsage::Use);
				for (const auto& State : LocalStates)
				{
					if (State->IsDeleted())
					{
						Provider.RemoveFileFromCache(State->GetFilename());
					}
				}

				Operation->SetSuccessMessage(ParseCommitResults(InCommand.InfoMessages));
				const FString Message = (InCommand.InfoMessages.Num() > 0) ? InCommand.InfoMessages[0] : TEXT("");
				UE_LOG(LogSourceControl, Log, TEXT("commit successful: %s"), *Message);

				// git-lfs: push and unlock files
				if (InCommand.bUsingGitLfsLocking && InCommand.bCommandSuccessful)
				{
					TArray<FString> Parameters2;
					// TODO Configure origin
					Parameters2.Add(TEXT("origin"));
					Parameters2.Add(TEXT("HEAD"));
					InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("push"), InCommand.PathToGitBinary, PathToRepositoryRoot, Parameters2, TArray<FString>(), InCommand.InfoMessages, InCommand.ErrorMessages);
					if (!InCommand.bCommandSuccessful)
					{
						// if out of date, pull first, then try again
						bool bWasOutOfDate = false;
						for (const auto& PushError : InCommand.ErrorMessages)
						{
							if (PushError.Contains(TEXT("[rejected]")) && PushError.Contains(TEXT("non-fast-forward")))
							{
								// Don't do it during iteration, want to append pull results to InCommand.ErrorMessages
								bWasOutOfDate = true;
								break;
							}
						}
						if (bWasOutOfDate)
						{
							UE_LOG(LogSourceControl, Log, TEXT("Push failed because we're out of date, pulling automatically to try to resolve"));
							// Use pull --rebase since that's what the pull command does by default
							// This requires that we stash if dirty working copy though
							bool bStashed = false;
							bool bStashNeeded = false;
							const TArray<FString> ParametersStatus{ "--porcelain --untracked-files=no" };
							TArray<FString> StatusInfoMessages;
							TArray<FString> StatusErrorMessages;
							// Check if there is any modification to the working tree
							const bool bStatusOk = GitSourceControlUtils::RunCommand(TEXT("status"), InCommand.PathToGitBinary, PathToRepositoryRoot, ParametersStatus, TArray<FString>(), StatusInfoMessages, StatusErrorMessages);
							if ((bStatusOk) && (StatusInfoMessages.Num() > 0))
							{
								bStashNeeded = true;
								const TArray<FString> ParametersStash{ "save \"Stashed by Unreal Engine Git Plugin\"" };
								bStashed = GitSourceControlUtils::RunCommand(TEXT("stash"), InCommand.PathToGitBinary, PathToRepositoryRoot, ParametersStash, TArray<FString>(), InCommand.InfoMessages, InCommand.ErrorMessages);
								if (!bStashed)
								{
									FMessageLog SourceControlLog("SourceControl");
									SourceControlLog.Warning(LOCTEXT("SourceControlMenu_StashFailed", "Stashing away modifications failed!"));
									SourceControlLog.Notify();
								}
							}
							if (!bStashNeeded || bStashed)
							{
								InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("pull --rebase"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), TArray<FString>(), InCommand.InfoMessages, InCommand.ErrorMessages);
								if (InCommand.bCommandSuccessful)
								{
									// Repeat the push
									InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("push origin HEAD"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), TArray<FString>(), InCommand.InfoMessages, InCommand.ErrorMessages);
								}

								// Succeed or fail, restore the stash
								if (bStashed)
								{
									const TArray<FString> ParametersStashPop{ "pop" };
									InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("stash"), InCommand.PathToGitBinary, PathToRepositoryRoot, ParametersStashPop, TArray<FString>(), InCommand.InfoMessages, InCommand.ErrorMessages);
									if (!InCommand.bCommandSuccessful)
									{
										FMessageLog SourceControlLog("SourceControl");
										SourceControlLog.Warning(LOCTEXT("SourceControlMenu_UnstashFailed", "Unstashing previously saved modifications failed!"));
										SourceControlLog.Notify();
									}
								}
							}
						}
					}
					if (InCommand.bCommandSuccessful)
					{
						// unlock files: execute the LFS command on relative filenames
						// (unlock only locked files, that is, not Added files)
						TArray<FString> LockedFiles = GetLockedFiles(Files);
						if (LockedFiles.Num() >= 0)
						{
							const TArray<FString> RelativeFiles = GitSourceControlUtils::RelativeFilenames(LockedFiles, PathToRepositoryRoot);
							for (const auto& RelativeFile : RelativeFiles)
							{
								TArray<FString> OneFile;
								OneFile.Add(RelativeFile);

								//GitSourceControlUtils::RunCommand(TEXT("lfs unlock"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), OneFile, InCommand.InfoMessages, InCommand.ErrorMessages);
								while (FGitSourceControlLocksWorker::IsWrittingCache()) {}
								FGitSourceControlLocksWorker::LockCache();
								GitSourceControlUtils::CacheLockRemove(OneFile);
								FGitSourceControlLocksWorker::UnlockCache();
								FGitSourceControlLocksWorker::PushCommand(TEXT("lfs unlock"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), OneFile);
							}
						}
					}
				}
			}
		}

		// now update the status of our files
		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, Files, InCommand.ErrorMessages, States);
		GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);
	}
	return InCommand.bCommandSuccessful;
}

bool FGitCheckInWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitMarkForAddWorker::GetName() const
{
	return "MarkForAdd";
}

bool FGitMarkForAddWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TArray<FString> AllProjects;
	GitSourceControlUtils::GetSubModulesRoots(AllProjects);
	AllProjects.Add(TEXT(""));
	TArray<FString> InFiles;
	for (const auto& File : InCommand.Files) InFiles.Add(File);
	for (const auto& Sub : AllProjects) {
		FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
		if (Sub != TEXT("")) PathToRepositoryRoot += TEXT("/") + Sub;
		TArray<FString> Files;
		for (const auto& File : InFiles) {
			if (FPaths::IsRelative(File)) {
				FString AbsFileName = PathToRepositoryRoot + TEXT("/") + File;
				if (FPaths::FileExists(AbsFileName)) {
					Files.Add(File);
				}
			}
			else {
				if (File.StartsWith(PathToRepositoryRoot))
					Files.Add(File);
			}
		}
		if (Files.Num() == 0) continue;
		for (const auto& File : Files) {
			UE_LOG(LogSourceControl, Warning, TEXT("Adding file: %s at dir: %s"), *File, *PathToRepositoryRoot);
			InFiles.RemoveSwap(File, true);
		}
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), Files, InCommand.InfoMessages, InCommand.ErrorMessages);

		// now update the status of our files
		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, Files, InCommand.ErrorMessages, States);
	}
	return InCommand.bCommandSuccessful;
}

bool FGitMarkForAddWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitDeleteWorker::GetName() const
{
	return "Delete";
}

bool FGitDeleteWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TArray<FString> AllProjects;
	GitSourceControlUtils::GetSubModulesRoots(AllProjects);
	AllProjects.Add(TEXT(""));
	TArray<FString> InFiles;
	for (const auto& File : InCommand.Files) InFiles.Add(File);
	for (const auto& Sub : AllProjects) {
		FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
		if (Sub != TEXT("")) PathToRepositoryRoot += TEXT("/") + Sub;
		TArray<FString> Files;
		for (const auto& File : InFiles) {
			if (FPaths::IsRelative(File)) {
				FString AbsFileName = PathToRepositoryRoot + TEXT("/") + File;
				if (FPaths::FileExists(AbsFileName)) {
					Files.Add(File);
				}
			}
			else {
				if (File.StartsWith(PathToRepositoryRoot))
					Files.Add(File);
			}
		}
		if (Files.Num() == 0) continue;
		for (const auto& File : Files) {
			UE_LOG(LogSourceControl, Warning, TEXT("Deleting file: %s at dir: %s"), *File, *PathToRepositoryRoot);
			InFiles.RemoveSwap(File, true);
		}

		InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), Files, InCommand.InfoMessages, InCommand.ErrorMessages);

		// now update the status of our files
		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, Files, InCommand.ErrorMessages, States);
	}
	return InCommand.bCommandSuccessful;
}

bool FGitDeleteWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}


// Get lists of Missing files (ie "deleted"), Modified files, and "other than Added" Existing files
void GetMissingVsExistingFiles(const TArray<FString>& InFiles, TArray<FString>& OutMissingFiles, TArray<FString>& OutAllExistingFiles, TArray<FString>& OutOtherThanAddedExistingFiles)
{
	FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>("GitSourceControl");
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	const TArray<FString> Files = (InFiles.Num() > 0) ? (InFiles) : (Provider.GetFilesInCache());

	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(Files, LocalStates, EStateCacheUsage::Use);
	for(const auto& State : LocalStates)
	{
		if(FPaths::FileExists(State->GetFilename()))
		{
			if(State->IsAdded())
			{
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if(State->IsModified())
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if(State->CanRevert()) // for locked but unmodified files
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
			}
		}
		else
		{
			if (State->IsSourceControlled())
			{
				OutMissingFiles.Add(State->GetFilename());
			}
		}
	}
}

FName FGitRevertWorker::GetName() const
{
	return "Revert";
}

bool FGitRevertWorker::Execute(FGitSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = true;
	TArray<FString> AllProjects;
	GitSourceControlUtils::GetSubModulesRoots(AllProjects);
	AllProjects.Add(TEXT(""));
	TArray<FString> InFiles;
	for (const auto& File : InCommand.Files) InFiles.Add(File);
	for (const auto& Sub : AllProjects) {
		FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
		if (Sub != TEXT("")) PathToRepositoryRoot += TEXT("/") + Sub;
		TArray<FString> Files;
		for (const auto& File : InFiles) {
			if (FPaths::IsRelative(File)) {
				FString AbsFileName = PathToRepositoryRoot + TEXT("/") + File;
				if (FPaths::FileExists(AbsFileName)) {
					Files.Add(File);
				}
			}
			else {
				if (File.StartsWith(PathToRepositoryRoot))
					Files.Add(File);
			}
		}
		for (const auto& File : Files) {
			UE_LOG(LogSourceControl, Warning, TEXT("Reverting file: %s at dir: %s"), *File, *PathToRepositoryRoot);
			InFiles.RemoveSwap(File, true);
		}
		// Filter files by status to use the right "revert" commands on them
		TArray<FString> MissingFiles;
		TArray<FString> AllExistingFiles;
		TArray<FString> OtherThanAddedExistingFiles;
		GetMissingVsExistingFiles(Files, MissingFiles, AllExistingFiles, OtherThanAddedExistingFiles);
		UE_LOG(LogSourceControl, Warning, TEXT("Number of MissingFiles: %d"), MissingFiles.Num());
		UE_LOG(LogSourceControl, Warning, TEXT("Number of AllExistingFiles: %d"), AllExistingFiles.Num());
		UE_LOG(LogSourceControl, Warning, TEXT("Number of OtherThanAddedExistingFiles: %d"), OtherThanAddedExistingFiles.Num());


		if (MissingFiles.Num() > 0)
		{
			// "Added" files that have been deleted needs to be removed from source control
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), MissingFiles, InCommand.InfoMessages, InCommand.ErrorMessages);
		}
		if (AllExistingFiles.Num() > 0)
		{
			// reset any changes already added to the index
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("reset"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), AllExistingFiles, InCommand.InfoMessages, InCommand.ErrorMessages);
		}
		if (OtherThanAddedExistingFiles.Num() > 0)
		{
			// revert any changes in working copy (this would fails if the asset was in "Added" state, since after "reset" it is now "untracked")
			InCommand.bCommandSuccessful &= GitSourceControlUtils::RunCommand(TEXT("checkout"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), OtherThanAddedExistingFiles, InCommand.InfoMessages, InCommand.ErrorMessages);
		}

		//if (InCommand.bUsingGitLfsLocking)
		//{
		//	// unlock files: execute the LFS command on relative filenames
		//	// (unlock only locked files, that is, not Added files)
		//	const TArray<FString> LockedFiles = GetLockedFiles(OtherThanAddedExistingFiles);
		//	if (LockedFiles.Num() > 0)
		//	{
		//		const TArray<FString> RelativeFiles = GitSourceControlUtils::RelativeFilenames(LockedFiles, PathToRepositoryRoot);
		//		for (const auto& RelativeFile : RelativeFiles)
		//		{
		//			TArray<FString> OneFile;
		//			OneFile.Add(RelativeFile);
		//			GitSourceControlUtils::RunCommand(TEXT("lfs unlock"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), OneFile, InCommand.InfoMessages, InCommand.ErrorMessages);
		//		}
		//	}
		//}

		// If no files were specified (full revert), refresh all relevant files instead of the specified files (which is an empty list in full revert)
		// This is required so that files that were "Marked for add" have their status updated after a full revert.
		TArray<FString> FilesToUpdate = Files;
		if (Files.Num() <= 0)
		{
			for (const auto& File : MissingFiles) FilesToUpdate.Add(File);
			for (const auto& File : AllExistingFiles) FilesToUpdate.Add(File);
			for (const auto& File : OtherThanAddedExistingFiles) FilesToUpdate.Add(File);
		}
		// now update the status of our files
		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, FilesToUpdate, InCommand.ErrorMessages, States);
	}

	return InCommand.bCommandSuccessful;
}

bool FGitRevertWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitSyncWorker::GetName() const
{
	return "Sync";
}

bool FGitSyncWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// pull the branch to get remote changes by rebasing any local commits (not merging them to avoid complex graphs)	
	TArray<FString> AllProjects;
	GitSourceControlUtils::GetSubModulesRoots(AllProjects);
	AllProjects.Add(TEXT(""));
	for (const auto& Sub : AllProjects) {
		FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot + TEXT("/") + Sub;

		TArray<FString> Parameters;
		Parameters.Add(TEXT("--rebase"));
		Parameters.Add(TEXT("--autostash"));
		// TODO Configure origin
		Parameters.Add(TEXT("origin"));
		Parameters.Add(TEXT("HEAD"));
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("pull"), InCommand.PathToGitBinary, PathToRepositoryRoot, Parameters, TArray<FString>(), InCommand.InfoMessages, InCommand.ErrorMessages);
		// now update the status of our files
		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, InCommand.Files, InCommand.ErrorMessages, States);
		GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);
	}

	return InCommand.bCommandSuccessful;
}

bool FGitSyncWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}


FName FGitPushWorker::GetName() const
{
	return "Push";
}

bool FGitPushWorker::Execute(FGitSourceControlCommand& InCommand)
{
	TArray<FString> AllProjects;
	GitSourceControlUtils::GetSubModulesRoots(AllProjects);
	AllProjects.Add(TEXT(""));
	// If we have any locked files, check if we should unlock them
	TArray<FString> FilesToUnlock;
	TMap<FString, FString> FilesRepoRoot;
	if (InCommand.bUsingGitLfsLocking)
	{
		TMap<FString, FString> Locks;
		// Get locks as relative paths
		GitSourceControlUtils::GetAllLocks(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, false, InCommand.ErrorMessages, Locks);
		if(Locks.Num() > 0)
		{		
			for (const auto& Sub : AllProjects) {
				FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
				if(Sub != TEXT("")) PathToRepositoryRoot += TEXT("/") + Sub;
				// test to see what lfs files we would push, and compare to locked files, unlock after if push OK
				FString BranchName;
				GitSourceControlUtils::GetBranchName(InCommand.PathToGitBinary, PathToRepositoryRoot, BranchName);

				TArray<FString> LfsPushParameters;
				LfsPushParameters.Add(TEXT("push"));
				LfsPushParameters.Add(TEXT("--dry-run"));
				LfsPushParameters.Add(TEXT("origin"));
				LfsPushParameters.Add(BranchName);
				TArray<FString> LfsPushInfoMessages;
				TArray<FString> LfsPushErrMessages;
				InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("lfs"), InCommand.PathToGitBinary, PathToRepositoryRoot, LfsPushParameters, TArray<FString>(), LfsPushInfoMessages, LfsPushErrMessages);

				//if (InCommand.bCommandSuccessful)
				//{
				//	// Result format is of the form
				//	// push f4ee401c063058a78842bb3ed98088e983c32aa447f346db54fa76f844a7e85e => Path/To/Asset.uasset
				//	// With some potential informationals we can ignore
				//	for (auto& Line : LfsPushInfoMessages)
				//	{
				//		if (Line.StartsWith(TEXT("push")))
				//		{
				//			FString Prefix, Filename;
				//			if (Line.Split(TEXT("=>"), &Prefix, &Filename))
				//			{
				//				Filename = Filename.TrimStartAndEnd();
				//				if (Locks.Contains(Filename))
				//				{
				//					// We do not need to check user or if the file has local modifications before attempting unlocking, git-lfs will reject the unlock if so
				//					// No point duplicating effort here
				//					FilesToUnlock.Add(Filename);
				//					FilesRepoRoot.Add(MoveTemp(Filename), MoveTemp(PathToRepositoryRoot));
				//					UE_LOG(LogSourceControl, Log, TEXT("Post-push will try to unlock: %s"), *Filename);
				//				}
				//			}
				//		}
				//	}
				//}
			}
		}		
		
	}
	// push the branch to its default remote
	// (works only if the default remote "origin" is set and does not require authentication)
	for (const auto& Sub : AllProjects) {
		FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot + TEXT("/") + Sub;
		TArray<FString> Parameters;
		Parameters.Add(TEXT("--set-upstream"));
		// TODO Configure origin
		Parameters.Add(TEXT("origin"));
		Parameters.Add(TEXT("HEAD"));
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("push"), InCommand.PathToGitBinary, PathToRepositoryRoot, Parameters, TArray<FString>(), InCommand.InfoMessages, InCommand.ErrorMessages);
	}

	//if(InCommand.bCommandSuccessful && InCommand.bUsingGitLfsLocking && FilesToUnlock.Num() > 0)
	//{
	//	TArray<FString> AbsFilesToUnlock;
	//	// unlock files: execute the LFS command on relative filenames
	//	for(const auto& FileToUnlock : FilesToUnlock)
	//	{
	//		TArray<FString> OneFile;
	//		OneFile.Add(FileToUnlock);
	//		bool bUnlocked = GitSourceControlUtils::RunCommand(TEXT("lfs unlock"), InCommand.PathToGitBinary, FilesRepoRoot[FileToUnlock], TArray<FString>(), OneFile, InCommand.InfoMessages, InCommand.ErrorMessages);
	//		if (!bUnlocked)
	//		{
	//			// Report but don't fail, it's not essential
	//			UE_LOG(LogSourceControl, Log, TEXT("Unlock failed for %s"), *FileToUnlock);	
	//		}
	//		AbsFilesToUnlock.Add(FilesRepoRoot[FileToUnlock] + TEXT("/") + FileToUnlock);
	//	}
	//	
	//	// We need to update status if we unlock
	//	// This command needs absolute filenames
	//	GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, AbsFilesToUnlock, InCommand.ErrorMessages, States);
	//	
	//}

	return InCommand.bCommandSuccessful;
}

bool FGitPushWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitUpdateStatusWorker::GetName() const
{
	return "UpdateStatus";
}

bool FGitUpdateStatusWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TArray<FString> AllProjects;
	GitSourceControlUtils::GetSubModulesRoots(AllProjects);
	AllProjects.Add(TEXT(""));
	if (InCommand.Files.Num() > 0) {
		TArray<FString> InFiles;
		for (const auto& File : InCommand.Files) InFiles.Add(File);
		for (const auto& Sub : AllProjects) {
			FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
			if (Sub != TEXT("")) PathToRepositoryRoot += TEXT("/") + Sub;
			TArray<FString> Files;
			for (const auto& File : InFiles) {
				if (FPaths::IsRelative(File)) {
					FString AbsFileName = PathToRepositoryRoot + TEXT("/") + File;
					if (FPaths::FileExists(AbsFileName)) {
						Files.Add(File);
					}
				}
				else {
					if (File.StartsWith(PathToRepositoryRoot))
						Files.Add(File);
				}
			}
			if (Files.Num() == 0) continue;
			for (const auto& File : Files) {
				UE_LOG(LogSourceControl, Warning, TEXT("Updating file: %s at dir: %s"), *File, *PathToRepositoryRoot);
				InFiles.RemoveSwap(File, true);
			}

			TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);
			InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, Files, InCommand.ErrorMessages, States);
			GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));

			if (Operation->ShouldUpdateHistory())
			{
				for (int32 Index = 0; Index < States.Num(); Index++)
				{
					FString& File = Files[Index];
					TGitSourceControlHistory History;

					if (States[Index].IsConflicted())
					{
						// In case of a merge conflict, we first need to get the tip of the "remote branch" (MERGE_HEAD)
						GitSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, PathToRepositoryRoot, File, true, InCommand.ErrorMessages, History);
					}
					// Get the history of the file in the current branch
					InCommand.bCommandSuccessful &= GitSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, PathToRepositoryRoot, File, false, InCommand.ErrorMessages, History);
					Histories.Add(*File, History);
				}
			}
		}
		//GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);
	}
	else
	{
		for (const auto& Sub : AllProjects) {
			FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
			if (Sub != TEXT("")) PathToRepositoryRoot += TEXT("/") + Sub;
			TArray<FString> ProjectDirs;
			ProjectDirs.Add(PathToRepositoryRoot + TEXT("/"));
			InCommand.bCommandSuccessful = GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, ProjectDirs, InCommand.ErrorMessages, States);
		}
	}
	// don't use the ShouldUpdateModifiedState() hint here as it is specific to Perforce: the above normal Git status has already told us this information (like Git and Mercurial)

	return InCommand.bCommandSuccessful;
}

bool FGitUpdateStatusWorker::UpdateStates() const
{
	bool bUpdated = GitSourceControlUtils::UpdateCachedStates(States);

	FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>( "GitSourceControl" );
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	const FDateTime Now = FDateTime::Now();

	// add history, if any
	for(const auto& History : Histories)
	{
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(History.Key);
		State->History = History.Value;
		State->TimeStamp = Now;
		bUpdated = true;
	}

	return bUpdated;
}

FName FGitCopyWorker::GetName() const
{
	return "Copy";
}

bool FGitCopyWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TArray<FString> AllProjects;
	GitSourceControlUtils::GetSubModulesRoots(AllProjects);
	AllProjects.Add(TEXT(""));
	TArray<FString> InFiles;
	for (const auto& File : InCommand.Files) InFiles.Add(File);
	for (const auto& Sub : AllProjects) {
		FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
		if (Sub != TEXT("")) PathToRepositoryRoot += TEXT("/") + Sub;
		TArray<FString> Files;
		for (const auto& File : InFiles) {
			if (FPaths::IsRelative(File)) {
				FString AbsFileName = PathToRepositoryRoot + TEXT("/") + File;
				if (FPaths::FileExists(AbsFileName)) {
					Files.Add(File);
				}
			}
			else {
				if (File.StartsWith(PathToRepositoryRoot))
					Files.Add(File);
			}
		}
		if (Files.Num() == 0) continue;
		for (const auto& File : Files) {
			UE_LOG(LogSourceControl, Warning, TEXT("Adding file: %s at dir: %s"), *File, *PathToRepositoryRoot);
			InFiles.RemoveSwap(File, true);
		}

		// Copy or Move operation on a single file : Git does not need an explicit copy nor move,
		// but after a Move the Editor create a redirector file with the old asset name that points to the new asset.
		// The redirector needs to be commited with the new asset to perform a real rename.
		// => the following is to "MarkForAdd" the redirector, but it still need to be committed by selecting the whole directory and "check-in"
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), Files, InCommand.InfoMessages, InCommand.ErrorMessages);

		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, Files, InCommand.ErrorMessages, States);
	}
	return InCommand.bCommandSuccessful;
}

bool FGitCopyWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitResolveWorker::GetName() const
{
	return "Resolve";
}

bool FGitResolveWorker::Execute( class FGitSourceControlCommand& InCommand )
{
	check(InCommand.Operation->GetName() == GetName());

	TArray<FString> AllProjects;
	GitSourceControlUtils::GetSubModulesRoots(AllProjects);
	AllProjects.Add(TEXT(""));
	TArray<FString> InFiles;
	for (const auto& File : InCommand.Files) InFiles.Add(File);
	for (const auto& Sub : AllProjects) {
		FString PathToRepositoryRoot = InCommand.PathToRepositoryRoot;
		if (Sub != TEXT("")) PathToRepositoryRoot += TEXT("/") + Sub;
		TArray<FString> Files;
		for (const auto& File : InFiles) {
			if (FPaths::IsRelative(File)) {
				FString AbsFileName = PathToRepositoryRoot + TEXT("/") + File;
				if (FPaths::FileExists(AbsFileName)) {
					Files.Add(File);
				}
			}
			else {
				if (File.StartsWith(PathToRepositoryRoot))
					Files.Add(File);
			}
		}
		if (Files.Num() == 0) continue;
		for (const auto& File : Files) {
			UE_LOG(LogSourceControl, Warning, TEXT("Resolving file: %s at dir: %s"), *File, *PathToRepositoryRoot);
			InFiles.RemoveSwap(File, true);
		}

		// mark the conflicting files as resolved:
		TArray<FString> Results;
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("add"), InCommand.PathToGitBinary, PathToRepositoryRoot, TArray<FString>(), Files, Results, InCommand.ErrorMessages);

		// now update the status of our files
		GitSourceControlUtils::RunUpdateStatus(InCommand.PathToGitBinary, PathToRepositoryRoot, InCommand.bUsingGitLfsLocking, Files, InCommand.ErrorMessages, States);
	}
	return InCommand.bCommandSuccessful;
}

bool FGitResolveWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

#undef LOCTEXT_NAMESPACE
