/* Copyright Reflection Contributors 2024-2026 */

#include "Commandlets/MacroPinsCommandlet.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"

DEFINE_LOG_CATEGORY_STATIC(LogMacroPins, Display, All);

int32 UMacroPinsCommandlet::Main(const FString& Params) {
	FString Macro;
	if (!FParse::Value(*Params, TEXT("macro="), Macro)) {
		UE_LOG(LogMacroPins, Error, TEXT("usage: -run=MacroPins -macro=DecrementInt"));
		return 1;
	}

	const UBlueprint* Macros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
	if (!Macros) {
		UE_LOG(LogMacroPins, Error, TEXT("no StandardMacros"));
		return 1;
	}

	for (const UEdGraph* Graph : Macros->MacroGraphs) {
		if (!Graph || Graph->GetName() != Macro) {
			continue;
		}

		for (const UEdGraphNode* Node : Graph->Nodes) {
			UE_LOG(LogMacroPins, Display, TEXT("node %s (%s)"), *Node->GetName(), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

			for (const UEdGraphPin* Pin : Node->Pins) {
				FString Links;
				for (const UEdGraphPin* Linked : Pin->LinkedTo) {
					Links += FString::Printf(TEXT(" %s.%s"), *Linked->GetOwningNode()->GetName(), *Linked->PinName.ToString());
				}

				UE_LOG(LogMacroPins, Display, TEXT("    %s %s [%s%s] default=%s ->%s"), Pin->Direction == EGPD_Input ? TEXT("in ") : TEXT("out"), *Pin->PinName.ToString(), *Pin->PinType.PinCategory.ToString(), Pin->PinType.bIsReference ? TEXT(" &") : TEXT(""), *Pin->DefaultValue, *Links);
			}
		}

		return 0;
	}

	UE_LOG(LogMacroPins, Error, TEXT("StandardMacros has no %s"), *Macro);
	return 1;
}
