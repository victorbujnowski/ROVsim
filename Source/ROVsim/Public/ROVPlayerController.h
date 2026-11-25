#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ROVPlayerController.generated.h"

class AROVPawn;

/**
 * PlayerController do ROVsim.
 *
 * No multiplayer existem dois papéis:
 *   PILOTO      o jogador do servidor (host): possui o ROV e o controla.
 *   OBSERVADOR  quem conecta depois (supervisor, cliente, instrutor): não possui pawn; a câmera dele
 *               olha pelo ROV que o servidor simula, e o HUD mostra a telemetria replicada.
 *
 * Com "-ROVObserverTest[=segundos]" na linha de comando, o observador registra no log o que está
 * recebendo, tira screenshots e fecha sozinho (teste automático de rede).
 */
UCLASS()
class ROVSIM_API AROVPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AROVPlayerController();

	virtual void PlayerTick(float DeltaTime) override;

	/** true se este jogador só observa (não possui o ROV) */
	bool IsObserver() const { return GetPawn() == nullptr; }

	/** O ROV da partida (o próprio pawn, ou o do servidor para um observador) */
	AROVPawn* FindROV() const;

private:
	void TickObserverTest(AROVPawn* ROV);

	bool bAnnouncedObserver = false;
	bool bObserverTest = false;
	float ObserverTestDuration = 60.f;
	float ObserverTime = 0.f;
	float NextObserverLog = 0.f;
	int32 ObserverScreenshots = 0;
};
