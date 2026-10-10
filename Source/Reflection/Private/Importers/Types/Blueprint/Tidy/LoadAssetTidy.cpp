/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_LoadAsset.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/Crc.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyLoadAsset, All, All);

namespace {
	/* The node a callback was named after: OnLoaded_ and its guid, with the last quarter swapped for a checksum of the whole */
	bool NamedAfter(const FName Called, FGuid& Out) {
		FString Spelled = Called.ToString();

		if (!Spelled.RemoveFromStart(TEXT("OnLoaded_"))) return false;

		FGuid Said;

		if (!FGuid::ParseExact(Spelled, EGuidFormats::Digits, Said)) return false;

		uint32 Table[256];

		for (uint32 Index = 0; Index < 256; ++Index) {
			uint32 Value = Index;

			for (int32 Bit = 0; Bit < 8; ++Bit) {
				Value = (Value & 1) != 0 ? (Value >> 1) ^ 0xEDB88320u : Value >> 1;
			}

			Table[Index] = Value;
		}

		/* As far as the checksum gets through the part that was kept */
		const uint32 Kept[3] = { Said.A, Said.B, Said.C };
		const uint8* Read = reinterpret_cast<const uint8*>(Kept);

		uint32 State = 0xFFFFFFFFu;

		for (int32 Index = 0; Index < 12; ++Index) {
			State = Table[(State ^ Read[Index]) & 0xFF] ^ (State >> 8);
		}

		/* Which row each of the last four bytes picked, read from the end, since every row starts differently */
		uint8 Rows[4];
		uint32 Left = ~Said.D;

		for (int32 Step = 3; Step >= 0; --Step) {
			int32 Row = INDEX_NONE;

			for (int32 Index = 0; Index < 256; ++Index) {
				if ((Table[Index] >> 24) == (Left >> 24)) {
					Row = Index;

					break;
				}
			}

			if (Row == INDEX_NONE) return false;

			Rows[Step] = static_cast<uint8>(Row);
			Left = (Left ^ Table[Row]) << 8;
		}

		/* And the bytes that pick them, from the start */
		uint8 Last[4];

		for (int32 Step = 0; Step < 4; ++Step) {
			Last[Step] = static_cast<uint8>((State ^ Rows[Step]) & 0xFF);
			State = Table[Rows[Step]] ^ (State >> 8);
		}

		uint32 D = 0;
		FMemory::Memcpy(&D, Last, sizeof(D));

		const FGuid Was(Said.A, Said.B, Said.C, D);

		/* Asked of the engine's own checksum, which is the one the name was made with */
		if (FCrc::MemCrc32(&Was, sizeof(Was)) != Said.D) return false;

		Out = Was;

		return true;
	}

	/* Everything reaching one pin, reaching another instead */
	void TakeOverLoaded(UEdGraphPin* From, UEdGraphPin* To) {
		if (From == nullptr || To == nullptr) return;

		for (UEdGraphPin* Held : From->LinkedTo) {
			if (Held != nullptr) To->MakeLinkTo(Held);
		}
	}

	/* A pin by what it carries rather than by what it is called, where there is only one of them */
	UEdGraphPin* TheOnly(const UEdGraphNode* Node, const EEdGraphPinDirection Direction, const FName Carrying) {
		UEdGraphPin* Found = nullptr;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != Direction || Pin->PinType.PinCategory != Carrying) continue;
			if (Found != nullptr) return nullptr;

			Found = Pin;
		}

		return Found;
	}

	/* What the node hands its value in or out through, which is its one pin that is neither the
	 * run nor a function stood in for */
	UEdGraphPin* Carries(const UEdGraphNode* Node, const EEdGraphPinDirection Direction) {
		UEdGraphPin* Found = nullptr;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin == nullptr || Pin->Direction != Direction) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate) continue;

			if (Found != nullptr) return nullptr;

			Found = Pin;
		}

		return Found;
	}

	/* The event a delegate was laid down to stand for */
	UK2Node_CustomEvent* AnsweringTo(const UEdGraph* Graph, const FName Called) {
		for (UEdGraphNode* Held : Graph->Nodes) {
			UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Held);

			if (Event != nullptr && Event->CustomFunctionName == Called) return Event;
		}

		return nullptr;
	}
}

/* Asking for something to be loaded and carrying on once it is.
 *
 * One node in a graph, and three things in the script. Loading takes as long as it takes, so there
 * is nothing for the run to wait on: what the node does is hand the loader a function to call back,
 * and everything drawn after Completed is written into that function instead.
 *
 *     a delegate made to stand for OnLoaded_<something>
 *     LoadAsset, given the asset and that delegate
 *     OnLoaded_<something>, which takes the loaded thing and runs the rest
 *
 * Read back as it is written that is a Create Event, a call, and an event of its own sitting apart
 * from everything, with the run seeming to stop at the call. The three only ever appear together in
 * that shape, so the one can be put back and the run reads straight through again.
 *
 * The same goes for loading a class, which is the same node asking for a different kind of thing. */
struct FLoadAssetTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("LoadAsset"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 PutBack = 0;

		/* Over a copy, since the graph is being taken apart as it is read */
		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_CallFunction* Asking = Cast<UK2Node_CallFunction>(Node);

			if (Asking == nullptr) continue;
			if (Asking->FunctionReference.GetMemberParentClass() != UKismetSystemLibrary::StaticClass()) continue;

			/* What each of the two nodes compiles down to, which is the only thing telling them
			 * apart from this side */
			const FName Called = Asking->FunctionReference.GetMemberName();
			UClass* Drawn = nullptr;

			if (Called == TEXT("LoadAsset")) Drawn = UK2Node_LoadAsset::StaticClass();
			else if (Called == TEXT("LoadAssetClass")) Drawn = UK2Node_LoadAssetClass::StaticClass();
			else continue;

			/* The function it is given to call back, laid down for it and for nothing else */
			UEdGraphPin* Handed = TheOnly(Asking, EGPD_Input, UEdGraphSchema_K2::PC_Delegate);

			if (Handed == nullptr || Handed->LinkedTo.Num() != 1) continue;

			UK2Node_CreateDelegate* Laid = Cast<UK2Node_CreateDelegate>(Handed->LinkedTo[0]->GetOwningNode());

			if (Laid == nullptr) continue;

			UEdGraphPin* Stands = Laid->GetDelegateOutPin();

			if (Stands == nullptr || Stands->LinkedTo.Num() != 1) continue;

			UK2Node_CustomEvent* Answers = AnsweringTo(Graph, Laid->GetFunctionName());

			if (Answers == nullptr) {
				UE_LOG(LogReflectionTidyLoadAsset, Warning, TEXT("\"%s\" loads something and was left as it was read: it calls back \"%s\", which no event in this graph answers to"),
					*Graph->GetName(), *Laid->GetFunctionName().ToString());

				continue;
			}

			UEdGraphPin* Entered = TheOnly(Asking, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
			UEdGraphPin* After = Asking->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* Done = Answers->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* Gives = Carries(Answers, EGPD_Output);

			if (Entered == nullptr || Done == nullptr) continue;

			UEdGraphNode* Loads = NewObject<UEdGraphNode>(Graph, Drawn);

			Graph->AddNode(Loads, false, false);

			Loads->CreateNewGuid();

			/* Given back the guid the game's one had, so what it calls back is named what it was */
			if (FGuid Was; NamedAfter(Laid->GetFunctionName(), Was)) {
				Loads->NodeGuid = Was;
			} else {
				UE_LOG(LogReflectionTidyLoadAsset, Warning, TEXT("\"%s\" loads something whose callback \"%s\" says no node it was named after, so it calls back one of its own"),
					*Graph->GetName(), *Laid->GetFunctionName().ToString());
			}

			Loads->PostPlacedNewNode();
			Loads->AllocateDefaultPins();

			/* Where the call stood, since that is where the run reaches it */
			Loads->NodePosX = Asking->NodePosX;
			Loads->NodePosY = Asking->NodePosY;

			UEdGraphPin* Runs = TheOnly(Loads, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
			UEdGraphPin* Carries_On = Loads->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* Completed = Loads->FindPin(UEdGraphSchema_K2::PN_Completed, EGPD_Output);
			UEdGraphPin* Wanted = Carries(Loads, EGPD_Input);
			UEdGraphPin* Loaded = Carries(Loads, EGPD_Output);

			if (Runs == nullptr || Completed == nullptr || Wanted == nullptr || Loaded == nullptr) {
				UE_LOG(LogReflectionTidyLoadAsset, Warning, TEXT("\"%s\" loads something and was left as it was read: the node grew no way to say %s"),
					*Graph->GetName(), Wanted == nullptr ? TEXT("what to load") : Loaded == nullptr ? TEXT("what was loaded") : TEXT("when it is done"));

				Graph->RemoveNode(Loads);

				continue;
			}

			/* What it was told to load, named the same way on both since one grew from the other */
			if (UEdGraphPin* Named = Asking->FindPin(Wanted->PinName, EGPD_Input)) {
				TakeOverLoaded(Named, Wanted);

				Wanted->DefaultValue = Named->DefaultValue;
				Wanted->DefaultObject = Named->DefaultObject;
			}

			TakeOverLoaded(Entered, Runs);
			TakeOverLoaded(After, Carries_On);
			TakeOverLoaded(Done, Completed);
			TakeOverLoaded(Gives, Loaded);

			Graph->RemoveNode(Answers);
			Graph->RemoveNode(Laid);
			Graph->RemoveNode(Asking);

			PutBack++;
		}

		if (PutBack > 0) {
			UE_LOG(LogReflectionTidyLoadAsset, Display, TEXT("\"%s\" had %d load(s) written as everything it takes to ask for one"), *Graph->GetName(), PutBack);
		}

		return PutBack;
	}
};

REGISTER_TIDY(FLoadAssetTidy)
