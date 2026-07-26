/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Cascade/ParticleSystemImporter.h"

/* Particle System Includes */
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleSystem.h"

UObject* IParticleSystemImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UParticleSystem>(GetPackage(), UParticleSystem::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IParticleSystemImporter::Import() {
	const auto ParticleSystem = Create<UParticleSystem>();

	/* Ensure any default emitters are cleared */
	WipeEmitters();

	/* Emitters ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	GetObjectSerializer()->bUseExperimentalSpawning = true;
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), ParticleSystem);

	/* Handle edit changes, and add it to the content browser */
	return OnAssetCreation(ParticleSystem);
}

void IParticleSystemImporter::WipeEmitters() const {
	const auto ParticleSystem = GetTypedAsset<UParticleSystem>();
	
	if (ParticleSystem->Emitters.Num() > 0) {
		/* Destroy any existing emitters */
		for (UParticleEmitter* Emitter : ParticleSystem->Emitters) {
			if (Emitter) {
				Emitter->ConditionalBeginDestroy();
			}
		}

		/* Clear the emitters array */
		ParticleSystem->Emitters.Empty();
	}
}
