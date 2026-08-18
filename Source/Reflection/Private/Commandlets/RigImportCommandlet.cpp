/* Copyright Reflection Contributors 2024-2026 */

#include "RigImportCommandlet.h"

#if REFLECTION_CONTROL_RIG

#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Settings/SettingsAccess.h"

#include "ControlRigBlueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Animation/MorphTarget.h"
#include "Animation/PoseAsset.h"
#include "Engine/AssetUserData.h"

#if REFLECTION_RIG_LOGIC
#include "AnimGraphNode_RigLogic.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_PoseBlendNode.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "DNAAsset.h"
#include "DNAReader.h"
#include "RigLogic.h"
#include "SkelMeshDNAUtils.h"
#endif
#if UE5_1_BEYOND
#include "Engine/SkinnedAssetCommon.h"
#endif
#include "Rigs/RigHierarchy.h"
#include "RigVMHost.h"
#include "RigVMCore/RigVM.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMPin.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"

DEFINE_LOG_CATEGORY_STATIC(LogRigImportTest, Log, All);

/* What a morph actually moves, per LOD, so a rebuild that drops the deltas is visible */
static void ReportMorphTargets(const USkeletalMesh* Mesh, const TCHAR* Prefix) {
	for (const TObjectPtr<UMorphTarget>& MorphTarget : Mesh->GetMorphTargets()) {
		if (MorphTarget == nullptr) continue;

		FString Counts;
		float Largest = 0.0f;

		const TArray<FMorphTargetLODModel>& MorphLods = MorphTarget->GetMorphLODModels();

		for (int32 Index = 0; Index < MorphLods.Num(); ++Index) {
			Counts += FString::Printf(TEXT("%s%d"), Index > 0 ? TEXT("/") : TEXT(""), MorphLods[Index].Vertices.Num());

			for (const FMorphTargetDelta& Delta : MorphLods[Index].Vertices) {
				Largest = FMath::Max(Largest, Delta.PositionDelta.Size());
			}
		}

		UE_LOG(LogRigImportTest, Display, TEXT("%s morph '%s': %s deltas, largest %f"),
			Prefix, *MorphTarget->GetName(), *Counts, Largest);
	}
}

/* What the mesh's DNA holds, when it has one */
static void ReportDna(USkeletalMesh* Mesh, const TCHAR* Prefix) {
#if REFLECTION_RIG_LOGIC
	const TArray<UAssetUserData*>* UserData = Mesh->GetAssetUserDataArray();
	if (UserData == nullptr) return;

	for (UAssetUserData* Entry : *UserData) {
		UDNAAsset* DNAAsset = Cast<UDNAAsset>(Entry);
		if (DNAAsset == nullptr) continue;

		const TSharedPtr<IDNAReader> Behavior = DNAAsset->GetBehaviorReader();

		if (!Behavior.IsValid()) {
			UE_LOG(LogRigImportTest, Display, TEXT("%s dna '%s': no behavior"), Prefix, *DNAAsset->DnaFileName);

			continue;
		}

		UE_LOG(LogRigImportTest, Display, TEXT("%s dna '%s': name %s, db %s, %d lods, %d joints, %d blend shapes, %d raw controls, %d meshes"),
			Prefix, *DNAAsset->DnaFileName, *Behavior->GetName(), *Behavior->GetDBName(),
			Behavior->GetLODCount(), Behavior->GetJointCount(), Behavior->GetBlendShapeChannelCount(),
			Behavior->GetRawControlCount(), Behavior->GetMeshCount());

		const FCoordinateSystem Coordinates = Behavior->GetCoordinateSystem();

		UE_LOG(LogRigImportTest, Display, TEXT("%s dna header: generation %d, version %d"),
			Prefix, Behavior->GetFileFormatGeneration(), Behavior->GetFileFormatVersion());

		UE_LOG(LogRigImportTest, Display, TEXT("%s dna descriptor: archetype %d, gender %d, age %d, translation %d, rotation %d, axes %d/%d/%d, max lod %d, complexity %s"),
			Prefix, static_cast<int32>(Behavior->GetArchetype()), static_cast<int32>(Behavior->GetGender()), Behavior->GetAge(),
			static_cast<int32>(Behavior->GetTranslationUnit()), static_cast<int32>(Behavior->GetRotationUnit()),
			Coordinates.XAxis, Coordinates.YAxis, Coordinates.ZAxis,
			Behavior->GetDBMaxLOD(), *Behavior->GetDBComplexity());

		for (uint32 Index = 0; Index < Behavior->GetMetaDataCount(); ++Index) {
			const FString Key = Behavior->GetMetaDataKey(Index);

			UE_LOG(LogRigImportTest, Display, TEXT("%s dna metadata: '%s' = '%s'"), Prefix, *Key, *Behavior->GetMetaDataValue(Key));
		}
	}
#endif
}

/* The pose asset a baked DNA leaves beside the mesh */
static void ReportPoseAsset(const FString& MeshPath, const TCHAR* Prefix, bool bSave) {
	const FString PosePath = MeshPath + TEXT("_DNA_PoseAsset.") + FPackageName::GetShortName(MeshPath) + TEXT("_DNA_PoseAsset");

	UPoseAsset* PoseAsset = LoadObject<UPoseAsset>(nullptr, *PosePath);

	if (PoseAsset == nullptr) {
		UE_LOG(LogRigImportTest, Display, TEXT("%s pose asset: none at %s"), Prefix, *PosePath);

		return;
	}

	UE_LOG(LogRigImportTest, Display, TEXT("%s pose asset '%s': %d poses, %d tracks, additive %d, skeleton %s"),
		Prefix, *PoseAsset->GetName(), PoseAsset->GetNumPoses(), PoseAsset->GetTrackNames().Num(),
		PoseAsset->IsValidAdditive() ? 1 : 0,
		PoseAsset->GetSkeleton() ? *PoseAsset->GetSkeleton()->GetName() : TEXT("<none>"));

	const TArray<FName>& PoseFNames = PoseAsset->GetPoseFNames();

	for (int32 Index = 0; Index < FMath::Min(4, PoseFNames.Num()); ++Index) {
		UE_LOG(LogRigImportTest, Display, TEXT("%s   pose %d: %s"), Prefix, Index, *PoseFNames[Index].ToString());
	}

	if (bSave) {
		UPackage* Package = PoseAsset->GetPackage();

		Package->FullyLoad();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		UE_LOG(LogRigImportTest, Display, TEXT("%s saved %s: %d"), Prefix, *FileName, UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs) ? 1 : 0);
	}
}

/* Whether the pose the DNA calls neutral is the pose the skeleton was built at. Everything a rig
 * evaluates is expressed against the DNA's neutral, so if the two disagree, every transform taken
 * out of the rig lands in the wrong frame. */
static void ReportDnaNeutral(USkeletalMesh* Mesh) {
#if REFLECTION_RIG_LOGIC
	UDNAAsset* DNAAsset = USkelMeshDNAUtils::GetMeshDNA(Mesh);
	if (DNAAsset == nullptr) return;

	const TSharedPtr<IDNAReader> Behavior = DNAAsset->GetBehaviorReader();
	if (!Behavior.IsValid()) return;

	FRigLogic RigLogic(Behavior.Get());

	const TArrayView<const float> Neutral = RigLogic.GetRawNeutralJointValues();
	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();

	int32 Compared = 0;
	int32 Mismatched = 0;

	float WorstAngle = 0.0f;
	float WorstDistance = 0.0f;
	FString WorstBone;

	for (uint16 Joint = 0; Joint < Behavior->GetJointCount(); ++Joint) {
		const FName BoneName(*Behavior->GetJointName(Joint));

		const int32 Bone = RefSkeleton.FindBoneIndex(BoneName);
		if (Bone == INDEX_NONE) continue;

		const int32 Attribute = Joint * 9;

		const auto Read = [&Neutral](const int32 Index) { return Neutral.IsValidIndex(Index) ? Neutral[Index] : 0.0f; };

		const FVector DnaTranslation(Read(Attribute + 0), Read(Attribute + 1), Read(Attribute + 2));
		const FQuat DnaRotation(FRotator(-Read(Attribute + 4), Read(Attribute + 5), -Read(Attribute + 3)));

		const FTransform& RefPose = RefSkeleton.GetRefBonePose()[Bone];

		const float Distance = static_cast<float>(FVector::Dist(DnaTranslation, RefPose.GetTranslation()));
		const float Angle = FMath::RadiansToDegrees(static_cast<float>(DnaRotation.AngularDistance(RefPose.GetRotation())));

		Compared++;

		if (Distance > 0.05f || Angle > 1.0f) {
			Mismatched++;

			UE_LOG(LogRigImportTest, Display, TEXT("  differs: '%s' %.2f degrees / %.3f cm"), *BoneName.ToString(), Angle, Distance);
		}

		if (Angle > WorstAngle) {
			WorstAngle = Angle;
			WorstDistance = Distance;
			WorstBone = BoneName.ToString();
		}
	}

	UE_LOG(LogRigImportTest, Display, TEXT("dna neutral vs reference pose: %d of %d joints differ, worst '%s' at %.2f degrees / %.3f cm"),
		Mismatched, Compared, *WorstBone, WorstAngle, WorstDistance);

	/* The pose the mesh is actually bound at, measured against both ways of reading the DNA: the
	 * one the stock anim node uses, and the one that reproduces this bind pose. Whichever matches
	 * is the convention a rig has to evaluate in for this mesh not to deform. */
	for (int32 Convention = 0; Convention < 2; ++Convention) {
		float Total = 0.0f;
		float Worst = 0.0f;
		float TranslationError = 0.0f;
		float ScaleError = 0.0f;
		int32 Counted = 0;
		int32 Matching = 0;

		for (uint16 Joint = 0; Joint < Behavior->GetJointCount(); ++Joint) {
			const FName BoneName(*Behavior->GetJointName(Joint));

			const int32 Bone = RefSkeleton.FindBoneIndex(BoneName);
			if (Bone == INDEX_NONE) continue;
			if (!BoneName.ToString().StartsWith(TEXT("FACIAL_"))) continue;

			const int32 Attribute = Joint * 9;
			const auto Read = [&Neutral](const int32 Index) { return Neutral.IsValidIndex(Index) ? Neutral[Index] : 0.0f; };

			const FQuat Candidate = Convention == 0
				? FQuat(FRotator(-Read(Attribute + 4), -Read(Attribute + 5), Read(Attribute + 3)))
				: FQuat(FRotator(-Read(Attribute + 4), Read(Attribute + 5), -Read(Attribute + 3)));

			const FVector CandidateTranslation = Convention == 0
				? FVector(Read(Attribute + 0), -Read(Attribute + 1), Read(Attribute + 2))
				: FVector(Read(Attribute + 0), Read(Attribute + 1), Read(Attribute + 2));

			const FVector CandidateScale(Read(Attribute + 6), Read(Attribute + 7), Read(Attribute + 8));

			const FTransform& Bound = RefSkeleton.GetRefBonePose()[Bone];

			TranslationError += static_cast<float>(FVector::Dist(CandidateTranslation, Bound.GetTranslation()));
			ScaleError += static_cast<float>(FVector::Dist(CandidateScale, Bound.GetScale3D()));

			const float Angle = FMath::RadiansToDegrees(static_cast<float>(Candidate.AngularDistance(Bound.GetRotation())));

			Total += Angle;
			Worst = FMath::Max(Worst, Angle);

			if (Angle < 1.0f) Matching++;

			Counted++;
		}

		if (Counted == 0) continue;

		UE_LOG(LogRigImportTest, Display, TEXT("  %s convention: %d of %d match, mean %.3f degrees (worst %.2f), translation %.4f cm, scale %.4f"),
			Convention == 0 ? TEXT("anim node") : TEXT("fitted   "), Matching, Counted, Total / Counted, Worst,
			TranslationError / Counted, ScaleError / Counted);
	}

	/* Where the bones actually ended up, so a bind pose that moved the mesh is visible */
	for (const TCHAR* Name : { TEXT("spine_04"), TEXT("head"), TEXT("FACIAL_C_Forehead") }) {
		const int32 Bone = RefSkeleton.FindBoneIndex(FName(Name));
		if (Bone == INDEX_NONE) continue;

		FTransform Component = RefSkeleton.GetRefBonePose()[Bone];

		for (int32 Parent = RefSkeleton.GetParentIndex(Bone); Parent != INDEX_NONE; Parent = RefSkeleton.GetParentIndex(Parent)) {
			Component = Component * RefSkeleton.GetRefBonePose()[Parent];
		}

		UE_LOG(LogRigImportTest, Display, TEXT("  bone '%s' local %s r %s, component %s"),
			Name, *RefSkeleton.GetRefBonePose()[Bone].GetTranslation().ToString(),
			*RefSkeleton.GetRefBonePose()[Bone].GetRotation().Rotator().ToString(),
			*Component.GetTranslation().ToString());
	}

	/* The raw numbers side by side, so the mapping between them can be read off rather than guessed */
	UE_LOG(LogRigImportTest, Display, TEXT("dna neutral vs reference pose: %d of %d joints differ, worst '%s' at %.2f degrees / %.3f cm"),
		Mismatched, Compared, *WorstBone, WorstAngle, WorstDistance);

#endif
}

/* Drives the same control through the rig and through the baked poses, and compares the two.
 *
 * A pose asset is meant to stand in for the rig, so for a given control the two should move the
 * same bones the same way. Anything mirrored shows up as a negative dot product, which is what a
 * pose read out of the DNA with the wrong signs looks like. */
static void ComparePoseAssetToRig(USkeletalMesh* Mesh, const FString& PoseAssetPath) {
#if REFLECTION_RIG_LOGIC
	USkeleton* Skeleton = Mesh->GetSkeleton();
	if (Skeleton == nullptr) return;

	UPoseAsset* PoseAsset = LoadObject<UPoseAsset>(nullptr, *PoseAssetPath);

	if (PoseAsset == nullptr) {
		UE_LOG(LogRigImportTest, Display, TEXT("pose comparison: no pose asset at %s"), *PoseAssetPath);

		return;
	}

	UDNAAsset* DNAAsset = USkelMeshDNAUtils::GetMeshDNA(Mesh);
	if (DNAAsset == nullptr) return;

	const TSharedPtr<IDNAReader> Behavior = DNAAsset->GetBehaviorReader();
	if (!Behavior.IsValid()) return;

	static const TCHAR* Wanted[] = { TEXT("jawOpen"), TEXT("browDownL"), TEXT("browRaiseInL"), TEXT("eyeBlinkL") };

	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();

	for (const TCHAR* Pattern : Wanted) {
		FString CurveName;

		for (uint16 Control = 0; Control < Behavior->GetRawControlCount(); ++Control) {
			const FString Name = Behavior->GetRawControlName(Control);

			if (Name.Contains(Pattern)) {
				CurveName = Name.Replace(TEXT("."), TEXT("_"));

				break;
			}
		}

		if (CurveName.IsEmpty()) continue;

		/* Each graph evaluated with the control off and on, so whatever is static drops out */
		TArray<FVector> Displacement[2];

		for (int32 Graph = 0; Graph < 2; ++Graph) {
			TArray<FTransform> Sampled[2];

			for (int32 Pass = 0; Pass < 2; ++Pass) {
				UAnimSequence* Driver = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
				Driver->SetSkeleton(Skeleton);

				{
					IAnimationDataController& Controller = Driver->GetController();

					Controller.OpenBracket(NSLOCTEXT("Reflection", "DriveControl", "Driving one control"), false);
					Controller.InitializeModel();
					Controller.SetFrameRate(FFrameRate(30, 1), false);
					Controller.SetNumberOfFrames(FFrameNumber(1), false);

					const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

					Controller.AddCurve(CurveId, 0x00000004, false);
					Controller.SetCurveKeys(CurveId, { FRichCurveKey(0.0f, static_cast<float>(Pass)), FRichCurveKey(1.0f, static_cast<float>(Pass)) }, false);

					Controller.NotifyPopulated();
					Controller.CloseBracket(false);
				}

				/* A fresh name each time: making a blueprint over one that already exists hands
				 * back nothing, and every graph here would be the first one */
				static int32 Sequence = 0;

				const FString BlueprintName = FString::Printf(TEXT("PoseCompare_AnimBP_%d"), Sequence++);

				UPackage* Package = CreatePackage(*(TEXT("/Game/ReflectedRigs/") + BlueprintName));

				UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
					UAnimInstance::StaticClass(), Package, FName(*BlueprintName),
					BPTYPE_Normal, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));

				if (AnimBlueprint == nullptr) {
					UE_LOG(LogRigImportTest, Error, TEXT("pose comparison: could not make '%s'"), *BlueprintName);

					return;
				}

				AnimBlueprint->TargetSkeleton = Skeleton;

				UEdGraph* AnimGraph = nullptr;

				for (UEdGraph* Candidate : AnimBlueprint->FunctionGraphs) {
					if (Candidate != nullptr && Candidate->GetFName() == UEdGraphSchema_K2::GN_AnimGraph) {
						AnimGraph = Candidate;

						break;
					}
				}

				if (AnimGraph == nullptr) {
					UE_LOG(LogRigImportTest, Error, TEXT("pose comparison: no anim graph"));

					return;
				}

				UAnimGraphNode_Root* Result = nullptr;

				for (UEdGraphNode* Node : AnimGraph->Nodes) {
					if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node)) {
						Result = Root;

						break;
					}
				}

				UAnimGraphNode_SequencePlayer* Player = NewObject<UAnimGraphNode_SequencePlayer>(AnimGraph);

				Player->CreateNewGuid();
				Player->PostPlacedNewNode();
				Player->Node.SetSequence(Driver);
				Player->AllocateDefaultPins();

				AnimGraph->AddNode(Player, false, false);

				UEdGraphNode* Consumer = nullptr;
				UEdGraphPin* ConsumerIn = nullptr;
				UEdGraphPin* ConsumerOut = nullptr;

				if (Graph == 0) {
					UAnimGraphNode_RigLogic* RigLogicNode = NewObject<UAnimGraphNode_RigLogic>(AnimGraph);

					RigLogicNode->CreateNewGuid();
					RigLogicNode->PostPlacedNewNode();
					RigLogicNode->AllocateDefaultPins();

					AnimGraph->AddNode(RigLogicNode, false, false);

					Consumer = RigLogicNode;
					ConsumerIn = RigLogicNode->FindPin(TEXT("AnimSequence"), EGPD_Input);
					ConsumerOut = RigLogicNode->FindPin(TEXT("Pose"), EGPD_Output);
				} else {
					UAnimGraphNode_PoseBlendNode* PoseNode = NewObject<UAnimGraphNode_PoseBlendNode>(AnimGraph);

					PoseNode->CreateNewGuid();
					PoseNode->PostPlacedNewNode();
					PoseNode->Node.PoseAsset = PoseAsset;
					PoseNode->AllocateDefaultPins();

					AnimGraph->AddNode(PoseNode, false, false);

					Consumer = PoseNode;
					ConsumerIn = PoseNode->FindPin(TEXT("SourcePose"), EGPD_Input);
					ConsumerOut = PoseNode->FindPin(TEXT("Pose"), EGPD_Output);
				}

				UEdGraphPin* PlayerOut = Player->FindPin(TEXT("Pose"), EGPD_Output);
				UEdGraphPin* ResultIn = Result != nullptr ? Result->FindPin(TEXT("Result"), EGPD_Input) : nullptr;

				if (PlayerOut != nullptr && ConsumerIn != nullptr) PlayerOut->MakeLinkTo(ConsumerIn);
				if (ConsumerOut != nullptr && ResultIn != nullptr) ConsumerOut->MakeLinkTo(ResultIn);

				FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);

				UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
				if (World == nullptr) return;

				USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(World);

				Component->SetSkeletalMesh(Mesh);
				Component->SetAnimInstanceClass(AnimBlueprint->GeneratedClass);
				Component->RegisterComponentWithWorld(World);
				Component->InitAnim(true);
				Component->TickAnimation(0.0f, false);
				Component->RefreshBoneTransforms();

				Sampled[Pass] = Component->GetComponentSpaceTransforms();

				Component->UnregisterComponent();
				World->DestroyWorld(false);
			}

			Displacement[Graph].SetNum(FMath::Min(Sampled[0].Num(), Sampled[1].Num()));

			for (int32 Bone = 0; Bone < Displacement[Graph].Num(); ++Bone) {
				Displacement[Graph][Bone] = Sampled[1][Bone].GetTranslation() - Sampled[0][Bone].GetTranslation();
			}
		}

		/* The bone the rig moved most, and what the poses did with it */
		int32 Loudest = INDEX_NONE;
		float Largest = 0.0f;

		for (int32 Bone = 0; Bone < FMath::Min(Displacement[0].Num(), Displacement[1].Num()); ++Bone) {
			const float Size = static_cast<float>(Displacement[0][Bone].Size());

			if (Size > Largest) {
				Largest = Size;
				Loudest = Bone;
			}
		}

		if (Loudest == INDEX_NONE) {
			UE_LOG(LogRigImportTest, Display, TEXT("  '%s': the rig moved nothing"), *CurveName);

			continue;
		}

		const FVector Rig = Displacement[0][Loudest];
		const FVector Pose = Displacement[1][Loudest];

		const double Agreement = FVector::DotProduct(Rig.GetSafeNormal(), Pose.GetSafeNormal());

		UE_LOG(LogRigImportTest, Display, TEXT("  '%s' on '%s': rig %.3f cm, poses %.3f cm, agreement %.2f (%s)"),
			*CurveName, *RefSkeleton.GetBoneName(Loudest).ToString(), Rig.Size(), Pose.Size(), Agreement,
			Pose.Size() < 0.005 ? TEXT("poses did nothing") : (Agreement > 0.9 ? TEXT("same way") : (Agreement < -0.5 ? TEXT("MIRRORED") : TEXT("different"))));
	}
#endif
}

/* Drives one control at a time through a real RigLogic node and reports which bones moved and
 * which way.
 *
 * The rest pose says nothing: negating the neutral makes it match by construction. What the curves
 * drive are the deltas, and a delta with a sign wrong still moves the right bones by the right
 * amount, in the wrong direction. Nothing is assumed about which bone a control owns -- the bones
 * that moved most are simply reported, with the direction they went. */
static void ReportControlDirections(USkeletalMesh* Mesh, UAnimBlueprint* AnimBlueprint, UAnimGraphNode_SequencePlayer* Player) {
#if REFLECTION_RIG_LOGIC
	USkeleton* Skeleton = Mesh->GetSkeleton();
	if (Skeleton == nullptr || AnimBlueprint == nullptr || Player == nullptr) return;

	UDNAAsset* DNAAsset = USkelMeshDNAUtils::GetMeshDNA(Mesh);
	if (DNAAsset == nullptr) return;

	const TSharedPtr<IDNAReader> Behavior = DNAAsset->GetBehaviorReader();
	if (!Behavior.IsValid()) return;

	/* A handful whose direction is obvious to anyone looking at a face */
	static const TCHAR* Wanted[] = { TEXT("jawOpen"), TEXT("browDownL"), TEXT("browRaiseInL"), TEXT("eyeBlinkL") };

	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();

	TArray<FTransform> Bound;
	Bound.SetNum(RefSkeleton.GetNum());

	for (int32 Bone = 0; Bone < RefSkeleton.GetNum(); ++Bone) {
		const int32 Parent = RefSkeleton.GetParentIndex(Bone);

		Bound[Bone] = Parent == INDEX_NONE
			? RefSkeleton.GetRefBonePose()[Bone]
			: RefSkeleton.GetRefBonePose()[Bone] * Bound[Parent];
	}

	for (const TCHAR* Pattern : Wanted) {
		FString CurveName;

		for (uint16 Control = 0; Control < Behavior->GetRawControlCount(); ++Control) {
			const FString Name = Behavior->GetRawControlName(Control);

			if (Name.Contains(Pattern)) {
				CurveName = Name.Replace(TEXT("."), TEXT("_"));

				break;
			}
		}

		if (CurveName.IsEmpty()) {
			UE_LOG(LogRigImportTest, Display, TEXT("  control '%s': the DNA has no such control"), Pattern);

			continue;
		}

		/* Evaluated twice, with the control off and on. Everything static -- the pose the player
		 * hands over for bones it has no track for, the bind pose, the body -- is in both, so the
		 * difference between them is the control and nothing else. */
		TArray<FTransform> Sampled[2];

		for (int32 Pass = 0; Pass < 2; ++Pass) {
			UAnimSequence* Driver = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
			Driver->SetSkeleton(Skeleton);

			{
				IAnimationDataController& Controller = Driver->GetController();

				Controller.OpenBracket(NSLOCTEXT("Reflection", "DriveControl", "Driving one control"), false);
				Controller.InitializeModel();
				Controller.SetFrameRate(FFrameRate(30, 1), false);
				Controller.SetNumberOfFrames(FFrameNumber(1), false);

				const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

				Controller.AddCurve(CurveId, 0x00000004, false);
				Controller.SetCurveKeys(CurveId, { FRichCurveKey(0.0f, static_cast<float>(Pass)), FRichCurveKey(1.0f, static_cast<float>(Pass)) }, false);

				Controller.NotifyPopulated();
				Controller.CloseBracket(false);
			}

			Player->Node.SetSequence(Driver);

			FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);

			UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
			if (World == nullptr) return;

			USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(World);

			Component->SetSkeletalMesh(Mesh);
			Component->SetAnimInstanceClass(AnimBlueprint->GeneratedClass);
			Component->RegisterComponentWithWorld(World);
			Component->InitAnim(true);
			Component->TickAnimation(0.0f, false);
			Component->RefreshBoneTransforms();

			Sampled[Pass] = Component->GetComponentSpaceTransforms();

			Component->UnregisterComponent();
			World->DestroyWorld(false);
		}

		TArray<TPair<float, int32>> Moved;

		for (int32 Bone = 0; Bone < FMath::Min(Sampled[0].Num(), Sampled[1].Num()); ++Bone) {
			const float Distance = static_cast<float>(FVector::Dist(Sampled[1][Bone].GetTranslation(), Sampled[0][Bone].GetTranslation()));

			if (Distance > 0.005f) {
				Moved.Emplace(Distance, Bone);
			}
		}

		Moved.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key > B.Key; });

		UE_LOG(LogRigImportTest, Display, TEXT("  control '%s': %d bones moved"), *CurveName, Moved.Num());

		for (int32 Index = 0; Index < FMath::Min(3, Moved.Num()); ++Index) {
			const int32 Bone = Moved[Index].Value;
			const FVector Delta = Sampled[1][Bone].GetTranslation() - Sampled[0][Bone].GetTranslation();

			UE_LOG(LogRigImportTest, Display, TEXT("      '%s' %.3f cm, direction X=%.2f Y=%.2f Z=%.2f"),
				*RefSkeleton.GetBoneName(Bone).ToString(), Moved[Index].Key,
				Delta.X / FMath::Max(Delta.Size(), UE_SMALL_NUMBER),
				Delta.Y / FMath::Max(Delta.Size(), UE_SMALL_NUMBER),
				Delta.Z / FMath::Max(Delta.Size(), UE_SMALL_NUMBER));
		}
	}
#endif
}

/* Runs the mesh through a real RigLogic node and reports what it does to the skeleton.
 *
 * Reasoning about what the node computes is what got this wrong repeatedly, so nothing here is
 * reasoned: an anim blueprint with the node in it is built, played on a component, and the bones
 * it produces are measured against the pose the mesh is bound at. A rig with every control at rest
 * has to leave the mesh exactly where it was bound, so anything above a rounding error is the node
 * disagreeing with the DNA it was given. */
static void ReportRigLogicResult(USkeletalMesh* Mesh) {
#if REFLECTION_RIG_LOGIC
	USkeleton* Skeleton = Mesh->GetSkeleton();
	if (Skeleton == nullptr) return;

	UPackage* Package = CreatePackage(TEXT("/Game/ReflectedRigs/RigLogicProbe_AnimBP"));

	UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), Package, TEXT("RigLogicProbe_AnimBP"),
		BPTYPE_Normal, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));

	if (AnimBlueprint == nullptr) {
		UE_LOG(LogRigImportTest, Error, TEXT("rig logic probe: could not make an anim blueprint"));

		return;
	}

	AnimBlueprint->TargetSkeleton = Skeleton;

	/* The graph the node goes in, and the node the pose comes out of */
	UEdGraph* AnimGraph = nullptr;

	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs) {
		if (Graph != nullptr && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph) {
			AnimGraph = Graph;

			break;
		}
	}

	if (AnimGraph == nullptr) {
		UE_LOG(LogRigImportTest, Error, TEXT("rig logic probe: the blueprint has no anim graph"));

		return;
	}

	UAnimGraphNode_Root* Result = nullptr;

	for (UEdGraphNode* Node : AnimGraph->Nodes) {
		if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node)) {
			Result = Root;

			break;
		}
	}

	UAnimGraphNode_RigLogic* RigLogicNode = NewObject<UAnimGraphNode_RigLogic>(AnimGraph);

	RigLogicNode->CreateNewGuid();
	RigLogicNode->PostPlacedNewNode();
	RigLogicNode->AllocateDefaultPins();
	RigLogicNode->NodePosX = -300;

	AnimGraph->AddNode(RigLogicNode, false, false);

	UAnimGraphNode_SequencePlayer* Player = nullptr;

	/* The rig reads the controls off the curves in the pose handed to it, so a face animation
	 * playing into it is what actually drives the DNA */
	UAnimSequence* Face = FParse::Param(FCommandLine::Get(), TEXT("norig"))
		? nullptr
		: LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Animation/Game/MainPlayer/Menu/Facial/Frontend_Default_3L_Face_Idle.Frontend_Default_3L_Face_Idle"));

	if (Face != nullptr) {
		if (Face->GetSkeleton() != Skeleton) {
			Face->SetSkeleton(Skeleton);
		}

		int32 ControlCurves = 0;

		const TArray<FFloatCurve>& Curves = Face->GetDataModel()->GetFloatCurves();

		for (const FFloatCurve& Curve : Curves) {
			if (Curve.GetName().ToString().StartsWith(TEXT("CTRL_"))) ControlCurves++;
		}

		UE_LOG(LogRigImportTest, Display, TEXT("face animation: %s, %.2f seconds, %d curves, %d of them controls"),
			*Face->GetName(), Face->GetPlayLength(), Curves.Num(), ControlCurves);

		Player = NewObject<UAnimGraphNode_SequencePlayer>(AnimGraph);

		Player->CreateNewGuid();
		Player->PostPlacedNewNode();
		Player->Node.SetSequence(Face);
		Player->AllocateDefaultPins();
		Player->NodePosX = -600;

		AnimGraph->AddNode(Player, false, false);

		UEdGraphPin* PlayerPose = Player->FindPin(TEXT("Pose"), EGPD_Output);
		UEdGraphPin* RigLogicPose = RigLogicNode->FindPin(TEXT("AnimSequence"), EGPD_Input);

		if (PlayerPose != nullptr && RigLogicPose != nullptr) {
			PlayerPose->MakeLinkTo(RigLogicPose);
		} else {
			UE_LOG(LogRigImportTest, Warning, TEXT("rig logic probe: could not wire the animation into the node"));
		}
	} else {
		UE_LOG(LogRigImportTest, Warning, TEXT("rig logic probe: no face animation, measuring the rig at rest only"));
	}

	if (Result != nullptr) {
		UEdGraphPin* Source = RigLogicNode->FindPin(TEXT("Pose"), EGPD_Output);
		UEdGraphPin* Target = Result->FindPin(TEXT("Result"), EGPD_Input);

		if (Source != nullptr && Target != nullptr) {
			Source->MakeLinkTo(Target);
		} else {
			UE_LOG(LogRigImportTest, Warning, TEXT("rig logic probe: could not wire the node to the result"));
		}
	}

	FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);

	if (AnimBlueprint->GeneratedClass == nullptr) {
		UE_LOG(LogRigImportTest, Error, TEXT("rig logic probe: the blueprint did not compile"));

		return;
	}

	/* Played on a component, the way anything else would play it */
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
	if (World == nullptr) return;

	USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(World);

	Component->SetSkeletalMesh(Mesh);
	Component->SetAnimInstanceClass(AnimBlueprint->GeneratedClass);
	Component->RegisterComponentWithWorld(World);
	Component->InitAnim(true);
	Component->RefreshBoneTransforms();

	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();

	TArray<FTransform> BoundPose;
	BoundPose.SetNum(RefSkeleton.GetNum());

	for (int32 Bone = 0; Bone < RefSkeleton.GetNum(); ++Bone) {
		const int32 Parent = RefSkeleton.GetParentIndex(Bone);

		BoundPose[Bone] = Parent == INDEX_NONE
			? RefSkeleton.GetRefBonePose()[Bone]
			: RefSkeleton.GetRefBonePose()[Bone] * BoundPose[Parent];
	}

	/* Sampled across the animation: a face moves, so what is being looked for is movement on the
	 * scale a face makes, not the scale a skeleton in the wrong frame makes */
	for (int32 Sample = 0; Sample < 5; ++Sample) {
		Component->TickAnimation(Sample == 0 ? 0.0f : 0.25f, false);
		Component->RefreshBoneTransforms();

		const TArray<FTransform>& Posed = Component->GetComponentSpaceTransforms();

		if (Posed.Num() == 0) {
			UE_LOG(LogRigImportTest, Error, TEXT("rig logic probe: the component produced no pose"));

			return;
		}

		float Worst = 0.0f;
		float Total = 0.0f;
		int32 Counted = 0;
		FString WorstBone;

		for (int32 Bone = 0; Bone < FMath::Min(Posed.Num(), BoundPose.Num()); ++Bone) {
			const FName BoneName = RefSkeleton.GetBoneName(Bone);
			if (!BoneName.ToString().StartsWith(TEXT("FACIAL_"))) continue;

			const float Distance = static_cast<float>(FVector::Dist(Posed[Bone].GetTranslation(), BoundPose[Bone].GetTranslation()));

			Total += Distance;

			if (Distance > Worst) {
				Worst = Distance;
				WorstBone = BoneName.ToString();
			}

			Counted++;
		}

		UE_LOG(LogRigImportTest, Display, TEXT("rig logic at %.2fs: %d facial bones, mean %.4f cm off the bind pose, worst '%s' at %.4f cm"),
			Sample * 0.25f, Counted, Counted > 0 ? Total / Counted : 0.0f, *WorstBone, Worst);

		/* Where the displacement enters the chain: the first bone from the root down that the node
		 * put somewhere else is the one carrying everything below it */
		if (Sample == 0) {
			for (int32 Bone = 0; Bone < FMath::Min(Posed.Num(), BoundPose.Num()); ++Bone) {
				const float Distance = static_cast<float>(FVector::Dist(Posed[Bone].GetTranslation(), BoundPose[Bone].GetTranslation()));

				if (Distance <= 0.05f) continue;

				const FTransform Local = Posed[Bone].GetRelativeTransform(
					RefSkeleton.GetParentIndex(Bone) == INDEX_NONE ? FTransform::Identity : Posed[RefSkeleton.GetParentIndex(Bone)]);

				const float LocalError = static_cast<float>(FVector::Dist(Local.GetTranslation(), RefSkeleton.GetRefBonePose()[Bone].GetTranslation()));

				UE_LOG(LogRigImportTest, Display, TEXT("  chain '%s': %.4f cm in component space, %.4f cm of it its own"),
					*RefSkeleton.GetBoneName(Bone).ToString(), Distance, LocalError);

				if (Bone > 20) break;
			}
		}
	}

	Component->UnregisterComponent();
	World->DestroyWorld(false);

	/* What the curves actually drive */
	ReportControlDirections(Mesh, AnimBlueprint, Player);
#endif
}

int32 URigImportCommandlet::Main(const FString& Params) {
	/* Load-only mode: a separate process reading the saved asset cold, exactly the way the user's
	 * editor does. Nothing from the importing process is alive here. */
	FString LoadPath;

	if (FParse::Value(*Params, TEXT("load="), LoadPath)) {
		/* Skeletal meshes read back what they were built out of */
		if (USkeletalMesh* ColdMesh = LoadObject<USkeletalMesh>(nullptr, *LoadPath)) {
			const FSkeletalMeshModel* ColdModel = ColdMesh->GetImportedModel();

			int32 TotalVerts = 0;

			if (ColdModel != nullptr) {
				for (const FSkeletalMeshLODModel& Lod : ColdModel->LODModels) {
					TotalVerts += Lod.NumVertices;
				}
			}

			UE_LOG(LogRigImportTest, Display, TEXT("cold mesh: %d lods, %d verts, %d bones, %d materials, skeleton %s, colors %d"),
				ColdModel ? ColdModel->LODModels.Num() : 0, TotalVerts, ColdMesh->GetRefSkeleton().GetNum(),
				ColdMesh->GetMaterials().Num(), ColdMesh->GetSkeleton() ? *ColdMesh->GetSkeleton()->GetName() : TEXT("<none>"),
				ColdMesh->GetHasVertexColors() ? 1 : 0);

			UE_LOG(LogRigImportTest, Display, TEXT("cold morph targets: %d"), ColdMesh->GetMorphTargets().Num());

			ReportMorphTargets(ColdMesh, TEXT("cold"));
			ReportDna(ColdMesh, TEXT("cold"));
			ReportPoseAsset(FPackageName::ObjectPathToPackageName(LoadPath), TEXT("cold"), false);

			const FBoxSphereBounds ColdBounds = ColdMesh->GetImportedBounds();

			UE_LOG(LogRigImportTest, Display, TEXT("cold bounds: radius %f, material slot '%s'"),
				ColdBounds.SphereRadius,
				ColdMesh->GetMaterials().Num() > 0 ? *ColdMesh->GetMaterials()[0].MaterialSlotName.ToString() : TEXT("<none>"));

			return 0;
		}

		UControlRigBlueprint* Reloaded = LoadObject<UControlRigBlueprint>(nullptr, *LoadPath);

		if (Reloaded == nullptr) {
			UE_LOG(LogRigImportTest, Error, TEXT("cold load failed: %s"), *LoadPath);

			return 1;
		}

		const URigVMGraph* Model = Reloaded->GetDefaultModel();

		if (Model == nullptr) {
			UE_LOG(LogRigImportTest, Error, TEXT("cold load: no model"));

			return 1;
		}

		int32 Links = 0, Orphans = 0;

		for (const URigVMNode* Node : Model->GetNodes()) {
			int32 NodeLinks = 0;

			for (const URigVMPin* Pin : Node->GetPins()) {
				NodeLinks += Pin->GetSourceLinks(true).Num() + Pin->GetTargetLinks(true).Num();
				Links += Pin->GetTargetLinks(true).Num();
			}

			if (NodeLinks == 0) Orphans++;
		}

		UE_LOG(LogRigImportTest, Display, TEXT("cold load: %d nodes, %d links, %d orphans"), Model->GetNodes().Num(), Links, Orphans);

		/* Layout sanity: how far each data link travels, in columns. Adjacent is what we want. */
		{
			int32 Backward = 0, Adjacent = 0, Far = 0;

			for (const URigVMLink* GraphLink : Model->GetLinks()) {
				if (GraphLink->GetSourcePin() == nullptr || GraphLink->GetTargetPin() == nullptr) continue;
				if (GraphLink->GetSourcePin()->IsExecuteContext()) continue;

				const URigVMNode* Source = GraphLink->GetSourcePin()->GetNode();
				const URigVMNode* Target = GraphLink->GetTargetPin()->GetNode();

				if (Source == nullptr || Target == nullptr) continue;

				const float Delta = (Target->GetPosition().X - Source->GetPosition().X) / 480.0f;

				if (Delta <= 0.0f) Backward++;
				else if (Delta <= 2.0f) Adjacent++;
				else Far++;
			}

			UE_LOG(LogRigImportTest, Display, TEXT("cold data links: %d adjacent (1-2 cols), %d far, %d backward"), Adjacent, Far, Backward);
		}

		for (const URigVMNode* Node : Model->GetNodes()) {
			if (!Node->GetName().Contains(TEXT("ModifyTransforms"))) continue;

			if (const URigVMPin* Pin = Node->FindPin(TEXT("ItemToModify"))) {
				UE_LOG(LogRigImportTest, Display, TEXT("cold %s.ItemToModify = '%s'"), *Node->GetName(), *Pin->GetDefaultValue());
			}

			break;
		}

		for (const URigVMNode* Node : Model->GetNodes()) {
			if (!Node->GetName().Contains(TEXT("MathQuaternionSlerp"))) continue;

			for (const URigVMPin* Pin : Node->GetPins()) {
				if (Pin->GetName() != TEXT("A") && Pin->GetName() != TEXT("B")) continue;

				UE_LOG(LogRigImportTest, Display, TEXT("cold %s.%s [%s] = '%s' (stored '%s')"),
					*Node->GetName(), *Pin->GetName(), *Pin->GetCPPType(), *Pin->GetDefaultValue(),
					*Pin->GetDefaultValueStoredByUserInterface());
			}
		}

		for (const URigVMNode* Node : Model->GetNodes()) {
			if (!Node->GetNodeTitle().Contains(TEXT("Make Array"))) continue;

			if (const URigVMPin* Pin = Node->FindPin(TEXT("Values"))) {
				UE_LOG(LogRigImportTest, Display, TEXT("cold %s.Values (%d elements) = '%s'"), *Node->GetName(), Pin->GetSubPins().Num(), *Pin->GetDefaultValue());
			}
		}

		for (const URigVMNode* Node : Model->GetNodes()) {
			if (!Node->GetNodeTitle().Contains(TEXT("Or"))) continue;

			for (const URigVMPin* Pin : Node->GetPins()) {
				if (Pin->IsExecuteContext()) continue;

				UE_LOG(LogRigImportTest, Display, TEXT("cold %s.%s in=%d out=%d"),
					*Node->GetName(), *Pin->GetName(), Pin->GetSourceLinks(true).Num(), Pin->GetTargetLinks(true).Num());
			}

			break;
		}

		return 0;
	}

	FString JsonPath;

	if (!FParse::Value(*Params, TEXT("json="), JsonPath)) {
		UE_LOG(LogRigImportTest, Error, TEXT("no -json= given"));

		return 1;
	}

	FString FileContents;

	if (!FFileHelper::LoadFileToString(FileContents, *JsonPath)) {
		UE_LOG(LogRigImportTest, Error, TEXT("could not read %s"), *JsonPath);

		return 1;
	}

	TArray<TSharedPtr<FJsonValue>> Exports;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContents);

	if (!FJsonSerializer::Deserialize(Reader, Exports)) {
		UE_LOG(LogRigImportTest, Error, TEXT("could not parse %s"), *JsonPath);

		return 1;
	}

	UE_LOG(LogRigImportTest, Display, TEXT("read %d exports"), Exports.Num());

	FImportIssues::Begin();

	/* Test only: the setting lives in the editor's per project config, which a commandlet run has
	 * no way to be pointed at */
	if (FParse::Param(*Params, TEXT("bakedna"))) {
		GetSettings()->AssetSettings.SkeletalMesh.BakeDnaToPoseAsset = true;

		UE_LOG(LogRigImportTest, Display, TEXT("baking dna to a pose asset"));
	}

	IImporter* Importer = nullptr;
	IImportReader::ReadExportsAndImport(Exports, TEXT("/Game/ReflectedRigs/Rig.json"), Importer, true, false);

	if (Importer == nullptr) {
		UE_LOG(LogRigImportTest, Error, TEXT("no importer ran"));

		return 1;
	}

	/* Skeletal meshes report what they were built out of and stop there */
	if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Importer->GetAsset())) {
		UE_LOG(LogRigImportTest, Display, TEXT("=== asset: %s"), *Mesh->GetPathName());
		UE_LOG(LogRigImportTest, Display, TEXT("skeleton: %s, bones: %d"),
			Mesh->GetSkeleton() ? *Mesh->GetSkeleton()->GetName() : TEXT("<none>"), Mesh->GetRefSkeleton().GetNum());

		const FSkeletalMeshModel* Model = Mesh->GetImportedModel();

		UE_LOG(LogRigImportTest, Display, TEXT("morph targets: %d, min lod: %d"), Mesh->GetMorphTargets().Num(), Mesh->GetMinLod().Default);

		ReportMorphTargets(Mesh, TEXT(" "));
		ReportDna(Mesh, TEXT(" "));
		ReportDnaNeutral(Mesh);
		ReportRigLogicResult(Mesh);
		ComparePoseAssetToRig(Mesh, Mesh->GetPackage()->GetName() + TEXT("_DNA_PoseAsset.") + FPackageName::GetShortName(Mesh->GetPackage()->GetName()) + TEXT("_DNA_PoseAsset"));
		ReportPoseAsset(Mesh->GetPackage()->GetName(), TEXT(" "), true);
		UE_LOG(LogRigImportTest, Display, TEXT("lods: %d, materials: %d, sockets: %d, vertex colors: %d"),
			Model ? Model->LODModels.Num() : 0, Mesh->GetMaterials().Num(),
			Mesh->GetMeshOnlySocketList().Num(), Mesh->GetHasVertexColors() ? 1 : 0);

		if (Model != nullptr) {
			for (int32 Index = 0; Index < Model->LODModels.Num(); ++Index) {
				const FSkeletalMeshLODModel& Lod = Model->LODModels[Index];

				UE_LOG(LogRigImportTest, Display, TEXT("  lod %d: %d verts, %d sections, %d bones used, %d influences"),
					Index, Lod.NumVertices, Lod.Sections.Num(), Lod.ActiveBoneIndices.Num(), Lod.Sections.Num() > 0 ? Lod.Sections[0].MaxBoneInfluences : 0);
			}
		}

		const FBoxSphereBounds Bounds = Mesh->GetImportedBounds();

		UE_LOG(LogRigImportTest, Display, TEXT("bounds: origin %s extent %s radius %f"),
			*Bounds.Origin.ToString(), *Bounds.BoxExtent.ToString(), Bounds.SphereRadius);

		for (const TSharedPtr<FImportIssueAsset>& Asset : FImportIssues::GetAssets()) {
			for (const TSharedPtr<FImportIssue>& Issue : Asset->Issues) {
				UE_LOG(LogRigImportTest, Display, TEXT("issue: %s -- %s"), *Issue->Summary, *Issue->Detail);
			}
		}

		/* Written out so a separate process can read it back the way the editor would */
		{
			UPackage* Package = Mesh->GetPackage();

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

			const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

			UE_LOG(LogRigImportTest, Display, TEXT("saved %s: %d"), *FileName, UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs) ? 1 : 0);
		}

		return 0;
	}

	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Importer->GetAsset());

	if (Blueprint == nullptr) {
		UE_LOG(LogRigImportTest, Error, TEXT("no control rig blueprint came out"));

		return 1;
	}

	UE_LOG(LogRigImportTest, Display, TEXT("=== asset: %s"), *Blueprint->GetPathName());

	/* Hierarchy */
	if (const URigHierarchy* Hierarchy = Blueprint->GetHierarchy()) {
		int32 Bones = 0, Nulls = 0, Controls = 0, Curves = 0, Parented = 0;

		for (const FRigElementKey& Key : Hierarchy->GetAllKeys(true)) {
			switch (Key.Type) {
				case ERigElementType::Bone: Bones++; break;
				case ERigElementType::Null: Nulls++; break;
				case ERigElementType::Control: Controls++; break;
				case ERigElementType::Curve: Curves++; break;
				default: break;
			}

			if (Hierarchy->GetFirstParent(Key).IsValid()) {
				Parented++;
			}
		}

		UE_LOG(LogRigImportTest, Display, TEXT("hierarchy: %d elements (%d bones, %d nulls, %d controls, %d curves), %d with a parent"),
			Hierarchy->Num(), Bones, Nulls, Controls, Curves, Parented);

		/* Spot check one control's settings */
		for (const FRigElementKey& Key : Hierarchy->GetAllKeys(false)) {
			if (Key.Type != ERigElementType::Control) continue;

			if (const FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key)) {
				UE_LOG(LogRigImportTest, Display, TEXT("  control %s: type %d, shape %s, colour %s, parent %s"),
					*Key.Name.ToString(),
					static_cast<int32>(Control->Settings.ControlType),
					*Control->Settings.ShapeName.ToString(),
					*Control->Settings.ShapeColor.ToString(),
					*Hierarchy->GetFirstParent(Key).ToString());
			}

			break;
		}
	}

	/* Variables */
	UE_LOG(LogRigImportTest, Display, TEXT("variables: %d"), Blueprint->NewVariables.Num());

	for (const FBPVariableDescription& Variable : Blueprint->NewVariables) {
		UE_LOG(LogRigImportTest, Display, TEXT("  %s : %s"), *Variable.VarName.ToString(), *Variable.VarType.PinCategory.ToString());
	}

	/* Graph */
	if (const URigVMGraph* Graph = Blueprint->GetDefaultModel()) {
		int32 Links = 0;

		for (const URigVMNode* Node : Graph->GetNodes()) {
			for (const URigVMPin* Pin : Node->GetPins()) {
				Links += Pin->GetTargetLinks(true).Num();
			}
		}

		int32 Mutable = 0, ExecuteLinks = 0, MutableWithSource = 0;

		for (const URigVMNode* Node : Graph->GetNodes()) {
			if (!Node->IsMutable()) continue;

			Mutable++;

			for (const URigVMPin* Pin : Node->GetPins()) {
				if (!Pin->IsExecuteContext()) continue;

				ExecuteLinks += Pin->GetTargetLinks(true).Num();

				if (Pin->GetSourceLinks(true).Num() > 0) {
					MutableWithSource++;

					break;
				}
			}
		}

		UE_LOG(LogRigImportTest, Display, TEXT("graph: %d nodes, %d links, %d mutable nodes, %d execute links, %d mutable nodes reached"),
			Graph->GetNodes().Num(), Links, Mutable, ExecuteLinks, MutableWithSource);

		/* Nodes nothing reaches and nothing leaves */
		int32 Orphans = 0;
		int32 Shown = 0;

		for (const URigVMNode* Node : Graph->GetNodes()) {
			int32 NodeLinks = 0;

			for (const URigVMPin* Pin : Node->GetPins()) {
				NodeLinks += Pin->GetSourceLinks(true).Num() + Pin->GetTargetLinks(true).Num();
			}

			if (NodeLinks > 0) continue;

			Orphans++;

			if (Shown++ >= 12) continue;

			UE_LOG(LogRigImportTest, Display, TEXT("  orphan %s (%s)"), *Node->GetName(), *Node->GetNodeTitle());

			for (const URigVMPin* Pin : Node->GetPins()) {
				UE_LOG(LogRigImportTest, Display, TEXT("     pin %s [%s] dir=%d default='%s'"),
					*Pin->GetName(), *Pin->GetCPPType(), static_cast<int32>(Pin->GetDirection()), *Pin->GetDefaultValue());
			}
		}

		UE_LOG(LogRigImportTest, Display, TEXT("orphan nodes: %d of %d"), Orphans, Graph->GetNodes().Num());

		/* What an element key pin actually came out as */
		for (const URigVMNode* Node : Graph->GetNodes()) {
			if (!Node->GetName().Contains(TEXT("ModifyTransforms"))) continue;

			for (const URigVMPin* Pin : Node->GetPins()) {
				if (Pin->GetName() != TEXT("ItemToModify")) continue;

				UE_LOG(LogRigImportTest, Display, TEXT("%s.ItemToModify = '%s'"), *Node->GetName(), *Pin->GetDefaultValue());
			}

			break;
		}
	}

	/* What the graph compiles back into */
	Blueprint->RecompileVM();

	if (const URigVMHost* Host = Cast<URigVMHost>(Blueprint->GeneratedClass->GetDefaultObject())) {
		if (URigVM* VM = const_cast<URigVMHost*>(Host)->GetVM()) {
			UE_LOG(LogRigImportTest, Display, TEXT("recompiled VM: %d instructions"), VM->GetByteCode().GetNumInstructions());

			/* Per function: what the recompiled VM actually calls, to diff against the cook */
			TMap<FString, int32> Calls;

			const FRigVMByteCode& Code = VM->GetByteCode();
			const FRigVMInstructionArray RecompiledInstructions = Code.GetInstructions();

			for (const FRigVMInstruction& Instruction : RecompiledInstructions) {
				if (Instruction.OpCode != ERigVMOpCode::Execute) continue;

				const FRigVMExecuteOp& Op = Code.GetOpAt<FRigVMExecuteOp>(Instruction);
				const TArray<const FRigVMFunction*>& Functions = VM->GetFunctions();

				if (Functions.IsValidIndex(Op.FunctionIndex) && Functions[Op.FunctionIndex] != nullptr) {
					Calls.FindOrAdd(Functions[Op.FunctionIndex]->Name)++;
				}
			}

			Calls.KeySort(TLess<FString>());

			for (const TPair<FString, int32>& Call : Calls) {
				UE_LOG(LogRigImportTest, Display, TEXT("vmcall %d %s"), Call.Value, *Call.Key);
			}
		}
	}

	/* What the editor actually opens: the saved asset, and the ed graph it draws rather than the
	 * model underneath it */
	{
		UPackage* Package = Blueprint->GetPackage();
		const FString PackageName = Package->GetName();
		const FString AssetPath = Blueprint->GetPathName();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		const bool bSaved = UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs);

		UE_LOG(LogRigImportTest, Display, TEXT("saved %s: %d"), *FileName, bSaved ? 1 : 0);

		/* Count the ed graph's own links before anything is unloaded */
		for (const UEdGraph* EdGraph : Blueprint->UbergraphPages) {
			int32 EdLinks = 0;

			for (const UEdGraphNode* EdNode : EdGraph->Nodes) {
				for (const UEdGraphPin* EdPin : EdNode->Pins) {
					EdLinks += EdPin->LinkedTo.Num();
				}
			}

			UE_LOG(LogRigImportTest, Display, TEXT("ed graph %s: %d nodes, %d pin links"), *EdGraph->GetName(), EdGraph->Nodes.Num(), EdLinks);
		}

		/* Now throw it all away and read the asset back off disk */
		Blueprint = nullptr;
		Importer = nullptr;

		CollectGarbage(RF_NoFlags);

		if (UControlRigBlueprint* Reloaded = LoadObject<UControlRigBlueprint>(nullptr, *AssetPath)) {
			int32 ModelLinks = 0;

			if (const URigVMGraph* Model = Reloaded->GetDefaultModel()) {
				for (const URigVMNode* Node : Model->GetNodes()) {
					for (const URigVMPin* Pin : Node->GetPins()) {
						ModelLinks += Pin->GetTargetLinks(true).Num();
					}
				}

				UE_LOG(LogRigImportTest, Display, TEXT("reloaded model: %d nodes, %d links"), Model->GetNodes().Num(), ModelLinks);

				/* The exact node kinds that looked unconnected in the editor */
				const TArray<FString> Interesting = { TEXT("MathBoolOr"), TEXT("ArrayMake"), TEXT("FromEuler"), TEXT("GetTransform"), TEXT("ArrayClone") };

				int32 Shown = 0;

				for (const URigVMNode* Node : Model->GetNodes()) {
					const bool bInteresting = Interesting.ContainsByPredicate([Node](const FString& Needle) {
						return Node->GetName().Contains(Needle) || Node->GetNodeTitle().Contains(Needle);
					});

					if (!bInteresting || Shown++ >= 10) continue;

					FString PinReport;

					for (const URigVMPin* Pin : Node->GetPins()) {
						if (Pin->IsExecuteContext()) continue;

						PinReport += FString::Printf(TEXT("%s(in %d out %d) "),
							*Pin->GetName(), Pin->GetSourceLinks(true).Num(), Pin->GetTargetLinks(true).Num());
					}

					UE_LOG(LogRigImportTest, Display, TEXT("  %s (%s): %s"), *Node->GetName(), *Node->GetNodeTitle(), *PinReport);
				}
			}

			for (const UEdGraph* EdGraph : Reloaded->UbergraphPages) {
				int32 EdLinks = 0;

				for (const UEdGraphNode* EdNode : EdGraph->Nodes) {
					for (const UEdGraphPin* EdPin : EdNode->Pins) {
						EdLinks += EdPin->LinkedTo.Num();
					}
				}

				UE_LOG(LogRigImportTest, Display, TEXT("reloaded ed graph %s: %d nodes, %d pin links"), *EdGraph->GetName(), EdGraph->Nodes.Num(), EdLinks);
			}
		} else {
			UE_LOG(LogRigImportTest, Error, TEXT("could not reload %s"), *AssetPath);
		}
	}

	/* Everything the import had to say */
	for (const TSharedPtr<FImportIssueAsset>& Asset : FImportIssues::GetAssets()) {
		for (const TSharedPtr<FImportIssue>& Issue : Asset->Issues) {
			UE_LOG(LogRigImportTest, Display, TEXT("issue: %s -- %s"), *Issue->Summary, *Issue->Detail);
		}
	}

	return 0;
}

#else

int32 URigImportCommandlet::Main(const FString& Params) {
	return 1;
}

#endif
