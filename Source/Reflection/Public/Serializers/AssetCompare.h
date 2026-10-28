/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UPropertySerializer;

/* What became of one property the export names. */
enum class EAssetCompareVerdict : uint8 {
	/* Both sides say the same thing */
	Same,

	/* Both sides say something and they disagree, which is the only verdict that means a fault */
	Differs,

	/* The export names a property the class in this engine has not got */
	Absent,

	/* A property an import never reads, so the two sides were never meant to agree */
	Skipped,
};

/* One property, and what the two sides said about it. */
struct FAssetCompareEntry {
	FString Property;

	EAssetCompareVerdict Verdict = EAssetCompareVerdict::Same;

	/* Both sides written out as condensed json, so what disagreed can be read rather than guessed */
	FString Theirs;
	FString Ours;
};

struct FAssetCompareReport {
	FString Named;
	FString Type;

	int32 Same = 0;
	int32 Differs = 0;
	int32 Absent = 0;
	int32 Skipped = 0;

	TArray<FAssetCompareEntry> Entries;

	/* Whether everything an import was meant to read came out the same */
	bool Agrees() const { return Differs == 0; }
};

/* Compares what the Cloud serves for an asset against the asset this project has.
 *
 * Nothing is built and nothing is deserialized. The export's values are read as the json they
 * already are, and ours are read off the live object. Deserializing is what makes objects out of
 * an export: it spawns the exports under it and loads what it references, so asking the
 * deserializer to describe a value would import the asset over again to answer a question about
 * it.
 *
 * Which means the two sides arrive in different shapes, and the comparison is the one place that
 * knows how each side spells the same value. Where neither spelling can be reconciled the
 * property is reported as differing rather than quietly called equal. */
REFLECTION_API FAssetCompareReport CompareExportAgainstAsset(const TSharedPtr<FJsonObject>& Export, UObject* Ours, const UPropertySerializer* Serializer = nullptr);

/* The report as lines, one per property, for a log or a notification. Only what disagreed unless
 * bEverything is set. */
REFLECTION_API TArray<FString> DescribeAssetCompare(const FAssetCompareReport& Report, bool bEverything = false);
