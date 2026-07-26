/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IAnimationBaseImporter : public IImporter {
public:
	virtual bool Import() override;
};

REGISTER_IMPORTER(IAnimationBaseImporter, (TArray<FString>{ 
	TEXT("AnimSequence"),
	TEXT("AnimMontage")
}), TEXT("Animation Assets"));