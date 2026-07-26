/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IMaterialInstanceConstantImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
	
protected:
	static void ReadStaticParameters(const FUObjectJsonValueExport& StaticParameters, TArray<FUObjectJsonValueExport>& StaticSwitchParameters, TArray<FUObjectJsonValueExport>& StaticComponentMaskParameters);
	static void ConvertParameterNamesToInfos(TArray<FUObjectJsonValueExport>& Input);
};

REGISTER_IMPORTER(IMaterialInstanceConstantImporter, {
	"MaterialInstanceConstant"
}, "Material Assets");