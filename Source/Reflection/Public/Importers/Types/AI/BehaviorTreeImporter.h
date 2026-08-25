/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

class IBehaviorTreeImporter final : public IObjectImporter {
public:
	virtual void Repair(UObject* Asset) const override;
	virtual void Validate(UObject* Asset) const override;

private:
	/* Takes out the child slots that name nothing, all the way down, and says how many */
	static int32 PruneEmptyChildren(class UBTCompositeNode* Composite);
};

REGISTER_IMPORTER(IBehaviorTreeImporter, TArray<FString>{
	TEXT("BehaviorTree")
}, TEXT("AI Assets"));
