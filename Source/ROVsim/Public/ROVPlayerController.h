#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ROVPlayerController.generated.h"

class AROVPawn;
class UInputMappingContext;
class UInputAction;
struct FInputActionValue;

/** Como o observador está olhando o ROV */
UENUM()
enum class EROVObserverView : uint8
{
	Orbit,     // câmera livre em órbita: o observador escolhe o ângulo (mouse / stick direito) e o zoom
	RovCamera  // a câmera de vídeo do próprio ROV (cockpit): o que o piloto vê
};

/**
 * PlayerController do ROVsim.
 *
 * No multiplayer existem dois papéis:
 *   PILOTO      o jogador do servidor (host): possui o ROV e o controla.
 *   OBSERVADOR  quem conecta depois (supervisor, cliente, instrutor): não possui pawn; tem uma câmera
 *               livre em volta do ROV que o servidor simula, e o HUD mostra a telemetria replicada.
 *
 * Linha de comando (testes de rede):
 *   -ROVObserverTest[=segundos]  o observador registra o que recebe, mede "saltos" do ROV, tira
 *                                screenshots e fecha sozinho
 *   -ROVNetTrace                 registra a posição do ROV 10 vezes por segundo ("NETTRACE"), no host e no
 *                                observador; Tools/rov_net_compare.ps1 compara os dois logs e mede o atraso
 */
UCLASS()
class ROVSIM_API AROVPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AROVPlayerController();

	virtual void PlayerTick(float DeltaTime) override;
	virtual void SetupInputComponent() override;
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;

	/** true se este jogador só observa (não possui o ROV) */
	bool IsObserver() const { return GetPawn() == nullptr; }

	/** O ROV da partida (o próprio pawn, ou o do servidor para um observador) */
	AROVPawn* FindROV() const;

	EROVObserverView GetObserverView() const { return ObserverView; }

private:
	// --- Câmera do observador ---
	void BuildObserverInput();
	void OnObserverLook(const FInputActionValue& Value);
	void OnObserverZoom(const FInputActionValue& Value);
	void OnObserverToggleView();
	void SetObserverView(EROVObserverView NewView);

	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> ObserverContext;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ObserverLookAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ObserverZoomAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ObserverViewAction;

	EROVObserverView ObserverView = EROVObserverView::Orbit;
	float OrbitYawOffset = 0.f;   // graus, em relação ao rumo do ROV (0 = atrás dele)
	float OrbitPitch = -15.f;     // graus (negativo = olhando para baixo)
	float OrbitDistance = 900.f;  // cm
	FVector SmoothedPivot = FVector::ZeroVector;
	bool bPivotInitialized = false;

	// --- Testes de rede ---
	void TickObserverTest(AROVPawn* ROV, float DeltaTime);

	bool bAnnouncedObserver = false;
	bool bObserverTest = false;
	bool bNetTrace = false;
	float ObserverTestDuration = 60.f;
	float ObserverTime = 0.f;
	float NextObserverLog = 0.f;
	float NextTraceTime = 0.f;
	int32 ObserverScreenshots = 0;

	// "Salto" = quanto o ROV andou num frame ALÉM do que a velocidade dele explica (correção brusca da rede)
	FVector LastObservedPosition = FVector::ZeroVector;
	bool bHasLastPosition = false;
	float RovSeenTime = 0.f;    // há quanto tempo o ROV chegou: os primeiros 3 s são a convergência inicial (não contam)
	float MaxJumpSimM = 0.f;   // no modo simulador (física suavizada pela Unreal)
	float MaxJumpDemoM = 0.f;  // no modo demonstração (sem física)
};
