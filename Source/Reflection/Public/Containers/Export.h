/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Containers/JsonValueExport.h"
#include "Dom/JsonObject.h"
#include "UObject/Object.h"

inline FString StripObjectOuter(const FString& InObjectName) {
	FString Result = InObjectName;

	const int32 FirstQuote = Result.Find(TEXT("'"));
	const int32 LastQuote = Result.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);

	if (FirstQuote != INDEX_NONE && LastQuote != INDEX_NONE && LastQuote > FirstQuote) {
		Result = Result.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
	}

	return Result;
}

inline FString GetObjectNameFromOuter(const FString& InObjectName) {
	return FPackageName::ObjectPathToObjectName(StripObjectOuter(InObjectName));
}

inline FString GetOuterFromObjectOuter(const TSharedPtr<FJsonValue>& Outer) {
	if (!Outer.IsValid()) {
		return TEXT("");
	}
	
	if (Outer->Type == EJson::Object) {
		return GetObjectNameFromOuter(StripObjectOuter(Outer->AsObject()->GetStringField(TEXT("ObjectName"))));
	}
	
	return Outer->AsString();
}

inline FString ReadPathFromObject(const FUObjectJsonValueExport& PackageIndex);

/* A structure to hold data for a UObject export. */
struct REFLECTION_API FUObjectExport : FUObjectJsonValueExport {
	FUObjectExport(): Object(nullptr), Parent(nullptr), Package(nullptr), Position(-1) { };
	
	TSharedPtr<void> ExtraData;
	FName ExtraDataType;

	/* Object created */
	UObject* Object;

	template<typename T>
	T* Get() const {
		return Object ? Cast<T>(Object) : nullptr;
	}

	/* Parent of this expression */
	UObject* Parent;
	UPackage* Package;
	int Position;

	void SetParent(UObject* NewParent) {
		Parent = NewParent;
	}

	void SetObject(UObject* NewObject) {
		Object = NewObject;
	}

	void SetPosition(const int NewPosition) {
		Position = NewPosition;
	}

	explicit FUObjectExport(const TSharedPtr<FJsonObject>& InJsonObject)
		: FUObjectJsonValueExport(InJsonObject), Object(nullptr), Parent(nullptr), Package(nullptr), Position(-1)
	{
	}

	FUObjectExport(const TSharedPtr<FJsonObject>& InJsonObject, UObject* Object, UObject* Parent, const int Position = -1)
		: FUObjectJsonValueExport(InJsonObject), Object(Object), Parent(Parent), Package(nullptr), Position(Position)
	{
	}

	FUObjectExport(const FName OuterOverride, const TSharedPtr<FJsonObject>& InJsonObject, UObject* Object, UObject* Parent, const int Position = -1)
		: FUObjectJsonValueExport(InJsonObject), Object(Object), Parent(Parent), Package(nullptr), Position(Position),
		  OuterOverride(OuterOverride)
	{
	}

	FUObjectExport(const FName NameOverride, const FName TypeOverride, const FName OuterOverride,
	               const TSharedPtr<FJsonObject>& InJsonObject, UObject* Object, UObject* Parent, const int Position = -1)
		: FUObjectJsonValueExport(InJsonObject),
		  Object(Object),
		  Parent(Parent), Package(nullptr),
		  Position(Position),
		  NameOverride(NameOverride),
		  TypeOverride(TypeOverride),
		  OuterOverride(OuterOverride)
	{
	}

	const TSharedPtr<FJsonObject>& GetProperties() const {
		return JsonObject->GetObjectField(TEXT("Properties"));
	}

	FUObjectJsonValueExport GetPropertiesAsValue() const {
		return FUObjectJsonValueExport(GetProperties());
	}

	FUObjectJsonValueExport AsValueExport() const {
		return FUObjectJsonValueExport(JsonObject);
	}

	FUObjectJsonValueExport GetJsonObject() const {
		return FUObjectJsonValueExport(JsonObject);
	}
	
	UClass* GetClass();
	
	FName NameOverride;

	FName GetName() const {
		if (!NameOverride.IsNone()) {
			return NameOverride;
		}
		
		if (!JsonObject.IsValid() || !JsonObject->HasField(TEXT("Name"))) {
			return NAME_None;
		}

		return FName(*JsonObject->GetStringField(TEXT("Name")));
	}

	FName TypeOverride;

	FName GetType() const {
		if (!TypeOverride.IsNone()) {
			return TypeOverride;
		}
		
		if (!JsonObject.IsValid() || !JsonObject->HasField(TEXT("Type"))) {
			return NAME_None;
		}
		
		return FName(*JsonObject->GetStringField(TEXT("Type")));
	}
	
	FName OuterOverride;

	FName GetOuter() const {
		if (!OuterOverride.IsNone()) {
			return OuterOverride;
		}

		if (!JsonObject.IsValid() || !JsonObject->HasField(TEXT("Outer"))) {
			return NAME_None;
		}
		
		return FName(*GetOuterFromObjectOuter(JsonObject->TryGetField(TEXT("Outer"))));
	}

	FName GetOuterTree() const {
		bool bIsPath = JsonObject->HasField(TEXT("ObjectName"));
		
		if (!JsonObject.IsValid()) {
			return NAME_None;
		}

		if (!bIsPath) {
			if (!JsonObject->HasField(TEXT("Outer"))) {
				return NAME_None;
			}
		}

		const TSharedPtr<FJsonObject> OuterObject = bIsPath ? JsonObject : JsonObject->GetObjectField(TEXT("Outer"));
		if (!OuterObject.IsValid() || !OuterObject->HasField(TEXT("ObjectName"))) {
			return NAME_None;
		}
		
		return FName(*StripObjectOuter(OuterObject->GetStringField(TEXT("ObjectName"))));
	}

	TArray<FName> GetOuterTreeSegments(const bool bRemoveLast = false) const {
		TArray<FName> Result;

		FString OuterTree = GetOuterTree().ToString();
		if (OuterTree.IsEmpty()) {
			return Result;
		}

		/* Normalize delimiters: treat '.' like ':' */
		OuterTree.ReplaceInline(TEXT("."), TEXT(":"));

		TArray<FString> Parts;
		OuterTree.ParseIntoArray(Parts, TEXT(":"), true);

		for (const FString& Part : Parts) {
			Result.Add(FName(*Part));
		}

		if (!JsonObject->HasField(TEXT("ObjectName"))) {
			Result.Add(GetName());
		}

		if (bRemoveLast && Result.Num() > 0) {
	#if UE5_6_BEYOND
			/* Pop's bool parameter became EAllowShrinking in 5.4, and the bool overload was removed in 5.8 */
			Result.Pop(EAllowShrinking::No);
	#else
			Result.Pop(false);
	#endif
		}

		return Result;
	}

	FName GetOuterPath() const {
		if (!JsonObject->HasField(TEXT("Outer"))) {
			return NAME_None;
		}

		FString ObjectPath = JsonObject->GetObjectField(TEXT("Outer"))->GetStringField(TEXT("ObjectPath"));
		if (ObjectPath.Contains(".")) {
			ObjectPath.Split(".", &ObjectPath, nullptr);
		}

		return FName(*ObjectPath);
	}

	bool IsJsonAndObjectValid() const {
		return JsonObject != nullptr && Object != nullptr;
	}

	bool IsJsonValid() const {
		return JsonObject != nullptr && !bEmpty;
	}

	bool IsJsonInvalid() const {
		return !IsJsonValid();
	}

	/* Handed back by every container lookup that finds nothing, so a caller can chain off the
	 * result without checking for null first.
	 *
	 * There is one of these for the whole process, and callers that write to what a lookup gave
	 * them without testing it first end up writing here. Object is cleared on the way out so a
	 * stale one can never be read back by the next miss, which would hand out a pointer to an
	 * object from an unrelated import that may no longer exist. */
	static FUObjectExport* EmptyExport() {
		static FUObjectExport* Empty = []() {
			FUObjectExport* Export = new FUObjectExport();
			Export->bEmpty = true;

			return Export;
		}();

		Empty->Object = nullptr;
		Empty->Parent = nullptr;

		return Empty;
	}

	bool IsEmptyExport() const {
		return bEmpty;
	}

	explicit operator bool() const {
		return IsJsonValid();
	}

	bool HasProperty(const FString& FieldName) const {
		return GetPropertiesAsValue().Has(FieldName);
	}
	
protected:
	UClass* Class = nullptr;

	/* Only ever true for the one EmptyExport */
	bool bEmpty = false;
};