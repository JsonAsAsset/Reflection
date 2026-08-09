/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

enum class EImportIssue : uint8 {
	/* The asset didn't import */
	Failed,

	/* A reference that is in neither the project nor the Cloud */
	MissingAsset,

	/* A node or class this engine build doesn't have */
	MissingClass,

	/* Project configuration the import needed */
	Setting,

	/* The export said something the engine wouldn't take */
	Data
};

REFLECTION_API FText GetImportIssueText(EImportIssue Kind);
REFLECTION_API FLinearColor GetImportIssueColor(EImportIssue Kind);

struct FImportIssue {
	EImportIssue Kind = EImportIssue::Failed;

	FString Summary;
	FString Detail;

	/* How many times the same thing was reported for the asset */
	int32 Count = 1;
};

/* One asset and everything that went wrong while it imported */
struct FImportIssueAsset {
	FString Name;
	FString Path;
	FString Type;

	TArray<TSharedPtr<FImportIssue>> Issues;
};

/* What an import run has to say for itself, kept per asset and shown once at the end */
class REFLECTION_API FImportIssues {
public:
	/* Drops the last run's issues */
	static void Begin();

	/* Shows the report, only if the run collected anything */
	static void Finish();

	/* Attributes issues to an asset until the matching Pop */
	static void Push(const FString& Name, const FString& Path, const FString& Type);
	static void Pop();

	static void Report(EImportIssue Kind, const FString& Summary, const FString& Detail = FString());
	static void ReportFor(const FString& Name, const FString& Path, const FString& Type, EImportIssue Kind, const FString& Summary, const FString& Detail = FString());

	/* A reference that resolved to nothing, ignored when it names part of the asset being imported */
	static void ReportUnresolvedReference(const FString& Type, const FString& Name, const FString& Path);

	static const TArray<TSharedPtr<FImportIssueAsset>>& GetAssets();

	/* Issues across every asset, counting repeats */
	static int32 NumIssues();

	static void Clear();
};

/* Attributes everything reported while it is alive to one asset */
struct REFLECTION_API FImportIssueScope {
	FImportIssueScope(const FString& Name, const FString& Path, const FString& Type) {
		FImportIssues::Push(Name, Path, Type);
	}

	~FImportIssueScope() {
		FImportIssues::Pop();
	}
};
