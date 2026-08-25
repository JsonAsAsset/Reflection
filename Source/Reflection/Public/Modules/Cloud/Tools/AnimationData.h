/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimMontage.h"
#include "Importers/Constructor/Importer.h"
#include "Modules/Tools/SelectedAssetsBase.h"
#include "Utilities/JsonHelpers.h"

/* The curves a sequence carries, read out of whichever field the export put them in.
 *
 * Split out of ReadAnimationData so an importer that builds the rest of the sequence itself can
 * still get them. It has to run after any InitializeModel: that resets the model, curves included. */
inline bool ReadAnimationCurves(USerializerContainer* Container, UAnimSequenceBase* AnimSequenceBase) {
	if (AnimSequenceBase == nullptr) return false;

	USkeleton* Skeleton = AnimSequenceBase->GetSkeleton();

	if (!Skeleton) {
		UE_LOG(LogReflection, Error, TEXT("Could not get valid Skeleton"));
		return false;
	}
	
	/* In Unreal Engine 5, a new data model has been added to edit animation curves */
	/* Unreal Engine 5.2 changed handling getting a data model */
#if UE5_2_BEYOND
	IAnimationDataController& Controller = AnimSequenceBase->GetController();
#if ENGINE_MINOR_VERSION >= 3
	IAnimationDataModel* DataModel = AnimSequenceBase->GetDataModel();
#endif
#endif

	/* Empty curves */
#if UE5_2_BEYOND
	Controller.RemoveAllCurvesOfType(ERawCurveTrackTypes::RCT_Float, false);
#endif

	/* Some UEParse versions have different named objects for curves ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	const TSharedPtr<FJsonObject>* RawCurveData;
	
	TArray<TSharedPtr<FJsonValue>> FloatCurves;
	TArray<TSharedPtr<FJsonValue>> Notifies;
	
	if (Container->GetAssetData()->TryGetObjectField(TEXT("RawCurveData"), RawCurveData)) {
		FloatCurves = Container->GetAssetData()->GetObjectField(TEXT("RawCurveData"))->GetArrayField(TEXT("FloatCurves"));
	}
	
	if (Container->GetAssetExport()->TryGetObjectField(TEXT("CompressedCurveData"), RawCurveData)) {
		FloatCurves = Container->GetAssetExport()->GetObjectField(TEXT("CompressedCurveData"))->GetArrayField(TEXT("FloatCurves"));
	}

	int32 UnnamedCurves = 0;

	/* Import the curves */
	for (const auto& FloatCurveObject : FloatCurves) {
		/* Curve Display Name */
		FString DisplayName = "";
		if (FloatCurveObject->AsObject()->HasField(TEXT("Name"))) {
			DisplayName = FloatCurveObject->AsObject()->GetObjectField(TEXT("Name"))->GetStringField(TEXT("DisplayName"));
		} else {
			DisplayName = FloatCurveObject->AsObject()->GetStringField(TEXT("CurveName"));
		}

		/* A curve the export names nowhere, which a cook leaves behind where it kept the curve's
		 * UID and dropped the name it stood for. It does not arrive empty: a name is an FName, and
		 * an FName holding nothing is written out as the word None, so the check is what the string
		 * reads back as rather than whether there is one.
		 *
		 * Nothing can be added under it either way. The skeleton refuses a None and hands back the
		 * name untouched, which leaves the identifier below on the 65535 an unset UID reads as, and
		 * the curve goes onto the sequence as one the editor can neither show nor drive. */
		if (FName(*DisplayName).IsNone()) {
			++UnnamedCurves;

			continue;
		}

		/* Used to define if a curve is a curve is metadata or not. */
		int CurveTypeFlags = FloatCurveObject->AsObject()->GetIntegerField(TEXT("CurveTypeFlags"));

		/* Adding the track name to skeletons differ between Unreal Engine 4 and 5 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
#if ENGINE_UE4
		FSmartName NewTrackName;

		Skeleton->AddSmartNameAndModify(USkeleton::AnimCurveMappingName, FName(*DisplayName), NewTrackName);
		ensureAlways(Skeleton->GetSmartNameByUID(USkeleton::AnimCurveMappingName, NewTrackName.UID, NewTrackName));
#endif
		
#if ENGINE_UE5
#if ENGINE_MINOR_VERSION <= 3
		FSmartName NewTrackName;

		Skeleton->AddSmartNameAndModify(USkeleton::AnimCurveMappingName, FName(*DisplayName), NewTrackName);
		
		ensureAlways(Skeleton->GetSmartNameByUID(USkeleton::AnimCurveMappingName, NewTrackName.UID, NewTrackName));
		FAnimationCurveIdentifier CurveId = FAnimationCurveIdentifier(NewTrackName, ERawCurveTrackTypes::RCT_Float);
#endif
		/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		
#if ENGINE_MINOR_VERSION >= 4
		Controller.OpenBracket(FText::FromString("Curve Import"));

		/* Create Curve Identifier */
		FName CurveName = FName(*DisplayName);
		FAnimationCurveIdentifier CurveId(CurveName, ERawCurveTrackTypes::RCT_Float);

		/* Add curve metadata to skeleton */
		Skeleton->AddCurveMetaData(CurveName);
		/* Add or update the curve */
		const FFloatCurve* ExistingCurve = DataModel->FindFloatCurve(CurveId);
		if (ExistingCurve != nullptr)
		{
			Controller.RemoveCurve(CurveId);
		}
		Controller.AddCurve(CurveId, CurveTypeFlags);
#endif
		/* For Unreal Engine 5.3 and above, the smart name's display name is required */
#if ENGINE_MINOR_VERSION == 3 && ENGINE_PATCH_VERSION < 2
		Controller->AddCurve(CurveId, CurveTypeFlags);
#elif (ENGINE_MINOR_VERSION == 3 && ENGINE_PATCH_VERSION == 2)
		Controller.AddCurve(CurveId, CurveTypeFlags);
#endif
		/* For Unreal Engine 5.2 and below, just the smart name is required */
#if ENGINE_MINOR_VERSION < 3
		AnimSequenceBase->Modify(true);

		IAnimationDataController& LocalOneController = AnimSequenceBase->GetController();
		LocalOneController.AddCurve(FAnimationCurveIdentifier(NewTrackName, ERawCurveTrackTypes::RCT_Float), CurveTypeFlags);

		TArray<FLinearColor> RandomizedColorArray = {
			FLinearColor(.904, .323, .539),
			FLinearColor(.552, .737, .328),
			FLinearColor(.947, .418, .219),
			FLinearColor(.156, .624, .921),
			FLinearColor(.921, .314, .337),
			FLinearColor(.361, .651, .332),
			FLinearColor(.982, .565, .254),
			FLinearColor(.246, .223, .514),
			FLinearColor(.208, .386, .687),
			FLinearColor(.223, .590, .337),
			FLinearColor(.230, .291, .591)
		}; { 
			auto Index = rand() % RandomizedColorArray.Num();

			if (RandomizedColorArray.IsValidIndex(Index)) {
				LocalOneController.SetCurveColor(FAnimationCurveIdentifier(NewTrackName, ERawCurveTrackTypes::RCT_Float), RandomizedColorArray[Index]);
			}
		}

		AnimSequenceBase->PostEditChange();
#endif
#endif

		/* Keys of the track */
		TArray<TSharedPtr<FJsonValue>> Keys = FloatCurveObject->AsObject()->GetObjectField(TEXT("FloatCurve"))->GetArrayField(TEXT("Keys"));

		for (const auto& JsonKey : Keys) {
			TSharedPtr<FJsonObject> Key = JsonKey->AsObject();

			FRichCurveKey RichKey = ObjectToRichCurveKey(Key);

			/* Unreal Engine 5 and Unreal Engine 4
			 * have different ways of adding curves
			 *
			 * Unreal Engine 4: Simply adding curves to RawCurveData
			 * Unreal Engine 5: Using a AnimDataController to handle adding curves */
#if UE5_2_BEYOND
			Controller.SetCurveKey(CurveId, RichKey);
#endif
#if ENGINE_UE4
			FRawCurveTracks& Tracks = AnimSequenceBase->RawCurveData;
#endif
#if ENGINE_UE4
			Tracks.AddFloatCurveKey(NewTrackName, CurveTypeFlags, RichKey.Time, RichKey.Value);

			for (FFloatCurve& Track : Tracks.FloatCurves) {
				if (Track.Name == NewTrackName) {
					const int32 LastIndex = Track.FloatCurve.Keys.Num() - 1;
					Track.FloatCurve.Keys[LastIndex].ArriveTangent = RichKey.ArriveTangent;
					Track.FloatCurve.Keys[LastIndex].LeaveTangent = RichKey.LeaveTangent;
					Track.FloatCurve.Keys[LastIndex].InterpMode = RichKey.InterpMode;
				}
			}
#endif
#if UE5_1_BELOW
			/* 5.0 and 5.1 reach the controller off the sequence rather than through the one held
			 * above, which those versions are not given. Without this the curve is added and left
			 * with no keys at all, so it shows on the sequence and drives nothing. */
			AnimSequenceBase->GetController().SetCurveKey(
				FAnimationCurveIdentifier(NewTrackName, ERawCurveTrackTypes::RCT_Float), RichKey, false);
#endif
		}
#if UE5_2_BEYOND
		Controller.CloseBracket();
#endif
	}

	if (UnnamedCurves > 0) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" left %d curve(s) out, the export naming none of them"),
			*AnimSequenceBase->GetName(), UnnamedCurves);
	}

	return true;
}

inline bool ReadAnimationData(USerializerContainer* Container, const bool UseSelectedAsset, const IImporter* Importer = nullptr) {
	/* Animation Sequence Base reference, either by using the selected asset in the browser, or through an importer */
	UAnimSequenceBase* AnimSequenceBase = nullptr;

	if (UseSelectedAsset) {
		AnimSequenceBase = GetSelectedAsset<UAnimSequenceBase>(false, Container->GetAssetName());
	} else {
		if (Container->GetAsset()) {
			AnimSequenceBase = Cast<UAnimSequenceBase>(Container->GetAsset());
		}
	}

	/* Animation Montages: If we're importing a montage, create it */
	if (!AnimSequenceBase && Container->GetAssetClass()->IsChildOf<UAnimMontage>()) {
		AnimSequenceBase = NewObject<UAnimMontage>(Container->GetPackage(), Container->GetAssetClass(), *Container->GetAssetName(), RF_Public | RF_Standalone);
	}

	if (UseSelectedAsset) {
		if (!AnimSequenceBase) {
			return false;
		}
	}

	if (!AnimSequenceBase) return false;

	/* Empty all Notifies */
	if (UAnimSequence* CastedAnimSequence = Cast<UAnimSequence>(AnimSequenceBase)) {
		CastedAnimSequence->AuthoredSyncMarkers.Empty();
		CastedAnimSequence->Notifies.Empty();
	}

	Container->DeserializeExports(AnimSequenceBase);

	/* Update Sequence Length */
	if (const auto& Data = Container->GetAssetData(); Data->HasField(TEXT("SequenceLength"))) {
		const float SequenceLength = Data->GetNumberField(TEXT("SequenceLength"));

		SetAnimSequenceLength(AnimSequenceBase, SequenceLength);
	}
	
	/* Deserialize properties */
	Container->GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(Container->GetAssetData(), {
		"NumFrames",
		"TrackToSkeletonMapTable",
		"SequenceLength",
		"SkeletonGuid",
		"CompressedTrackToSkeletonMapTable",
		"CompressedDataStructure",
		"CompressedRawDataSize",
		"RawCurveData"
	}), AnimSequenceBase);

	BuildAnimNotifyTracks(AnimSequenceBase);

	if (!ReadAnimationCurves(Container, AnimSequenceBase)) return false;

	UpdateAnimationCaching(AnimSequenceBase);
	
	AnimSequenceBase->Modify();
	AnimSequenceBase->PostEditChange();

	if (Importer) {
		return Importer->OnAssetCreation(AnimSequenceBase);
	}

	return true;
}

class REFLECTION_API TToolAnimationData : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("AnimSequence"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Animations"); }
	virtual FText GetTooltip() const override { return FText::FromString("Reflects curve data, notifies and other properties"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Animation_24x"); }
};

REGISTER_TOOL(TToolAnimationData)