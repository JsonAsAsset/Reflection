/* Copyright Reflection Contributors 2024-2026 */

#pragma once

/* Where the rig blueprint comes from.
 *
 * 5.7 pulled the parts of a rig asset anything can implement out into an interface of their own
 * and left the blueprint behind under a name that says so. UControlRigBlueprint is the same class
 * either side of that, so the header it is declared in is all that changes. */

#include "Engine/Compatibility.h"

#if REFLECTION_RIGVM

#if UE5_7_BEYOND
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif

#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"

/* Drawing a graph, said the same way on either side of RigVM's move into UE5.
 *
 * The parts are the same and always were: a graph, a controller that edits it, nodes made from the
 * struct behind them, links between pins, and defaults written onto pins. What changed is the
 * spelling. A node was added by AddStructNode and is now added by AddUnitNode, the calls that
 * changed anything grew arguments for whether to record undo and print Python, and the model and
 * controller a blueprint keeps went from members to accessors.
 *
 * Nodes are made from the path of the struct rather than the struct itself. Half of these units are
 * declared in private headers no other module can include, and the registry resolves a path either
 * way, so nothing here has to reach for a header that may not be reachable. */

/* Where a rig keeps the graph its forwards solve is drawn in */
inline URigVMGraph* RigModelOf(UControlRigBlueprint* Blueprint) {
	if (Blueprint == nullptr) return nullptr;

#if ENGINE_UE5
	return Blueprint->GetDefaultModel();
#else
	return Blueprint->Model;
#endif
}

/* And the thing edits to it go through */
inline URigVMController* RigControllerOf(UControlRigBlueprint* Blueprint, URigVMGraph* Graph) {
	if (Blueprint == nullptr) return nullptr;

#if ENGINE_UE5
	return Blueprint->GetOrCreateController(Graph);
#else
	return Blueprint->Controller;
#endif
}

/* One node, from the path of the struct that says what it does */
inline URigVMNode* RigAddNode(URigVMController* Controller, const FString& StructPath, const FVector2D& Position, const FString& Name) {
	if (Controller == nullptr) return nullptr;

#if ENGINE_UE5
	return Controller->AddUnitNodeFromStructPath(StructPath, TEXT("Execute"), Position, Name, false, false);
#else
	return Controller->AddStructNodeFromStructPath(StructPath, TEXT("Execute"), Position, Name, false);
#endif
}

inline bool RigLink(URigVMController* Controller, const FString& Output, const FString& Input) {
	if (Controller == nullptr || Output.IsEmpty() || Input.IsEmpty()) return false;

#if ENGINE_UE5
	return Controller->AddLink(Output, Input, false, false);
#else
	return Controller->AddLink(Output, Input, false);
#endif
}

inline bool RigSetDefault(URigVMController* Controller, const FString& Pin, const FString& Value) {
	if (Controller == nullptr || Pin.IsEmpty()) return false;

#if ENGINE_UE5
	return Controller->SetPinDefaultValue(Pin, Value, true, false, false);
#else
	return Controller->SetPinDefaultValue(Pin, Value, true, false, false);
#endif
}

/* The pin execution runs through, found by what it is rather than what it is called.
 *
 * It was ExecuteContext when a rig unit carried one of those and is ExecutePin now, and asking the
 * pin whether it is an execute context is true of it under either name. */
inline URigVMPin* RigExecutePin(URigVMNode* Node) {
	if (Node == nullptr) return nullptr;

	for (URigVMPin* Pin : Node->GetPins()) {
		if (Pin != nullptr && Pin->IsExecuteContext()) return Pin;
	}

	return nullptr;
}

/* The units a curve mapping is drawn out of.
 *
 * The rig's own units kept their names. The float arithmetic did not: it moved out of Control Rig
 * and into RigVM when RigVM became a thing of its own, and was renamed on the way. */
namespace RigUnits {
	static const FString BeginExecution = TEXT("/Script/ControlRig.RigUnit_BeginExecution");
	static const FString GetCurveValue = TEXT("/Script/ControlRig.RigUnit_GetCurveValue");
	static const FString SetCurveValue = TEXT("/Script/ControlRig.RigUnit_SetCurveValue");

#if ENGINE_UE5
	static const FString Multiply = TEXT("/Script/RigVM.RigVMFunction_MathFloatMul");
	static const FString Add = TEXT("/Script/RigVM.RigVMFunction_MathFloatAdd");
	static const FString Clamp = TEXT("/Script/RigVM.RigVMFunction_MathFloatClamp");
#else
	static const FString Multiply = TEXT("/Script/ControlRig.RigUnit_MathFloatMul");
	static const FString Add = TEXT("/Script/ControlRig.RigUnit_MathFloatAdd");
	static const FString Clamp = TEXT("/Script/ControlRig.RigUnit_MathFloatClamp");
#endif
}

#endif
