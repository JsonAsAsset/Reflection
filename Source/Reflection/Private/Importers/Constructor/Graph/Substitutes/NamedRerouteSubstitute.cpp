/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/MaterialNodeSubstitute.h"

#include "Materials/MaterialExpressionReroute.h"

#include "Utilities/JsonHelpers.h"

static const TCHAR* NamedRerouteDeclarationType = TEXT("MaterialExpressionNamedRerouteDeclaration");
static const TCHAR* NamedRerouteUsageType = TEXT("MaterialExpressionNamedRerouteUsage");

/* The name a declaration goes by in the graph, which is what a usage is reading when it names it */
static FString GetNamedRerouteName(const TSharedPtr<FJsonObject>& Properties) {
	FString Name;

	if (Properties.IsValid()) {
		Properties->TryGetStringField(TEXT("Name"), Name);
	}

	return Name;
}

/* A usage and its declaration are paired by guid rather than by a wire, so DeclarationGuid against
 * the declaration's VariableGuid is the link that is still there when the object reference is not. */
static UMaterialExpression* FindNamedRerouteDeclarationByGuid(FUObjectExportContainer* Container, const TSharedPtr<FJsonObject>& UsageProperties) {
	FString DeclarationGuid;

	if (!UsageProperties->TryGetStringField(TEXT("DeclarationGuid"), DeclarationGuid) || DeclarationGuid.IsEmpty()) {
		return nullptr;
	}

	for (FUObjectExport* Export : Container->Exports) {
		if (Export->GetType() != FName(NamedRerouteDeclarationType)) {
			continue;
		}

		const TSharedPtr<FJsonObject> Properties = Export->GetProperties();
		FString VariableGuid;

		if (Properties.IsValid() && Properties->TryGetStringField(TEXT("VariableGuid"), VariableGuid) && VariableGuid == DeclarationGuid) {
			return Export->Get<UMaterialExpression>();
		}
	}

	return nullptr;
}

/* Named reroutes arrived in 5.0. A declaration is a reroute with a name on it, and a usage is a
 * read of that name from anywhere else in the graph, the two joined by a guid instead of a wire.
 * Both collapse onto the plain reroute, which every engine has: what a named reroute adds over one
 * is a way to carry a wire across the graph without drawing it, and that is presentation.
 *
 * The declaration keeps its own connection for free. Its input is spelled Input on both classes,
 * so the property serializer wires it without being told. The usage has no input to fill in, only
 * a declaration to find, which is put off until the graph is whole. */
class FNamedRerouteSubstitute final : public FMaterialNodeSubstitute {
public:
	virtual bool Handles(const FName Type) const override {
		return Type == FName(NamedRerouteDeclarationType) || Type == FName(NamedRerouteUsageType);
	}

	virtual UMaterialExpression* Create(FMaterialSubstituteContext& Context) override {
		const FName Type = Context.GetType();
		const TSharedPtr<FJsonObject> Properties = Context.GetProperties();

		UMaterialExpressionReroute* Reroute = NewObject<UMaterialExpressionReroute>(
			Context.GetParent(),
			UMaterialExpressionReroute::StaticClass(),
			Context.GetName(),
			RF_Transactional
		);

		if (Type == FName(NamedRerouteDeclarationType)) {
			/* Nothing else carries the name once the class is gone, and a graph full of anonymous
			 * reroutes is not one anybody can read */
			const FString Name = GetNamedRerouteName(Properties);

			if (!Name.IsEmpty()) {
				Reroute->Desc = Name;
			}
		} else {
			/* The declaration may not have an object yet, so the wire waits for the whole container */
			Context.Defer([Reroute, Properties](FMaterialSubstituteContext& DeferredContext) {
				ResolveUsage(DeferredContext, Reroute, Properties);
			});
		}

		GLog->Log(*FString::Printf(TEXT("Reflection: Rebuilt %s \"%s\" as a Reroute"), *Type.ToString(), *Context.GetBaseName()));

		return Reroute;
	}

private:
	static void ResolveUsage(FMaterialSubstituteContext& Context, UMaterialExpressionReroute* Usage, const TSharedPtr<FJsonObject>& Properties) {
		if (Usage == nullptr || !Properties.IsValid()) {
			return;
		}

		FUObjectExportContainer* Container = Context.GetContainer();
		UMaterialExpression* Declaration = nullptr;

		/* The reference to the declaration, for the exports that still carry one */
		const TSharedPtr<FJsonObject>* DeclarationObject;

		if (Properties->TryGetObjectField(TEXT("Declaration"), DeclarationObject)) {
			FString ObjectName;

			if ((*DeclarationObject)->TryGetStringField(TEXT("ObjectName"), ObjectName)) {
				Declaration = Container->Find<UMaterialExpression>(GetExportNameOfSubobject(ObjectName));
			}
		}

		/* Otherwise the guid, which is the link the pair is really kept by */
		if (Declaration == nullptr) {
			Declaration = FindNamedRerouteDeclarationByGuid(Container, Properties);
		}

		/* A usage whose declaration is nowhere in this graph is left unconnected, which is the
		 * same thing an unresolved reference leaves behind everywhere else */
		if (Declaration == nullptr) {
			GLog->Log(*FString::Printf(TEXT("Reflection: Named reroute usage \"%s\" names a declaration this graph does not have"), *Usage->GetName()));

			return;
		}

		Usage->Input.Expression = Declaration;
		Usage->Input.OutputIndex = 0;

		/* Reads the same name it points at, so both ends of the pair say so */
		Usage->Desc = Declaration->Desc;
	}
};

REGISTER_MATERIAL_NODE_SUBSTITUTE(FNamedRerouteSubstitute);
