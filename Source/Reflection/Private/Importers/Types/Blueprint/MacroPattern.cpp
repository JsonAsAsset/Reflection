/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/MacroPattern.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"

namespace {
	/* Held here rather than beside the macros, so the order they are tried in is the order they
	 * were taken in and nothing depends on when a file happened to be compiled */
	TArray<TSharedRef<FMacroPattern>>& Patterns() {
		static TArray<TSharedRef<FMacroPattern>> Held;

		return Held;
	}
}

void AddMacroPattern(const TSharedRef<FMacroPattern>& Pattern) {
	Patterns().Add(Pattern);
}

const TArray<TSharedRef<FMacroPattern>>& GetMacroPatterns() {
	return Patterns();
}

namespace MacroReading {
	FString NamedProperty(const FUObjectJsonValueExport& Variable) {
		if (!Variable.JsonObject.IsValid()) return FString();

		/* Named through a field path, which is what a property written into its owner comes as */
		if (Variable.Has(TEXT("Property"))) {
			return Variable.GetObject(TEXT("Property")).GetString(TEXT("Name"));
		}

		/* Spelled as the names it is reached by, the last of which is what it is called */
		if (const TArray<TSharedPtr<FJsonValue>>* Path; Variable.JsonObject->TryGetArrayField(TEXT("Path"), Path) && Path->Num() > 0) {
			return (*Path)[Path->Num() - 1]->AsString();
		}

		/* Written out where it is used, which is a property that carries its own name */
		if (FString Name; Variable.JsonObject->TryGetStringField(TEXT("Name"), Name)) {
			return Name;
		}

		/* Pointed at the way any object is pointed at: Class'Owner:Name' */
		FString Reference;

		if (!Variable.JsonObject->TryGetStringField(TEXT("ObjectName"), Reference)) return FString();

		if (Reference.Contains(TEXT("'"))) {
			Reference.Split(TEXT("'"), nullptr, &Reference);
			Reference.Split(TEXT("'"), &Reference, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		}

		FString Owner, Member;

		if (Reference.Split(TEXT(":"), &Owner, &Member)) Reference = Member;
		if (Reference.Split(TEXT("."), &Owner, &Member)) Reference = Member;

		return Reference;
	}

	FString WrittenTo(const FUObjectJsonValueExport& Statement) {
		if (!Statement.Has(TEXT("Variable"))) return FString();

		const FUObjectJsonValueExport Assigned = Statement.GetObject(TEXT("Variable"));

		if (!Assigned.Has(TEXT("Variable"))) return FString();

		return NamedProperty(Assigned.GetObject(TEXT("Variable")));
	}

	FString ReadFrom(const FUObjectJsonValueExport& Expression) {
		if (!Expression.Has(TEXT("Variable"))) return FString();

		return NamedProperty(Expression.GetObject(TEXT("Variable")));
	}

	FString CallsTo(const FUObjectJsonValueExport& Expression) {
		if (!Expression.Has(TEXT("Function"))) return FString();

		if (Expression.JsonObject.IsValid() && Expression.JsonObject->HasTypedField<EJson::String>(TEXT("Function"))) {
			return Expression.GetString(TEXT("Function"));
		}

		FString Name = Expression.GetObject(TEXT("Function")).GetString(TEXT("ObjectName"));

		if (Name.Contains(TEXT("'"))) {
			Name.Split(TEXT("'"), nullptr, &Name);
			Name.Split(TEXT("'"), &Name, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		}

		FString Owner, Member;

		return Name.Split(TEXT(":"), &Owner, &Member) ? Member : Name;
	}

	namespace {
		bool SameValue(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B) {
			if (!A.IsValid() || !B.IsValid()) return A.IsValid() == B.IsValid();
			if (A->Type != B->Type) return false;

			switch (A->Type) {
			case EJson::Object: {
				const TSharedPtr<FJsonObject> Left = A->AsObject();
				const TSharedPtr<FJsonObject> Right = B->AsObject();

				if (!Left.IsValid() || !Right.IsValid()) return Left.IsValid() == Right.IsValid();
				if (Left->Values.Num() != Right->Values.Num()) return false;

				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Left->Values) {
					const TSharedPtr<FJsonValue>* Other = Right->Values.Find(StringToJsonKey(Field.Key));

					if (Other == nullptr || !SameValue(Field.Value, *Other)) return false;
				}

				return true;
			}

			case EJson::Array: {
				const TArray<TSharedPtr<FJsonValue>>& Left = A->AsArray();
				const TArray<TSharedPtr<FJsonValue>>& Right = B->AsArray();

				if (Left.Num() != Right.Num()) return false;

				for (int32 Index = 0; Index < Left.Num(); ++Index) {
					if (!SameValue(Left[Index], Right[Index])) return false;
				}

				return true;
			}

			case EJson::String:  return A->AsString() == B->AsString();
			case EJson::Number:  return A->AsNumber() == B->AsNumber();
			case EJson::Boolean: return A->AsBool() == B->AsBool();
			default:             return true;
			}
		}
	}

	bool Same(const FUObjectJsonValueExport& A, const FUObjectJsonValueExport& B) {
		if (!A.JsonObject.IsValid() || !B.JsonObject.IsValid()) return false;

		return SameValue(MakeShared<FJsonValueObject>(A.JsonObject), MakeShared<FJsonValueObject>(B.JsonObject));
	}

	UEdGraph* StandardMacro(const TCHAR* Named) {
		/* The engine keeps the only statement of what a macro means */
		const UBlueprint* Standard = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));

		if (Standard == nullptr) return nullptr;

		for (UEdGraph* Macro : Standard->MacroGraphs) {
			if (Macro != nullptr && Macro->GetName() == Named) return Macro;
		}

		return nullptr;
	}

	FString TokenOf(const FUObjectJsonValueExport& Statement) {
		return Statement.Has(TEXT("Token")) ? Statement.GetString(TEXT("Token")) : FString();
	}

	bool IsLet(const FString& Token) {
		return Token == TEXT("EX_Let")
			|| Token == TEXT("EX_LetBool")
			|| Token == TEXT("EX_LetObj")
			|| Token == TEXT("EX_LetWeakObjPtr")
			|| Token == TEXT("EX_LetDelegate")
			|| Token == TEXT("EX_LetMulticastDelegate")
			|| Token == TEXT("EX_LetValueOnPersistentFrame");
	}

	int32 AddressOf(const FUObjectJsonValueExport& Statement) {
		return Statement.GetInteger(TEXT("StatementIndex"), -1);
	}

	int32 NextInRun(const TArray<FUObjectJsonValueExport>& Statements, const int32 Address, TSet<int32>& Passed) {
		const int32 At = IndexOfAddress(Statements, Address);

		if (At == INDEX_NONE) return INDEX_NONE;

		int32 Next = TokenOf(Statements[At]) == TEXT("EX_Jump")
			? Statements[At].GetInteger(TEXT("CodeOffset"), INDEX_NONE)
			: (Statements.IsValidIndex(At + 1) ? AddressOf(Statements[At + 1]) : INDEX_NONE);

		TSet<int32> Walked;

		/* Straight over anything that only says where to carry on, however many of them there are */
		while (Next != INDEX_NONE && !Walked.Contains(Next)) {
			const int32 Landed = IndexOfAddress(Statements, Next);

			if (Landed == INDEX_NONE || TokenOf(Statements[Landed]) != TEXT("EX_Jump")) break;

			Walked.Add(Next);
			Passed.Add(Next);

			Next = Statements[Landed].GetInteger(TEXT("CodeOffset"), INDEX_NONE);
		}

		return Next;
	}

	int32 IndexOfAddress(const TArray<FUObjectJsonValueExport>& Statements, const int32 Address) {
		for (int32 Index = 0; Index < Statements.Num(); ++Index) {
			if (AddressOf(Statements[Index]) == Address) return Index;
		}

		return INDEX_NONE;
	}
}
