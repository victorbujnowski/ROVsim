#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ROVGameMode.generated.h"

class AROVPawn;

/**
 * GameMode do simulador: define o HUD de telemetria.
 *
 * Também contém um smoke test automatizado. Rodando o jogo com o parâmetro
 * "-ROVSmokeTest", ele pilota o ROV sozinho pelos dois modos e pelas duas câmeras,
 * registra a telemetria no log (LogROV), tira screenshots e fecha o jogo.
 */
UCLASS()
class ROVSIM_API AROVGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AROVGameMode();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	AROVPawn* FindROV() const;
	void LogTelemetry(const TCHAR* Label) const;
	void TakeScreenshot(const FString& Name) const;

	bool bSmokeTest = false;
	int32 SmokeStep = 0;
	float SmokeStartTime = 0.f;
	float NextLogTime = 0.f;
};
