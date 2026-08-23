/* Copyright Reflection Contributors 2024-2026 */

/* The DNA half of a skeletal mesh import.
 *
 * A MetaHuman head carries its face as a DNA: the joints, the neutral pose, and the behavior that
 * moves one from the other. None of it is anything the mesh importer needs to build a mesh, so it
 * lives here, and SkeletalMeshImporter.cpp keeps to geometry, materials and morph targets. */

#include "Importers/Types/Mesh/SkeletalMeshImporter.h"

#if REFLECTION_RIG_LOGIC
#include "DNAAsset.h"

#if REFLECTION_DNA_USER_DATA
#include "DNA.h"
#include "DNAAssetUserData.h"
#endif

#include "DNAUtils.h"
#include "Engine/RigLogicCompatibility.h"
#include "SkelMeshDNAUtils.h"
#include "FMemoryResource.h"
#include "riglogic/RigLogic.h"
#include "dna/BinaryStreamReader.h"
#include "dna/BinaryStreamWriter.h"
#include "trio/streams/MemoryStream.h"
#include "Animation/AnimSequence.h"
#include "Animation/PoseAsset.h"

#if REFLECTION_CURVE_EXPRESSION
#include "CurveExpressionsDataAsset.h"
#include "ExpressionEvaluator.h"
#endif
#include "Animation/AnimData/IAnimationDataController.h"
#endif

#include "Engine/AssetUserData.h"
#include "Engine/EngineUtilities.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Utilities/JsonHelpers.h"

#include "Modules/Cloud/Cloud.h"

bool ISkeletalMeshImporter::ExportNamesDna() {
	const TArray<TSharedPtr<FJsonValue>>* UserData;

	if (!GetAssetData()->TryGetArrayField(TEXT("AssetUserData"), UserData)) {
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Entry : *UserData) {
		const TSharedPtr<FJsonObject> Reference = Entry.IsValid() ? Entry->AsObject() : nullptr;
		if (!Reference.IsValid()) continue;

		FString ObjectName;

		if (Reference->TryGetStringField(TEXT("ObjectName"), ObjectName) && ObjectName.StartsWith(TEXT("DNAAsset"))) {
			return true;
		}
	}

	return false;
}

TArray<uint8> ISkeletalMeshImporter::RewriteDnaForAnimNode(USkeletalMesh* SkeletalMesh, const TArray<uint8>& Dna) {
#if REFLECTION_RIG_LOGIC
	/* A DNA asset is only ever read by the RigLogic anim node, and that node reads a joint's nine
	 * numbers in MetaHuman's axes: translation as (x, -y, z), rotation as a rotator of
	 * (-ry, -rz, rx). This data is not in those axes the pose the mesh is bound at is the same
	 * numbers read as (x, y, z) and (-ry, rz, -rx) so the node rests somewhere the mesh was
	 * never skinned and deforms the face.
	 *
	 * The node can't be changed, so the DNA is. Negating the three attributes the two readings
	 * disagree on cancels exactly: the node's own formula then lands on the pose the mesh is
	 * bound at. Translation Y, rotation X and rotation Z, wherever they appear in the neutral
	 * the rig rests at, and in the deltas every control drives. */
	static constexpr int32 AttributesPerJoint = 9;

	TArray<uint8> Source = Dna;

	auto InStream = rl4::makeScoped<trio::MemoryStream>();
	InStream->write(reinterpret_cast<const char*>(Source.GetData()), Source.Num());
	InStream->seek(0);

	auto Reader = rl4::makeScoped<dna::BinaryStreamReader>(
		InStream.get(), dna::DataLayer::All, dna::UnknownLayerPolicy::Preserve, 0u, FMemoryResource::Instance());

	Reader->read();

	if (!rl4::Status::isOk()) {
		UE_LOG(LogReflection, Warning, TEXT("Reading the DNA to rewrite it failed: %s"), ANSI_TO_TCHAR(rl4::Status::get().message));

		return Dna;
	}

	auto OutStream = rl4::makeScoped<trio::MemoryStream>();
	auto Writer = rl4::makeScoped<dna::BinaryStreamWriter>(OutStream.get(), FMemoryResource::Instance());

	Writer->setFrom(Reader.get(), dna::DataLayer::All, dna::UnknownLayerPolicy::Preserve, FMemoryResource::Instance());

	/* The pose the rig rests at. Not the DNA's own: the mesh's, taken from the package and written
	 * back in the node's axes, so the rig rests exactly where the mesh is bound for every joint it
	 * touches. The DNA and this skeleton disagree about the body arms, spine, neck and the
	 * node writes those joints too, so leaving them as the DNA had them drags the whole head. */
	const uint16 JointCount = Reader->getJointCount();

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();

	TArray<dna::Vector3> Translations;
	TArray<dna::Vector3> Rotations;

	Translations.Reserve(JointCount);
	Rotations.Reserve(JointCount);

	int32 Copied = 0;

	for (uint16 Joint = 0; Joint < JointCount; ++Joint) {
		const dna::StringView JointName = Reader->getJointName(Joint);

		const int32 Bone = RefSkeleton.FindBoneIndex(FName(FString(static_cast<int32>(JointName.size()), JointName.data())));

		if (Bone != INDEX_NONE) {
			const FTransform& Bound = RefSkeleton.GetRefBonePose()[Bone];
			const FRotator Rotation = Bound.GetRotation().Rotator();

			/* Inverted through the node's own reading: it takes translation as (x, -y, z) and
			 * rotation as a rotator of (-ry, -rz, rx), so these are the numbers that come back
			 * out of it as the pose the mesh is bound at */
			Translations.Add(dna::Vector3{
				static_cast<float>(Bound.GetTranslation().X),
				static_cast<float>(-Bound.GetTranslation().Y),
				static_cast<float>(Bound.GetTranslation().Z)
			});

			Rotations.Add(dna::Vector3{
				static_cast<float>(Rotation.Roll),
				static_cast<float>(-Rotation.Pitch),
				static_cast<float>(-Rotation.Yaw)
			});

			Copied++;

			continue;
		}

		/* A joint the skeleton has no bone for keeps what the DNA said, read the same way round */
		dna::Vector3 Translation = Reader->getNeutralJointTranslation(Joint);
		dna::Vector3 Rotation = Reader->getNeutralJointRotation(Joint);

		Translation.y = -Translation.y;

		Rotation.x = -Rotation.x;
		Rotation.z = -Rotation.z;

		Translations.Add(Translation);
		Rotations.Add(Rotation);
	}

	Writer->setNeutralJointTranslations(Translations.GetData(), JointCount);
	Writer->setNeutralJointRotations(Rotations.GetData(), JointCount);

	/* What the controls do on top of it. A joint group is a matrix of one row per attribute it
	 * drives, so a row is negated when the attribute it lands on is one of the three. */
	int32 NegatedRows = 0;

	for (uint16 Group = 0; Group < Reader->getJointGroupCount(); ++Group) {
		const auto Values = Reader->getJointGroupValues(Group);
		const auto Outputs = Reader->getJointGroupOutputIndices(Group);

		if (Values.size() == 0 || Outputs.size() == 0) continue;

		const int32 Columns = static_cast<int32>(Values.size() / Outputs.size());
		if (Columns == 0) continue;

		TArray<float> Patched;
		Patched.Append(Values.data(), static_cast<int32>(Values.size()));

		for (int32 Row = 0; Row < static_cast<int32>(Outputs.size()); ++Row) {
			const int32 Attribute = Outputs[Row] % AttributesPerJoint;

			if (Attribute != 1 && Attribute != 3 && Attribute != 5) continue;

			for (int32 Column = 0; Column < Columns; ++Column) {
				const int32 Index = Row * Columns + Column;

				if (Patched.IsValidIndex(Index)) {
					Patched[Index] = -Patched[Index];
				}
			}

			NegatedRows++;
		}

		Writer->setJointGroupValues(Group, Patched.GetData(), static_cast<uint32>(Patched.Num()));
	}

	Writer->write();

	if (!rl4::Status::isOk()) {
		UE_LOG(LogReflection, Warning, TEXT("Writing the rewritten DNA failed: %s"), ANSI_TO_TCHAR(rl4::Status::get().message));

		return Dna;
	}

	TArray<uint8> Rewritten;
	Rewritten.AddUninitialized(static_cast<int32>(OutStream->size()));

	OutStream->seek(0);
	OutStream->read(reinterpret_cast<char*>(Rewritten.GetData()), OutStream->size());

	UE_LOG(LogReflection, Display, TEXT("\"%s\" rewrote its DNA into the anim node's axes: %d joint(s), %d from the mesh's own bind pose, %d delta row(s)"),
		*GetAssetName(), JointCount, Copied, NegatedRows);

	return Rewritten;
#else
	return Dna;
#endif
}

bool ISkeletalMeshImporter::ApplyDna(USkeletalMesh* SkeletalMesh, const FString& FetchPath) {
#if REFLECTION_RIG_LOGIC
	TArray<uint8> Cooked = Cloud::Export::GetDnaBlocking(FetchPath);

	if (Cooked.Num() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The mesh's DNA did not come back"),
			TEXT("The Cloud returned nothing for this head's DNA, so there is no face rig to apply.")
		);

		return false;
	}

	/* Which DNA arrived is the first thing worth knowing when a face ends up holding still: a head
	 * cooked by a newer RigLogic only carries a rig if the Cloud rebuilt one, and that is a
	 * different size to the one the package ships. */
	UE_LOG(LogReflection, Display, TEXT("\"%s\" fetched %d byte(s) of DNA"), *GetAssetName(), Cooked.Num());

	/* A DNA kept in a package of its own has a few bytes of that package's own ahead of the stream,
	 * where one hung straight off the mesh starts at it. The reader wants the stream. */
	{
		int32 Start = INDEX_NONE;

		for (int32 Index = 0; Index + 2 < Cooked.Num() && Index < 64; ++Index) {
			if (Cooked[Index] == 'D' && Cooked[Index + 1] == 'N' && Cooked[Index + 2] == 'A') {
				Start = Index;

				break;
			}
		}

		if (Start == INDEX_NONE) {
			FImportIssues::Report(
				EImportIssue::Data,
				TEXT("The mesh's DNA isn't a DNA"),
				TEXT("What came back doesn't start with a DNA stream, so there is nothing RigLogic can read a face out of.")
			);

			return false;
		}

		if (Start > 0) {
			Cooked.RemoveAt(0, Start, EAllowShrinking::No);

			UE_LOG(LogReflection, Display, TEXT("\"%s\" trimmed %d byte(s) ahead of its DNA stream"), *GetAssetName(), Start);
		}
	}

	/* Rewritten into the axes the anim node reads, since that node is the only thing that ever
	 * reads a DNA asset */
	TArray<uint8> Dna = RewriteDnaForAnimNode(SkeletalMesh, Cooked);

	/* Behavior is what RigLogic runs a face with, geometry is what the editor updates the mesh
	 * from. A cook keeps the first and leaves an empty stream where the second was, so the mesh
	 * animates and the design time half is simply not there to rebuild. */
	const TSharedPtr<IDNAReader> Behavior = ReadDNAFromBuffer(&Dna, EDNADataLayer::Behavior | EDNADataLayer::MachineLearnedBehavior, 0u);

	/* The only silent way out of here, and the one that leaves a mesh with no DNA on it at all */
	if (!Behavior.IsValid()) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("RigLogic would not read the mesh's DNA"),
			FString::Printf(
				TEXT("'%s' came back as %d byte(s) that this engine's RigLogic rejected: %s. Nothing is put on the mesh, so it has no DNA at all."),
				*GetAssetName(),
				Dna.Num(),
				ANSI_TO_TCHAR(rl4::Status::get().message)
			)
		);

		return false;
	}

	UE_LOG(LogReflection, Display,
		TEXT("\"%s\" read a DNA with %d joint group(s) and %d joint(s)"),
		*GetAssetName(), Behavior->GetJointGroupCount(), Behavior->GetJointCount());

#if REFLECTION_DNA_USER_DATA
	/* Where the engine has one, the DNA belongs in a UDNA named by a UDNAAssetUserData. The anim
	 * node takes that user data over the old UDNAAsset and does not fall back when it finds one:
	 *
	 *     if (UDNAAssetUserData* UserData = SkeletalMesh->GetAssetUserData<UDNAAssetUserData>()) {
	 *         if (UDNA* DNA = UserData->DNAAsset) { ... }
	 *     } else if (UDNAAsset* Legacy = ...) { ... }
	 *
	 * Deserializing the mesh's properties makes that user data with nothing in it, since the DNA it
	 * names lives in a package of its own. Left that way it shadows anything written the old way,
	 * and the face holds still. */
	UDNAAssetUserData* DnaUserData = SkeletalMesh->GetAssetUserData<UDNAAssetUserData>();

	if (DnaUserData == nullptr) {
		DnaUserData = NewObject<UDNAAssetUserData>(SkeletalMesh, TEXT("DNAAssetUserData"), RF_Public);

		SkeletalMesh->AddAssetUserData(DnaUserData);
	}

	if (DnaUserData->DNAAsset == nullptr) {
		DnaUserData->DNAAsset = NewObject<UDNA>(SkeletalMesh, TEXT("DNA"), RF_Public);
	}

	DnaUserData->DNAAsset->SetDNAReader(Behavior);
#else
	UDNAAsset* DNAAsset = nullptr;

	/* The mesh's AssetUserData names the DNA asset, so deserializing the properties has usually
	 * made an empty one already. Filling that one keeps the mesh pointing at a single DNA. */
	if (const TArray<UAssetUserData*>* UserData = SkeletalMesh->GetAssetUserDataArray()) {
		for (UAssetUserData* Entry : *UserData) {
			if (UDNAAsset* Existing = Cast<UDNAAsset>(Entry)) {
				DNAAsset = Existing;

				break;
			}
		}
	}

	if (DNAAsset == nullptr) {
		DNAAsset = NewObject<UDNAAsset>(SkeletalMesh, TEXT("DNAAsset"), RF_Public);

		SkeletalMesh->AddAssetUserData(DNAAsset);
	}

	DNAAsset->DnaFileName = SkeletalMesh->GetName() + TEXT(".dna");
	DNAAsset->SetBehaviorReader(Behavior);

	/* The cook names the mesh's DNA user data by a class this engine does not have: newer RigLogic
	 * hangs a UDNAAssetUserData off the mesh pointing at a UDNA, where this one keeps a UDNAAsset
	 * directly. Whatever the properties made of that reference is still sitting in this array next
	 * to the DNA that did attach, and an entry the details panel cannot draw is what reads as an
	 * asset user data slot it does not recognise. Named here rather than guessed at. */
	{
		const UDNAAsset* Found = SkeletalMesh->GetAssetUserData<UDNAAsset>();
		const TArray<UAssetUserData*>* All = SkeletalMesh->GetAssetUserDataArray();

		UE_LOG(LogReflection, Display,
			TEXT("\"%s\" attached DNA as \"MetaHuman DNA Data\": lookup %s, %d entr(ies) of asset user data"),
			*GetAssetName(),
			Found != nullptr ? TEXT("found it") : TEXT("FOUND NOTHING"),
			All != nullptr ? All->Num() : -1);

		if (All != nullptr) {
			for (int32 Index = 0; Index < All->Num(); ++Index) {
				const UAssetUserData* Entry = (*All)[Index];

				UE_LOG(LogReflection, Display, TEXT("    user data %d: %s"),
					Index,
					Entry != nullptr ? *Entry->GetClass()->GetName() : TEXT("<empty slot>"));
			}
		}
	}
#endif

	/* Some heads keep the DNA in a package of its own and only the definition with it: the names,
	 * the hierarchy and the pose, and a behavior layer that is a stub. There is a DNA on the mesh
	 * either way, and nothing in it for a rig to run. */
	if (Behavior->GetJointGroupCount() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The mesh's DNA has no rig in it"),
			FString::Printf(
				TEXT("'%s' carries the face's joints and neutral pose but no behavior, so RigLogic has nothing to drive them with. The head imports and holds still."),
				*SkeletalMesh->GetName()
			)
		);
	}

#if !REFLECTION_DNA_USER_DATA
	if (const TSharedPtr<IDNAReader> Geometry = ReadDNAFromBuffer(&Dna, EDNADataLayer::Geometry, 0u)) {
		DNAAsset->SetGeometryReader(Geometry);
	}
#endif

	return true;
#else
	return false;
#endif
}

#if REFLECTION_RIG_LOGIC
/* What RigLogic hands back per joint, which is not the same on every engine.
 *
 * Up to 5.6 it is nine floats, translation, euler rotation and scale, three each and the anim
 * node is what puts them in UE axes as it writes the pose. From 5.7 rotation comes back as a
 * quaternion instead, which makes it ten, moves scale along by one, and leaves the node nothing to
 * flip because RigLogicDNAReader already did it on the way in.
 *
 * Read off the data rather than off a version macro: the neutral pose is one entry per attribute
 * per joint, so its length over the joint count is the stride, whatever engine produced it. */
static constexpr int32 GDnaJointAttributesEuler = 9;
static constexpr int32 GDnaJointAttributesQuaternion = 10;

static int32 DnaJointAttributeCount(const TArrayView<const float>& Neutral, const int32 JointCount) {
	if (JointCount <= 0) {
		return GDnaJointAttributesEuler;
	}

	return (Neutral.Num() / JointCount) >= GDnaJointAttributesQuaternion
		? GDnaJointAttributesQuaternion
		: GDnaJointAttributesEuler;
}

namespace {
	/* A joint's local transform: the DNA's neutral, then whatever the controls evaluated to on top.
	 *
	 * These are the signs FAnimNode_RigLogic uses, and they have to be: by the time anything reads
	 * a joint here the DNA has already been rewritten into that node's axes, so reading it any
	 * other way inverts every rotation and every sideways offset. */
	FTransform ComposeDnaJoint(const TArrayView<const float>& Neutral, const TArrayView<const float>& Delta, const int32 Attribute, const int32 Stride) {
		const auto Read = [](const TArrayView<const float>& Values, const int32 Index) {
			return Values.IsValidIndex(Index) ? Values[Index] : 0.0f;
		};

		/* 5.7 and up: rotation is a quaternion at 3..6 with scale behind it, and everything is
		 * already in the node's axes, so nothing is negated here */
		if (Stride >= GDnaJointAttributesQuaternion) {
			const FVector QTranslation(
				Read(Neutral, Attribute + 0) + Read(Delta, Attribute + 0),
				Read(Neutral, Attribute + 1) + Read(Delta, Attribute + 1),
				Read(Neutral, Attribute + 2) + Read(Delta, Attribute + 2)
			);

			/* A rotation that isn't there reads as four zeroes, and a zero quaternion is not a
			 * rotation at all: composed as one it multiplies the neutral away rather than leaving
			 * it where it is. The base pose is written with no deltas at all, so that frame is
			 * exactly where it shows. Euler never had the problem, three zero angles are the
			 * identity which is why it only appears once rotation comes back as a quaternion. */
			const auto ReadRotation = [&Read](const TArrayView<const float>& Values, const int32 At) {
				const FQuat Quaternion(Read(Values, At + 3), Read(Values, At + 4), Read(Values, At + 5), Read(Values, At + 6));

				return Quaternion.SizeSquared() > SMALL_NUMBER ? Quaternion.GetNormalized() : FQuat::Identity;
			};

			const FQuat QRotation = ReadRotation(Neutral, Attribute) * ReadRotation(Delta, Attribute);

			const FVector QScale(
				Read(Neutral, Attribute + 7) + Read(Delta, Attribute + 7),
				Read(Neutral, Attribute + 8) + Read(Delta, Attribute + 8),
				Read(Neutral, Attribute + 9) + Read(Delta, Attribute + 9)
			);


			return FTransform(QRotation, QTranslation, QScale);
		}

		const FVector Translation(
			Read(Neutral, Attribute + 0) + Read(Delta, Attribute + 0),
			-(Read(Neutral, Attribute + 1) + Read(Delta, Attribute + 1)),
			Read(Neutral, Attribute + 2) + Read(Delta, Attribute + 2)
		);

		const FQuat Rotation =
			FQuat(FRotator(-Read(Neutral, Attribute + 4), -Read(Neutral, Attribute + 5), Read(Neutral, Attribute + 3))) *
			FQuat(FRotator(-Read(Delta, Attribute + 4), -Read(Delta, Attribute + 5), Read(Delta, Attribute + 3)));

		const FVector Scale(
			Read(Neutral, Attribute + 6) + Read(Delta, Attribute + 6),
			Read(Neutral, Attribute + 7) + Read(Delta, Attribute + 7),
			Read(Neutral, Attribute + 8) + Read(Delta, Attribute + 8)
		);

		return FTransform(Rotation, Translation, Scale);
	}
}
#endif

bool ISkeletalMeshImporter::AlignBindPoseToDna(USkeletalMesh* SkeletalMesh) {
#if REFLECTION_RIG_LOGIC
	UDNAAsset* DNAAsset = USkelMeshDNAUtils::GetMeshDNA(SkeletalMesh);
	if (DNAAsset == nullptr) return false;

	const TSharedPtr<IDNAReader> Behavior = DNAAsset->GetBehaviorReader();
	if (!Behavior.IsValid()) return false;

	USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	if (Skeleton == nullptr) return false;

	/* RigLogic doesn't add to the pose a bone is already in, it puts the bone where the DNA's
	 * neutral says. A mesh bound anywhere else wears that difference as a deformation for as long
	 * as the rig runs, which is what a head bound to the shared skeleton rather than its own does.
	 *
	 * The engine has USkelMeshDNAUtils::UpdateJoints for this, and it can't be used here: it
	 * rebuilds the whole chain in component space off the DNA's own root, which is written in the
	 * DNA's axes, and this DNA is not in the ones that code was written for. It lands the skeleton
	 * on its side. Only the joints are taken, in the frame the skeleton already has them. */
	/* The same numbers the rig evaluates against, rather than the reader's own accessors: those
	 * two do not report a rotation the same way round, and this is the pair that has to agree. */
	FRigLogic RigLogic = MakeDnaRigLogic(Behavior);

	const TArrayView<const float> Neutral = GetDnaNeutralJoints(RigLogic);
	const int32 Stride = DnaJointAttributeCount(Neutral, Behavior->GetJointCount());

	int32 Aligned = 0;

	{
	FReferenceSkeletonModifier Modifier(SkeletalMesh->GetRefSkeleton(), Skeleton);

	for (uint16 Joint = 0; Joint < Behavior->GetJointCount(); ++Joint) {
		/* The DNA roots its hierarchy at a joint that is its own parent, and that one holds where
		 * the rig sits rather than how a bone is offset from its parent. Read as an offset it
		 * throws the skeleton clean out of its own mesh, so the game's placement is kept. */
		if (Behavior->GetJointParentIndex(Joint) == Joint) continue;

		const int32 Bone = SkeletalMesh->GetRefSkeleton().FindBoneIndex(FName(*Behavior->GetJointName(Joint)));
		if (Bone == INDEX_NONE) continue;

		/* Where the rig rests, which is where the mesh has to be bound */
		Modifier.UpdateRefPoseTransform(Bone, ComposeDnaJoint(Neutral, {}, Joint * Stride, Stride));

		Aligned++;
	}

	}

	SkeletalMesh->GetRefBasesInvMatrix().Reset();

	UE_LOG(LogReflection, Display, TEXT("\"%s\" bound %d joint(s) to its DNA's neutral"), *GetAssetName(), Aligned);

	return Aligned > 0;
#else
	return false;
#endif
}

/* One pose to bake, and what the rig has to be driven with to arrive at it.
 *
 * Named at file scope rather than tucked into an anonymous namespace: the header declares the
 * function that fills these in, and a type with internal linkage cannot be the one it means. */
struct FDnaPosePlan {
	FName Name;
	TArray<TPair<uint16, float>> Drive;
};

#if REFLECTION_RIG_LOGIC

namespace {
	/* The rig's controls by the name a curve mapping would call them. A DNA writes a control as
	 * group and name with a dot between, and a mapping writes the same control with an underscore,
	 * so they only meet once one is spelled the other's way. */
	TMap<FString, uint16> MapControlsByName(const TSharedPtr<IDNAReader>& Behavior) {
		const int32 ControlCount = Behavior->GetRawControlCount();

		TMap<FString, uint16> ByName;
		ByName.Reserve(ControlCount);

		for (int32 Control = 0; Control < ControlCount; ++Control) {
			ByName.Add(
				Behavior->GetRawControlName(static_cast<uint16>(Control)).Replace(TEXT("."), TEXT("_")),
				static_cast<uint16>(Control));
		}

		return ByName;
	}
}

/* The older head's poses, read out of the mapping that says what each of its curves is made of.
 *
 * The Cloud hands back a weight per control per curve, worked out by running the compiled
 * expression rather than reading the text it was written as, so a curve driven by three controls
 * in different amounts arrives as those three amounts. Driving the rig with exactly those is what
 * makes the pose: the mapping's own statement of what that curve means, evaluated by the rig it
 * was written against.
 *
 * Empty when the mapping cannot be had or none of its controls are ones this DNA has, which leaves
 * the caller to bake the rig's own controls instead. */
bool ISkeletalMeshImporter::BuildBackportedPosePlan(const TSharedPtr<IDNAReader>& Behavior, TArray<FDnaPosePlan>& OutPlan) {
	const FString MappingPath = GetSettings()->AssetSettings.DNA.Backport.CurveMapping;

	if (MappingPath.IsEmpty()) return false;

#if REFLECTION_CURVE_EXPRESSION
	/* The mapping is read as the asset rather than as text, and run by the engine's own evaluator,
	 * because that is the thing that decides what these expressions mean. Reading coefficients off
	 * them instead only works while they stay a weighted sum, and they do not: half of them clamp. */
	UCurveExpressionsDataAsset* Mapping = LoadObject<UCurveExpressionsDataAsset>(nullptr, *MappingPath);

	const TSharedPtr<const FExpressionData> Data = Mapping != nullptr ? Mapping->GetCompiledExpressionData() : nullptr;

	if (!Data.IsValid() || Data->ExpressionMap.IsEmpty()) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The backport mapping isn't in the project"),
			FString::Printf(
				TEXT("'%s' is what says how the older head's curves drive this rig, and nothing is there to read. Import it first, or the rig's own controls are baked instead."),
				*MappingPath)
		);

		return false;
	}

	/* A pose per curve of the older head, which is what the mapping reads rather than what it
	 * writes: the expressions are named for this rig's controls and driven by the older head's
	 * curves, so the curves are the constants in them.
	 *
	 * Taken off the compiled data rather than asked of each expression, because the call that would
	 * ask is not one the plugin exports and nothing outside its own module can link it. */
	const TMap<FString, uint16> ByName = MapControlsByName(Behavior);

	TArray<FName> Ordered = Data->NamedConstants;
	Ordered.Sort(FNameLexicalLess());

	int32 Unresolved = 0;

	for (const FName& Source : Ordered) {
		FDnaPosePlan Pose;
		Pose.Name = Source;

		/* That one curve the whole way up and nothing else, which is what the pose means */
		const auto DriveOne = [&Source](const FName Constant) -> TOptional<float> {
			return Constant == Source ? TOptional<float>(1.0f) : TOptional<float>(0.0f);
		};

		for (const TTuple<FName, CurveExpression::Evaluator::FExpressionObject>& Assignment : Data->ExpressionMap) {
			const float Value = CurveExpression::Evaluator::FEngine().Execute(Assignment.Value, DriveOne);

			if (FMath::IsNearlyZero(Value)) continue;

			const uint16* Control = ByName.Find(Assignment.Key.ToString());

			/* A mapping covers a whole family of heads, so a curve naming a control this DNA has
			 * not got is the mapping being broader than this face rather than a fault */
			if (Control == nullptr) {
				Unresolved++;

				continue;
			}

			Pose.Drive.Add({ *Control, Value });
		}

		/* A curve none of whose controls this rig has is a pose that would come out as the neutral,
		 * which is worse than not having it: it would overwrite whatever plays underneath */
		if (Pose.Drive.Num() == 0) continue;

		OutPlan.Add(MoveTemp(Pose));
	}

	if (OutPlan.Num() == 0) return false;

	UE_LOG(LogReflection, Display,
		TEXT("\"%s\" backporting %d pose(s) from \"%s\"%s"),
		*GetAssetName(), OutPlan.Num(), *FPaths::GetBaseFilename(MappingPath),
		Unresolved > 0 ? *FString::Printf(TEXT(", %d control reference(s) this DNA hasn't got"), Unresolved) : TEXT(""));

	return true;
#else
	return false;
#endif
}
#else

bool ISkeletalMeshImporter::BuildBackportedPosePlan(const TSharedPtr<IDNAReader>&, TArray<FDnaPosePlan>&) {
	return false;
}

#endif

UPoseAsset* ISkeletalMeshImporter::BakeDnaPoseAsset(USkeletalMesh* SkeletalMesh) {
#if REFLECTION_RIG_LOGIC
	UDNAAsset* DNAAsset = USkelMeshDNAUtils::GetMeshDNA(SkeletalMesh);
	if (DNAAsset == nullptr) return nullptr;

	const TSharedPtr<IDNAReader> Behavior = DNAAsset->GetBehaviorReader();
	if (!Behavior.IsValid()) return nullptr;

	USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	if (Skeleton == nullptr) return nullptr;

	const int32 ControlCount = Behavior->GetRawControlCount();
	if (ControlCount == 0) return nullptr;

	/* Controls with no joint groups behind them evaluate to nothing, and asking the rig to run
	 * anyway walks off the end of tables it never built */
	if (Behavior->GetJointGroupCount() == 0 || Behavior->GetLODCount() == 0) {
		return nullptr;
	}

	FRigLogic RigLogic = MakeDnaRigLogic(Behavior);
	FRigInstance Instance(&RigLogic);

	const TArrayView<const float> Neutral = GetDnaNeutralJoints(RigLogic);
	const int32 Stride = DnaJointAttributeCount(Neutral, Behavior->GetJointCount());

	UE_LOG(LogReflection, Display,
		TEXT("\"%s\" rig reports %d attribute(s) per joint (%s rotation)"),
		*GetAssetName(), Stride,
		Stride >= GDnaJointAttributesQuaternion ? TEXT("quaternion") : TEXT("euler"));

	/* A DNA names the joints it drives, and the mesh knows them as bones. Only those get a track:
	 * everything else stays wherever the pose it is played over left it. */
	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();

	TArray<FName> BoneNames;
	TArray<int32> BoneJoints;

	for (uint16 Joint = 0; Joint < Behavior->GetJointCount(); ++Joint) {
		/* Skipped for the same reason the bind pose skips it: the DNA's root holds a placement,
		 * and a pose that wrote it as a bone offset would take the head with it */
		if (Behavior->GetJointParentIndex(Joint) == Joint) continue;

		const FName BoneName(*Behavior->GetJointName(Joint));

		const int32 Bone = RefSkeleton.FindBoneIndex(BoneName);
		if (Bone == INDEX_NONE) continue;

		BoneNames.Add(BoneName);
		BoneJoints.Add(Joint);
	}

	if (BoneNames.Num() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The DNA's joints aren't the mesh's bones"),
			TEXT("None of the joints the DNA drives are named in the skeleton, so a pose built out of it would move nothing.")
		);

		return nullptr;
	}

	/* One frame per control, and a first frame with the rig standing still for the rest of them to
	 * be measured against */
	/* What to bake: the older head's poses where the mapping could be had and that was asked for,
	 * otherwise one pose per control the rig names */
	TArray<FDnaPosePlan> Plan;

	const bool bBackported =
		GetSettings()->AssetSettings.DNA.Backport.BackportPoses &&
		BuildBackportedPosePlan(Behavior, Plan);

	if (!bBackported) {
		Plan.Reserve(ControlCount);

		for (int32 Control = 0; Control < ControlCount; ++Control) {
			/* A control is named with a dot between its group and itself, which reads as a path
			 * everywhere a curve name is typed */
			Plan.Add({
				FName(*Behavior->GetRawControlName(static_cast<uint16>(Control)).Replace(TEXT("."), TEXT("_"))),
				{ { static_cast<uint16>(Control), 1.0f } }
			});
		}
	}

	const int32 FrameCount = Plan.Num() + 1;

	TArray<FName> PoseNames;
	PoseNames.Reserve(FrameCount);

	TArray<TArray<FVector>> PositionKeys;
	TArray<TArray<FQuat>> RotationKeys;
	TArray<TArray<FVector>> ScaleKeys;

	PositionKeys.SetNum(BoneNames.Num());
	RotationKeys.SetNum(BoneNames.Num());
	ScaleKeys.SetNum(BoneNames.Num());

	/* The pose the rig itself would write for that control, so a pose and the rig agree bone for
	 * bone. The mesh is bound to the same neutral these are composed from. */
	const auto WriteFrame = [&](const TArrayView<const float>& Delta) {
		for (int32 Bone = 0; Bone < BoneNames.Num(); ++Bone) {
			const FTransform Local = ComposeDnaJoint(Neutral, Delta, BoneJoints[Bone] * Stride, Stride);

			PositionKeys[Bone].Add(Local.GetTranslation());
			RotationKeys[Bone].Add(Local.GetRotation());
			ScaleKeys[Bone].Add(Local.GetScale3D());
		}
	};

	WriteFrame({});
	PoseNames.Add(TEXT("base_pose"));

	for (const FDnaPosePlan& Pose : Plan) {
		for (int32 Reset = 0; Reset < ControlCount; ++Reset) {
			Instance.SetRawControl(static_cast<uint16>(Reset), 0.0f);
		}

		for (const TPair<uint16, float>& Drive : Pose.Drive) {
			Instance.SetRawControl(Drive.Key, Drive.Value);
		}

		RigLogic.Calculate(&Instance);

		WriteFrame(GetDnaJointOutputs(Instance));
		PoseNames.Add(Pose.Name);
	}

	/* A pose asset is built out of an animation, one pose per frame, and keeps pointing at it: the
	 * poses can be rebuilt from the animation, and the animation is the only place the frames stay
	 * readable once the poses are additive. Both land beside the mesh. */
	const FString Folder = FPackageName::GetLongPackagePath(GetPackage()->GetName());

	const FString SequenceName = GetAssetName() + (bBackported
		? TEXT("_DNA_Facial_Pose_Backport")
		: TEXT("_DNA_Facial_Pose_Export"));
	const FString PoseAssetName = SequenceName + TEXT("_PoseAsset");

	UPackage* SequencePackage = CreatePackage(*(Folder / SequenceName));
	if (SequencePackage == nullptr) return nullptr;

	SequencePackage->FullyLoad();

	UAnimSequence* Sequence = NewObject<UAnimSequence>(SequencePackage, FName(*SequenceName), RF_Public | RF_Standalone);
	Sequence->SetSkeleton(Skeleton);
	Sequence->SetPreviewMesh(SkeletalMesh);

	{
		IAnimationDataController& Controller = Sequence->GetController();

		Controller.OpenBracket(NSLOCTEXT("Reflection", "BuildDnaPoses", "Building poses from DNA"), false);
		Controller.InitializeModel();

		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1, FrameCount - 1)), false);

		for (int32 Bone = 0; Bone < BoneNames.Num(); ++Bone) {
			Controller.AddBoneCurve(BoneNames[Bone], false);
			Controller.SetBoneTrackKeys(BoneNames[Bone], PositionKeys[Bone], RotationKeys[Bone], ScaleKeys[Bone], false);
		}

		Controller.NotifyPopulated();
		Controller.CloseBracket(false);
	}

	UPackage* PosePackage = CreatePackage(*(Folder / PoseAssetName));
	if (PosePackage == nullptr) return nullptr;

	/* Re-importing lands on the package the last import wrote, and a package read back off disk
	 * comes in on demand: saving one that was never finished reading is fatal */
	PosePackage->FullyLoad();

	UPoseAsset* PoseAsset = NewObject<UPoseAsset>(PosePackage, FName(*PoseAssetName), RF_Public | RF_Standalone);

	PoseAsset->SetSkeleton(Skeleton);
	PoseAsset->SetPreviewMesh(SkeletalMesh);
	PoseAsset->CreatePoseFromAnimation(Sequence, &PoseNames);

	/* Additive against the base pose, so the controls a face is driven by add up the way they do in
	 * the rig rather than each replacing the last */
	PoseAsset->ConvertSpace(true, 0);

	PoseAsset->SourceAnimation = Sequence;

	HandleAssetCreation(Sequence, SequencePackage);
	HandleAssetCreation(PoseAsset, PosePackage);

	if (GetSettings()->AssetSettings.SaveAssets) {
		SavePackage(SequencePackage);
		SavePackage(PosePackage);
	}

	return PoseAsset;
#else
	return nullptr;
#endif
}
