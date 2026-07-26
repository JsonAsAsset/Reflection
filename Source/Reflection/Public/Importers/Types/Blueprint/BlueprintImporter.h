/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class IBlueprintImporter final : public IImporter {
protected:
	UBlueprint* Blueprint = nullptr;
	
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	
	virtual bool Import() override;
	
protected:
	/* Handles SimpleConstructionScript, the component layout for Actor blueprints */
	void ConstructScript() const;

	/* Handles WidgetTree, the UI layout for Widget blueprints */
	void ConstructWidgetTree();
};

REGISTER_IMPORTER(IBlueprintImporter, (TArray<FString>{ 
	TEXT("BlueprintGeneratedClass"),
	TEXT("WidgetBlueprintGeneratedClass")
}), TEXT("Blueprints"));