/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Types.h"

#include "Settings/ReflectionSettings.h"
#include "Engine/EngineUtilities.h"

/* Define supported template asset class here */
TMap<FString, TArray<FString>> ImportTypes::Templated = {
	{
		TEXT("Curve Assets"),
		{
			TEXT("CurveFloat"),
			TEXT("CurveLinearColor"),
		}
	},
	{
		TEXT("Data Assets"),
		{
			TEXT("SlateBrushAsset"),
			TEXT("SlateWidgetStyleAsset"),
			TEXT("LandscapeLayerInfoObject"),
			TEXT("HLODProxy"),
			TEXT("AnimBoneCompressionSettings"),
			TEXT("AnimCurveCompressionSettings"),
		}
	},
	{
		TEXT("Niagara Assets"),
		{
			TEXT("NiagaraEffectType"),
			TEXT("NiagaraDataChannelAsset"),
		}
	},
	{
		TEXT("Landscape Assets"),
		{
			TEXT("LandscapeGrassType"),
			TEXT("FoliageType_InstancedStaticMesh"),
			TEXT("FoliageType_Actor"),
		}
	},
	{
		TEXT("Material Assets"),
		{
			TEXT("MaterialParameterCollection"),
			TEXT("SubsurfaceProfile"),
		}
	},
	{
		TEXT("Skeletal Assets"),
		{
			TEXT("SkeletalMeshLODSettings"),
		}
	},
	{
		TEXT("Physics Assets"),
		{
			TEXT("PhysicalMaterial"),
		}
	},
{
		TEXT("UI Assets"),
		{
			TEXT("Font"),
		}
	},
	{
		TEXT("Sound Assets"),
		{
			TEXT("ReverbEffect"),
			TEXT("SoundAttenuation"),
			TEXT("SoundConcurrency"),
			TEXT("SoundClass"),
			TEXT("SoundMix"),
			TEXT("SoundSourceBus"),
			TEXT("DialogueVoice"),
			TEXT("DialogueWave"),
			TEXT("SoundModulationParameterVolume"),
			TEXT("SoundControlBusMix"),
			TEXT("SoundModulationDestination"),
			TEXT("SoundModulationPatch"),
			TEXT("SubmixEffectDynamicsProcessorPreset"),
			TEXT("SubmixEffectStereoToQuadPreset"),
			TEXT("SoundModulationGeneratorEnvelopeFollower"),
			TEXT("SubmixEffectDynamicReverbPreset"),
			TEXT("SoundSubmix"),
			TEXT("SoundControlBus"),
			TEXT("AudioBus"),
		}
	},
	{
		TEXT("Texture Assets"),
		{
			TEXT("TextureRenderTarget2D"),
			TEXT("RuntimeVirtualTexture"),
			TEXT("PaperSprite"),
		}
	},
	{
		TEXT("Sequencer Assets"),
		{
			TEXT("CameraAnim"),
			TEXT("ForceFeedbackEffect")
		}
	}
};

bool ImportTypes::Cloud::Allowed(const FString& Type) {
	if (Blacklisted.Contains(Type)) {
		return false;
	}

	return true;
}

bool ImportTypes::Allowed(const FString& ImporterType) {
	if (Experimental.Contains(ImporterType)) {
		const UReflectionSettings* Settings = GetSettings();

		if (!Settings->EnableExperiments) return false;
	}

	return true;
}
