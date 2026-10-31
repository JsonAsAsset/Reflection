/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Audio/SoundCueImporter.h"
#include "Sound/SoundCue.h"
#include "Utilities/JsonHelpers.h"

UObject* ISoundCueImporter::CreateAsset(UObject* CreatedAsset) {
	/* Built as the class the export is. A game's sound cue template is a cue with editor-only
	 * settings on top, and made as a plain cue it loses its type and every reference to it as one. */
	UClass* Class = AssetExport->GetClass();

	if (Class == nullptr || !Class->IsChildOf(USoundCue::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract)) {
		Class = USoundCue::StaticClass();
	}

	return IImporter::CreateAsset(NewObject<USoundCue>(GetPackage(), Class, *GetAssetName(), RF_Public | RF_Standalone));
}

bool ISoundCueImporter::Import() {
	USoundCue* SoundCue = Create<USoundCue>();

	if (!SoundCue) {
		return false;
	}

	SoundCue->PreEditChange(nullptr);

	/* Start importing nodes ~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	{
		TMap<FString, USoundNode*> SoundCueNodes;

		ConstructNodes(SoundCue, SoundCueNodes);
		SetupNodes(SoundCue, SoundCueNodes);
	}
	/* End of importing nodes ~~~~~~~~~~~~~~~~~~~~~~~~~~ */

	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), TArray<FString> {
		"FirstNode"
	}), SoundCue);
	
	SoundCue->PostEditChange();
	SoundCue->CompileSoundNodesFromGraphNodes();

	return OnAssetCreation(SoundCue);
}
