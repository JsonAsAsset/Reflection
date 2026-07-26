/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"
#include "Particles/ParticleEmitter.h"

class IParticleSystemImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

private:
	void WipeEmitters() const;
};

REGISTER_IMPORTER(IParticleSystemImporter, {
	"ParticleSystem"
}, "Cascade Assets");