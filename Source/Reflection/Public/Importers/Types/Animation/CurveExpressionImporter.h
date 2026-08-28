/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class ICurveExpressionImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	virtual bool Import() override;

private:
	/* What the Cloud says the asset drives, one entry per curve it writes */
	TArray<TSharedPtr<FJsonValue>> FetchExpressions();

	/* The assignments the asset was authored with, as the plugin's own "target = expression" lines */
	static FString GetAssignments(const TArray<TSharedPtr<FJsonValue>>& Expressions, TArray<FName>& OutTargets);

	/* The same arithmetic, drawn as a rig that performs it rather than an asset that states it */
	bool ImportAsControlRig(const TArray<TSharedPtr<FJsonValue>>& Expressions);
};

REGISTER_IMPORTER(ICurveExpressionImporter, {
	TEXT("CurveExpressionsDataAsset")
}, "Data Assets");
