#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "Common/TcpListener.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "ROVTcpCommandComponent.generated.h"

class FSocket;

/** Uma conexão TCP aberta com um cliente */
struct FROVTcpClient
{
	FSocket* Socket = nullptr;
	FIPv4Endpoint Endpoint;
	TArray<uint8> Buffer; // bytes que chegaram mas ainda não formam uma linha completa
};

/** Conexão recém-aceita pela thread do listener, esperando o game thread "adotá-la" */
struct FROVPendingConnection
{
	FSocket* Socket = nullptr;
	FIPv4Endpoint Endpoint;
};

/**
 * Servidor TCP de comandos discretos (porta 5002): MODE, LIGHTS, CAMERA, RESET, STATUS, PING.
 * Protocolo de texto: um comando por linha, terminado em '\n'; uma resposta OK/ERR por linha.
 */
UCLASS(ClassGroup = (ROV), meta = (BlueprintSpawnableComponent))
class ROVSIM_API UROVTcpCommandComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UROVTcpCommandComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, Category = "ROV|TCP")
	int32 CommandPort = 5002;

private:
	// ATENÇÃO: chamado na thread do listener. Só enfileira a conexão.
	bool OnConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint);

	void ReadClient(FROVTcpClient& Client, bool& bOutDisconnected);
	FString ExecuteCommand(const FString& Line);
	void SendLine(FSocket* Socket, const FString& Line);
	void CloseSocket(FSocket*& Socket);

	FSocket* ListenSocket = nullptr;
	TUniquePtr<FTcpListener> Listener;
	TQueue<FROVPendingConnection, EQueueMode::Spsc> PendingConnections;
	TArray<FROVTcpClient> Clients;
};