#include "ROVCreateMarineSnowCommandlet.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogROVTools, Log, All);

namespace
{
	const TCHAR* TemplatePath = TEXT("/Niagara/DefaultAssets/Templates/Emitters/HangingParticulates.HangingParticulates");
	const TCHAR* PackagePath = TEXT("/Game/ROV/FX/NS_MarineSnow");
	const TCHAR* AssetName = TEXT("NS_MarineSnow");

	/** Lista os parâmetros ajustáveis ("rapid iteration") de um script: é o que aparece nos módulos do editor Niagara */
	void LogParameters(const UNiagaraScript* Script)
	{
		if (!Script)
		{
			return;
		}
		TArray<FNiagaraVariable> Parameters;
		Script->RapidIterationParameters.GetParameters(Parameters);
		for (const FNiagaraVariable& Parameter : Parameters)
		{
			FString Value;
			const FNiagaraTypeDefinition& Type = Parameter.GetType();
			if (Type == FNiagaraTypeDefinition::GetFloatDef())
			{
				float V = 0.f; Script->RapidIterationParameters.GetParameterValue(V, Parameter); Value = FString::SanitizeFloat(V);
			}
			else if (Type == FNiagaraTypeDefinition::GetIntDef())
			{
				int32 V = 0; Script->RapidIterationParameters.GetParameterValue(V, Parameter); Value = FString::FromInt(V);
			}
			else if (Type == FNiagaraTypeDefinition::GetVec3Def())
			{
				FVector3f V; Script->RapidIterationParameters.GetParameterValue(V, Parameter); Value = V.ToString();
			}
			else if (Type == FNiagaraTypeDefinition::GetVec2Def())
			{
				FVector2f V; Script->RapidIterationParameters.GetParameterValue(V, Parameter); Value = V.ToString();
			}
			else if (Type == FNiagaraTypeDefinition::GetColorDef())
			{
				FLinearColor V; Script->RapidIterationParameters.GetParameterValue(V, Parameter); Value = V.ToString();
			}
			UE_LOG(LogROVTools, Verbose, TEXT("  [%s] %s (%s) = %s"),
				*Script->GetName(), *Parameter.GetName().ToString(), *Type.GetName(), *Value);
		}
	}

	/**
	 * Muda o valor de uma entrada de módulo (o mesmo que digitar no painel do editor Niagara).
	 * O nome completo é "Constants.<Emissor>.<Módulo>.<Entrada>"; procuramos pelo final ".<Módulo>.<Entrada>"
	 * em todos os scripts do emissor (spawn e update têm cópias próprias). Devolve quantas trocou.
	 */
	template <typename T>
	int32 SetInput(const TArray<UNiagaraScript*>& Scripts, const FString& ModuleAndInput, const T& Value)
	{
		int32 Changed = 0;
		for (UNiagaraScript* Script : Scripts)
		{
			TArray<FNiagaraVariable> Parameters;
			Script->RapidIterationParameters.GetParameters(Parameters);
			for (const FNiagaraVariable& Parameter : Parameters)
			{
				if (Parameter.GetName().ToString().EndsWith(TEXT(".") + ModuleAndInput))
				{
					Script->RapidIterationParameters.SetParameterValue(Value, Parameter);
					++Changed;
				}
			}
		}
		UE_LOG(LogROVTools, Display, TEXT("  %-45s %s"), *ModuleAndInput,
			Changed > 0 ? *FString::Printf(TEXT("ok (%d)"), Changed) : TEXT("NAO ENCONTRADO"));
		return Changed;
	}
}

int32 UROVCreateMarineSnowCommandlet::Main(const FString& Params)
{
	// 1) Template da engine
	UNiagaraEmitter* Template = LoadObject<UNiagaraEmitter>(nullptr, TemplatePath);
	if (!Template)
	{
		UE_LOG(LogROVTools, Error, TEXT("Template não encontrado: %s"), TemplatePath);
		return 1;
	}

	// 2) Pacote e sistema novos (Standalone + Public: o asset existe sozinho e pode ser referenciado)
	UPackage* Package = CreatePackage(PackagePath);
	Package->FullyLoad();
	// Rodando de novo: tira o asset antigo do caminho (senão NewObject com o mesmo nome falha)
	if (UObject* Old = StaticFindObject(UNiagaraSystem::StaticClass(), Package, AssetName))
	{
		Old->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	}
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, AssetName, RF_Public | RF_Standalone | RF_Transactional);
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);

	// 3) Copia o emissor do template para dentro do sistema
	FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Template, Template->GetExposedVersion().VersionGuid);

	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Data)
		{
			continue;
		}
		// Espaço do MUNDO: a partícula fica parada na água enquanto o ROV passa por ela (é isso que dá a
		// sensação de movimento). Em espaço local, a "neve" andaria grudada na câmera.
		Data->bLocalSpace = false;

		TArray<UNiagaraScript*> Scripts;
		Data->GetScripts(Scripts, false);
		for (UNiagaraScript* Script : Scripts)
		{
			LogParameters(Script); // -LogCmds="LogROVTools Verbose" mostra todas as entradas e valores
		}

		// Neve marinha: flocos de matéria orgânica (mm a cm) suspensos, afundando muito devagar.
		// Ficam numa caixa de 24 x 24 x 14 m em volta da câmera (em cm, unidade da Unreal), ~2 flocos por m³.
		UE_LOG(LogROVTools, Display, TEXT("Emissor %s: ajustes de neve marinha"), *Handle.GetName().ToString());
		SetInput(Scripts, TEXT("ShapeLocation.Box Size"), FVector3f(2400.f, 2400.f, 1400.f));
		SetInput(Scripts, TEXT("InitializeParticle.Lifetime Min"), 20.f); // vida longa: o floco "mora" na água
		SetInput(Scripts, TEXT("InitializeParticle.Lifetime Max"), 30.f);
		SetInput(Scripts, TEXT("SpawnRate.SpawnRate"), 400.f);           // 400/s x 25 s ≈ 10.000 flocos
		SetInput(Scripts, TEXT("InitializeParticle.Uniform Sprite Size Min"), 1.5f); // cm (menor que isso some na câmera externa, a 9 m: 1 pixel ≈ 1,4 cm)
		SetInput(Scripts, TEXT("InitializeParticle.Uniform Sprite Size Max"), 3.5f);
		SetInput(Scripts, TEXT("InitializeParticle.Color"), FLinearColor(0.75f, 0.78f, 0.72f, 0.8f)); // branco-acinzentado
		SetInput(Scripts, TEXT("WindForce.Wind Speed"), FVector3f(0.f, 0.f, -1.f)); // afunda ~1 cm/s, sem vento lateral
		SetInput(Scripts, TEXT("WindForce.Scale"), 1.f);                 // turbulência suave (era 4)
		SetInput(Scripts, TEXT("WindForce.Offset By Time"), FVector3f(0.f, 0.f, -5.f));

		for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			UE_LOG(LogROVTools, Display, TEXT("Renderer: %s"), *GetNameSafe(Renderer));
		}
	}

	// 4) Compila e salva
	System->RequestCompile(false);
	System->WaitForCompilationComplete();
	FAssetRegistryModule::AssetCreated(System);
	Package->MarkPackageDirty();

	const FString FileName = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, System, *FileName, SaveArgs))
	{
		UE_LOG(LogROVTools, Error, TEXT("Falha ao salvar %s"), *FileName);
		return 1;
	}
	UE_LOG(LogROVTools, Display, TEXT("Criado: %s"), *FileName);
	return 0;
}
