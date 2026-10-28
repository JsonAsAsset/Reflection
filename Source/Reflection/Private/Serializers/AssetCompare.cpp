/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/AssetCompare.h"

#include "Engine/Compatibility.h"
#include "Serializers/PropertySerializer.h"

#include "JsonObjectConverter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace {
	/* How close two numbers have to be to count as the same.
	 *
	 * A float written out as text and read back is not the number it started as, and the export
	 * came through json either way. Anything coarser than this hides a real difference. */
	constexpr double SameNumberTolerance = 1.0e-4;

	FString Rendered(const TSharedPtr<FJsonValue>& Value) {
		if (!Value.IsValid() || Value->IsNull()) return TEXT("none");

		/* The scalars are worth reading as themselves rather than as quoted json */
		switch (Value->Type) {
			case EJson::String: return Value->AsString();
			case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
			case EJson::Number: return FString::SanitizeFloat(Value->AsNumber());
			default: break;
		}

		FString Written;

		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Written);

		if (Value->Type == EJson::Object) {
			FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer);
		} else if (Value->Type == EJson::Array) {
			FJsonSerializer::Serialize(Value->AsArray(), Writer);
		}

		return Written;
	}

	/* The last name in a path, which is the one both sides agree on.
	 *
	 * A reference is written as a path by one side and as a name and a path by the other, and the
	 * paths themselves disagree: the export says where the asset lived in the build it came from
	 * and ours says where it landed here. The leaf is the part that survives that. */
	FString LeafOf(const FString& Path) {
		FString Held = Path;

		/* Written as Class'/Path/To/Thing.Thing' by one side and bare by the other */
		if (int32 Quote; Held.FindChar(TEXT('\''), Quote)) {
			Held = Held.RightChop(Quote + 1);
			Held.RemoveFromEnd(TEXT("'"));
		}

		int32 At = INDEX_NONE;

		if (Held.FindLastChar(TEXT('.'), At)) Held = Held.RightChop(At + 1);
		if (Held.FindLastChar(TEXT('/'), At)) Held = Held.RightChop(At + 1);

		/* The generated class of a blueprint names itself with a _C the asset does not carry */
		Held.RemoveFromEnd(TEXT("_C"));

		return Held;
	}

	/* What a reference says it points at, out of whichever shape it came in.
	 *
	 * Empty when the value is not a reference at all, which is how the caller tells the two
	 * cases apart. */
	FString ReferenceName(const TSharedPtr<FJsonValue>& Value) {
		if (!Value.IsValid()) return FString();

		if (Value->Type == EJson::String) {
			const FString Held = Value->AsString();

			/* A path, or something that merely contains a slash, is as far as this can tell */
			return Held.Contains(TEXT("/")) ? LeafOf(Held) : FString();
		}

		if (Value->Type != EJson::Object) return FString();

		const TSharedPtr<FJsonObject> Object = Value->AsObject();

		/* ObjectName and ObjectPath for a hard reference, AssetPathName for a soft one */
		for (const TCHAR* Field : { TEXT("ObjectName"), TEXT("AssetPathName"), TEXT("ObjectPath") }) {
			if (FString Held; Object->TryGetStringField(Field, Held) && !Held.IsEmpty()) {
				return LeafOf(Held);
			}
		}

		return FString();
	}

	/* An enum value without the type in front of it, since one side writes EFoo::Bar and the
	 * other writes Bar */
	FString WithoutEnumPrefix(const FString& Held) {
		int32 At = INDEX_NONE;

		return Held.FindLastChar(TEXT(':'), At) ? Held.RightChop(At + 1) : Held;
	}

	bool Agree(const TSharedPtr<FJsonValue>& Theirs, const TSharedPtr<FJsonValue>& Ours);

	bool AgreeAsObjects(const TSharedPtr<FJsonObject>& Theirs, const TSharedPtr<FJsonObject>& Ours) {
		/* Only what the export names. Ours carries every property the struct has, including the
		 * ones the export left at their defaults and never mentioned. */
		for (const auto& Pair : Theirs->Values) {
			const FString Key = JsonKeyToString(Pair.Key);

			/* Asked for by name rather than looked up in the map, whose key type is not the same
				 * on every engine */
			const TSharedPtr<FJsonValue> Mine = Ours->TryGetField(Key);

			if (!Mine.IsValid()) return false;
			if (!Agree(Pair.Value, Mine)) return false;
		}

		return true;
	}

	bool Agree(const TSharedPtr<FJsonValue>& Theirs, const TSharedPtr<FJsonValue>& Ours) {
		const bool bTheirsNothing = !Theirs.IsValid() || Theirs->IsNull();
		const bool bOursNothing = !Ours.IsValid() || Ours->IsNull();

		if (bTheirsNothing || bOursNothing) return bTheirsNothing == bOursNothing;

		/* A reference, whichever shape each side wrote it in */
		const FString TheirReference = ReferenceName(Theirs);
		const FString OurReference = ReferenceName(Ours);

		if (!TheirReference.IsEmpty() || !OurReference.IsEmpty()) {
			return TheirReference.Equals(OurReference, ESearchCase::IgnoreCase);
		}

		if (Theirs->Type == EJson::Array || Ours->Type == EJson::Array) {
			if (Theirs->Type != Ours->Type) return false;

			const TArray<TSharedPtr<FJsonValue>>& Left = Theirs->AsArray();
			const TArray<TSharedPtr<FJsonValue>>& Right = Ours->AsArray();

			if (Left.Num() != Right.Num()) return false;

			for (int32 Index = 0; Index < Left.Num(); ++Index) {
				if (!Agree(Left[Index], Right[Index])) return false;
			}

			return true;
		}

		if (Theirs->Type == EJson::Object && Ours->Type == EJson::Object) {
			return AgreeAsObjects(Theirs->AsObject(), Ours->AsObject());
		}

		/* A number either side may have written as text */
		if (Theirs->Type == EJson::Number || Ours->Type == EJson::Number) {
			double Left = 0.0;
			double Right = 0.0;

			if (!Theirs->TryGetNumber(Left) || !Ours->TryGetNumber(Right)) return false;

			return FMath::IsNearlyEqual(Left, Right, SameNumberTolerance);
		}

		if (Theirs->Type == EJson::Boolean && Ours->Type == EJson::Boolean) {
			return Theirs->AsBool() == Ours->AsBool();
		}

		if (Theirs->Type == EJson::String && Ours->Type == EJson::String) {
			const FString Left = Theirs->AsString();
			const FString Right = Ours->AsString();

			if (Left.Equals(Right, ESearchCase::IgnoreCase)) return true;

			/* An enum, named with its type by one side and without it by the other */
			return WithoutEnumPrefix(Left).Equals(WithoutEnumPrefix(Right), ESearchCase::IgnoreCase);
		}

		return false;
	}

	/* Every property the export names, which is the Properties object plus whatever the export
	 * carries alongside it.
	 *
	 * Read into a map rather than moved into Properties, because an import does move them and
	 * this is only meant to describe what is already there. */
	TMap<FString, TSharedPtr<FJsonValue>> NamedByExport(const TSharedPtr<FJsonObject>& Export) {
		TMap<FString, TSharedPtr<FJsonValue>> Named;

		if (const TSharedPtr<FJsonObject>* Properties = nullptr; Export->TryGetObjectField(TEXT("Properties"), Properties)) {
			for (const auto& Pair : (*Properties)->Values) {
				Named.Add(JsonKeyToString(Pair.Key), Pair.Value);
			}
		}

		for (const auto& Pair : Export->Values) {
			const FString Key = JsonKeyToString(Pair.Key);

			/* What the export says about itself rather than about the object */
			if (Key == TEXT("Type") || Key == TEXT("Name") || Key == TEXT("Class")
				|| Key == TEXT("Flags") || Key == TEXT("Properties") || Key == TEXT("Outer")) {
				continue;
			}

			if (!Named.Contains(Key)) Named.Add(Key, Pair.Value);
		}

		return Named;
	}

	/* The property a key names, and which element of it, since a static array is written out one
	 * key per element as Name[Index] */
	FProperty* PropertyFor(const UStruct* Class, const FString& Key, int32& OutIndex) {
		OutIndex = 0;

		FString Named = Key;

		if (int32 Open; Key.FindChar(TEXT('['), Open) && Key.EndsWith(TEXT("]"))) {
			Named = Key.Left(Open);

			const FString Held = Key.Mid(Open + 1, Key.Len() - Open - 2);

			OutIndex = FCString::Atoi(*Held);
		}

		FProperty* Property = FindFProperty<FProperty>(Class, *Named);

		/* An index this array has no room for is something the export may well carry: a newer
		 * engine can declare the same array longer than it is here. */
		if (Property != nullptr && (OutIndex < 0 || OutIndex >= Property->ArrayDim)) return nullptr;

		return Property;
	}
}

FAssetCompareReport CompareExportAgainstAsset(const TSharedPtr<FJsonObject>& Export, UObject* Ours, const UPropertySerializer* Serializer) {
	FAssetCompareReport Report;

	if (!Export.IsValid() || Ours == nullptr) return Report;

	Export->TryGetStringField(TEXT("Name"), Report.Named);
	Export->TryGetStringField(TEXT("Type"), Report.Type);

	const UClass* Class = Ours->GetClass();

	TMap<FString, TSharedPtr<FJsonValue>> Named = NamedByExport(Export);

	/* Read in a settled order, so two runs of this describe the same asset the same way */
	TArray<FString> Keys;
	Named.GetKeys(Keys);
	Keys.Sort([](const FString& Left, const FString& Right) { return Left.Compare(Right) < 0; });

	for (const FString& Key : Keys) {
		const TSharedPtr<FJsonValue>& Theirs = Named[Key];

		FAssetCompareEntry Entry;
		Entry.Property = Key;
		Entry.Theirs = Rendered(Theirs);

		int32 Index = 0;
		FProperty* Property = PropertyFor(Class, Key, Index);

		if (Property == nullptr) {
			Entry.Verdict = EAssetCompareVerdict::Absent;
			Report.Absent++;
			Report.Entries.Add(Entry);

			continue;
		}

		/* A property an import would pass over. Saying it differs would report the serializer's
		 * own choices as faults in the asset. */
		if (Serializer != nullptr && !Serializer->ShouldDeserializeProperty(Property)) {
			Entry.Verdict = EAssetCompareVerdict::Skipped;
			Report.Skipped++;
			Report.Entries.Add(Entry);

			continue;
		}

		const void* Value = Property->ContainerPtrToValuePtr<void>(Ours, Index);

		/* Ours as the json it would be written as, which is a read of the live value and builds
		 * nothing. The flags are passed because the oldest engines here declare none. */
		const TSharedPtr<FJsonValue> Mine = FJsonObjectConverter::UPropertyToJsonValue(Property, Value, 0, 0);

		Entry.Ours = Rendered(Mine);

		if (Agree(Theirs, Mine)) {
			Entry.Verdict = EAssetCompareVerdict::Same;
			Report.Same++;
		} else {
			Entry.Verdict = EAssetCompareVerdict::Differs;
			Report.Differs++;
		}

		Report.Entries.Add(Entry);
	}

	return Report;
}

TArray<FString> DescribeAssetCompare(const FAssetCompareReport& Report, const bool bEverything) {
	TArray<FString> Lines;

	Lines.Add(FString::Printf(TEXT("%s (%s): %d same, %d differ, %d not on this class, %d not read"),
		*Report.Named, *Report.Type, Report.Same, Report.Differs, Report.Absent, Report.Skipped));

	for (const FAssetCompareEntry& Entry : Report.Entries) {
		switch (Entry.Verdict) {
			case EAssetCompareVerdict::Differs:
				Lines.Add(FString::Printf(TEXT("  differs  %s: theirs %s, ours %s"), *Entry.Property, *Entry.Theirs, *Entry.Ours));
				break;

			case EAssetCompareVerdict::Absent:
				Lines.Add(FString::Printf(TEXT("  absent   %s: theirs %s, and this class has no such property"), *Entry.Property, *Entry.Theirs));
				break;

			case EAssetCompareVerdict::Skipped:
				if (bEverything) Lines.Add(FString::Printf(TEXT("  not read %s: theirs %s"), *Entry.Property, *Entry.Theirs));
				break;

			default:
				if (bEverything) Lines.Add(FString::Printf(TEXT("  same     %s: %s"), *Entry.Property, *Entry.Ours));
				break;
		}
	}

	return Lines;
}
