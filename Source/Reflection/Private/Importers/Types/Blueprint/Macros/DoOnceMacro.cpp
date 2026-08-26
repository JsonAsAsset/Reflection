/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/MacroPattern.h"

using namespace MacroReading;

/* DoOnce, as StandardMacros defines it:
 *
 *     Tunnel(in)  execute -> Sequence,  Reset -> IsClosed = false -> HasBeenInitd = false
 *                 Start Closed -> Branch(closing)
 *     Sequence    then_0 -> Branch(initialised),  then_1 -> Branch(closed)
 *     Branch(initialised)  Condition <- HasBeenInitd
 *                 then -> nothing,  else -> HasBeenInitd = true -> Branch(closing)
 *     Branch(closing)      Condition <- Start Closed
 *                 then -> IsClosed = true,  else -> nothing
 *     Branch(closed)       Condition <- IsClosed
 *                 then -> nothing,  else -> IsClosed = true -> Completed
 *     Tunnel(out) Completed
 *
 * So the gate keeps two things: whether it has ever been reached, which decides whether it starts
 * closed, and whether it is closed now. Reached the first time it opens itself, runs, and shuts;
 * reached again it does nothing. Both are locals of the macro, so both come back as compiler names
 * nobody wrote.
 *
 * Compiled, the sequence is a push and each way out that leads nowhere is a pop, which is what
 * makes this recognisable: a push, a test of the first local ending in a pop, and somewhere else
 * the setting of it. Read one statement after another none of that is together, so this follows
 * the run and says where its one way out goes. */
struct FDoOnceMacro final : FMacroPattern {
	virtual const TCHAR* GetName() const override { return TEXT("DoOnce"); }

	virtual bool Match(const TArray<FUObjectJsonValueExport>& Statements, const int32 At, FMacroMatch& Out) const override {
		if (!Statements.IsValidIndex(At)) return false;

		/* The sequence, which runs the gate's test after settling whether it starts closed */
		if (TokenOf(Statements[At]) != TEXT("EX_PushExecutionFlow")) return false;

		const int32 Testing = Statements[At].GetInteger(TEXT("PushingAddress"), INDEX_NONE);

		if (Testing == INDEX_NONE) return false;

		/* Whether it has ever been reached */
		if (!Statements.IsValidIndex(At + 1)) return false;

		const FUObjectJsonValueExport& Asking = Statements[At + 1];

		if (TokenOf(Asking) != TEXT("EX_JumpIfNot")) return false;

		const FString Reached = ReadFrom(Asking.GetObject(TEXT("BooleanExpression")));

		if (!Reached.StartsWith(TEXT("Temp_bool_"))) return false;

		/* Reached before, there is nothing to settle and the thread ends */
		if (!Statements.IsValidIndex(At + 2) || TokenOf(Statements[At + 2]) != TEXT("EX_PopExecutionFlow")) return false;

		/* The first time through, which settles whether the gate starts closed */
		const int32 Settling = IndexOfAddress(Statements, Asking.GetInteger(TEXT("CodeOffset"), INDEX_NONE));

		if (Settling == INDEX_NONE || !IsLet(TokenOf(Statements[Settling]))) return false;
		if (WrittenTo(Statements[Settling]) != Reached) return false;

		/* Whether it was asked to start closed, which is the macro's own input */
		if (!Statements.IsValidIndex(Settling + 1) || TokenOf(Statements[Settling + 1]) != TEXT("EX_PopExecutionFlowIfNot")) return false;

		const FUObjectJsonValueExport Closing = Statements[Settling + 1].GetObject(TEXT("BooleanExpression"));

		/* Closing it, where it was asked to start closed */
		if (!Statements.IsValidIndex(Settling + 2) || !IsLet(TokenOf(Statements[Settling + 2]))) return false;

		const FString Shut = WrittenTo(Statements[Settling + 2]);

		if (!Shut.StartsWith(TEXT("Temp_bool_")) || Shut == Reached) return false;

		if (!Statements.IsValidIndex(Settling + 3) || TokenOf(Statements[Settling + 3]) != TEXT("EX_PopExecutionFlow")) return false;

		/* And the gate itself, which is what the sequence pushed */
		const int32 Gate = IndexOfAddress(Statements, Testing);

		if (Gate == INDEX_NONE || TokenOf(Statements[Gate]) != TEXT("EX_JumpIfNot")) return false;
		if (ReadFrom(Statements[Gate].GetObject(TEXT("BooleanExpression"))) != Shut) return false;

		/* Closed, nothing runs */
		if (!Statements.IsValidIndex(Gate + 1) || TokenOf(Statements[Gate + 1]) != TEXT("EX_PopExecutionFlow")) return false;

		/* Open, it shuts itself and runs */
		const int32 Opening = IndexOfAddress(Statements, Statements[Gate].GetInteger(TEXT("CodeOffset"), INDEX_NONE));

		if (Opening == INDEX_NONE || !IsLet(TokenOf(Statements[Opening]))) return false;
		if (WrittenTo(Statements[Opening]) != Shut) return false;

		/* What it lets through, which begins after the gate shuts itself */
		if (!Statements.IsValidIndex(Opening + 1)) return false;

		Out.Leads.Add(TEXT("Completed"), AddressOf(Statements[Opening + 1]));

		Out.Inputs.Add(TEXT("Start Closed"), Closing);

		for (const int32 Which : { At, At + 1, At + 2, Settling, Settling + 1, Settling + 2, Settling + 3, Gate, Gate + 1, Opening }) {
			Out.Internal.Add(Which);
		}

		/* Placed where the run reaches it, which is the sequence it begins with */
		Out.First = At;
		Out.Last = At;

		for (const int32 Which : Out.Internal) Out.Last = FMath::Max(Out.Last, Which);

		return true;
	}
};

REGISTER_MACRO(FDoOnceMacro)
