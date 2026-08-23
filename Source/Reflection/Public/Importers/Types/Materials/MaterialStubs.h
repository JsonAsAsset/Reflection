/* CREDITS!!
 * Function originally created by C0bra5.
 *
 * Fork can be seen here:
 * https://github.com/C0bra5/Reflection */

#pragma once

#if ENGINE_UE5
#if ENGINE_MINOR_VERSION >= 2
#include "MaterialDomain.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#endif

#include "Importers/Types/Materials/MaterialImporter.h"
#include "Engine/Package.h"
#include "Utilities/AssetPaths.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#if UE5_8_BEYOND
#include "Materials/MaterialExpressionUtils.h"
#endif

/* UMaterial only routes its expressions through an expression collection from 5.1 on;
 * before that they sit in the editor-only Expressions array directly */
inline void AddStubExpression(UMaterial* Material, UMaterialExpression* Expression) {
#if UE5_1_BEYOND
	Material->GetExpressionCollection().AddExpression(Expression);
#else
	Material->Expressions.Add(Expression);
#endif
}

inline void CreateStubs(IMaterialImporter* MaterialImporter) {
	UMaterial* Material = MaterialImporter->GetTypedAsset<UMaterial>();
	int32 x = 0, y = 0;

	FExpressionInput* matInput;
	if (Material->bUseMaterialAttributes) {
		// for mats that use attribtues
		x -= 16*8*2;
		FExpressionInput* AttributeInput = Material->GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes);
		UMaterialExpressionSetMaterialAttributes* setAttrNode = NewObject<UMaterialExpressionSetMaterialAttributes>(Material);
		AddStubExpression(Material, setAttrNode);
		setAttrNode->MaterialExpressionEditorX = x;
		setAttrNode->MaterialExpressionEditorY = 0;
		AttributeInput->Expression = setAttrNode;

		// determine input type based on shading model.
		switch (Material->MaterialDomain) {
			case MD_Surface:
				if (Material->GetShadingModels().HasShadingModel(EMaterialShadingModel::MSM_Unlit)) {
					setAttrNode->AttributeSetTypes.Add(FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_EmissiveColor));
				} else {
					setAttrNode->AttributeSetTypes.Add(FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_BaseColor));
				}
				break;
			case MD_LightFunction:
			case MD_PostProcess:
			case MD_UI:
				setAttrNode->AttributeSetTypes.Add(FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_EmissiveColor));
				break;
			case MD_DeferredDecal:
				setAttrNode->AttributeSetTypes.Add(FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_WorldPositionOffset));
				break;
			case MD_Volume:
			default:
				setAttrNode->AttributeSetTypes.Add(FMaterialAttributeDefinitionMap::GetID(EMaterialProperty::MP_BaseColor));
				break;
		}
		setAttrNode->Inputs.Add(FExpressionInput());
		setAttrNode->Inputs.Last().InputName = FName(*FMaterialAttributeDefinitionMap::GetDisplayNameForMaterial(setAttrNode->AttributeSetTypes.Last(), Material).ToString());
		matInput = &setAttrNode->Inputs.Last();
	} else {
		// for mats that don't use attributes
		switch (Material->MaterialDomain) {
			case MD_Surface:
				if (Material->GetShadingModels().HasShadingModel(EMaterialShadingModel::MSM_Unlit)) {
					matInput = Material->GetExpressionInputForProperty(EMaterialProperty::MP_EmissiveColor);
				} else {
					matInput = Material->GetExpressionInputForProperty(EMaterialProperty::MP_BaseColor);
				}
				break;
			case MD_LightFunction:
			case MD_PostProcess:
			case MD_UI:
				matInput = Material->GetExpressionInputForProperty(EMaterialProperty::MP_EmissiveColor);
				break;
			case MD_DeferredDecal:
				matInput = Material->GetExpressionInputForProperty(EMaterialProperty::MP_WorldPositionOffset);
				break;
			case MD_Volume:
			default:
				matInput = Material->GetExpressionInputForProperty(EMaterialProperty::MP_BaseColor);
				break;
		}
	}

	x -= 16*8*2;
	UMaterialExpressionClamp* clamp = NewObject<UMaterialExpressionClamp>(Material);
	clamp->MaterialExpressionEditorX = x + 16*4;
	clamp->MaterialExpressionEditorY = 0;
	clamp->MinDefault = 0.0f;
	clamp->MaxDefault = 1.0f;
	AddStubExpression(Material, clamp);
	matInput->Expression = clamp;

	FUObjectExportContainer ParamContainer;
	TSharedPtr<FJsonObject> CachedExpressionData = MaterialImporter->GetAssetData()->GetObjectField(TEXT("CachedExpressionData"));
	UMaterialExpressionAdd* lastAdd = NULL;

	/* Legacy (UE 4.2x-era) exports nest the cached entries under "Parameters" ("ParameterInfos",
	 * textures at RuntimeEntries[2]); UE 5.x keeps them flat ("ParameterInfoSet", [3]). */
	const bool bNestedParameters = CachedExpressionData->HasTypedField<EJson::Object>(TEXT("Parameters"));
	if (bNestedParameters) {
		CachedExpressionData = CachedExpressionData->GetObjectField(TEXT("Parameters"));
	}
	const FString ParameterInfosKey = bNestedParameters ? TEXT("ParameterInfos") : TEXT("ParameterInfoSet");
	const FString TextureEntriesKey = bNestedParameters ? TEXT("RuntimeEntries[2]") : TEXT("RuntimeEntries[3]");


	// map known scalar params
	if (
		CachedExpressionData->HasTypedField<EJson::Object>(TEXT("RuntimeEntries"))
		&& CachedExpressionData->GetObjectField(TEXT("RuntimeEntries"))->HasTypedField<EJson::Array>(ParameterInfosKey)
		&& (bNestedParameters || CachedExpressionData->HasTypedField<EJson::Array>(TEXT("ScalarPrimitiveDataIndexValues")))
		&& CachedExpressionData->HasTypedField<EJson::Array>(TEXT("ScalarValues"))
		) {
		const TArray<TSharedPtr<FJsonValue>>
			paramsPtr = CachedExpressionData->GetObjectField(TEXT("RuntimeEntries"))->GetArrayField(ParameterInfosKey),
			paramValuesPtr = CachedExpressionData->GetArrayField(TEXT("ScalarValues"));
		const TArray<TSharedPtr<FJsonValue>> EmptyIndexes;
		const TArray<TSharedPtr<FJsonValue>>& paramValueIndexesPtr = bNestedParameters
			? EmptyIndexes
			: CachedExpressionData->GetArrayField(TEXT("ScalarPrimitiveDataIndexValues"));
		if ((bNestedParameters || paramsPtr.Num() == paramValueIndexesPtr.Num()) && paramsPtr.Num() == paramValuesPtr.Num()) {
			int32 i = 0;
			y -= 0;
			x -= 16*8*3;
			for (const TSharedPtr<FJsonValue> paramVal : paramsPtr) {
				const FJsonObject* paramObj = paramVal->AsObject().Get();
				FString paramName;
				int32 index = -1;
				float value;
				const bool bHasValueIndex = bNestedParameters || paramValueIndexesPtr[i]->TryGetNumber(index);
				const int32 ValueIndex = bNestedParameters ? i : (index == -1 ? i : index);
				if (
					bHasValueIndex
					&& paramObj->TryGetStringField(TEXT("Name"), paramName)
					&& paramValuesPtr.IsValidIndex(ValueIndex)
					&& paramValuesPtr[ValueIndex]->TryGetNumber(value)
					) {
					UMaterialExpressionScalarParameter* param = NewObject<UMaterialExpressionScalarParameter>(Material);
					AddStubExpression(Material, param);
					param->ParameterName = FName(paramName);
					param->MaterialExpressionEditorX = x;
					param->MaterialExpressionEditorY = y;
					param->DefaultValue = value;

					UMaterialExpressionAdd* newAdd = NewObject<UMaterialExpressionAdd>(Material);
					AddStubExpression(Material, newAdd);
					newAdd->MaterialExpressionEditorX = x + 16 * 8 * 2;
					newAdd->MaterialExpressionEditorY = y;
					newAdd->A.Connect(0, param);
					if (lastAdd == NULL) {
						clamp->Input.Connect(0, newAdd);
					} else {
						lastAdd->B.Connect(0, newAdd);
					}
					lastAdd = newAdd;
				}
				y += 16 * 6;
				i++;
			}
		}
	}


	// map known vector params
	if (
		CachedExpressionData->HasTypedField<EJson::Object>(TEXT("RuntimeEntries[1]"))
		&& CachedExpressionData->GetObjectField(TEXT("RuntimeEntries[1]"))->HasTypedField<EJson::Array>(ParameterInfosKey)
		&& (bNestedParameters || CachedExpressionData->HasTypedField<EJson::Array>(TEXT("VectorPrimitiveDataIndexValues")))
		&& CachedExpressionData->HasTypedField<EJson::Array>(TEXT("VectorValues"))
		) {
		const TArray<TSharedPtr<FJsonValue>>
			paramsPtr = CachedExpressionData->GetObjectField(TEXT("RuntimeEntries[1]"))->GetArrayField(ParameterInfosKey),
			paramValuesPtr = CachedExpressionData->GetArrayField(TEXT("VectorValues"));
		const TArray<TSharedPtr<FJsonValue>> EmptyIndexes;
		const TArray<TSharedPtr<FJsonValue>>& paramValueIndexesPtr = bNestedParameters
			? EmptyIndexes
			: CachedExpressionData->GetArrayField(TEXT("VectorPrimitiveDataIndexValues"));
		if ((bNestedParameters || paramsPtr.Num() == paramValueIndexesPtr.Num()) && paramsPtr.Num() == paramValuesPtr.Num()) {
			int32 i = 0;
			x -= 16*8*4;
			y = 0;
			for (const TSharedPtr<FJsonValue> paramVal : paramsPtr) {
				const FJsonObject* paramObj = paramVal->AsObject().Get();
				FString paramName;
				int32 index = -1;
				TSharedPtr<FJsonObject>* value;
				float r, g, b, a;
				const bool bHasValueIndex = bNestedParameters || paramValueIndexesPtr[i]->TryGetNumber(index);
				const int32 ValueIndex = bNestedParameters ? i : (index == -1 ? i : index);
				if (
					bHasValueIndex
					&& paramObj->TryGetStringField(TEXT("Name"), paramName)
					&& paramValuesPtr.IsValidIndex(ValueIndex)
					&& paramValuesPtr[ValueIndex]->TryGetObject(value)
					&& value->Get()->TryGetNumberField(TEXT("R"), r)
					&& value->Get()->TryGetNumberField(TEXT("G"), g)
					&& value->Get()->TryGetNumberField(TEXT("B"), b)
					&& value->Get()->TryGetNumberField(TEXT("A"), a)
					) {
					UMaterialExpressionVectorParameter* param = NewObject<UMaterialExpressionVectorParameter>(Material);
					AddStubExpression(Material, param);
					param->ParameterName = FName(paramName);
					param->MaterialExpressionEditorX = x;
					param->MaterialExpressionEditorY = y;
					param->DefaultValue = FLinearColor(r, g, b, a);

					UMaterialExpressionAdd* newAdd = NewObject<UMaterialExpressionAdd>(Material);
					AddStubExpression(Material, newAdd);
					newAdd->MaterialExpressionEditorX = x + 16 * 8 * 2;
					newAdd->MaterialExpressionEditorY = y;
					newAdd->A.Connect(0, param);
					if (lastAdd == NULL) {
						clamp->Input.Connect(0, newAdd);
					} else {
						lastAdd->B.Connect(0, newAdd);
					}
					lastAdd = newAdd;
				}
				y += 16 * 13;
				i++;
			}
		}
	}


	// map known texture params
	if (
		CachedExpressionData->HasTypedField<EJson::Object>(TextureEntriesKey)
		&& CachedExpressionData->GetObjectField(TextureEntriesKey)->HasTypedField<EJson::Array>(ParameterInfosKey)
		&& CachedExpressionData->HasTypedField<EJson::Array>(TEXT("TextureValues"))
		) {
		const TArray<TSharedPtr<FJsonValue>>
			paramsPtr = CachedExpressionData->GetObjectField(TextureEntriesKey)->GetArrayField(ParameterInfosKey),
			paramValuesPtr = CachedExpressionData->GetArrayField(TEXT("TextureValues"));
		if (paramsPtr.Num() == paramValuesPtr.Num()) {
			int32 i = 0;
			x -= 16*8*4;
			y = 0;
			// import all known referenced textures;
			if (CachedExpressionData->HasTypedField<EJson::Array>(TEXT("ReferencedTextures"))) {
				for (const TSharedPtr<FJsonValue> refedTexture : CachedExpressionData->GetArrayField(TEXT("ReferencedTextures"))) {
				}
			}
			for (const TSharedPtr<FJsonValue> paramVal : paramsPtr) {
				const FJsonObject* paramObj = paramVal->AsObject().Get();
				FString paramName;
				UTexture* tex = nullptr;
				if (paramObj->TryGetStringField(TEXT("Name"), paramName) && paramValuesPtr.IsValidIndex(i)) {
					if (bNestedParameters) {
						/* Legacy cached data references textures by package index */
						const FJsonObject* TextureValueObj = paramValuesPtr[i]->AsObject().Get();
						FString ObjectPath;
						if (TextureValueObj && TextureValueObj->TryGetStringField(TEXT("ObjectPath"), ObjectPath)) {
							ObjectPath.Split(TEXT("."), &ObjectPath, nullptr);
							tex = LoadObjectByPath<UTexture>(ToEditorPackagePath(ObjectPath));
						}
					}
#if ENGINE_MINOR_VERSION <= 5
					else {
						/* UE 5.x textures are soft object paths (two-arg ctor gone in 5.6) */
						TSharedPtr<FJsonObject>* TextureValueObj;
						FString TextureAssetPath, TextureSubPath;
						if (paramValuesPtr[i]->TryGetObject(TextureValueObj)
							&& TextureValueObj->Get()->TryGetStringField(TEXT("AssetPathName"), TextureAssetPath)
							&& TextureValueObj->Get()->TryGetStringField(TEXT("SubPathString"), TextureSubPath)) {
							tex = TSoftObjectPtr<UTexture>(FSoftObjectPath(FName(TextureAssetPath), TextureSubPath)).LoadSynchronous();
						}
					}
#endif
				}
				if (tex) {
					UMaterialExpressionTextureSampleParameter2D* param = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
					AddStubExpression(Material, param);
					param->ParameterName = FName(paramName);
					param->MaterialExpressionEditorX = x;
					param->MaterialExpressionEditorY = y;
					param->Texture = tex;
#if UE5_8_BEYOND
					param->SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(tex);
#else
					param->SamplerType = param->GetSamplerTypeForTexture(tex);
#endif
					UMaterialExpressionAdd* newAdd = NewObject<UMaterialExpressionAdd>(Material);
					AddStubExpression(Material, newAdd);
					newAdd->MaterialExpressionEditorX = x + 16 * 8 * 2;
					newAdd->MaterialExpressionEditorY = y;
					newAdd->A.Connect(0, param);
					if (lastAdd == NULL) {
						clamp->Input.Connect(0, newAdd);
					} else {
						lastAdd->B.Connect(0, newAdd);
					}
					lastAdd = newAdd;
				}
				y += 16*8*2;
				i++;
			}
		}
	}


	// map known switch params
	/* Legacy exports keep switches in editor-only StaticSwitchParameters; no-op for them. */
	if (
		CachedExpressionData->HasTypedField<EJson::Object>(TEXT("RuntimeEntries[7]"))
		&& CachedExpressionData->GetObjectField(TEXT("RuntimeEntries[7]"))->HasTypedField<EJson::Array>(ParameterInfosKey)
		&& CachedExpressionData->HasTypedField<EJson::Array>(TEXT("StaticSwitchValues"))
		)
	{
		const TArray<TSharedPtr<FJsonValue>>
			paramsPtr = CachedExpressionData->GetObjectField(TEXT("RuntimeEntries[7]"))->GetArrayField(ParameterInfosKey),
			paramValuesPtr = CachedExpressionData->GetArrayField(TEXT("StaticSwitchValues"));
		if (paramsPtr.Num() == paramValuesPtr.Num()) {
			int32 i = 0;
			x -= 16*8*4;
			y = 0;
			for (const TSharedPtr<FJsonValue> paramVal : paramsPtr) {
				const FJsonObject* paramObj = paramVal->AsObject().Get();
				FString paramName;
				bool value;
				if (
					paramObj->TryGetStringField(TEXT("Name"), paramName)
					&& paramValuesPtr.IsValidIndex(i)
					&& paramValuesPtr[i]->TryGetBool(value)
					) {

					UMaterialExpressionConstant* constVal = NewObject<UMaterialExpressionConstant>(Material);
					AddStubExpression(Material, constVal);
					constVal->MaterialExpressionEditorX = x;
					constVal->MaterialExpressionEditorY = y;
					constVal->R = 1.0f;

					UMaterialExpressionStaticSwitchParameter* param = NewObject<UMaterialExpressionStaticSwitchParameter>(Material);
					AddStubExpression(Material, param);
					param->A.Connect(0, constVal);
					param->B.Connect(0, constVal);
					param->ParameterName = FName(paramName);
					param->MaterialExpressionEditorX = x;
					param->MaterialExpressionEditorY = y;
					param->DefaultValue = value;

					UMaterialExpressionAdd* newAdd = NewObject<UMaterialExpressionAdd>(Material);
					AddStubExpression(Material, newAdd);
					newAdd->MaterialExpressionEditorX = x + 16 * 8 * 2;
					newAdd->MaterialExpressionEditorY = y;
					newAdd->A.Connect(0, param);
					if (lastAdd == NULL) {
						clamp->Input.Connect(0, newAdd);
					} else {
						lastAdd->B.Connect(0, newAdd);
					}
					lastAdd = newAdd;
					}
				y += 16*9;
				i++;
			}
		}
	}
}
#endif