/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/MacroPattern.h"

using namespace MacroReading;

/* ForLoop, as StandardMacros defines it:
 *
 *     Tunnel(in)  execute -> Assign,  FirstIndex -> Assign,  LastIndex -> LessEqual.B
 *     Assign      Counter = FirstIndex                 -> Branch
 *     Branch      Condition <- LessEqual(Counter, LastIndex)
 *                 then -> Sequence,  else -> Completed
 *     Sequence    then_0 -> LoopBody,  then_1 -> Raise
 *     Raise       Counter = Add_IntInt(Counter, 1)     -> Branch
 *     Tunnel(out) LoopBody,  Index <- Counter,  Completed
 *
 * so the script reads: set the counter, work out the last index, compare, end the run where the
 * comparison fails, push where the counter is raised, run the body, raise the counter, and jump
 * back to the comparison.
 *
 * What lies between setting the counter and comparing it is not the macro's: it is whatever the
 * caller wrote for the last index, and it can be any length. */
struct FForLoopMacro final : FMacroPattern {
	virtual const TCHAR* GetName() const override { return TEXT("ForLoop"); }

	virtual bool Match(const TArray<FUObjectJsonValueExport>& Statements, const int32 At, FMacroMatch& Out) const override {
		if (!Statements.IsValidIndex(At) || !IsLet(TokenOf(Statements[At]))) return false;

		/* The counter is a macro's own scratch, which is what the compiler calls Temp_ */
		const FString Counter = WrittenTo(Statements[At]);

		if (!Counter.StartsWith(TEXT("Temp_"))) return false;

		/* Compared against the last index somewhere after it, with whatever works that out in
		 * between. Nothing may touch the counter before then. */
		int32 Compare = INDEX_NONE;

		for (int32 Look = At + 1; Look < Statements.Num(); ++Look) {
			const FUObjectJsonValueExport& Statement = Statements[Look];

			if (WrittenTo(Statement) == Counter) return false;
			if (!IsLet(TokenOf(Statement))) break;

			if (CallsTo(Statement.GetObject(TEXT("Expression"))) == TEXT("LessEqual_IntInt")) {
				Compare = Look;

				break;
			}
		}

		if (Compare == INDEX_NONE || Compare + 2 >= Statements.Num()) return false;

		const FUObjectJsonValueExport Condition = Statements[Compare].GetObject(TEXT("Expression"));
		const TArray<FUObjectJsonValueExport> Operands = Condition.Has(TEXT("Parameters")) ? Condition.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

		/* It is the counter that is compared, or this is some other comparison entirely */
		if (Operands.Num() < 2 || ReadFrom(Operands[0]) != Counter) return false;

		/* The run ends where the comparison fails, and the body is pushed after it */
		const FString Ending = TokenOf(Statements[Compare + 1]);

		if (Ending != TEXT("EX_PopExecutionFlowIfNot") && Ending != TEXT("EX_JumpIfNot")) return false;
		if (TokenOf(Statements[Compare + 2]) != TEXT("EX_PushExecutionFlow")) return false;

		/* Where the counter is raised, which the push names */
		const int32 Raise = IndexOfAddress(Statements, Statements[Compare + 2].GetInteger(TEXT("PushingAddress"), -1));

		if (Raise == INDEX_NONE || Raise + 2 >= Statements.Num()) return false;
		if (CallsTo(Statements[Raise].GetObject(TEXT("Expression"))) != TEXT("Add_IntInt")) return false;
		if (WrittenTo(Statements[Raise + 1]) != Counter) return false;
		if (TokenOf(Statements[Raise + 2]) != TEXT("EX_Jump")) return false;

		Out.First = At;
		Out.Last = Raise + 2;

		/* What the caller wrote, which is all the macro takes from them */
		Out.Inputs.Add(TEXT("FirstIndex"), Statements[At].GetObject(TEXT("Expression")));
		Out.Deferred.Add(TEXT("LastIndex"), Operands[1]);

		/* Every read of the counter in the body is a read of what the macro hands out */
		Out.Handouts.Add(Counter, TEXT("Index"));

		/* Where each way out goes, said by address rather than by carrying the run on.
		 *
		 * What runs after the loop is written among the statements the loop covers: the compiler
		 * puts it wherever there was room, not after the last thing the loop does. So where the
		 * loop ends cannot be told from where the macro's own statements stop, and the way out has
		 * to name the address it leads to. */
		if (Statements.IsValidIndex(Compare + 3)) {
			Out.Leads.Add(TEXT("LoopBody"), AddressOf(Statements[Compare + 3]));
		}

		/* A conditional jump says where it goes when it fails. A conditional pop carries on wherever
		 * it was pushed to, which is not this macro's to say. */
		if (TokenOf(Statements[Compare + 1]) == TEXT("EX_JumpIfNot")) {
			Out.Leads.Add(TEXT("Completed"), Statements[Compare + 1].GetInteger(TEXT("CodeOffset"), INDEX_NONE));
		}

		/* Everything else here is the macro talking to itself */
		Out.Internal.Add(At);
		Out.Internal.Add(Compare);
		Out.Internal.Add(Compare + 1);
		Out.Internal.Add(Compare + 2);
		Out.Internal.Add(Raise);
		Out.Internal.Add(Raise + 1);
		Out.Internal.Add(Raise + 2);

		return true;
	}
};

REGISTER_MACRO(FForLoopMacro)
