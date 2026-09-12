/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyByteEnum, All, All);

namespace {
	/* The enum a byte pin is really of, which is either said on the pin or said by whatever is
	 * wired into it */
	UEnum* EnumOn(const UEdGraphPin* Pin) {
		if (Pin == nullptr || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Byte) return nullptr;

		if (UEnum* Said = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get())) return Said;

		for (const UEdGraphPin* Linked : Pin->LinkedTo) {
			if (Linked == nullptr) continue;

			if (UEnum* Held = Cast<UEnum>(Linked->PinType.PinSubCategoryObject.Get())) return Held;
		}

		return nullptr;
	}

	/* Nothing wired in and a number sat on it, which is a value somebody picked */
	bool IsPicked(const UEdGraphPin* Pin) {
		return Pin != nullptr
			&& Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte
			&& Pin->PinType.PinSubCategoryObject.Get() == nullptr
			&& Pin->LinkedTo.Num() == 0
			&& !Pin->DefaultValue.IsEmpty()
			&& Pin->DefaultValue.IsNumeric();
	}
}

/* An enum compared against, which the script keeps as the number it is worth.
 *
 * A byte is a byte to the machine. The enum lives on the pin, and a pin that had one is written
 * into the script as the plain number, so a call taking two of them comes back with the enum on
 * the side something is wired into and a bare number on the side nobody wired.
 *
 * Read like that the graph says the same thing and reads like nothing anybody would draw: the value
 * shows as a number with no name on it, and picking a different one means knowing what the numbers
 * stand for. What the other side is of is what this one is of, so it is said there too. */
struct FByteEnumTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("ByteEnum"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 Named = 0;

		for (UEdGraphNode* Node : Graph->Nodes) {
			UK2Node_CallFunction* Calling = Cast<UK2Node_CallFunction>(Node);

			if (Calling == nullptr) continue;

			UEnum* Enum = nullptr;

			bool bAgreed = true;

			for (const UEdGraphPin* Pin : Calling->Pins) {
				if (Pin == nullptr || Pin->Direction != EGPD_Input) continue;

				UEnum* Held = EnumOn(Pin);

				if (Held == nullptr) continue;

				/* Two different ones is a call whose bytes have nothing to do with each other, and
				 * there is no saying which of them the number was meant as */
				if (Enum != nullptr && Enum != Held) bAgreed = false;

				Enum = Held;
			}

			if (Enum == nullptr || !bAgreed) continue;

			for (UEdGraphPin* Pin : Calling->Pins) {
				if (!IsPicked(Pin)) continue;

				const FString Says = Enum->GetNameStringByValue(FCString::Atoi64(*Pin->DefaultValue));

				/* A number the enum has no entry for is not one of its values, whatever the pin
				 * next to it is of */
				if (Says.IsEmpty()) continue;

				Pin->PinType.PinSubCategoryObject = Enum;
				Pin->DefaultValue = Says;

				Named++;
			}
		}

		if (Named > 0) {
			UE_LOG(LogReflectionTidyByteEnum, Display, TEXT("\"%s\" had %d value(s) compared as a number where what they are of says a name"), *Graph->GetName(), Named);
		}

		return Named;
	}
};

REGISTER_TIDY(FByteEnumTidy)
