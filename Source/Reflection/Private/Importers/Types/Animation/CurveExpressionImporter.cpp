/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/CurveExpressionImporter.h"
#include "Importers/Types/Animation/CurveExpressionRig.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/SettingsAccess.h"

#if REFLECTION_CURVE_EXPRESSION
#include "CurveExpressionsDataAsset.h"
#endif

#if REFLECTION_RIGVM
#include "Engine/ControlRigCompatibility.h"
#include "ControlRig.h"

#if ENGINE_UE5
#include "ControlRigBlueprintFactory.h"
#else
#include "ControlRigBlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#endif
#endif

/* A curve expression asset drives one curve from others by arithmetic, and that arithmetic is the
 * whole asset. Cooking keeps only the compiled instructions and drops the text they came from, so
 * the Cloud writes the instructions back out as expressions and they are set as the source here.
 * Saving compiles them again, which is how the asset ends up with the data the runtime reads. */

UObject* ICurveExpressionImporter::CreateAsset(UObject* CreatedAsset) {
#if REFLECTION_RIGVM
	if (ImportsCurveMappingAsRig()) {
		/* Reflected over an earlier run, which is rebuilt in place rather than added beside */
		if (UControlRigBlueprint* Existing = LoadObject<UControlRigBlueprint>(nullptr, *GetPackage()->GetPathName())) {
			return IImporter::CreateAsset(Existing);
		}

#if ENGINE_UE5
		/* The factory is what knows to give a blueprint its rig graph, which nothing else creates */
		UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
		Factory->ParentClass = UControlRig::StaticClass();

		return IImporter::CreateAsset(Factory->FactoryCreateNew(
			UControlRigBlueprint::StaticClass(),
			GetPackage(),
			FName(*GetAssetName()),
			RF_Public | RF_Standalone,
			nullptr,
			GWarn
		));
#else
		/* The factory that would do this lives in the rig's own editor module and is private there,
		 * so the one call it makes is made here instead.
		 *
		 * Made this way rather than outright because a rig blueprint needs the generated class that
		 * comes with it. The graph tells the blueprint every time a node is added, and what the
		 * blueprint does about it reaches for the class's default object, so a blueprint without one
		 * takes the first node badly. */
		UControlRigBlueprint* Made = Cast<UControlRigBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UControlRig::StaticClass(),
			GetPackage(),
			FName(*GetAssetName()),
			BPTYPE_Normal,
			UControlRigBlueprint::StaticClass(),
			UControlRigBlueprintGeneratedClass::StaticClass()
		));

		/* And the controller the graph is edited through, which the blueprint makes on request */
		if (Made != nullptr) {
			Made->InitializeModelIfRequired(false);
		}

		return IImporter::CreateAsset(Made);
#endif
	}
#endif

#if REFLECTION_CURVE_EXPRESSION
	return IImporter::CreateAsset(NewObject<UCurveExpressionsDataAsset>(GetPackage(), GetAssetClass(), StringToName(GetAssetName()), RF_Public | RF_Standalone));
#else
	return IImporter::CreateAsset(CreatedAsset);
#endif
}

/* The mapping as the rig that performs it.
 *
 * Nothing of the asset comes across but its arithmetic, because the arithmetic is the whole of what
 * it does: the rest is the plugin's own bookkeeping for compiling the expressions, and a rig has no
 * use for it. */
bool ICurveExpressionImporter::ImportAsControlRig(const TArray<TSharedPtr<FJsonValue>>& Expressions) {
#if REFLECTION_RIGVM
	UControlRigBlueprint* Blueprint = Create<UControlRigBlueprint>();

	if (Blueprint == nullptr) return false;

	auto _ = Blueprint->MarkPackageDirty();

	FCurveExpressionRigStats Stats;

	if (!FCurveExpressionRig::Build(Blueprint, Expressions, GetAssetName(), Stats)) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Nothing in the mapping could be drawn as a rig"),
			TEXT("A rig is built out of what each curve is worth to the ones it drives, and the Cloud found nothing here it could put a number on. The asset is created with an empty graph.")
		);
	}

	/* The graph is only what the rig is edited as. Compiling is what turns it into something that
	 * runs, and until it has been the rig poses nothing. */
	Blueprint->RecompileVM();

	return OnAssetCreation(Blueprint);
#else
	FImportIssues::Report(
		EImportIssue::Failed,
		TEXT("Control Rig is not in this engine"),
		TEXT("A curve mapping can only be drawn as a rig where there is a rig to draw it in. Bring it in as a data asset instead.")
	);

	return false;
#endif
}

bool ICurveExpressionImporter::Import() {
	/* Asked for once, since either shape is built out of the same answer */
	const TArray<TSharedPtr<FJsonValue>> Expressions = FetchExpressions();

#if REFLECTION_RIGVM
	if (ImportsCurveMappingAsRig()) {
		return ImportAsControlRig(Expressions);
	}
#endif

#if REFLECTION_CURVE_EXPRESSION
	UCurveExpressionsDataAsset* Asset = Create<UCurveExpressionsDataAsset>();
	auto _ = Asset->MarkPackageDirty();

	DeserializeExports(Asset);
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), Asset);

	TArray<FName> Targets;

	const FString Assignments = GetAssignments(Expressions, Targets);

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

TArray<TSharedPtr<FJsonValue>> ICurveExpressionImporter::FetchExpressions() {
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
		return {};
	}

	return *Expressions;
}

FString ICurveExpressionImporter::GetAssignments(const TArray<TSharedPtr<FJsonValue>>& Expressions, TArray<FName>& OutTargets) {
	TArray<FString> Lines;
	Lines.Reserve(Expressions.Num());

	for (const TSharedPtr<FJsonValue>& Value : Expressions) {
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Entry.IsValid()) continue;

		FString Target, Expression;

		if (!Entry->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty()) continue;
		if (!Entry->TryGetStringField(TEXT("expression"), Expression) || Expression.IsEmpty()) continue;

		Lines.Add(FString::Printf(TEXT("%s = %s"), *Target, *Expression));
		OutTargets.Add(FName(*Target));
	}

	return FString::Join(Lines, TEXT("\n"));
}
