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

	/* How many of a thing came across incomplete, said once rather than per entry.
	 *
	 * An asset is rarely all or nothing: a blackboard arrives with keys whose type object did not, a
	 * font names typefaces that resolved to nothing. Each of those looks like an ordinary entry in
	 * the editor and only misbehaves when something reads it, so the count is worth stating even
	 * though the import itself worked.
	 *
	 * Summary is filled in as "3 of 12 <summary>", and nothing is reported when nothing is missing.
	 * The count comes back so the caller can log it or decide the asset is not worth keeping. */
	template <typename TContainer, typename TPredicate>
	static int32 ReportIncomplete(const TContainer& Items, TPredicate&& IsMissing, const FString& Summary, const FString& Detail, const EImportIssue Kind = EImportIssue::Data) {
		int32 Missing = 0;
		int32 Total = 0;

		for (const auto& Item : Items) {
			Total++;

			if (IsMissing(Item)) Missing++;
		}

		if (Missing > 0) {
			Report(Kind, FString::Printf(TEXT("%d of %d %s"), Missing, Total, *Summary), Detail);
		}

		return Missing;
	}

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
