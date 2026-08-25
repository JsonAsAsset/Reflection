/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/AI/BlackboardDataImporter.h"

#include "BehaviorTree/BlackboardData.h"
#include "Engine/EngineUtilities.h"

/* A key whose type never came through is a key nothing can read or write, and it looks like an
 * ordinary one in the editor until something asks it for a value */
void IBlackboardDataImporter::Validate(UObject* Asset) const {
	const UBlackboardData* Blackboard = Cast<UBlackboardData>(Asset);
	if (Blackboard == nullptr) return;

	const int32 Untyped = FImportIssues::ReportIncomplete(
		Blackboard->Keys,
		[](const FBlackboardEntry& Key) { return Key.KeyType == nullptr; },
		TEXT("blackboard keys have no type"),
		FString::Printf(
			TEXT("'%s' carries keys whose type object didn't come across, so nothing can read or write them. A key type this engine hasn't got is the usual reason."),
			*GetAssetName()
		)
	);

	UE_LOG(LogReflection, Display, TEXT("\"%s\" carries %d key(s)%s%s"),
		*GetAssetName(),
		Blackboard->Keys.Num(),
		Blackboard->Parent != nullptr ? TEXT(", standing on a parent") : TEXT(""),
		Untyped > 0 ? *FString::Printf(TEXT(", %d of them untyped"), Untyped) : TEXT(""));
}
