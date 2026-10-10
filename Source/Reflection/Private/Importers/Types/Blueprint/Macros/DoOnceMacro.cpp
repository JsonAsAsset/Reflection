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

		/* Every address the macro covers, including the jumps that only stitch its parts together */
		TSet<int32> Inside;

		/* Where an address leads once the jumps are stepped over, since an event graph stitches its parts with them */
		const auto Land = [&Statements, &Inside](int32 Address) -> int32 {
			TSet<int32> Walked;

			while (Address != INDEX_NONE && !Walked.Contains(Address)) {
				Walked.Add(Address);

				const int32 Landed = IndexOfAddress(Statements, Address);

				if (Landed == INDEX_NONE) return INDEX_NONE;
				if (TokenOf(Statements[Landed]) != TEXT("EX_Jump")) return Landed;

				Inside.Add(Address);

				Address = Statements[Landed].GetInteger(TEXT("CodeOffset"), INDEX_NONE);
			}

			return INDEX_NONE;
		};

		/* The statement the run carries on to */
		const auto Next = [&Statements, &Land](const int32 Index) -> int32 {
			return Statements.IsValidIndex(Index + 1) ? Land(AddressOf(Statements[Index + 1])) : INDEX_NONE;
		};

		/* The sequence, which runs the gate's test after settling whether it starts closed */
		if (TokenOf(Statements[At]) != TEXT("EX_PushExecutionFlow")) return false;

		const int32 Testing = Statements[At].GetInteger(TEXT("PushingAddress"), INDEX_NONE);

		if (Testing == INDEX_NONE) return false;

		/* Whether it has ever been reached */
		const int32 Asking = Next(At);

		if (Asking == INDEX_NONE || TokenOf(Statements[Asking]) != TEXT("EX_JumpIfNot")) return false;

		const FString Reached = ReadFrom(Statements[Asking].GetObject(TEXT("BooleanExpression")));

		if (!Reached.StartsWith(TEXT("Temp_bool_"))) return false;

		/* Reached before, there is nothing to settle and the thread ends */
		const int32 Settled = Next(Asking);

		if (Settled == INDEX_NONE || TokenOf(Statements[Settled]) != TEXT("EX_PopExecutionFlow")) return false;

		/* The first time through, which settles whether the gate starts closed */
		const int32 Settling = Land(Statements[Asking].GetInteger(TEXT("CodeOffset"), INDEX_NONE));

		if (Settling == INDEX_NONE || !IsLet(TokenOf(Statements[Settling]))) return false;
		if (WrittenTo(Statements[Settling]) != Reached) return false;

		/* Whether it was asked to start closed, which is the macro's own input */
		const int32 Asked = Next(Settling);

		if (Asked == INDEX_NONE || TokenOf(Statements[Asked]) != TEXT("EX_PopExecutionFlowIfNot")) return false;

		const FUObjectJsonValueExport Closing = Statements[Asked].GetObject(TEXT("BooleanExpression"));

		/* Closing it, where it was asked to start closed */
		const int32 Closed = Next(Asked);

		if (Closed == INDEX_NONE || !IsLet(TokenOf(Statements[Closed]))) return false;

		const FString Shut = WrittenTo(Statements[Closed]);

		if (!Shut.StartsWith(TEXT("Temp_bool_")) || Shut == Reached) return false;

		const int32 Ends = Next(Closed);

		if (Ends == INDEX_NONE || TokenOf(Statements[Ends]) != TEXT("EX_PopExecutionFlow")) return false;

		/* And the gate itself, which is what the sequence pushed */
		const int32 Gate = Land(Testing);

		if (Gate == INDEX_NONE || TokenOf(Statements[Gate]) != TEXT("EX_JumpIfNot")) return false;
		if (ReadFrom(Statements[Gate].GetObject(TEXT("BooleanExpression"))) != Shut) return false;

		/* Closed, nothing runs */
		const int32 Refused = Next(Gate);

		if (Refused == INDEX_NONE || TokenOf(Statements[Refused]) != TEXT("EX_PopExecutionFlow")) return false;

		/* Open, it shuts itself and runs */
		const int32 Opening = Land(Statements[Gate].GetInteger(TEXT("CodeOffset"), INDEX_NONE));

		if (Opening == INDEX_NONE || !IsLet(TokenOf(Statements[Opening]))) return false;
		if (WrittenTo(Statements[Opening]) != Shut) return false;

		/* What it lets through, which begins after the gate shuts itself */
		const int32 Through = Next(Opening);

		if (Through == INDEX_NONE) return false;

		Out.Leads.Add(TEXT("Completed"), AddressOf(Statements[Through]));

		Out.Inputs.Add(TEXT("Start Closed"), Closing);

		for (const int32 Which : { At, Asking, Settled, Settling, Asked, Closed, Ends, Gate, Refused, Opening }) {
			Inside.Add(AddressOf(Statements[Which]));
		}

		/* And whoever opens it again: Reset puts the closed local down and marks the gate reached */
		for (int32 Look = 0; Look < Statements.Num(); ++Look) {
			if (Inside.Contains(AddressOf(Statements[Look])) || !IsLet(TokenOf(Statements[Look]))) continue;

			if (WrittenTo(Statements[Look]) != Shut) continue;
			if (TokenOf(Statements[Look].GetObject(TEXT("Expression"))) != TEXT("EX_False")) continue;

			Out.Takes.Add(Look, TEXT("Reset"));

			Inside.Add(AddressOf(Statements[Look]));

			if (const int32 Marked = Next(Look); Marked != INDEX_NONE && IsLet(TokenOf(Statements[Marked])) && WrittenTo(Statements[Marked]) == Reached) {
				Inside.Add(AddressOf(Statements[Marked]));

				/* And ends the thread, which is the macro's and nobody else's */
				if (const int32 Done = Next(Marked); Done != INDEX_NONE && TokenOf(Statements[Done]) == TEXT("EX_PopExecutionFlow")) {
					Inside.Add(AddressOf(Statements[Done]));
				}
			}

			break;
		}

		/* Placed where the run reaches it, which is the sequence it begins with */
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

REGISTER_MACRO(FDoOnceMacro)
