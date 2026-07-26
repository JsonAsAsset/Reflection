/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "EngineFactory.generated.h"

/* Engine implementation to allow importing by dragging files into the Content Browser */
UCLASS()
class REFLECTION_API UREngineImplementation : public UFactory {
	GENERATED_BODY()
public:
	UREngineImplementation();

public:
	static UObject* Import(const FString& Filename);
	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual UObject* FactoryCreateFile(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, const FString& Filename, const TCHAR* Params, FFeedbackContext* Warn, bool& bOutOperationCanceled) override;
};