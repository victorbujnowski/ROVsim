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

	/** Resultado da análise de uma oscilação vertical medida no smoke test */
	struct FOscillation
	{
		float PeriodS = 0.f;     // média do tempo entre cruzamentos do zero (subindo)
		float AmplitudeM = 0.f;  // metade de (máximo - mínimo), já sem a deriva
		float MeanDepthM = 0.f;
		int32 Crossings = 0;
	};
	static FOscillation AnalyzeOscillation(const TArray<FVector2D>& Samples);

	// Amostras (tempo, profundidade) gravadas a cada frame enquanto bSwellSampling está ligado
	bool bSwellSampling = false;
	TArray<FVector2D> SwellSamples;
	FOscillation SwellDeep;

	// Station keeping: desvio do alvo (m, Norte/Leste) e empuxo de surge/sway (%) a cada frame
	bool bHoldSampling = false;
	TArray<FVector2D> HoldOffsets;
	TArray<FVector2D> HoldThrust;

	bool bSmokeTest = false;
	int32 SmokeStep = 0;
	float SmokeStartTime = 0.f;
	float NextLogTime = 0.f;
};
