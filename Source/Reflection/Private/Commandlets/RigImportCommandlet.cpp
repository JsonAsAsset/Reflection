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
	const FString PosePath = MeshPath + TEXT("_PoseAsset.") + FPackageName::GetShortName(MeshPath) + TEXT("_PoseAsset");

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

		const FVector DnaTranslation(Read(Attribute + 0), -Read(Attribute + 1), Read(Attribute + 2));
		const FQuat DnaRotation(FRotator(-Read(Attribute + 4), -Read(Attribute + 5), Read(Attribute + 3)));

		const FTransform& RefPose = RefSkeleton.GetRefBonePose()[Bone];

		const float Distance = static_cast<float>(FVector::Dist(DnaTranslation, RefPose.GetTranslation()));
		const float Angle = FMath::RadiansToDegrees(static_cast<float>(DnaRotation.AngularDistance(RefPose.GetRotation())));

		Compared++;

		if (Distance > 0.05f || Angle > 1.0f) {
			Mismatched++;
		}

		if (Angle > WorstAngle) {
			WorstAngle = Angle;
			WorstDistance = Distance;
			WorstBone = BoneName.ToString();
		}
	}

	UE_LOG(LogRigImportTest, Display, TEXT("dna neutral vs reference pose: %d of %d joints differ, worst '%s' at %.2f degrees / %.3f cm"),
		Mismatched, Compared, *WorstBone, WorstAngle, WorstDistance);

	/* The raw numbers side by side, so the mapping between them can be read off rather than guessed */
	UE_LOG(LogRigImportTest, Display, TEXT("dna neutral vs reference pose: %d of %d joints differ, worst '%s' at %.2f degrees / %.3f cm"),
		Mismatched, Compared, *WorstBone, WorstAngle, WorstDistance);

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
