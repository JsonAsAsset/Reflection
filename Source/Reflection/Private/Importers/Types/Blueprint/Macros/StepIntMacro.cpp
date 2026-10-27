/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/MacroPattern.h"

using namespace MacroReading;

/* DecrementInt and IncrementInt, as StandardMacros defines them:
 *
 *     Tunnel(in)  " " -> Manipulate,  Value (by ref) -> Subtract_IntInt.A (or Add_IntInt.A),  Manipulate.IntegerRef
 *     Subtract_IntInt(Value, 1) -> Manipulate.NewValue
 *     Manipulate  ManipulateIntInternal: Temp = NewValue, then Value = Temp
 *     Tunnel(out) " ",  Result <- Temp
 *
 * so the script reads three lets in a row: the call into its return value, that into the macro's local, and the local back
 * into whatever was passed by reference.
 *
 * Left unread, the call comes back as a pure node feeding both the set and whatever reads the result, and a pure node is
 * worked out again for every node that reads it: the read after the set sees the value stepped twice. */
struct FStepIntMacro : FMacroPattern {
	virtual const TCHAR* Steps() const = 0;

	virtual bool Match(const TArray<FUObjectJsonValueExport>& Statements, const int32 At, FMacroMatch& Out) const override {
		if (!Statements.IsValidIndex(At + 2) || !IsLet(TokenOf(Statements[At]))) return false;

		/* The step itself, always by one */
		const FUObjectJsonValueExport Call = Statements[At].GetObject(TEXT("Expression"));

		if (CallsTo(Call) != Steps()) return false;

		const TArray<FUObjectJsonValueExport> Operands = Call.Has(TEXT("Parameters")) ? Call.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

		if (Operands.Num() != 2 || TokenOf(Operands[1]) != TEXT("EX_IntConst") || Operands[1].GetInteger(TEXT("Value"), 0) != 1) return false;

		const FString Stepped = WrittenTo(Statements[At]);

		if (Stepped.IsEmpty()) return false;

		/* Into the macro's own local, which is what the compiler calls Temp_ */
		if (!IsLet(TokenOf(Statements[At + 1]))) return false;

		const FString Local = WrittenTo(Statements[At + 1]);

		if (!Local.StartsWith(TEXT("Temp_int_Variable")) || ReadFrom(Statements[At + 1].GetObject(TEXT("Expression"))) != Stepped) return false;

		/* And back into what was passed in, which is what makes it this macro rather than a call and a copy */
		if (!IsLet(TokenOf(Statements[At + 2]))) return false;
		if (!Same(Statements[At + 2].GetObject(TEXT("Variable")), Operands[0])) return false;
		if (ReadFrom(Statements[At + 2].GetObject(TEXT("Expression"))) != Local) return false;

		Out.First = At;
		Out.Last = At + 2;

		Out.Inputs.Add(TEXT("Value"), Operands[0]);

		/* What the step is written back into is the caller's variable, wired to Value, not something the macro keeps */
		if (const FString Passed = WrittenTo(Statements[At + 2]); !Passed.IsEmpty()) {
			Out.ByReference.Add(Passed);
		}

		/* Every read of the local afterwards is a read of what the macro hands out */
		Out.Handouts.Add(Local, TEXT("Result"));

		/* The run carries on through the one way out, which StandardMacros names with a space */
		Out.Bodies.Add(TPair<FName, TPair<int32, int32>>(FName(TEXT(" ")), TPair<int32, int32>(At + 3, At + 3)));

		Out.Internal.Add(At);
		Out.Internal.Add(At + 1);
		Out.Internal.Add(At + 2);

		return true;
	}
};

struct FDecrementIntMacro final : FStepIntMacro {
	virtual const TCHAR* GetName() const override { return TEXT("DecrementInt"); }
	virtual const TCHAR* Steps() const override { return TEXT("Subtract_IntInt"); }
};

struct FIncrementIntMacro final : FStepIntMacro {
	virtual const TCHAR* GetName() const override { return TEXT("IncrementInt"); }
	virtual const TCHAR* Steps() const override { return TEXT("Add_IntInt"); }
};

REGISTER_MACRO(FDecrementIntMacro)
REGISTER_MACRO(FIncrementIntMacro)
