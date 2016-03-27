// Copyright (c) 2016 Codice Software - Sebastien Rombauts (sebastien.rombauts@gmail.com)

#include "PlasticSourceControlPrivatePCH.h"
#include "PlasticSourceControlUtils.h"
#include "PlasticSourceControlState.h"
#include "PlasticSourceControlModule.h"
#include "PlasticSourceControlCommand.h"

#if PLATFORM_LINUX
#include <sys/ioctl.h>
#endif


namespace PlasticSourceControlConstants
{
	/** The maximum number of files we submit in a single Plastic command */
	const int32 MaxFilesPerBatch = 50;
}

FScopedTempFile::FScopedTempFile(const FText& InText)
{
	Filename = FPaths::CreateTempFilename(*FPaths::GameLogDir(), TEXT("Plastic-Temp"), TEXT(".txt"));
	if(!FFileHelper::SaveStringToFile(InText.ToString(), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to write to temp file: %s"), *Filename);
	}
}

FScopedTempFile::~FScopedTempFile()
{
	if(FPaths::FileExists(Filename))
	{
		if(!FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Filename))
		{
			UE_LOG(LogSourceControl, Error, TEXT("Failed to delete temp file: %s"), *Filename);
		}
	}
}

const FString& FScopedTempFile::GetFilename() const
{
	return Filename;
}


namespace PlasticSourceControlUtils
{

// Launch the Plastic command line process and extract its results & errors
static bool RunCommandInternalRaw(const FString& InCommand, const FString& InPathToPlasticBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors)
{
	int32 ReturnCode = 0;
	// start with the Plastic command itself ("status", "log", "commit"...)
	FString FullCommand = InCommand;

	if(!InRepositoryRoot.IsEmpty())
	{
		// Specify the Plastic workspace (the root)
		FullCommand += TEXT(" \"");
		FullCommand += InRepositoryRoot;
		FullCommand += TEXT("\" ");
	}

	// Append to the command all parameters, and then finally the files
	for(const auto& Parameter : InParameters)
	{
		FullCommand += TEXT(" ");
		FullCommand += Parameter;
	}
	for(const auto& File : InFiles)
	{
		FullCommand += TEXT(" \"");
		FullCommand += File;
		FullCommand += TEXT("\"");
	}
	// TODO does Plastic have a "--non-interactive" option, or detects when there are no connected standard input/output streams

	UE_LOG(LogSourceControl, Log, TEXT("RunCommandInternalRaw: 'cm %s'"), *FullCommand);
// @todo: temporary debug logs
	UE_LOG(LogSourceControl, Log, TEXT("RunCommandInternalRaw: '%s %s'"), *InPathToPlasticBinary, *FullCommand);
	FPlatformProcess::ExecProcess(*InPathToPlasticBinary, *FullCommand, &ReturnCode, &OutResults, &OutErrors);
	UE_LOG(LogSourceControl, Log, TEXT("RunCommandInternalRaw: ExecProcess ReturnCode=%d OutResults='%s'"), ReturnCode, *OutResults);
	if (!OutErrors.IsEmpty())
	{
		UE_LOG(LogSourceControl, Error, TEXT("RunCommandInternalRaw: ExecProcess ReturnCode=%d OutErrors='%s'"), ReturnCode, *OutErrors);
	}

	return ReturnCode == 0;
}

// Basic parsing or results & errors from the Plastic command line process
static bool RunCommandInternal(const FString& InCommand, const FString& InPathToPlasticBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult;
	FString Results;
	FString Errors;

	bResult = RunCommandInternalRaw(InCommand, InPathToPlasticBinary, InRepositoryRoot, InParameters, InFiles, Results, Errors);
	Results.ParseIntoArray(OutResults, TEXT("\n"), true);
	Errors.ParseIntoArray(OutErrorMessages, TEXT("\n"), true);

	return bResult;
}

FString FindPlasticBinaryPath()
{
	return FString(TEXT("cm"));
}

bool CheckPlasticAvailability(const FString& InPathToPlasticBinary)
{
	bool bPlasticAvailable = false;

	FString InfoMessages;
	FString ErrorMessages;
	bPlasticAvailable = RunCommandInternalRaw(TEXT("version"), InPathToPlasticBinary, FString(), TArray<FString>(), TArray<FString>(), InfoMessages, ErrorMessages);

	return bPlasticAvailable;
}

// Find the root of the Plastic repository, looking from the GameDir and upward in its parent directories.
bool FindRootDirectory(const FString& InPathToGameDir, FString& OutRepositoryRoot)
{
	bool bFound = false;
	FString PathToPlasticSubdirectory;
	OutRepositoryRoot = InPathToGameDir;

	while(!bFound && !OutRepositoryRoot.IsEmpty())
	{
		PathToPlasticSubdirectory = OutRepositoryRoot;
		PathToPlasticSubdirectory += TEXT(".plastic"); // Look for the ".plastic" subdirectory present at the root of every Plastic repository
		bFound = IFileManager::Get().DirectoryExists(*PathToPlasticSubdirectory);
		if(!bFound)
		{
			int32 LastSlashIndex;
			OutRepositoryRoot = OutRepositoryRoot.LeftChop(5);
			if(OutRepositoryRoot.FindLastChar('/', LastSlashIndex))
			{
				OutRepositoryRoot = OutRepositoryRoot.Left(LastSlashIndex + 1);
			}
			else
			{
				OutRepositoryRoot.Empty();
			}
		}
	}
	if (!bFound)
	{
		OutRepositoryRoot = InPathToGameDir; // If not found, return the GameDir as best possible root.
	}
	return bFound;
}

bool RunCommand(const FString& InCommand, const FString& InPathToPlasticBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult = true;

	if(InFiles.Num() > PlasticSourceControlConstants::MaxFilesPerBatch)
	{
		// Batch files up so we dont exceed command-line limits
		int32 FileCount = 0;
		while(FileCount < InFiles.Num())
		{
			TArray<FString> FilesInBatch;
			for(int32 FileIndex = 0; FileCount < InFiles.Num() && FileIndex < PlasticSourceControlConstants::MaxFilesPerBatch; FileIndex++, FileCount++)
			{
				FilesInBatch.Add(InFiles[FileCount]);
			}

			TArray<FString> BatchResults;
			TArray<FString> BatchErrors;
			bResult &= RunCommandInternal(InCommand, InPathToPlasticBinary, InRepositoryRoot, InParameters, FilesInBatch, BatchResults, BatchErrors);
			OutResults += BatchResults;
			OutErrorMessages += BatchErrors;
		}
	}
	else
	{
		bResult &= RunCommandInternal(InCommand, InPathToPlasticBinary, InRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
	}

	return bResult;
}

bool UpdateCachedStates(const TArray<FPlasticSourceControlState>& InStates)
{
	FPlasticSourceControlModule& PlasticSourceControl = FModuleManager::LoadModuleChecked<FPlasticSourceControlModule>( "PlasticSourceControl" );
	FPlasticSourceControlProvider& Provider = PlasticSourceControl.GetProvider();
	int NbStatesUpdated = 0;

	for(const auto& InState : InStates)
	{
		TSharedRef<FPlasticSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(InState.LocalFilename);
		if(State->WorkingCopyState != InState.WorkingCopyState)
		{
			State->WorkingCopyState = InState.WorkingCopyState;
			State->PendingMergeBaseFileHash = InState.PendingMergeBaseFileHash;
		//	State->TimeStamp = InState.TimeStamp; // @todo Bug report: Workaround a bug with the Source Control Module not updating file state after a "Save"
			NbStatesUpdated++;
		}
	}

	return (NbStatesUpdated > 0);
}

/**
 * Helper struct for RemoveRedundantErrors()
 */
struct FRemoveRedundantErrors
{
	FRemoveRedundantErrors(const FString& InFilter)
		: Filter(InFilter)
	{
	}

	bool operator()(const FString& String) const
	{
		if(String.Contains(Filter))
		{
			return true;
		}

		return false;
	}

	/** The filter string we try to identify in the reported error */
	FString Filter;
};

void RemoveRedundantErrors(FPlasticSourceControlCommand& InCommand, const FString& InFilter)
{
	bool bFoundRedundantError = false;
	for(auto Iter(InCommand.ErrorMessages.CreateConstIterator()); Iter; Iter++)
	{
		if(Iter->Contains(InFilter))
		{
			InCommand.InfoMessages.Add(*Iter);
			bFoundRedundantError = true;
		}
	}

	InCommand.ErrorMessages.RemoveAll( FRemoveRedundantErrors(InFilter) );

	// if we have no error messages now, assume success!
	if(bFoundRedundantError && InCommand.ErrorMessages.Num() == 0 && !InCommand.bCommandSuccessful)
	{
		InCommand.bCommandSuccessful = true;
	}
}

}
