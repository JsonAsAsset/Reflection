/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_AddDelegate.h"
/* Kept under the kismet editor's own nodes folder before 4.25 */
#if __has_include("K2Node_AsyncAction.h")
#include "K2Node_AsyncAction.h"
#else
#include "Nodes/K2Node_AsyncAction.h"
#endif
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyAsyncTask, All, All);

namespace {
	/* Everything reaching one pin, reaching another instead */
	void TakeOverAsync(UEdGraphPin* From, UEdGraphPin* To) {
		if (From == nullptr || To == nullptr) return;

		for (UEdGraphPin* Held : From->LinkedTo) {
			if (Held != nullptr) To->MakeLinkTo(Held);
		}
	}

	/* Whether a node is told what the call handed back, straight or through what is worked out
	 * from it.
	 *
	 * The look to see the task was made is a branch, and a branch is told nothing: what it asks is
	 * worked out beside it and handed in as an answer. So what feeds a node is followed back
	 * through anything that only works something out, and those workings are kept, since they are
	 * there for the node and go when it goes. */
	bool ReadsAsync(const UEdGraphNode* Node, UEdGraphPin* Made, TArray<UK2Node*>& Through, const int32 Removes = 2) {
		if (Node == nullptr || Made == nullptr || Removes < 0) return false;

		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Input) continue;

			for (UEdGraphPin* From : Pin->LinkedTo) {
				if (From == Made) return true;

				UK2Node* Works = From != nullptr ? Cast<UK2Node>(From->GetOwningNode()) : nullptr;

				if (Works == nullptr || !Works->IsNodePure()) continue;

				if (ReadsAsync(Works, Made, Through, Removes - 1)) {
					Through.AddUnique(Works);

					return true;
				}
			}
		}

		return false;
	}

	/* The node a handler was laid down for, read back out of the handler's name. The compiler names it
	 * <way out>_<guid>, the node's own guid with its last part swapped for a CRC of the whole
	 * (FKismetCompilerContext::GetGuid), so the first three parts are the node's as they were. */
	FGuid NodeNamedBy(const FName Handler) {
		const FString Name = Handler.ToString();

		int32 Under = INDEX_NONE;

		if (!Name.FindLastChar(TEXT('_'), Under)) return FGuid();

		FGuid Written;

		if (!FGuid::ParseExact(Name.Mid(Under + 1), EGuidFormats::Digits, Written)) return FGuid();

		uint32 Table[256];

		for (uint32 Index = 0; Index < 256; ++Index) {
			uint32 Entry = Index;

			for (int32 Bit = 0; Bit < 8; ++Bit) {
				Entry = (Entry & 1) ? (Entry >> 1) ^ 0xEDB88320u : Entry >> 1;
			}

			Table[Index] = Entry;
		}

		/* Where the CRC stands once the three parts that are kept have gone through it */
		const uint32 Kept[3] = { Written.A, Written.B, Written.C };
		const uint8* KeptBytes = reinterpret_cast<const uint8*>(Kept);

		uint32 Register = 0xFFFFFFFFu;

		for (int32 Index = 0; Index < (int32)sizeof(Kept); ++Index) {
			Register = Table[(Register ^ KeptBytes[Index]) & 0xFF] ^ (Register >> 8);
		}

		/* Walked back from where it has to end: each step's top byte names the table entry it used */
		uint8 Used[4];
		uint32 Wanted = ~Written.D;

		for (int32 Step = 3; Step >= 0; --Step) {
			int32 Found = INDEX_NONE;

			for (int32 Index = 0; Index < 256; ++Index) {
				if ((Table[Index] >> 24) == (Wanted >> 24)) {
					Found = Index;

					break;
				}
			}

			if (Found == INDEX_NONE) return FGuid();

			Used[Step] = (uint8)Found;
			Wanted = (Wanted ^ Table[Found]) << 8;
		}

		/* And forward again, choosing each byte so the step uses the entry it has to */
		uint8 Tail[4];

		for (int32 Step = 0; Step < 4; ++Step) {
			Tail[Step] = (uint8)((Register ^ Used[Step]) & 0xFF);
			Register = Table[(Register ^ Tail[Step]) & 0xFF] ^ (Register >> 8);
		}

		uint32 Last = 0;
		FMemory::Memcpy(&Last, Tail, sizeof(Last));

		const FGuid Node(Written.A, Written.B, Written.C, Last);

		/* Asked of the engine's own CRC, so a mistake here is a fresh guid rather than a wrong one */
		if (FCrc::MemCrc32(&Node, sizeof(Node)) != Written.D) return FGuid();

		return Node;
	}

	/* Whether two ways out lead to the same places, where nowhere counts as a place */
	bool GoesWhereAsync(const UEdGraphPin* One, const UEdGraphPin* Other) {
		const int32 OneCount = One != nullptr ? One->LinkedTo.Num() : 0;
		const int32 OtherCount = Other != nullptr ? Other->LinkedTo.Num() : 0;

		if (OneCount != OtherCount) return false;

		for (int32 Index = 0; Index < OneCount; ++Index) {
			if (!Other->LinkedTo.Contains(One->LinkedTo[Index])) return false;
		}

		return true;
	}

	/* How many ways a run can leave a node */
	int32 WaysOutOf(const UEdGraphNode* Node) {
		int32 Ways = 0;

		for (const UEdGraphPin* Pin : Node->Pins) {
			if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) Ways++;
		}

		return Ways;
	}

	/* The one way a run leaves a node, where it leaves by one */
	UEdGraphPin* LeavesBy(const UEdGraphNode* Node) {
		UEdGraphPin* Only = nullptr;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != EGPD_Output) continue;
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->LinkedTo.Num() == 0) continue;

			if (Only != nullptr) return nullptr;

			Only = Pin;
		}

		return Only;
	}

	/* Where a run carries on to, where it carries on to one node */
	UK2Node* CarriesOnTo(const UEdGraphPin* Leaves) {
		return Leaves != nullptr && Leaves->LinkedTo.Num() == 1 ? Cast<UK2Node>(Leaves->LinkedTo[0]->GetOwningNode()) : nullptr;
	}

	/* A member the node keeps to itself, said through the property rather than the node.
	 *
	 * What one of these is made of is its own business, set by the engine while building one from a
	 * menu. Nothing here comes from a menu, so it is said the way anything is said about an object
	 * whose insides are not on offer. */
	template <typename PropertyType, typename ValueType>
	void SayTo(UObject* Object, const TCHAR* Named, ValueType Value) {
		if (Object == nullptr) return;

		if (PropertyType* Held = FindFProperty<PropertyType>(Object->GetClass(), Named)) {
			Held->SetPropertyValue_InContainer(Object, Value);
		}
	}

	/* plainest kind used. The list files a call under the class it belongs to rather than under the
	 * call, so that is where it is looked for, and a kind can refuse a graph, so the graph is asked. */
	UClass* DrawnAs(const UFunction* Makes, const UEdGraph* Graph) {
		if (Makes != nullptr) {
			const FBlueprintActionDatabase::FActionRegistry& Every = FBlueprintActionDatabase::Get().GetAllActions();

			if (const FBlueprintActionDatabase::FActionList* Claimed = Every.Find(FObjectKey(Makes->GetOwnerClass()))) {
				for (const UBlueprintNodeSpawner* Spawner : *Claimed) {
					if (Spawner == nullptr || Spawner->NodeClass == nullptr) continue;
					if (!Spawner->NodeClass->IsChildOf(UK2Node_BaseAsyncTask::StaticClass())) continue;

					const UBlueprintFunctionNodeSpawner* ForCall = Cast<UBlueprintFunctionNodeSpawner>(Spawner);

					if (ForCall == nullptr || ForCall->GetFunction() != Makes) continue;

					const UEdGraphNode* Kind = Spawner->NodeClass->GetDefaultObject<UEdGraphNode>();

					if (Graph != nullptr && Kind != nullptr && !Kind->IsCompatibleWithGraph(Graph)) continue;

					return Spawner->NodeClass;
				}
			}
		}

		return UK2Node_AsyncAction::StaticClass();
	}
}

/* A task started and waited on, which the compiler writes as everything it takes to start one.
 *
 * What somebody drew is one node: the call that makes the task, and a way out of it for each thing
 * the task can go on to say. What the compiler writes is the making of it, a look to see it was
 * made, a handler laid down for each of those ways out and bound to the task, and finally the word
 * to begin. The ways out are gone, and whatever left by one of them is an event standing on its own
 * with nothing running into it.
 *
 * None of that is drawable. The call is one the editor keeps out of its menus, since the node being
 * put back is what somebody places instead, and every piece of it reads the one task. So the node
 * goes back and each event's run is hung off the way out it was written for. */
struct FAsyncTaskTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("AsyncTask"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 Put = 0;

		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_CallFunction* Makes = Cast<UK2Node_CallFunction>(Node);

			if (Makes == nullptr) continue;

			UFunction* Called = Makes->GetTargetFunction();

			if (Called == nullptr || !Called->HasMetaData(FBlueprintMetadata::MD_BlueprintInternalUseOnly)) continue;

			UEdGraphPin* Made = Makes->GetReturnValuePin();

			if (Made == nullptr || Made->LinkedTo.Num() == 0) continue;

			UClass* Task = Cast<UClass>(Made->PinType.PinSubCategoryObject.Get());

			if (Task == nullptr) continue;

			/* From here on this is a task being started, whatever else it turns out to be.
			 *
			 * Nobody can draw a call to one of these, so one in a graph is always the expansion of a
			 * node that should go back. Where it cannot, saying so is worth more than leaving it
			 * looking like a graph somebody wrote that way. */
			const FString Named = Called->GetName();

			UEdGraphPin* Entered = Makes->GetExecPin();

			TArray<UK2Node_AddDelegate*> Binds;
			TArray<UK2Node*> Looks;
			TArray<UK2Node*> Workings;

			UK2Node_CallFunction* Begins = nullptr;
			UEdGraphPin* Leaves = LeavesBy(Makes);
			TArray<UEdGraphPin*> Otherwise;

			/* A task that spawns an actor for its node is asked to start the spawn and then to finish
			 * it before the word to begin, with a branch on whether the start gave it anything to
			 * finish (UK2Node_LatentGameplayTaskCall::ExpandNode). All of it is the node's. */
			UK2Node_CallFunction* SpawnStarts = nullptr;
			UK2Node_CallFunction* SpawnFinishes = nullptr;

			/* Everything the making runs into that reads what it made */
			while (UK2Node* Along = CarriesOnTo(Leaves)) {
				/* The branch on the start of a spawn asks what the start handed back, not the task */
				UK2Node_IfThenElse* AsksSpawn = SpawnStarts != nullptr && SpawnFinishes == nullptr ? Cast<UK2Node_IfThenElse>(Along) : nullptr;

				if (AsksSpawn != nullptr && !AsksSpawn->GetConditionPin()->LinkedTo.Contains(SpawnStarts->GetReturnValuePin())) AsksSpawn = nullptr;

				if (AsksSpawn == nullptr && !ReadsAsync(Along, Made, Workings)) break;

				if (UK2Node_AddDelegate* Binding = Cast<UK2Node_AddDelegate>(Along)) {
					Binds.Add(Binding);
				} else if (UK2Node_CallFunction* Begin = Cast<UK2Node_CallFunction>(Along)) {
					/* Only something the task does to itself begins it. The node calls it on the
					 * task it made, and a call that is handed the task instead has nothing on it to
					 * be called on, which the engine asserts on rather than reports. */
					const UFunction* Starts = Begin->GetTargetFunction();

					if (Starts == nullptr || Starts->HasAnyFunctionFlags(FUNC_Static) || !Task->IsChildOf(Starts->GetOwnerClass())) break;

					if (SpawnStarts == nullptr && Starts->GetFName() == TEXT("BeginSpawningActor")) {
						SpawnStarts = Begin;
					} else if (SpawnStarts != nullptr && SpawnFinishes == nullptr && Starts->GetFName() == TEXT("FinishSpawningActor")) {
						SpawnFinishes = Begin;
					} else {
						Begins = Begin;
					}
				} else if (WaysOutOf(Along) < 2 || LeavesBy(Along) != nullptr) {
					/* The look to see it was made, which is only this where nothing was drawn for
					 * it having not been. A graph that does something else when the task is missing
					 * was drawn that way and is none of this. */
					Looks.Add(Along);
				} else if (UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(Along); Branch != nullptr && (Otherwise.Num() == 0 || SpawnStarts != nullptr)) {
					/* The same look where the node had somewhere to go afterwards, which is only known once the way
					 * out is found. A spawning task has three: the task, the start of the spawn, and the task again. */
					Looks.Add(Along);
					Otherwise.Add(Branch->GetElsePin());
					Leaves = Branch->GetThenPin();

					continue;
				} else {
					break;
				}

				Leaves = LeavesBy(Along);

				if (Begins != nullptr || Leaves == nullptr) break;
			}

			if (Binds.Num() == 0) {
				/* What the run went into instead, since that is the whole of what went wrong */
				TArray<FString> Went;

				for (UEdGraphPin* Step = LeavesBy(Makes); Step != nullptr && Went.Num() < 6;) {
					UK2Node* Along = CarriesOnTo(Step);

					if (Along == nullptr) break;

					TArray<UK2Node*> Ignored;

					Went.Add(FString::Printf(TEXT("%s%s"), *Along->GetClass()->GetName(), ReadsAsync(Along, Made, Ignored) ? TEXT("") : TEXT(" (reads it not)")));

					Step = LeavesBy(Along);
				}

				UE_LOG(LogReflectionTidyAsyncTask, Warning, TEXT("\"%s\" starts \"%s\" and was left as it was read: nothing binds to it, and the run goes into %s"),
					*Graph->GetName(), *Named, Went.Num() > 0 ? *FString::Join(Went, TEXT(", ")) : TEXT("nothing"));

				continue;
			}

			/* The handler laid down for each way out, and the event it leads into */
			TMap<FName, UK2Node_CustomEvent*> Answers;
			TArray<UK2Node_CreateDelegate*> Handlers;

			FString Missed;

			for (const UK2Node_AddDelegate* Binding : Binds) {
				const UEdGraphPin* Handler = Binding->GetDelegatePin();

				UK2Node_CreateDelegate* Laid = Handler != nullptr && Handler->LinkedTo.Num() == 1
					? Cast<UK2Node_CreateDelegate>(Handler->LinkedTo[0]->GetOwningNode())
					: nullptr;

				if (Laid == nullptr) {
					Missed = FString::Printf(TEXT("nothing lays a handler down for \"%s\""), *Binding->GetPropertyName().ToString());

					break;
				}

				UK2Node_CustomEvent* Answered = nullptr;

				for (UEdGraphNode* Held : Graph->Nodes) {
					UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Held);

					if (Event != nullptr && Event->CustomFunctionName == Laid->GetFunctionName()) {
						Answered = Event;

						break;
					}
				}

				if (Answered == nullptr) {
					Missed = FString::Printf(TEXT("the handler for \"%s\" is \"%s\", which no event in this graph answers to"),
						*Binding->GetPropertyName().ToString(), *Laid->GetFunctionName().ToString());

					break;
				}

				Answers.Add(Binding->GetPropertyName(), Answered);
				Handlers.Add(Laid);
			}

			/* All of it or none of it: half a node put back is worse than what was read */
			if (Answers.Num() != Binds.Num()) {
				UE_LOG(LogReflectionTidyAsyncTask, Warning, TEXT("\"%s\" starts \"%s\" and was left as it was read: %s"), *Graph->GetName(), *Named, *Missed);

				continue;
			}

			/* A spawn started and never finished, or finished and never followed by the word to
			 * begin, is not the node's expansion. Spawned values set between the two would be the
			 * node's spawn pins, which are not put back yet, and stop the walk the same way. */
			if (SpawnStarts != nullptr && (SpawnFinishes == nullptr || Begins == nullptr)) {
				UE_LOG(LogReflectionTidyAsyncTask, Warning, TEXT("\"%s\" starts \"%s\" and was left as it was read: it starts spawning an actor, and what follows is not the finish and the word to begin"), *Graph->GetName(), *Named);

				continue;
			}

			/* A branch whose other side goes somewhere the node's way out does not was drawn */
			bool bDrawnBranch = false;

			for (const UEdGraphPin* Other : Otherwise) {
				if (!GoesWhereAsync(Other, Leaves)) bDrawnBranch = true;
			}

			if (bDrawnBranch) {
				UE_LOG(LogReflectionTidyAsyncTask, Warning, TEXT("\"%s\" starts \"%s\" and was left as it was read: the look to see it was made goes somewhere of its own when it was not"), *Graph->GetName(), *Named);

				continue;
			}

			UK2Node_BaseAsyncTask* Waits = NewObject<UK2Node_BaseAsyncTask>(Graph, DrawnAs(Called, Graph), NAME_None, RF_Transactional);

			Graph->AddNode(Waits, false, false);

			Waits->CreateNewGuid();
			Waits->PostPlacedNewNode();

			/* The guid the handlers were named after, so compiling the node names them the same again */
			if (Handlers.Num() > 0) {
				const FGuid Recovered = NodeNamedBy(Handlers[0]->GetFunctionName());

				if (Recovered.IsValid()) {
					Waits->NodeGuid = Recovered;
				} else {
					UE_LOG(LogReflectionTidyAsyncTask, Warning, TEXT("\"%s\" starts \"%s\": no node guid gives the handler name \"%s\", so it keeps a fresh one"),
						*Graph->GetName(), *Named, *Handlers[0]->GetFunctionName().ToString());
				}
			}

			SayTo<FNameProperty>(Waits, TEXT("ProxyFactoryFunctionName"), Called->GetFName());
			SayTo<FObjectProperty>(Waits, TEXT("ProxyFactoryClass"), Called->GetOwnerClass());
			SayTo<FObjectProperty>(Waits, TEXT("ProxyClass"), Task);

			/* Begun by whatever the script began it with, and by nothing where it began it with
			 * nothing. Left alone, the node keeps the word to begin its own kind starts with, and
			 * a task of another kind has nothing by that name to be called on. */
			SayTo<FNameProperty>(Waits, TEXT("ProxyActivateFunctionName"), Begins != nullptr && Begins->GetTargetFunction() != nullptr
				? Begins->GetTargetFunction()->GetFName()
				: FName(NAME_None));

			if (Waits->Pins.Num() == 0) Waits->AllocateDefaultPins();

			Waits->NodePosX = Makes->NodePosX;
			Waits->NodePosY = Makes->NodePosY;

			/* What ran into the making runs into the node, and what followed the word to begin
			 * follows it */
			TakeOverAsync(Entered, Waits->GetExecPin());
			TakeOverAsync(Leaves, Waits->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output));

			for (UEdGraphPin* Given : Makes->Pins) {
				if (Given == nullptr || Given->Direction != EGPD_Input) continue;
				if (Given->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

				UEdGraphPin* Takes = Waits->FindPin(Given->PinName, EGPD_Input);

				if (Takes == nullptr) continue;

				TakeOverAsync(Given, Takes);

				if (Given->LinkedTo.Num() == 0) {
					Takes->DefaultValue = Given->DefaultValue;
					Takes->DefaultObject = Given->DefaultObject;
					Takes->DefaultTextValue = Given->DefaultTextValue;
				}
			}

			/* And each way out, which is where its handler's run was */
			int32 Hung = 0;

			for (const TPair<FName, UK2Node_CustomEvent*>& Answer : Answers) {
				UEdGraphPin* Way = Waits->FindPin(Answer.Key, EGPD_Output);

				if (Way == nullptr) continue;

				for (UEdGraphPin* Held : Answer.Value->Pins) {
					if (Held == nullptr || Held->Direction != EGPD_Output) continue;

					if (Held->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) {
						TakeOverAsync(Held, Way);

						continue;
					}

					/* What the task says when it gets there, which the node hands out once however
					 * many ways out say it */
					TakeOverAsync(Held, Waits->FindPin(Held->PinName, EGPD_Output));
				}

				Hung++;
			}

			if (Hung == 0) {
				TArray<FString> Grew;

				for (const UEdGraphPin* Pin : Waits->Pins) {
					if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) Grew.Add(Pin->PinName.ToString());
				}

				UE_LOG(LogReflectionTidyAsyncTask, Warning, TEXT("\"%s\" starts \"%s\" and was left as it was read: the node grew no way out for any of them, only %s"),
					*Graph->GetName(), *Named, Grew.Num() > 0 ? *FString::Join(Grew, TEXT(", ")) : TEXT("none"));

				Graph->RemoveNode(Waits);

				continue;
			}

			for (const TPair<FName, UK2Node_CustomEvent*>& Answer : Answers) {
				Graph->RemoveNode(Answer.Value);
			}

			for (UK2Node_CreateDelegate* Laid : Handlers) {
				Graph->RemoveNode(Laid);
			}

			for (UK2Node_AddDelegate* Binding : Binds) {
				Graph->RemoveNode(Binding);
			}

			for (UK2Node* Held : Looks) {
				Graph->RemoveNode(Held);
			}

			if (Begins != nullptr) Graph->RemoveNode(Begins);
			if (SpawnStarts != nullptr) Graph->RemoveNode(SpawnStarts);
			if (SpawnFinishes != nullptr) Graph->RemoveNode(SpawnFinishes);

			Graph->RemoveNode(Makes);

			/* And whatever was worked out for them, where it was worked out for nothing else.
			 * Asked after the rest has gone, since until then it is still being read. */
			for (UK2Node* Works : Workings) {
				bool bWanted = false;

				for (const UEdGraphPin* Pin : Works->Pins) {
					if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0) bWanted = true;
				}

				if (!bWanted) Graph->RemoveNode(Works);
			}

			Put++;
		}

		if (Put > 0) {
			UE_LOG(LogReflectionTidyAsyncTask, Display, TEXT("\"%s\" had %d task(s) written as everything it takes to start one"), *Graph->GetName(), Put);
		}

		return Put;
	}
};

REGISTER_TIDY(FAsyncTaskTidy)
