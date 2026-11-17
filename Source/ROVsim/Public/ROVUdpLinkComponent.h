#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "Common/UdpSocketReceiver.h"
#include "ROVUdpLinkComponent.generated.h"

class FSocket;
class FInternetAddr;

/** Um comando de pilotagem recebido pela rede, já convertido do JSON */
struct FROVRemoteCommand
{
	uint32 Sequence = 0;
	FVector Translation = FVector::ZeroVector; // surge, sway, heave (-1 a 1)
	float Yaw = 0.f;                           // -1 a 1
};

/**
 * Link UDP do ROV: recebe comandos de pilotagem (porta 5000) e envia telemetria (porta 5001).
 *
 * A leitura do socket roda numa thread própria (FUdpSocketReceiver) para nunca travar o jogo.
 * Os comandos passam para o game thread por uma fila thread-safe e são aplicados no Tick.
 */
UCLASS(ClassGroup = (ROV), meta = (BlueprintSpawnableComponent))
class ROVSIM_API UROVUdpLinkComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UROVUdpLinkComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** true enquanto chegam comandos dentro do timeout */
	bool IsRemoteActive() const { return bRemoteActive; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, Category = "ROV|UDP")
	int32 CommandPort = 5000;

	UPROPERTY(EditAnywhere, Category = "ROV|UDP")
	FString TelemetryAddress = TEXT("127.0.0.1");

	UPROPERTY(EditAnywhere, Category = "ROV|UDP")
	int32 TelemetryPort = 5001;

	UPROPERTY(EditAnywhere, Category = "ROV|UDP")
	float TelemetryRateHz = 10.f;

	/** Sem comando por este tempo, os propulsores são zerados (failsafe / watchdog) */
	UPROPERTY(EditAnywhere, Category = "ROV|UDP")
	float CommandTimeout = 0.5f;

private:
	// ATENÇÃO: chamado na thread de rede, NÃO no game thread.
	// Aqui só se pode ler o pacote e colocar na fila - nada de mexer no ROV.
	void OnDataReceived(const FArrayReaderPtr& Data, const FIPv4Endpoint& Sender);

	void SendTelemetry();

	FSocket* CommandSocket = nullptr;
	FSocket* TelemetrySocket = nullptr;
	TUniquePtr<FUdpSocketReceiver> Receiver;

	// Destino da telemetria (IP + porta), montado uma vez no BeginPlay
	TSharedPtr<FInternetAddr> TelemetryEndpoint;

	// Fila de 1 produtor (thread de rede) e 1 consumidor (game thread): segura sem lock
	TQueue<FROVRemoteCommand, EQueueMode::Spsc> PendingCommands;

	uint32 LastAppliedSequence = 0;
	uint32 TelemetrySequence = 0;
	float TimeSinceLastCommand = 0.f;
	float TelemetryAccumulator = 0.f;
	bool bRemoteActive = false;
};