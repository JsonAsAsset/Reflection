/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/LegacyCurves.h"

#include "Importers/Constructor/ImportIssues.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Metadata.h"
#include "Engine/Log.h"

/* An animation written for the newer rig, said in the names an older head drives.
 *
 * The mapping is written one way round: each of the newer rig's controls is an expression over the
 * older head's curves. Read forward that turns an older animation into a newer one, and a pose
 * asset is built that way round too, one pose per older curve.
 *
 * An animation out of the game is already the newer rig's, so it has to be read the other way. Each
 * control the animation drives is one statement about the curves underneath it this much of this
 * curve plus that much of that one came to the number you can see and an emote drives far more
 * controls than there are curves beneath them. Forty one statements about thirty eight unknowns is
 * not a puzzle with a missing piece; it is one with more pieces than it needs, and the curves that
 * best account for the whole set at once are what comes out.
 *
 * Solved a frame at a time, holding every curve at or above zero because a face curve that went
 * negative would drive the rig somewhere it was never authored to go.
 *
 * It is a fit rather than a recovery, and it says so: the emote was authored on the newer rig
 * directly, so it moves controls in combinations no older curve can produce, and those parts have
 * nowhere to land. What comes back is the closest an older head can stand. */

namespace {
	/* One frame's worth of the mapping's own arithmetic, solved backwards.
	 *
	 * Projected gradient, which is the plain gradient step with anything that went negative put back
	 * to zero after each one. Slower to converge than the methods that pick a set of curves and
	 * solve exactly for those, and immune to the thing that makes those hard to trust: it cannot
	 * land on a set and stay there because it never commits to one. */
	void Solve(const TArray<double>& AtA, const TArray<double>& Atb, const double Step, TArray<double>& InOutX) {
		const int32 Curves = Atb.Num();

		TArray<double> Gradient;
		Gradient.SetNumZeroed(Curves);

		for (int32 Pass = 0; Pass < 300; ++Pass) {
			for (int32 Row = 0; Row < Curves; ++Row) {
				double Sum = -Atb[Row];

				for (int32 Column = 0; Column < Curves; ++Column) {
					Sum += AtA[Row * Curves + Column] * InOutX[Column];
				}

				Gradient[Row] = Sum;
			}

			for (int32 Row = 0; Row < Curves; ++Row) {
				InOutX[Row] = FMath::Max(0.0, InOutX[Row] - Gradient[Row] * Step);
			}
		}
	}

	/* What a curve reads at a time it has no key at. The keys come out of a cook already sampled per
	 * frame, so this is only ever reading between two of them or holding at an end. */
	double Sample(const TArray<TPair<double, double>>& Keys, const double Time) {
		if (Keys.Num() == 0) return 0.0;

		if (Time <= Keys[0].Key) return Keys[0].Value;
		if (Time >= Keys.Last().Key) return Keys.Last().Value;

		for (int32 Index = 1; Index < Keys.Num(); ++Index) {
			if (Keys[Index].Key < Time) continue;

			const double Span = Keys[Index].Key - Keys[Index - 1].Key;

			if (Span <= 0.0) return Keys[Index].Value;

			const double Along = (Time - Keys[Index - 1].Key) / Span;

			return FMath::Lerp(Keys[Index - 1].Value, Keys[Index].Value, Along);
		}

		return Keys.Last().Value;
	}

	/* The name a curve carries, however this cook chose to say it */
	FString NameOf(const TSharedPtr<FJsonObject>& Curve) {
		FString DisplayName;

		if (const TSharedPtr<FJsonObject>* Name = nullptr; Curve->TryGetObjectField(TEXT("Name"), Name)) {
			(*Name)->TryGetStringField(TEXT("DisplayName"), DisplayName);
		} else {
			Curve->TryGetStringField(TEXT("CurveName"), DisplayName);
		}

		return DisplayName;
	}
}

bool FReflectionLegacyCurves::Read(TMap<FName, TArray<FLegacyCurveDrive>>& OutByCurve) {
	const TSharedPtr<FJsonObject> Response = Cloud::Export::GetCurveMappingBlocking();

	if (!Response.IsValid()) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The Cloud hasn't got the legacy curve mapping"),
			TEXT("It says how this rig's controls are made of an older head's curves, and it is read from the project the Cloud has open. Without it the rig keeps its own curve names.")
		);

		return false;
	}

	/* Keyed by the curve rather than by the control it arrives under, since a pose is a curve and
	 * everything reading this wants them that way round */
	for (const TSharedPtr<FJsonValue>& Value : Response->GetArrayField(TEXT("entries"))) {
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

		FString Target;

		if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("target"), Target)) continue;

		const TArray<TSharedPtr<FJsonValue>>* Constants = nullptr;

		if (!Entry->TryGetArrayField(TEXT("constants"), Constants)) continue;

		for (const TSharedPtr<FJsonValue>& One : *Constants) {
			const TSharedPtr<FJsonObject> Constant = One.IsValid() ? One->AsObject() : nullptr;

			FString Name;
			double Weight = 0.0;

			if (!Constant.IsValid() || !Constant->TryGetStringField(TEXT("name"), Name)) continue;
			if (!Constant->TryGetNumberField(TEXT("weight"), Weight)) continue;

			OutByCurve.FindOrAdd(FName(*Name)).Add({ FName(*Target), static_cast<float>(Weight) });
		}
	}

	return OutByCurve.Num() > 0;
}

bool FReflectionLegacyCurves::Rewrite(TArray<TSharedPtr<FJsonValue>>& FloatCurves, const FString& Named) {
	if (FloatCurves.Num() == 0) return false;

	TMap<FName, TArray<FLegacyCurveDrive>> ByCurve;

	if (!Read(ByCurve)) return false;

	/* The mapping the other way up, since what is wanted is what each control the animation drives
	 * has to say about the curves underneath it */
	TMap<FString, TMap<FName, float>> ByControl;

	for (const TPair<FName, TArray<FLegacyCurveDrive>>& One : ByCurve) {
		for (const FLegacyCurveDrive& Drive : One.Value) {
			ByControl.FindOrAdd(Drive.Control.ToString().ToLower()).Add(One.Key, Drive.Weight);
		}
	}

	/* Which of the animation's curves are controls the mapping knows. A cook writes these names in
	 * lower case and the mapping was authored in camel case, so both are folded down to compare. */
	TArray<TSharedPtr<FJsonObject>> Rows;
	TArray<TSharedPtr<FJsonValue>> Kept;

	TSet<FName> Involved;

	/* Whatever the controls were flagged as, since a curve written here stands in for them and the
	 * reader downstream asks every curve for this */
	double Flags = 0.0;

	for (const TSharedPtr<FJsonValue>& One : FloatCurves) {
		const TSharedPtr<FJsonObject> Curve = One.IsValid() ? One->AsObject() : nullptr;

		if (!Curve.IsValid()) continue;

		const TMap<FName, float>* Drives = ByControl.Find(NameOf(Curve).ToLower());

		/* Not a control the mapping covers, which is the flags a sequence carries alongside the
		 * face. Those mean the same thing to either head and are left exactly as they are. */
		if (Drives == nullptr) {
			Kept.Add(One);

			continue;
		}

		Rows.Add(Curve);

		Curve->TryGetNumberField(TEXT("CurveTypeFlags"), Flags);

		for (const TPair<FName, float>& Drive : *Drives) {
			Involved.Add(Drive.Key);
		}
	}

	if (Rows.Num() == 0) return false;

	TArray<FName> Curves = Involved.Array();
	Curves.Sort(FNameLexicalLess());

	/* Every control the animation drives, read as a row of how much of each curve went into it */
	const int32 Height = Rows.Num();
	const int32 Width = Curves.Num();

	TArray<double> A;
	A.SetNumZeroed(Height * Width);

	TArray<TArray<TPair<double, double>>> Keyed;
	Keyed.SetNum(Height);

	TSet<double> Moments;

	for (int32 Row = 0; Row < Height; ++Row) {
		const TMap<FName, float>& Drives = ByControl[NameOf(Rows[Row]).ToLower()];

		for (int32 Column = 0; Column < Width; ++Column) {
			if (const float* Weight = Drives.Find(Curves[Column])) {
				A[Row * Width + Column] = *Weight;
			}
		}

		if (const TSharedPtr<FJsonObject>* Float = nullptr; Rows[Row]->TryGetObjectField(TEXT("FloatCurve"), Float)) {
			if (const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr; (*Float)->TryGetArrayField(TEXT("Keys"), Keys)) {
				for (const TSharedPtr<FJsonValue>& KeyValue : *Keys) {
					const TSharedPtr<FJsonObject> Key = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;

					double Time = 0.0;
					double Value = 0.0;

					if (!Key.IsValid() || !Key->TryGetNumberField(TEXT("Time"), Time)) continue;

					Key->TryGetNumberField(TEXT("Value"), Value);

					Keyed[Row].Add(MakeTuple(Time, Value));
					Moments.Add(Time);
				}
			}
		}

		Keyed[Row].Sort([](const TPair<double, double>& Left, const TPair<double, double>& Right) {
			return Left.Key < Right.Key;
		});
	}

	if (Moments.Num() == 0) return false;

	TArray<double> Times = Moments.Array();
	Times.Sort();

	/* The normal equations, which are the same every frame because the mapping is */
	TArray<double> AtA;
	AtA.SetNumZeroed(Width * Width);

	for (int32 Left = 0; Left < Width; ++Left) {
		for (int32 Right = 0; Right < Width; ++Right) {
			double Sum = 0.0;

			for (int32 Row = 0; Row < Height; ++Row) {
				Sum += A[Row * Width + Left] * A[Row * Width + Right];
			}

			AtA[Left * Width + Right] = Sum;
		}
	}

	/* A step short enough that the gradient never overshoots, which for this is one over the largest
	 * the matrix can stretch anything. Its own size read whole is an upper bound on that and needs
	 * no iterating to find. */
	double Stretch = 0.0;

	for (const double Entry : AtA) {
		Stretch += Entry * Entry;
	}

	Stretch = FMath::Sqrt(Stretch);

	if (Stretch <= 0.0) return false;

	const double Step = 1.0 / Stretch;

	/* Solved forward through the animation, each frame starting where the last one ended, since a
	 * face moves a little at a time and the answer is nearly always close to the one before it */
	TArray<TArray<double>> Solved;
	Solved.SetNum(Times.Num());

	TArray<double> X;
	X.SetNumZeroed(Width);

	TArray<double> Atb;
	Atb.SetNumZeroed(Width);

	for (int32 Frame = 0; Frame < Times.Num(); ++Frame) {
		for (int32 Column = 0; Column < Width; ++Column) {
			double Sum = 0.0;

			for (int32 Row = 0; Row < Height; ++Row) {
				Sum += A[Row * Width + Column] * Sample(Keyed[Row], Times[Frame]);
			}

			Atb[Column] = Sum;
		}

		Solve(AtA, Atb, Step, X);

		Solved[Frame] = X;
	}

	/* Written out as the curves an older head drives, keeping only the ones this animation actually
	 * moved: a curve that stayed at zero the whole way through is one the emote never touched, and
	 * writing it would hold the face down rather than leave it alone */
	int32 Still = 0;

	for (int32 Column = 0; Column < Width; ++Column) {
		double Most = 0.0;

		for (const TArray<double>& Frame : Solved) {
			Most = FMath::Max(Most, Frame[Column]);
		}

		if (FMath::IsNearlyZero(Most)) {
			Still++;

			continue;
		}

		TArray<TSharedPtr<FJsonValue>> Keys;

		for (int32 Frame = 0; Frame < Times.Num(); ++Frame) {
			const TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();

			Key->SetStringField(TEXT("InterpMode"), TEXT("RCIM_Linear"));
			Key->SetStringField(TEXT("TangentMode"), TEXT("RCTM_Auto"));
			Key->SetStringField(TEXT("TangentWeightMode"), TEXT("RCTWM_WeightedNone"));
			Key->SetNumberField(TEXT("Time"), Times[Frame]);
			Key->SetNumberField(TEXT("Value"), Solved[Frame][Column]);
			Key->SetNumberField(TEXT("ArriveTangent"), 0.0);
			Key->SetNumberField(TEXT("ArriveTangentWeight"), 0.0);
			Key->SetNumberField(TEXT("LeaveTangent"), 0.0);
			Key->SetNumberField(TEXT("LeaveTangentWeight"), 0.0);

			Keys.Add(MakeShared<FJsonValueObject>(Key));
		}

		const TSharedPtr<FJsonObject> Float = MakeShared<FJsonObject>();
		Float->SetArrayField(TEXT("Keys"), Keys);

		const TSharedPtr<FJsonObject> Name = MakeShared<FJsonObject>();
		Name->SetStringField(TEXT("DisplayName"), Curves[Column].ToString());

		const TSharedPtr<FJsonObject> Curve = MakeShared<FJsonObject>();
		Curve->SetObjectField(TEXT("Name"), Name);
		Curve->SetStringField(TEXT("CurveName"), Curves[Column].ToString());
		Curve->SetNumberField(TEXT("CurveTypeFlags"), Flags);
		Curve->SetObjectField(TEXT("FloatCurve"), Float);

		Kept.Add(MakeShared<FJsonValueObject>(Curve));
	}

	/* The rig's own controls go, since what is wanted is an animation an older head can play and
	 * leaving both would drive the face twice over */
	FloatCurves = MoveTemp(Kept);

	UE_LOG(LogReflection, Display,
		TEXT("\"%s\" read %d of this rig's control(s) back into %d of an older head's curve(s) over %d frame(s)%s"),
		*Named, Height, Width - Still, Times.Num(),
		Still > 0 ? *FString::Printf(TEXT(", %d the animation never moved"), Still) : TEXT(""));

	return true;
}
