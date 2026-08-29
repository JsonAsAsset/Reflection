/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Export.h"
#include "Containers/JsonValueExport.h"
#include "Dom/JsonObject.h"
#include "UObject/Object.h"

struct FUObjectExportContainer {
	/* Array of Expression Exports */
	TArray<FUObjectExport*> Exports;
	TArray<TSharedPtr<FJsonValue>> JsonObjects = { };
	
public:
	FUObjectExportContainer() { }
	FUObjectExportContainer(const TArray<TSharedPtr<FJsonValue>>& Array) {
		Fill(Array);
	}

	void Fill(TArray<TSharedPtr<FJsonValue>> Array) {
		JsonObjects = Array;
		
		int Index = -1;
	
		for (const auto& Value : Array) {
			Index++;

			TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object->HasField(TEXT("Name")) || !Object->HasField(TEXT("Type"))) continue;

			/* Add it to the referenced objects */
			Exports.Add(new FUObjectExport(Object, nullptr, nullptr, Index));
		}
	}

	FUObjectExport* Find(const FName Name) {
		for (FUObjectExport* Export : Exports) {
			if (Export->IsJsonValid() && Export->GetName() == Name) {
				return Export;
			}
		}

		return FUObjectExport::EmptyExport();
	}

	template<typename T>
	T* Find(const FName Name) {
		for (const FUObjectExport* Export : Exports) {
			if (Export->GetName() == Name) {
				return Export->Get<T>();
			}
		}

		return nullptr;
	}

	FUObjectExport* Find(const FName Name, const FName Outer) {
		for (FUObjectExport* Export : Exports) {
			if (Export->GetName() == Name && Export->GetOuter() == Outer) {
				return Export;
			}
		}

		return FUObjectExport::EmptyExport();
	}

	TArray<FUObjectExport*> GetExportsWithPropertyNameStartingWith(const FString& PropertyName, const FString& StartingWith) {
		TArray<FUObjectExport*> Result;
		
		for (FUObjectExport* Export : Exports) {
			if (Export->IsJsonValid() && Export->JsonObject->HasField(PropertyName)) {
				const FString TypeValue = Export->JsonObject->GetStringField(PropertyName);

				if (TypeValue.StartsWith(StartingWith)) {
					Result.Add(Export);
				}
			}
		}

		return Result;
	}

	FUObjectExport* GetExport(const FUObjectJsonValueExport& PackageIndex) {
		FString ObjectName = PackageIndex.GetString("ObjectName"); /* Class'Asset:ExportName' */
		FString ObjectPath = PackageIndex.GetString("ObjectPath"); /* Path/Asset.Index */
		FString Outer;
	
		/* Clean up ObjectName (Class'Asset:ExportName' --> Asset:ExportName --> ExportName) */
		ObjectName.Split("'", nullptr, &ObjectName);
		ObjectName.Split("'", &ObjectName, nullptr);

		if (ObjectName.Contains(":")) {
			ObjectName.Split(":", nullptr, &ObjectName); /* Asset:ExportName --> ExportName */
		}

		if (ObjectName.Contains(".")) {
			ObjectName.Split(".", nullptr, &ObjectName);
		}

		if (ObjectName.Contains(".")) {
			ObjectName.Split(".", &Outer, &ObjectName);
		}

		int Index = 0;

		/* Search for the object in the JsonObjects array */
		for (auto& Value : Exports) {
			FString Name;
			if (Value->JsonObject->TryGetStringField(TEXT("Name"), Name) && Name == ObjectName) {
				if (Value->JsonObject->HasField(TEXT("Outer")) && !Outer.IsEmpty()) {
					FString OuterName = GetOuterFromObjectOuter(Value->JsonObject->TryGetField(TEXT("Outer")));

					if (OuterName == Outer) {
						return Value;
					}
				} else {
					return Value;
				}
			}

			Index++;
		}

		return FUObjectExport::EmptyExport();
	}

	TSharedPtr<FJsonObject> GetExportJsonObjectByObjectPath(const TSharedPtr<FJsonObject>& Object) {
		const TSharedPtr<FJsonObject> ValueObject = TSharedPtr(Object);

		FString IndexAsString; {
			ValueObject->GetStringField(TEXT("ObjectPath")).Split(".", nullptr, &IndexAsString);
		}

		for (FUObjectExport* Export : Exports) {
			if (Export) {
				if (Export->Position == FCString::Atod(*IndexAsString)) {
					return Export->JsonObject;
				}
			}
		}

		return nullptr;
	}

	/* The one export a reference names, by the whole way down to it and nothing less.
	 *
	 * A name on its own is not enough to tell exports apart. An emitter holding a copy of what it
	 * was made from has a script source under each and both are called the same, an assignment
	 * node's own script has a third, and a graph sits under every one of them. Anything answering
	 * by the last part of the name hands back whichever of them it met first.
	 *
	 * Nothing is guessed at here: where the path does not lead to an export the reference is not
	 * naming one of ours, and it is left to be looked for wherever else it might be. */
	FUObjectExport* FindByFullObjectName(const FString& Named) {
		FString Whole = StripObjectOuter(Named);

		Whole.ReplaceInline(TEXT("."), TEXT(":"));

		TArray<FString> Parts;
		Whole.ParseIntoArray(Parts, TEXT(":"), true);

		if (Parts.Num() < 2) return FUObjectExport::EmptyExport();

		TArray<FName> Segments;

		for (const FString& Part : Parts) {
			Segments.Add(FName(*Part));
		}

		return FindByTreeSegment(Segments);
	}

	FUObjectExport* GetExportByObjectPath(const TSharedPtr<FJsonObject>& Object) {
		if (!Object.IsValid()) {
			return FUObjectExport::EmptyExport();
		}

		if (Object->HasField(TEXT("AssetPathName"))) {
			FString AssetPathName = Object->GetStringField(TEXT("AssetPathName"));
			
			FString AssetName;
			if (!AssetPathName.Split(TEXT("."), nullptr, &AssetName)) {
				return FUObjectExport::EmptyExport();
			}

			for (FUObjectExport* Export : Exports) {
				if (Export->GetName().ToString() == AssetName) {
					return Export;
				}
			}

			return FUObjectExport::EmptyExport();
		} else {
			if (!Object->HasField(TEXT("ObjectPath"))) {
				return FUObjectExport::EmptyExport();
			}
		}

		/* What the reference calls the object it wants, asked before the number it also carries.
		 *
		 * The two do not always agree. An asset cooked with an editor copy beside it is served as
		 * both packages at once, and the exports arrive in the order of one while the numbers in
		 * the reference count through the other: a validation rule set is one cooked export
		 * followed by fourteen rules that only the editor copy has, and every rule's number is a
		 * position in a table it is not being served from. Counted through, each reference lands on
		 * its neighbour. The name means the same thing in both. */
		if (FString Named; Object->TryGetStringField(TEXT("ObjectName"), Named)) {
			/* The whole way down to it first, since a name on its own is not always the one thing
			 * it names: a script keeps a graph per version of itself and they are copies, so the
			 * same node name sits in each of them. */
			FString Whole = StripObjectOuter(Named);

			Whole.ReplaceInline(TEXT("."), TEXT(":"));

			TArray<FString> Parts;
			Whole.ParseIntoArray(Parts, TEXT(":"), true);

			if (Parts.Num() > 1) {
				TArray<FName> Segments;

				for (const FString& Part : Parts) {
					Segments.Add(FName(*Part));
				}

				FUObjectExport* Walked = FindByTreeSegment(Segments);

				if (Walked->IsJsonValid()) {
					return Walked;
				}
			}

			const FString Leaf = GetObjectNameFromOuter(Named);

			FUObjectExport* Matched = nullptr;

			for (FUObjectExport* Export : Exports) {
				if (Leaf.IsEmpty() || !Export->IsJsonValid() || Export->GetName().ToString() != Leaf) {
					continue;
				}

				/* Two of a name, and nothing here to tell them apart, so the number has the say */
				if (Matched != nullptr) {
					Matched = nullptr;

					break;
				}

				Matched = Export;
			}

			if (Matched != nullptr) {
				return Matched;
			}
		}

		const FString ObjectPath = Object->GetStringField(TEXT("ObjectPath"));

		FString IndexString;
		if (!ObjectPath.Split(TEXT("."), nullptr, &IndexString)) {
			return FUObjectExport::EmptyExport();
		}

		const int32 Index = FCString::Atoi(*IndexString);

		for (FUObjectExport* Export : Exports) {
			if (Export->Position == Index) {
				return Export;
			}
		}

		return FUObjectExport::EmptyExport();
	}

	FUObjectExport* GetExportStartingWith(const FString& PropertyName, const FString& Name) {
		for (FUObjectExport* Export : Exports) {
			if (Export->GetString(PropertyName).StartsWith(Name)) {
				return Export;
			}
		}

		return FUObjectExport::EmptyExport();
	}

	FUObjectExport* GetExportByObjectPath(const FUObjectJsonValueExport& JsonExport) {
		return GetExportByObjectPath(JsonExport.JsonObject);
	}

	FUObjectExport* Find(const int Position) {
		for (FUObjectExport* Export : Exports) {
			if (Export->Position == Position) {
				return Export;
			}
		}

		return FUObjectExport::EmptyExport();
	}

	UObject* FindRef(const int Position) {
		for (const FUObjectExport* Export : Exports) {
			if (Export->Position == Position) {
				return Export->Object;
			}
		}

		return nullptr;
	}

	FUObjectExport* FindByPosition(const int Position) {
		for (FUObjectExport* Export : Exports) {
			if (Export->Position == Position) {
				return Export;
			}
		}

		return FUObjectExport::EmptyExport();
	}

	FUObjectExport* FindByPositionAndName(const int Position, const FString& Name) {
		for (FUObjectExport* Export : Exports) {
			if (Export->Position == Position && Export->GetName().ToString() == Name) {
				return Export;
			}
		}

		return FUObjectExport::EmptyExport();
	}

	FUObjectExport* Find(const FString& Name) {
		return Find(FName(*Name));
	}

	FUObjectExport* Find(const FString& Name, const FString& Outer) {
		return Find(FName(*Name), FName(*Outer));
	}

	FUObjectExport* FindByType(const FName Type) {
		for (FUObjectExport* Export : Exports) {
			if (Export->GetType() == Type) {
				return Export;
			}
		}

		return FUObjectExport::EmptyExport();
	}

	FUObjectExport* FindByType(const FString& Type) {
		return FindByType(FName(*Type));
	}

	FUObjectExport* FindByType(const FName Type, const FName Outer) {
		for (FUObjectExport* Export : Exports) {
			if (Export->GetType() == Type && Export->GetOuter() == Outer) {
				return Export;
			}
		}

		return FUObjectExport::EmptyExport();
	}

	FUObjectExport* FindByTreeSegment(const TArray<FName>& Segments, const bool bRemoveLast = false) {
    	for (FUObjectExport* Export : Exports) {
    		if (Export->GetOuterTreeSegments(bRemoveLast) == Segments) {
    			return Export;
    		}
    	}

    	return FUObjectExport::EmptyExport();
    }

	FUObjectExport* FindByType(const FString& Type, const FString& Outer) {
		return FindByType(FName(*Type), FName(*Outer));
	}
	
	bool Contains(const FName Name) {
		for (FUObjectExport* Export : Exports) {
			if (Export->GetName() == Name) {
				return true;
			}
		}

		return false;
	}

	/* Iterate exports, then execute lambda */
	template<typename FuncType>
	void ExportsLoop(const TArray<FUObjectJsonValueExport>& InExports, FuncType&& Func) {
		for (const FUObjectJsonValueExport& Export : InExports) {
			FUObjectExport* DirectExport = GetExportByObjectPath(Export);

			if (!DirectExport->IsJsonValid() || DirectExport == FUObjectExport::EmptyExport()) {
				continue;
			}

			Func(DirectExport);
		}
	}

	void Empty() {
		Exports.Empty();
	}
	
	int Num() const {
		return Exports.Num();
	}
};

