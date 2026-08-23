/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/CurveExpressionImporter.h"
#include "Modules/Cloud/Cloud.h"

#if REFLECTION_CURVE_EXPRESSION
#include "CurveExpressionsDataAsset.h"
#endif

/* A curve expression asset drives one curve from others by arithmetic, and that arithmetic is the
 * whole asset. Cooking keeps only the compiled instructions and drops the text they came from, so
 * the Cloud writes the instructions back out as expressions and they are set as the source here.
 * Saving compiles them again, which is how the asset ends up with the data the runtime reads. */

UObject* ICurveExpressionImporter::CreateAsset(UObject* CreatedAsset) {
#if REFLECTION_CURVE_EXPRESSION
	return IImporter::CreateAsset(NewObject<UCurveExpressionsDataAsset>(GetPackage(), GetAssetClass(), StringToName(GetAssetName()), RF_Public | RF_Standalone));
#else
	return IImporter::CreateAsset(CreatedAsset);
#endif
}

bool ICurveExpressionImporter::Import() {
#if REFLECTION_CURVE_EXPRESSION
	UCurveExpressionsDataAsset* Asset = Create<UCurveExpressionsDataAsset>();
	auto _ = Asset->MarkPackageDirty();

	DeserializeExports(Asset);
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), Asset);

	TArray<FName> Targets;

	const FString Assignments = GetAssignments(Targets);

	if (Assignments.IsEmpty()) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("No expressions from the Cloud"),
			TEXT("The asset is created without them, and drives nothing until its expressions are written in by hand.")
		);
	} else {
		Asset->Expressions.AssignmentExpressions = Assignments;

		/* Compiling is private and hangs off the property changing, so it is told which one did */
		if (FProperty* Property = FCurveExpressionList::StaticStruct()->FindPropertyByName(
			GET_MEMBER_NAME_CHECKED(FCurveExpressionList, AssignmentExpressions))) {
			FPropertyChangedEvent PropertyChangedEvent(Property);

			Asset->PostEditChangeProperty(PropertyChangedEvent);
		}

		/* Compiling keeps only what parsed, so anything missing afterward was turned down. The
		 * plugin's parser reads a bracket after a curve name as a call to a function by that name,
		 * so an expression like "a - (b - c)" is one of the ways a line does not survive. */
		TArray<FName> Rejected;

		if (const TSharedPtr<const FExpressionData> Data = Asset->GetCompiledExpressionData()) {
			for (const FName& Target : Targets) {
				if (!Data->ExpressionMap.Contains(Target)) Rejected.Add(Target);
			}
		}

		if (Rejected.Num() > 0) {
			TArray<FString> Names;

			for (const FName& Target : Rejected) Names.Add(Target.ToString());

			FImportIssues::Report(
				EImportIssue::Data,
				FString::Printf(TEXT("%d of %d expressions were turned down"), Rejected.Num(), Targets.Num()),
				FString::Printf(TEXT("The engine's expression parser would not take them, so those curves are driven by nothing: %s"), *FString::Join(Names, TEXT(", ")))
			);

			UE_LOG(LogReflection, Warning, TEXT("\"%s\" had %d of %d expression(s) turned down by the parser"), *GetAssetName(), Rejected.Num(), Targets.Num());
		}
	}

	return OnAssetCreation(Asset);
#else
	FImportIssues::Report(
		EImportIssue::Failed,
		TEXT("CurveExpression is not in this engine"),
		TEXT("The plugin that compiles curve expressions ships with Unreal Engine 5. Nothing here can read the asset without it.")
	);

	return false;
#endif
}

FString ICurveExpressionImporter::GetAssignments(TArray<FName>& OutTargets) {
	/* Asked for by the path the game cooked it under, the way the rest of the Cloud tools do */
	FString FetchPath = GetAssetExport()->HasField(TEXT("Package"))
		? GetAssetExport()->GetStringField(TEXT("Package"))
		: FString();

	if (FetchPath.IsEmpty()) {
		FetchPath = GetPackage()->GetPathName();

		FRRedirects::Reverse(FetchPath);
	}

	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "FetchingExpressions", "Reading expressions from {0}"),
		FText::FromString(FetchPath)
	));

	const TSharedPtr<FJsonObject> Payload = Cloud::Export::GetCurveExpressionsBlocking(FetchPath);

	const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;

	if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("expressions"), Expressions)) {
		return FString();
	}

	TArray<FString> Lines;
	Lines.Reserve(Expressions->Num());

	for (const TSharedPtr<FJsonValue>& Value : *Expressions) {
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Entry.IsValid()) continue;

		FString Target, Expression;

		if (!Entry->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty()) continue;
		if (!Entry->TryGetStringField(TEXT("expression"), Expression) || Expression.IsEmpty()) continue;

		Lines.Add(FString::Printf(TEXT("%s = %s"), *Target, *Expression));
		OutTargets.Add(FName(*Target));
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" read %d curve expression(s)"), *GetAssetName(), Lines.Num());

	return FString::Join(Lines, TEXT("\n"));
}
