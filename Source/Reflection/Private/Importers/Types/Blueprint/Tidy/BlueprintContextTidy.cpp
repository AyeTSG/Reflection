/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyContext, All, All);

namespace {
	/* The node this puts back, which is not in any engine.
	 *
	 * It comes from a game's own editor module, so it is asked for by name and everything set on it
	 * is set through the property system. A build without that module finds nothing and the tidying
	 * does nothing, which is the right answer: the call and the cast are what the bytecode says and
	 * they work. */
	UClass* ContextNodeClass() {
		static UClass* Kind = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintContextEditor.K2Node_GetBlueprintContext"));

		return Kind;
	}

	/* What the cast was asked to reach, which is what the node is for */
	UEdGraphPin* CastResult(UK2Node_DynamicCast* Cast) {
		for (UEdGraphPin* Pin : Cast->Pins) {
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object) return Pin;
		}

		return nullptr;
	}
}

/* A context asked for, and then asked what it is.
 *
 * A game that keeps its own per player state reaches it through one library call handed a class,
 * and the call is declared as giving back the base of them all. So whoever wrote it wrote one node,
 * and what the compiler wrote down is that node followed by a cast to the class they asked for,
 * because that is what the node does.
 *
 * Read back statement by statement the cast comes with it, and the graph gains a pair of nodes
 * saying what one said. Worse, the pair is not what the editor offers: the node the game's own
 * editor module gives has the class on the node itself, so a graph rebuilt as a call and a cast
 * cannot be edited into the shape it came from.
 *
 * The pair only ever appears together in this one shape, so it is put back into the one node. */
struct FBlueprintContextTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("BlueprintContext"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		UClass* Kind = ContextNodeClass();

		if (Kind == nullptr) return 0;

		FObjectProperty* Says = FindFProperty<FObjectProperty>(Kind, TEXT("CustomClass"));

		if (Says == nullptr) return 0;

		int32 Put = 0;

		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_CallFunction* Calling = Cast<UK2Node_CallFunction>(Node);

			if (Calling == nullptr) continue;

			if (Calling->FunctionReference.GetMemberName() != TEXT("GetContext")) continue;

			const UClass* Library = Calling->FunctionReference.GetMemberParentClass();

			if (Library == nullptr || Library->GetName() != TEXT("BlueprintContextLibrary")) continue;

			UEdGraphPin* Gives = Calling->GetReturnValuePin();

			if (Gives == nullptr || Gives->LinkedTo.Num() == 0) continue;

			/* Every cast that reads it, since the one call may be read by several.
			 *
			 * The base a context comes back as is worth nothing on its own, so whoever wrote two
			 * of these wrote a node each and the compiler wrote the one call they both work out
			 * from. Read back, that is one call with a cast hanging off it per node.
			 *
			 * Only where the casts are the whole of what reads it. Anything else holding the base
			 * still wants the call, and taking it out would leave them reading nothing. */
			TArray<UK2Node_DynamicCast*> Casts;

			bool bOnlyCasts = true;

			for (UEdGraphPin* Reader : Gives->LinkedTo) {
				UK2Node_DynamicCast* Held = Reader != nullptr ? Cast<UK2Node_DynamicCast>(Reader->GetOwningNode()) : nullptr;

				/* A cast that is asked whether it worked is a cast somebody wrote, and it is
				 * answering a question the one node cannot */
				if (Held == nullptr || Held->TargetType == nullptr || !Held->IsNodePure()) {
					bOnlyCasts = false;

					break;
				}

				UEdGraphPin* Worked = Held->GetBoolSuccessPin();

				if (Worked != nullptr && Worked->LinkedTo.Num() > 0) {
					bOnlyCasts = false;

					break;
				}

				if (CastResult(Held) == nullptr) {
					bOnlyCasts = false;

					break;
				}

				Casts.Add(Held);
			}

			if (!bOnlyCasts || Casts.Num() == 0) continue;

			int32 Stood = 0;

			for (UK2Node_DynamicCast* Casting : Casts) {
				UEdGraphNode* Made = NewObject<UEdGraphNode>(Graph, Kind);

				Graph->AddNode(Made, false, false);

				Made->CreateNewGuid();
				Made->PostPlacedNewNode();

				Made->NodePosX = Calling->NodePosX;
				Made->NodePosY = Casting->NodePosY;

				/* Said before the pins are made, since the pin it hands out is of whatever this says */
				Says->SetObjectPropertyValue_InContainer(Made, Casting->TargetType);

				Made->AllocateDefaultPins();

				UEdGraphPin* Hands = nullptr;

				for (UEdGraphPin* Pin : Made->Pins) {
					if (Pin->Direction == EGPD_Output) { Hands = Pin; break; }
				}

				UEdGraphPin* Reached = CastResult(Casting);

				if (Hands == nullptr || Reached == nullptr) {
					Graph->RemoveNode(Made);

					continue;
				}

				for (UEdGraphPin* Reader : Reached->LinkedTo) {
					Hands->MakeLinkTo(Reader);
				}

				Reached->BreakAllPinLinks();

				Graph->RemoveNode(Casting);

				Stood++;
			}

			/* And the call goes once nothing is left reading it */
			if (Stood == Casts.Num()) Graph->RemoveNode(Calling);

			Put += Stood;
		}

		if (Put > 0) {
			UE_LOG(LogReflectionTidyContext, Display, TEXT("\"%s\" had %d context(s) asked for and cast in two nodes where the editor draws one"), *Graph->GetName(), Put);
		}

		return Put;
	}
};

REGISTER_TIDY(FBlueprintContextTidy)
