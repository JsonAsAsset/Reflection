/* Copyright Reflection Contributors 2024-2026 */

/* Every class and struct this engine knows, written out as the properties it counts through:
 *
 *     -run=SchemaDump -out=<file>
 *
 * Mappings dumped from a game keep only what a build without editor data reflects, and a package
 * saved by the editor counts through everything the class really has. The two disagree from the
 * first editor-only property onwards, and every number after it lands on the wrong property.
 *
 * What the engine has in front of it is the whole class, so it is asked rather than guessed at. The
 * mappings are never touched: what comes out of here is read beside them and put over the top in
 * memory, so a class the dump is short of is completed and everything else is left as it was. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "SchemaDumpCommandlet.generated.h"

UCLASS()
class USchemaDumpCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
