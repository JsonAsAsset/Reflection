/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Materials/MaterialInstanceConstantImporter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Dom/JsonObject.h"
#include "RHIDefinitions.h"
#include "MaterialShared.h"
#include "Utilities/JsonHelpers.h"

UObject* IMaterialInstanceConstantImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UMaterialInstanceConstant>(GetPackage(), UMaterialInstanceConstant::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IMaterialInstanceConstantImporter::Import() {
	UMaterialInstanceConstant* MaterialInstanceConstant = Create<UMaterialInstanceConstant>();

	/* Specific fix for 4.16 engines */
	const TArray<FString> ParameterFields = {
		TEXT("ScalarParameterValues"),
		TEXT("TextureParameterValues"),
		TEXT("VectorParameterValues")
	};

	for (const FString& FieldName : ParameterFields) {
		if (GetAssetDataAsValue().Has(FieldName)) {
			TArray<FUObjectJsonValueExport> Params = GetAssetDataAsValue().GetArray(FieldName);
			
			ConvertParameterNamesToInfos(Params);
			GetAssetDataAsValue().SetArray(FieldName, Params);
		}
	}
	
	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(),
	{
		"CachedReferencedTextures"
	}), MaterialInstanceConstant);

	/* What to draw with instead where Nanite is drawing.
	 *
	 * The material an instance names for Nanite is kept twice over: a hard reference the editor
	 * shows and works from, and another the cook writes for the game to run on. The editor's is
	 * editor-only and is left out of the cook on purpose, so what comes back names the material in
	 * the half the editor never reads and the field it does read stays empty.
	 *
	 * Worse, the cooked half is not written as a property at all: the struct serializes itself and
	 * keeps that reference to itself, so nothing put it anywhere either. Set through the instance,
	 * which fills in both halves. */
	if (GetAssetDataAsValue().Has(TEXT("NaniteOverrideMaterial"))) {
		const FUObjectJsonValueExport Override = GetAssetDataAsValue().GetObject(TEXT("NaniteOverrideMaterial"));

		TSharedPtr<FJsonObject> Named = Override.GetObject(TEXT("OverrideMaterial")).JsonObject;

		/* Or the way an older one named it, which is a path rather than a reference */
		if (!Named.IsValid() || !Named->HasField(TEXT("ObjectName"))) {
			if (const FUObjectJsonValueExport Held = Override.GetObject(TEXT("OverrideMaterialRef")); Held.JsonObject.IsValid()) {
				if (FString Path; Held.JsonObject->TryGetStringField(TEXT("AssetPathName"), Path) && !Path.IsEmpty() && Path != TEXT("None")) {
					Named = MakeShared<FJsonObject>();

					Named->SetStringField(TEXT("ObjectName"), FPaths::GetBaseFilename(Path));
					Named->SetStringField(TEXT("ObjectPath"), Path);
				}
			}
		}

		if (Named.IsValid() && Named->HasField(TEXT("ObjectName"))) {
			TObjectPtr<UObject> Loaded;

			LoadExport<UObject>(&Named, Loaded);

			if (UMaterialInterface* Drawn = Cast<UMaterialInterface>(Loaded.Get())) {
				bool bOverride = true;

				Override.JsonObject->TryGetBoolField(TEXT("bEnableOverride"), bOverride);

				/* Put in place through the class, since the one call that does this is not one the
				 * engine hands out to anybody outside it */
				if (const FStructProperty* Holds = CastField<FStructProperty>(MaterialInstanceConstant->GetClass()->FindPropertyByName(TEXT("NaniteOverrideMaterial")))) {
					void* Inside = Holds->ContainerPtrToValuePtr<void>(MaterialInstanceConstant);

					/* The half the editor shows, and the half the cook writes whichever this
					 * build has. Older ones keep only the second. */
					const FObjectProperty* Shown = CastField<FObjectProperty>(Holds->Struct->FindPropertyByName(TEXT("OverrideMaterialEditor")));

					if (Shown == nullptr) {
						Shown = CastField<FObjectProperty>(Holds->Struct->FindPropertyByName(TEXT("OverrideMaterial")));
					}

					if (Shown != nullptr) {
						Shown->SetObjectPropertyValue(Shown->ContainerPtrToValuePtr<void>(Inside), Drawn);

						if (const FBoolProperty* Enabled = CastField<FBoolProperty>(Holds->Struct->FindPropertyByName(TEXT("bEnableOverride")))) {
							Enabled->SetPropertyValue(Enabled->ContainerPtrToValuePtr<void>(Inside), bOverride);
						}

						MaterialInstanceConstant->Modify();

						UE_LOG(LogReflection, Display, TEXT("\"%s\" draws with \"%s\" where Nanite draws"), *GetAssetName(), *Drawn->GetName());
					} else {
						UE_LOG(LogReflection, Warning, TEXT("\"%s\" names \"%s\" for Nanite, and this build keeps it nowhere the editor reads"), *GetAssetName(), *Drawn->GetName());
					}
				}
			}
		}
	}

	TArray<FUObjectJsonValueExport> StaticSwitchParametersObjects;
	TArray<FUObjectJsonValueExport> StaticComponentMaskParametersObjects;
	
	/* Optional Editor Data [contains static switch parameters] ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	if (FUObjectExport* EditorOnlyData = GetContainer()->FindByType(FString("MaterialInstanceEditorOnlyData"))) {
		if (EditorOnlyData->GetPropertiesAsValue().Has("StaticParameters")) {
			ReadStaticParameters(EditorOnlyData->GetPropertiesAsValue().GetObject("StaticParameters"), StaticSwitchParametersObjects, StaticComponentMaskParametersObjects);
		}
	}

	/* Read from potential properties inside of asset data */
	if (GetAssetDataAsValue().Has("StaticParametersRuntime")) {
		ReadStaticParameters(GetAssetDataAsValue().GetObject("StaticParametersRuntime"), StaticSwitchParametersObjects, StaticComponentMaskParametersObjects);
	}
	if (GetAssetDataAsValue().Has(TEXT("StaticParameters"))) {
		ReadStaticParameters(GetAssetDataAsValue().GetObject("StaticParameters"), StaticSwitchParametersObjects, StaticComponentMaskParametersObjects);
	}

	/* ~~~~~~~~~ STATIC PARAMETERS ~~~~~~~~~~~ */
/* 5.1 is the one version that hangs the static parameter set off editor-only data. Every other
 * version - 5.0 included, which has no GetEditorOnlyData at all - builds a set up front and
 * pushes it through UpdateStaticPermutation. */
#define MIC_STATICS_VIA_EDITOR_ONLY (UE5_1_BEYOND && !UE5_2_BEYOND)

#if !MIC_STATICS_VIA_EDITOR_ONLY
	FStaticParameterSet NewStaticParameterSet; /* Unreal Engine 5.2/4.26 and beyond have a different method */
#endif

	TArray<FStaticSwitchParameter> StaticSwitchParameters;
	for (FUObjectJsonValueExport& StaticParameter : StaticSwitchParametersObjects) {
		auto ParameterInfo = StaticParameter.GetObject("ParameterInfo");

		/* Create Material Parameter Info */
		FMaterialParameterInfo MaterialParameterParameterInfo = FMaterialParameterInfo(
			StringToName(ParameterInfo.GetString("Name")),
			static_cast<EMaterialParameterAssociation>(StaticEnum<EMaterialParameterAssociation>()->GetValueByNameString(ParameterInfo.GetString("Association"))),
			ParameterInfo.GetInteger("Index")
		);

		/* Now, create the actual switch parameter */
		FStaticSwitchParameter Parameter = FStaticSwitchParameter(
			MaterialParameterParameterInfo,
			StaticParameter.GetBool(TEXT("Value")),
			StaticParameter.GetBool("bOverride"),
			StringToGuid(StaticParameter.GetString("ExpressionGUID"))
		);

		StaticSwitchParameters.Add(Parameter);
#if MIC_STATICS_VIA_EDITOR_ONLY
		MaterialInstanceConstant->GetEditorOnlyData()->StaticParameters.StaticSwitchParameters.Add(Parameter);
#else
		/* Unreal Engine 5.2/4.26 and beyond have a different method */
		NewStaticParameterSet.StaticSwitchParameters.Add(Parameter);
#endif
	}

	TArray<FStaticComponentMaskParameter> StaticSwitchMaskParameters;
	
	for (FUObjectJsonValueExport& StaticParameter : StaticComponentMaskParametersObjects) {
		auto ParameterInfo = StaticParameter.GetObject("ParameterInfo");

		/* Create Material Parameter Info */
		FMaterialParameterInfo MaterialParameterParameterInfo = FMaterialParameterInfo(
			StringToName(ParameterInfo.GetString("Name")),
			static_cast<EMaterialParameterAssociation>(StaticEnum<EMaterialParameterAssociation>()->GetValueByNameString(ParameterInfo.GetString("Association"))),
			ParameterInfo.GetInteger("Index")
		);

		FStaticComponentMaskParameter Parameter = FStaticComponentMaskParameter(
			MaterialParameterParameterInfo,
			StaticParameter.GetBool("R"),
			StaticParameter.GetBool("G"),
			StaticParameter.GetBool("B"),
			StaticParameter.GetBool("A"),
			StaticParameter.GetBool("bOverride"),
			StringToGuid(StaticParameter.GetString("ExpressionGUID"))
		);

		StaticSwitchMaskParameters.Add(Parameter);
#if MIC_STATICS_VIA_EDITOR_ONLY
		MaterialInstanceConstant->GetEditorOnlyData()->StaticParameters.StaticComponentMaskParameters.Add(Parameter);
#else
		NewStaticParameterSet.
		/* EditorOnly is needed on 5.2+ */
		#if UE5_2_BEYOND
			EditorOnly.
		#endif
		StaticComponentMaskParameters.Add(Parameter);
#endif
	}

#if !MIC_STATICS_VIA_EDITOR_ONLY
	FMaterialUpdateContext MaterialUpdateContext(FMaterialUpdateContext::EOptions::Default & ~FMaterialUpdateContext::EOptions::RecreateRenderStates);

	MaterialInstanceConstant->UpdateStaticPermutation(NewStaticParameterSet, &MaterialUpdateContext);
	MaterialInstanceConstant->InitStaticPermutation();
#endif

	return OnAssetCreation(MaterialInstanceConstant);
}

void IMaterialInstanceConstantImporter::ReadStaticParameters(const FUObjectJsonValueExport& StaticParameters, TArray<FUObjectJsonValueExport>& StaticSwitchParameters, TArray<FUObjectJsonValueExport>& StaticComponentMaskParameters) {
	if (StaticParameters.Has("StaticSwitchParameters")) {
		TArray<FUObjectJsonValueExport> Params = StaticParameters.GetArray("StaticSwitchParameters");
		ConvertParameterNamesToInfos(Params);
		
		for (FUObjectJsonValueExport& Parameter : Params) {
			StaticSwitchParameters.Add(Parameter);
		}
	}

	if (StaticParameters.Has("StaticComponentMaskParameters")) {
		TArray<FUObjectJsonValueExport> Params = StaticParameters.GetArray("StaticComponentMaskParameters");
		ConvertParameterNamesToInfos(Params);
		
		for (FUObjectJsonValueExport& Parameter : Params) {
			StaticComponentMaskParameters.Add(Parameter);
		}
	}
}

void IMaterialInstanceConstantImporter::ConvertParameterNamesToInfos(TArray<FUObjectJsonValueExport>& Input) {
	/* Convert ParameterName to be inside ParameterInfo */
	for (FUObjectJsonValueExport& Parameter : Input) {
		if (Parameter.Has("ParameterName")) {
			TSharedPtr<FJsonObject> ParameterInfo = MakeShared<FJsonObject>();
			
			ParameterInfo->SetStringField(TEXT("Name"), Parameter.GetString("ParameterName"));
			Parameter.SetObject("ParameterInfo", ParameterInfo);

			/* Cleanup */
			Parameter.Remove("ParameterName");
		}
	}
}