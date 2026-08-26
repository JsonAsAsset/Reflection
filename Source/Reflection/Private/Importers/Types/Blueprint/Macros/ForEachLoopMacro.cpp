/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/MacroPattern.h"

using namespace MacroReading;

/* ForEachLoop, as StandardMacros defines it:
 *
 *     Tunnel(in)  execute -> Counter,  Array -> Length and -> Get
 *     Counter     Counter = 0                          -> Index
 *     Index       Index = 0                            -> Branch
 *     Branch      Condition <- Less(Counter, Length(Array))
 *                 then -> Keep,  else -> Completed
 *     Keep        Index = Counter                      -> Sequence
 *     Sequence    then_0 -> LoopBody,  then_1 -> Raise
 *     Raise       Counter = Add_IntInt(Counter, 1)     -> Branch
 *     Tunnel(out) LoopBody,  Array Element <- Get(Array, Index),  Array Index <- Index,  Completed
 *
 * so the script reads: zero the counter, zero the index, work out how long the array is, compare,
 * end the run where the comparison fails, copy the counter to the index, push where the counter is
 * raised, read the element, run the body, raise the counter, and go back to the comparison.
 *
 * None of that is written one statement after another. A function is written in the order it runs,
 * but an event graph is written as blocks stitched together by jumps, and a loop's first statement
 * can sit after everything the loop runs. So this is read by following the run rather than by
 * walking the list, and it says where each way out goes rather than where its parts end. */
struct FForEachLoopMacro final : FMacroPattern {
	virtual const TCHAR* GetName() const override { return TEXT("ForEachLoop"); }

	virtual bool Match(const TArray<FUObjectJsonValueExport>& Statements, const int32 At, FMacroMatch& Out) const override {
		if (!Statements.IsValidIndex(At)) return false;

		TSet<int32> Inside;

		/* Zeroing the counter, which is the first thing the loop does */
		int32 Where = AddressOf(Statements[At]);
		int32 Look = At;

		if (!IsLet(TokenOf(Statements[Look]))) return false;

		const FString Counter = WrittenTo(Statements[Look]);

		if (!Counter.StartsWith(TEXT("Temp_"))) return false;
		if (TokenOf(Statements[Look].GetObject(TEXT("Expression"))) != TEXT("EX_IntConst")) return false;

		Inside.Add(Where);

		/* Everything after this is reached by following the run rather than by reading on */
		auto Carry = [&Statements, &Inside, &Where, &Look]() {
			Where = NextInRun(Statements, Where, Inside);
			Look = Where == INDEX_NONE ? INDEX_NONE : IndexOfAddress(Statements, Where);

			return Look != INDEX_NONE;
		};

		/* Zeroing the index, which is a second scratch of the macro's own */
		if (!Carry() || !IsLet(TokenOf(Statements[Look]))) return false;

		const FString Index = WrittenTo(Statements[Look]);

		if (!Index.StartsWith(TEXT("Temp_")) || Index == Counter) return false;
		if (TokenOf(Statements[Look].GetObject(TEXT("Expression"))) != TEXT("EX_IntConst")) return false;

		Inside.Add(Where);

		/* How long the array is, which is the only place the array itself is named */
		if (!Carry() || !IsLet(TokenOf(Statements[Look]))) return false;

		const FString Length = WrittenTo(Statements[Look]);
		const FUObjectJsonValueExport Measuring = Statements[Look].GetObject(TEXT("Expression"));
		const FUObjectJsonValueExport Measures = Measuring.Has(TEXT("ContextExpression")) ? Measuring.GetObject(TEXT("ContextExpression")) : Measuring;

		if (CallsTo(Measures) != TEXT("Array_Length")) return false;

		const TArray<FUObjectJsonValueExport> Measured = Measures.Has(TEXT("Parameters")) ? Measures.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

		if (Measured.Num() < 1) return false;

		Inside.Add(Where);

		/* Where the run comes back to every time round */
		const int32 Compare = Where;

		/* Whether there is another one, which is the counter against the length */
		if (!Carry() || !IsLet(TokenOf(Statements[Look]))) return false;

		const FUObjectJsonValueExport Condition = Statements[Look].GetObject(TEXT("Expression"));

		if (CallsTo(Condition) != TEXT("Less_IntInt")) return false;

		const TArray<FUObjectJsonValueExport> Operands = Condition.Has(TEXT("Parameters")) ? Condition.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

		if (Operands.Num() < 2 || ReadFrom(Operands[0]) != Counter || ReadFrom(Operands[1]) != Length) return false;

		Inside.Add(Where);

		/* The run ends where the comparison fails */
		if (!Carry()) return false;

		const FString Ending = TokenOf(Statements[Look]);

		if (Ending != TEXT("EX_PopExecutionFlowIfNot") && Ending != TEXT("EX_JumpIfNot")) return false;

		Inside.Add(Where);

		/* A conditional jump says where it goes when it fails. A conditional pop carries on wherever
		 * it was pushed to, which is not this macro's to say, so nothing runs from Completed. */
		if (Ending == TEXT("EX_JumpIfNot")) {
			Out.Leads.Add(TEXT("Completed"), Statements[Look].GetInteger(TEXT("CodeOffset"), INDEX_NONE));
		}

		/* The index this time round is the counter */
		if (!Carry() || !IsLet(TokenOf(Statements[Look]))) return false;
		if (WrittenTo(Statements[Look]) != Index) return false;
		if (ReadFrom(Statements[Look].GetObject(TEXT("Expression"))) != Counter) return false;

		Inside.Add(Where);

		/* The body and the raising of the counter, one after the other */
		if (!Carry() || TokenOf(Statements[Look]) != TEXT("EX_PushExecutionFlow")) return false;

		const int32 Raise = Statements[Look].GetInteger(TEXT("PushingAddress"), INDEX_NONE);

		Inside.Add(Where);

		if (!Carry()) return false;

		/* The element out of the array, which the macro reads and the body only uses. It is only
		 * there at all where the body wanted it, so a body that ignored it has none. */
		const FUObjectJsonValueExport Reading = Statements[Look].Has(TEXT("ContextExpression")) ? Statements[Look].GetObject(TEXT("ContextExpression")) : Statements[Look];

		if (CallsTo(Reading) == TEXT("Array_Get")) {
			const TArray<FUObjectJsonValueExport> Got = Reading.Has(TEXT("Parameters")) ? Reading.GetArray(TEXT("Parameters")) : TArray<FUObjectJsonValueExport>();

			if (Got.Num() >= 3 && Same(Got[0], Measured[0]) && ReadFrom(Got[1]) == Index) {
				const FString Element = ReadFrom(Got[2]);

				if (!Element.IsEmpty()) {
					Out.Handouts.Add(Element, TEXT("Array Element"));
				}

				Inside.Add(Where);

				if (!Carry()) return false;
			}
		}

		Out.Leads.Add(TEXT("LoopBody"), Where);

		/* Raising the counter, which is where the body was pushed to */
		const int32 Raising = Raise == INDEX_NONE ? INDEX_NONE : IndexOfAddress(Statements, Raise);

		if (Raising == INDEX_NONE || !IsLet(TokenOf(Statements[Raising]))) return false;
		if (CallsTo(Statements[Raising].GetObject(TEXT("Expression"))) != TEXT("Add_IntInt")) return false;

		Inside.Add(Raise);

		Where = Raise;

		if (!Carry() || WrittenTo(Statements[Look]) != Counter) return false;

		Inside.Add(Where);

		/* And back round to the comparison, or this is some other loop entirely */
		if (!Carry() || Where != Compare) return false;

		/* Every read of the index in the body is a read of what the macro hands out */
		Out.Handouts.Add(Index, TEXT("Array Index"));

		Out.Inputs.Add(TEXT("Array"), Measured[0]);

		/* Placed where the run reaches it, which is where it began rather than where it is written */
		Out.First = At;
		Out.Last = At;

		for (const int32 Address : Inside) {
			const int32 Which = IndexOfAddress(Statements, Address);

			if (Which == INDEX_NONE) continue;

			Out.Internal.Add(Which);

			Out.Last = FMath::Max(Out.Last, Which);
		}

		return true;
	}
};

REGISTER_MACRO(FForEachLoopMacro)
