/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class ICurveExpressionImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	virtual bool Import() override;

private:
	/* The assignments the asset was authored with, as the plugin's own "target = expression" lines */
	FString GetAssignments(TArray<FName>& OutTargets);
};

REGISTER_IMPORTER(ICurveExpressionImporter, {
	TEXT("CurveExpressionsDataAsset")
}, "Data Assets");
