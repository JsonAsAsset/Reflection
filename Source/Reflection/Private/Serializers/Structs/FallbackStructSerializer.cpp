/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/Structs/FallbackStructSerializer.h"

#include "Serializers/PropertySerializer.h"

void FFallbackStructSerializer::Deserialize(UScriptStruct* Struct, void* StructValue, const TSharedPtr<FJsonObject> JsonValue, UObject* OptionalOuter) {
	for (FProperty* Property = Struct->PropertyLink; Property; Property = Property->PropertyLinkNext) {
		FString PropertyName = Property->GetName();
		FString ValueName = PropertyName;

#if ENGINE_UE5
		if (Struct->IsChildOf(FPostProcessSettings::StaticStruct())) {
			TArray<FString> DeprecatedInUE5 = {
				"bOverride_BloomConvolutionPreFilter",
				"bOverride_AutoExposureCalibrationConstant",
				"bOverride_LocalExposureContrastScale",
				"bOverride_GrainIntensity",
				"bOverride_GrainJitter",
				"bOverride_AmbientOcclusionDistance",
				"bOverride_LPVIntensity",
				"bOverride_LPVDirectionalOcclusionIntensity",
				"bOverride_LPVDirectionalOcclusionRadius",
				"bOverride_LPVDiffuseOcclusionExponent",
				"bOverride_LPVSpecularOcclusionExponent",
				"bOverride_LPVDiffuseOcclusionIntensity",
				"bOverride_LPVSpecularOcclusionIntensity",
				"bOverride_LPVSize",
				"bOverride_LPVSecondaryOcclusionIntensity",
				"bOverride_LPVSecondaryBounceIntensity",
				"bOverride_LPVGeometryVolumeBias",
				"bOverride_LPVVplInjectionBias",
				"bOverride_LPVEmissiveInjectionIntensity",
				"bOverride_LPVFadeRange",
				"bOverride_LPVDirectionalOcclusionFadeRange",
				"bOverride_ScreenPercentage",
				"bOverride_ReflectionsType",
				"DepthOfFieldMethod",
				"BloomConvolutionPreFilter",
				"ReflectionsType",
				"AutoExposureCalibrationConstant",
				"LocalExposureContrastScale",
				"GrainJitter",
				"GrainIntensity",
				"AmbientOcclusionDistance",
				"LPVIntensity",
				"LPVVplInjectionBias",
				"LPVSize",
				"LPVSecondaryOcclusionIntensity",
				"LPVSecondaryBounceIntensity",
				"LPVGeometryVolumeBias",
				"LPVEmissiveInjectionIntensity",
				"LPVDirectionalOcclusionIntensity",
				"LPVDirectionalOcclusionRadius",
				"LPVDiffuseOcclusionExponent",
				"LPVSpecularOcclusionExponent",
				"LPVDiffuseOcclusionIntensity",
				"LPVSpecularOcclusionIntensity",
				"LPVFadeRange",
				"LPVDirectionalOcclusionFadeRange",
				"ScreenPercentage",
				"Blendables"
			};

			if (DeprecatedInUE5.Contains(PropertyName)) {
				PropertyName = PropertyName + "_DEPRECATED";
			}
		}
#endif
		
		if (PropertySerializer->ShouldDeserializeProperty(Property)) {
			void* PropertyValue = Property->ContainerPtrToValuePtr<void>(StructValue);

			const bool bHasHandledProperty = PassthroughPropertyHandler(Property, PropertyName, PropertyValue, JsonValue, PropertySerializer);

			if (!bHasHandledProperty && JsonValue->HasField(ValueName)) {
				const TSharedPtr<FJsonValue> ValueObject = JsonValue->Values.FindChecked(StringToJsonKey(ValueName));

				if (Property->ArrayDim == 1 || ValueObject->Type == EJson::Array) {
					PropertySerializer->DeserializePropertyValue(Property, ValueObject.ToSharedRef(), PropertyValue, OptionalOuter);
				}
			}
		}

		if (Struct->IsChildOf(FRichCurveKey::StaticStruct())) {
			FRichCurveKey* RichCurveKey = static_cast<FRichCurveKey*>(StructValue);
			
			if (RichCurveKey->TangentWeightMode == 183) {
				RichCurveKey->TangentWeightMode = RCTWM_WeightedNone;
			}
		}
	}
}