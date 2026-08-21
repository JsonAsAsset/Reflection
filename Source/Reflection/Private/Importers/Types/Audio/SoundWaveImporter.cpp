/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Audio/SoundWaveImporter.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"

#include "Engine/EngineUtilities.h"
#include "Engine/Log.h"
#include "Importers/Constructor/Graph/SoundGraph.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/Redirector.h"
#include "Utilities/JsonHelpers.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the sound wave type used to come in from */
#if UE4_25_BELOW
#include "Sound/SoundWave.h"
#endif

namespace {
	/* Asking for the export alone gets json describing a wave. The audio only gets written out
	 * when the request says to save it, and the reply names the file that was written. */
	bool DownloadAudioFile(const FString& FetchPath, FString& OutSavePath) {
		const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(FetchPath, {
			{
				"save",
				"true"
			}
		});

		if (Response == nullptr) {
			return false;
		}

		return Response->TryGetStringField(TEXT("file"), OutSavePath) && !OutSavePath.IsEmpty();
	}

	/* Hands the file to the engine's audio factory, which is what reads the format and builds the
	 * wave. It creates the package it lands in as well, off the destination folder and the name of
	 * the file, rather than filling the one this import was handed. */
	USoundWave* ImportAudioFile(const FString& SavePath, const FString& ObjectPath) {
		IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();

		UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
		ImportData->Filenames.Add(SavePath);
		ImportData->DestinationPath = FPaths::GetPath(ObjectPath);
		ImportData->bReplaceExisting = true;

		const TArray<UObject*> AssetsImported = AssetTools.ImportAssetsAutomated(ImportData);

		/* Nothing imported where the wave is already in the project is the wave that is already in
		 * the project, which is what was being asked for */
		if (!AssetsImported.IsValidIndex(0)) {
			return LoadObjectByPath<USoundWave>(ObjectPath);
		}

		return Cast<USoundWave>(AssetsImported[0]);
	}
}

bool ISoundWaveImporter::Import() {
	/* Waves are what the sound groups act on, and a project missing them is worth saying once */
	ISoundGraph::NotifySoundGroupsUnconfigured();

	/* Cloud knows this by the path it had before the redirects and the export directory had their
	 * say, and the factory below is handed the same path back through them */
	FString FetchPath = GetPackage()->GetPathName();
	FRRedirects::Reverse(FetchPath);

	FString SavePath;

	if (!DownloadAudioFile(FetchPath, SavePath)) {
		FImportIssues::Report(EImportIssue::MissingAsset, TEXT("Cloud wrote no audio file for this wave"), FetchPath);

		return false;
	}

	if (!FPaths::FileExists(SavePath)) {
		FImportIssues::Report(EImportIssue::MissingAsset, TEXT("Couldn't download a sound wave"), SavePath);

		return false;
	}

	FString ObjectPath = FetchPath + TEXT(".") + GetAssetName();
	FRRedirects::Redirect(ObjectPath);

	USoundWave* SoundWave = ImportAudioFile(SavePath, ObjectPath);

	if (SoundWave == nullptr) {
		FImportIssues::Report(EImportIssue::Failed, TEXT("The audio factory built no wave from the file"), SavePath);

		return false;
	}

	/* The file Cloud wrote is a scratch file, and a wave pointing at one reimports from a path
	 * that won't be there */
	SoundWave->AssetImportData = nullptr;

	/* The factory read the file and nothing else, so everything on the export that is a setting
	 * rather than a fact of the audio is still to be put on */
	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), TArray<FString> {
		/* Measured off the file that was just imported. What the source game cooked describes the
		 * encoding it cooked, and writing that over these leaves a wave reporting a length, a rate
		 * and a channel count the audio it actually holds does not have. */
		"NumChannels",
		"SampleRate",
		"ImportedSampleRate",
		"Duration",
		"TotalSamples",
		"RawPCMDataSize",
		"CompressedDataGuid",

		/* Read out of the file's own chunks by the factory */
		"CuePoints"
	}), SoundWave);

	SoundWave->PostEditChange();

	/* The package the factory made is the one holding the asset, so that is the one that gets
	 * synced to the Content Browser and saved */
	CreateAsset(SoundWave);
	SetPackage(SoundWave->GetPackage());

	return OnAssetCreation(SoundWave);
}
