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

			/* Only where the cast is the whole of what reads it. Anything else holding the base
			 * still wants the call, and taking it out would leave them reading nothing. */
			if (Gives == nullptr || Gives->LinkedTo.Num() != 1) continue;

			UK2Node_DynamicCast* Casting = Cast<UK2Node_DynamicCast>(Gives->LinkedTo[0]->GetOwningNode());

			if (Casting == nullptr || Casting->TargetType == nullptr) continue;

			/* A cast that is asked whether it worked is a cast somebody wrote, and it is answering
			 * a question the one node cannot */
			if (!Casting->IsNodePure()) continue;

			UEdGraphPin* Worked = Casting->GetBoolSuccessPin();

			if (Worked != nullptr && Worked->LinkedTo.Num() > 0) continue;

			UEdGraphPin* Reached = CastResult(Casting);

			if (Reached == nullptr) continue;

			UEdGraphNode* Made = NewObject<UEdGraphNode>(Graph, Kind);

			Graph->AddNode(Made, false, false);

			Made->CreateNewGuid();
			Made->PostPlacedNewNode();

			Made->NodePosX = Calling->NodePosX;
			Made->NodePosY = Calling->NodePosY;

			/* Said before the pins are made, since the pin it hands out is of whatever this says */
			Says->SetObjectPropertyValue_InContainer(Made, Casting->TargetType);

			Made->AllocateDefaultPins();

			UEdGraphPin* Hands = nullptr;

			for (UEdGraphPin* Pin : Made->Pins) {
				if (Pin->Direction == EGPD_Output) { Hands = Pin; break; }
			}

			if (Hands == nullptr) {
				Graph->RemoveNode(Made);

				continue;
			}

			for (UEdGraphPin* Reader : Reached->LinkedTo) {
				Hands->MakeLinkTo(Reader);
			}

			Reached->BreakAllPinLinks();

			Graph->RemoveNode(Casting);
			Graph->RemoveNode(Calling);

			Put++;
		}

		if (Put > 0) {
			UE_LOG(LogReflectionTidyContext, Display, TEXT("\"%s\" had %d context(s) asked for and cast in two nodes where the editor draws one"), *Graph->GetName(), Put);
		}

		return Put;
	}
};

REGISTER_TIDY(FBlueprintContextTidy)
