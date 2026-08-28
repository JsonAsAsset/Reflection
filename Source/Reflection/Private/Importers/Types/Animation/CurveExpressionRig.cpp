/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/CurveExpressionRig.h"
#include "Engine/Log.h"

#if REFLECTION_RIGVM

#include "Importers/Constructor/ImportIssues.h"
#include "Modules/Metadata.h"

#include "Engine/ControlRigCompatibility.h"

/* The mapping, laid out as the arithmetic it describes.
 *
 * One read per curve the mapping reads, however many targets want it, since a control read twice is
 * the same number both times and a second node saying so is only more to look at. From there each
 * target is its own little run of arithmetic: multiply what needs weighing, add the terms up, clamp
 * where the mapping clamps, and write the answer.
 *
 * The writes are chained one after another off the forwards solve. Order between them does not
 * matter no target is read by any other, they read the rig's controls and write the head's
 * curves so they run in the order the mapping lists them, which is alphabetical and makes the
 * graph findable. */

namespace {
	/* Left to right, and down the page as the targets go by */
	FVector2D At(const int32 Column, const int32 Row) {
		return FVector2D(360.0 * Column, 130.0 * Row);
	}

	/* A pin by name, or nothing where this engine's version of the node hasn't got one */
	FString PathOf(const URigVMNode* Node, const TCHAR* Pin) {
		if (Node == nullptr) return FString();

		const URigVMPin* Found = Node->FindPin(Pin);

		return Found != nullptr ? Found->GetPinPath() : FString();
	}

	/* Written out far enough that the pin gets the weight the mapping had.
	 *
	 * Six places is the default and is not always enough: a weight is whatever the head was authored
	 * with, and one written shorter than it is, is a rig that does slightly the wrong thing forever.
	 * Nine decimal places carries any float that is not vanishingly small, and never reaches for
	 * exponent notation on the way. */
	FString Number(const double Value) {
		return FString::Printf(TEXT("%.9f"), Value);
	}
}

bool FCurveExpressionRig::Build(UControlRigBlueprint* Blueprint, const TArray<TSharedPtr<FJsonValue>>& Expressions,
	const FString& Named, FCurveExpressionRigStats& OutStats) {
	if (Blueprint == nullptr || Expressions.Num() == 0) return false;

	URigVMGraph* Graph = RigModelOf(Blueprint);
	URigVMController* Controller = RigControllerOf(Blueprint, Graph);

	if (Graph == nullptr || Controller == nullptr) return false;

	/* Every curve the mapping reads, gathered before anything is drawn so each one is read once and
	 * the reads can stand in a column of their own */
	TArray<FString> Reads;

	for (const TSharedPtr<FJsonValue>& Value : Expressions) {
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

		if (!Entry.IsValid()) continue;

		FString Form;

		Entry->TryGetStringField(TEXT("form"), Form);

		/* Anything the Cloud would not call a weighted sum has no weights worth building from, and
		 * a graph made out of them would be arithmetic the head never asked for */
		if (Form != TEXT("linear") && Form != TEXT("clamped")) continue;

		const TSharedPtr<FJsonObject>* Weights = nullptr;

		if (!Entry->TryGetObjectField(TEXT("weights"), Weights)) continue;

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Weight : (*Weights)->Values) {
			Reads.AddUnique(Weight.Key);
		}
	}

	if (Reads.Num() == 0) return false;

	Reads.Sort();

	int32 Made = 0;

	/* The forwards solve, which is what the rig runs when it poses a face */
	URigVMNode* Begin = RigAddNode(Controller, RigUnits::BeginExecution, At(0, 0), TEXT("RigUnit_BeginExecution"));

	if (Begin == nullptr) return false;

	Made++;

	/* One read per curve, in a column the rest of the graph reaches back into */
	TMap<FString, FString> ReadValue;

	for (int32 Index = 0; Index < Reads.Num(); ++Index) {
		URigVMNode* Read = RigAddNode(Controller, RigUnits::GetCurveValue, At(1, Index + 1),
			FString::Printf(TEXT("Get_%s"), *Reads[Index]));

		if (Read == nullptr) continue;

		Made++;

		const FString Curve = PathOf(Read, TEXT("Curve"));

		RigSetDefault(Controller, Curve, Reads[Index]);

		ReadValue.Add(Reads[Index], PathOf(Read, TEXT("Value")));
	}

	OutStats.Read = ReadValue.Num();

	/* Where execution has got to, which is the last write made */
	const URigVMPin* Started = RigExecutePin(Begin);

	FString Running = Started != nullptr ? Started->GetPinPath() : FString();

	int32 Row = 0;

	for (const TSharedPtr<FJsonValue>& Value : Expressions) {
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

		if (!Entry.IsValid()) continue;

		FString Target, Form;

		if (!Entry->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty()) continue;

		OutStats.Targets++;

		Entry->TryGetStringField(TEXT("form"), Form);

		if (Form != TEXT("linear") && Form != TEXT("clamped")) {
			OutStats.Refused.Add(Target);

			continue;
		}

		const TSharedPtr<FJsonObject>* Weights = nullptr;

		if (!Entry->TryGetObjectField(TEXT("weights"), Weights) || (*Weights)->Values.Num() == 0) {
			OutStats.Refused.Add(Target);

			continue;
		}

		/* Added up in the order the expression names them rather than the order a map hands them
		 * back. Adding floats is not associative, so the order is part of the answer, and the
		 * Cloud lists the constants as the expression reached them. */
		TArray<FString> Order;

		if (const TArray<TSharedPtr<FJsonValue>>* Constants = nullptr; Entry->TryGetArrayField(TEXT("constants"), Constants)) {
			for (const TSharedPtr<FJsonValue>& One : *Constants) {
				FString Read;

				if (One.IsValid() && One->TryGetString(Read)) Order.AddUnique(Read);
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Weight : (*Weights)->Values) {
			Order.AddUnique(Weight.Key);
		}

		Row++;

		/* Each term of the sum, weighed where the mapping weighs it. A control worth all of itself
		 * is taken straight off its read, since multiplying by one is a node that says nothing. */
		TArray<FString> Terms;

		int32 Depth = 0;

		for (const FString& Read : Order) {
			const TSharedPtr<FJsonValue> Held = (*Weights)->TryGetField(Read);

			if (!Held.IsValid()) continue;

			const FString* Source = ReadValue.Find(Read);

			if (Source == nullptr || Source->IsEmpty()) continue;

			const double Amount = Held->AsNumber();

			if (FMath::IsNearlyEqual(Amount, 1.0)) {
				Terms.Add(*Source);

				continue;
			}

			URigVMNode* Scale = RigAddNode(Controller, RigUnits::Multiply, At(2 + Depth++, Row),
				FString::Printf(TEXT("%s_%s_Weighed"), *Target, *Read));

			if (Scale == nullptr) continue;

			Made++;

			RigLink(Controller, *Source, PathOf(Scale, TEXT("A")));
			RigSetDefault(Controller, PathOf(Scale, TEXT("B")), Number(Amount));

			Terms.Add(PathOf(Scale, TEXT("Result")));
		}

		if (Terms.Num() == 0) {
			OutStats.Refused.Add(Target);

			continue;
		}

		/* Added up two at a time, which is the only shape the node has */
		FString Total = Terms[0];

		for (int32 Index = 1; Index < Terms.Num(); ++Index) {
			URigVMNode* Add = RigAddNode(Controller, RigUnits::Add, At(4 + Index, Row),
				FString::Printf(TEXT("%s_Sum_%d"), *Target, Index));

			if (Add == nullptr) continue;

			Made++;

			RigLink(Controller, Total, PathOf(Add, TEXT("A")));
			RigLink(Controller, Terms[Index], PathOf(Add, TEXT("B")));

			Total = PathOf(Add, TEXT("Result"));
		}

		/* Held where the mapping holds it. A sum of weights over one would otherwise drive the face
		 * past anything it was authored at, which is the whole reason the clamp is written. */
		if (Form == TEXT("clamped")) {
			URigVMNode* Clamp = RigAddNode(Controller, RigUnits::Clamp, At(4 + Terms.Num(), Row),
				FString::Printf(TEXT("%s_Clamp"), *Target));

			if (Clamp != nullptr) {
				Made++;

				RigLink(Controller, Total, PathOf(Clamp, TEXT("Value")));
				RigSetDefault(Controller, PathOf(Clamp, TEXT("Minimum")), Number(0.0));
				RigSetDefault(Controller, PathOf(Clamp, TEXT("Maximum")), Number(1.0));

				Total = PathOf(Clamp, TEXT("Result"));
			}
		}

		URigVMNode* Write = RigAddNode(Controller, RigUnits::SetCurveValue, At(6 + Terms.Num(), Row),
			FString::Printf(TEXT("Set_%s"), *Target));

		if (Write == nullptr) {
			OutStats.Refused.Add(Target);

			continue;
		}

		Made++;

		RigSetDefault(Controller, PathOf(Write, TEXT("Curve")), Target);
		RigLink(Controller, Total, PathOf(Write, TEXT("Value")));

		/* Strung onto the end of what has run so far */
		const URigVMPin* Carries = RigExecutePin(Write);

		if (Carries != nullptr && !Running.IsEmpty()) {
			RigLink(Controller, Running, Carries->GetPinPath());

			Running = Carries->GetPinPath();
		}

		OutStats.Written++;
	}

	OutStats.Nodes = Made;

	if (OutStats.Refused.Num() > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d of %d curves aren't a weighted sum"), OutStats.Refused.Num(), OutStats.Targets),
			FString::Printf(
				TEXT("The rig performs the rest. These drive their curve by arithmetic that weights alone don't describe, so nothing was built for them: %s"),
				*FString::Join(OutStats.Refused, TEXT(", ")))
		);
	}

	UE_LOG(LogReflection, Display,
		TEXT("\"%s\" built %d node(s): %d curve(s) written out of %d read"),
		*Named, OutStats.Nodes, OutStats.Written, OutStats.Read);

	return OutStats.Written > 0;
}

#endif
