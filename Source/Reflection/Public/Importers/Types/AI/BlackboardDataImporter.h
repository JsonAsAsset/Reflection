/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

class IBlackboardDataImporter final : public IObjectImporter {
public:
	virtual void Validate(UObject* Asset) const override;
};

REGISTER_IMPORTER(IBlackboardDataImporter, TArray<FString>{
	TEXT("BlackboardData")
}, TEXT("AI Assets"));
