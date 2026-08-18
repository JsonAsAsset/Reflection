/* Copyright Reflection Contributors 2024-2026 */

#include "RigImportCommandlet.h"

#if REFLECTION_CONTROL_RIG

#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/ImportIssues.h"

#include "ControlRigBlueprint.h"
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

int32 URigImportCommandlet::Main(const FString& Params) {
	/* Load-only mode: a separate process reading the saved asset cold, exactly the way the user's
	 * editor does. Nothing from the importing process is alive here. */
	FString LoadPath;

	if (FParse::Value(*Params, TEXT("load="), LoadPath)) {
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

	IImporter* Importer = nullptr;
	IImportReader::ReadExportsAndImport(Exports, TEXT("/Game/ReflectedRigs/Rig.json"), Importer, true, false);

	if (Importer == nullptr) {
		UE_LOG(LogRigImportTest, Error, TEXT("no importer ran"));

		return 1;
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
