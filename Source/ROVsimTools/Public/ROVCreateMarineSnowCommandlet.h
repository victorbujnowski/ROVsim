#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ROVCreateMarineSnowCommandlet.generated.h"

/**
 * Cria (ou recria) o sistema Niagara de neve marinha: /Game/ROV/FX/NS_MarineSnow.
 *
 * Commandlet = programa que roda DENTRO da Unreal, sem abrir a janela do editor. Serve para
 * automatizar trabalho de editor (criar/converter assets) de forma repetível:
 *
 *   UnrealEditor-Cmd.exe ROVsim.uproject -run=ROVCreateMarineSnow
 *
 * Parte do template de emissor "HangingParticulates" da engine (partículas suspensas que flutuam
 * devagar) e ajusta quantidade, tamanho, cor e espaço do mundo para parecer neve marinha.
 */
UCLASS()
class UROVCreateMarineSnowCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
