/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Validation/ValidatorRegistry.h"

/* Validation is UE5 only */
#if ENGINE_UE5

#include "Engine/Log.h"

/* Registration order is the order the dropdown lists them in, and the first one is what the
 * toolbar button runs, so this stays an array rather than a map */
static TArray<FReflectionValidator> GRegisteredValidators;

void FReflectionValidatorRegistry::Register(const FReflectionValidator& Validator) {
	if (Validator.Id.IsNone()) {
		UE_LOG(LogReflection, Warning, TEXT("Refused to register a validator without an Id."));

		return;
	}

	const int32 ExistingIndex = GRegisteredValidators.IndexOfByPredicate([&Validator](const FReflectionValidator& Existing) {
		return Existing.Id == Validator.Id;
	});

	/* Re-registering keeps the original position, so a hot reload doesn't shuffle the dropdown */
	if (ExistingIndex != INDEX_NONE) {
		GRegisteredValidators[ExistingIndex] = Validator;

		return;
	}

	GRegisteredValidators.Add(Validator);
}

const TArray<FReflectionValidator>& FReflectionValidatorRegistry::GetValidators() {
	return GRegisteredValidators;
}

const FReflectionValidator* FReflectionValidatorRegistry::Find(const FName Id) {
	return GRegisteredValidators.FindByPredicate([Id](const FReflectionValidator& Validator) {
		return Validator.Id == Id;
	});
}

void FReflectionValidatorRegistry::Open(const FName Id) {
	const FReflectionValidator* Validator = Find(Id);

	if (Validator != nullptr && Validator->OnOpen.IsBound()) {
		Validator->OnOpen.Execute();
	}
}

void FReflectionValidatorRegistry::OpenDefault() {
	if (GRegisteredValidators.Num() == 0) {
		return;
	}

	const FReflectionValidator& Validator = GRegisteredValidators[0];

	if (Validator.OnOpen.IsBound()) {
		Validator.OnOpen.Execute();
	}
}

#endif
