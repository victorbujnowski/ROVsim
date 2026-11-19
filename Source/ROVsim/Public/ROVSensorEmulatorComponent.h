#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "Common/TcpListener.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "ROVSensorEmulatorComponent.generated.h"

class FSocket;
class FROVSerialWriter;

/** Qual sensor real está sendo emulado */
UENUM(BlueprintType)
enum class EROVSensorKind : uint8
{
	Gyro UMETA(DisplayName = "Girobússola (rumo)"),
	Attitude UMETA(DisplayName = "Sensor de movimento (pitch/roll)"),
	Dvl UMETA(DisplayName = "DVL (velocidade)"),
	Depth UMETA(DisplayName = "Profundímetro"),
	Altimeter UMETA(DisplayName = "Altímetro")
};

/** Por onde o sensor envia as frases */
UENUM(BlueprintType)
enum class EROVSensorTransport : uint8
{
	Udp UMETA(DisplayName = "UDP"),
	Tcp UMETA(DisplayName = "TCP"),
	Serial UMETA(DisplayName = "Serial (COM)")
};

/** Configuração de uma saída de sensor */
USTRUCT(BlueprintType)
struct FROVSensorOutput
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sensor")
	EROVSensorKind Kind = EROVSensorKind::Gyro;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sensor")
	EROVSensorTransport Transport = EROVSensorTransport::Udp;

	/** Porta UDP de destino. Para sensor serial, é a porta RESERVA, usada enquanto a COM não existir (0 = sem reserva). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sensor")
	int32 UdpPort = 10001;

	/** Frases por segundo, como no sensor real */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sensor", meta = (ClampMin = "0.1", ClampMax = "50"))
	float RateHz = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sensor")
	bool bEnabled = true;

	// Estado em tempo de execução (não é salvo)
	float Accumulator = 0.f;
	uint32 SentCount = 0;
	FString LastSentence;
};

/**
 * Emulador de sensores: o simulador "fala" como os instrumentos de um ROV real, em frases NMEA 0183.
 *
 *   Girobússola           $HEHDT   UDP  (rápido, vários leitores, perda tolerável)
 *   Sensor de movimento   $PRDID   UDP
 *   DVL                   $VDVBW   TCP  (a navegação integra a velocidade: cada amostra conta)
 *   Profundímetro         $PRSMD   COM  (instrumentos seriais clássicos)
 *   Altímetro             $SDDPT   COM
 */
UCLASS(ClassGroup = (ROV), meta = (BlueprintSpawnableComponent))
class ROVSIM_API UROVSensorEmulatorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UROVSensorEmulatorComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Monta a frase NMEA de um sensor com a telemetria atual do ROV */
	FString BuildSentence(EROVSensorKind Kind) const;

	// Para o HUD
	const TArray<FROVSensorOutput>& GetOutputs() const { return Outputs; }
	int32 GetTcpPort() const { return TcpPort; }
	int32 GetTcpClientCount() const { return TcpClients.Num(); }
	const FString& GetSerialPortName() const { return SerialPortName; }
	bool IsSerialOpen() const { return SerialWriter != nullptr; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, Category = "ROV|Sensores")
	TArray<FROVSensorOutput> Outputs;

	/** Para onde vão as frases UDP */
	UPROPERTY(EditAnywhere, Category = "ROV|Sensores")
	FString UdpAddress = TEXT("127.0.0.1");

	/** Porta do servidor TCP (os clientes conectam e recebem o fluxo) */
	UPROPERTY(EditAnywhere, Category = "ROV|Sensores")
	int32 TcpPort = 10010;

	/** Porta serial (um lado de um par virtual, ex.: com0com COM20 <-> COM21) */
	UPROPERTY(EditAnywhere, Category = "ROV|Sensores")
	FString SerialPortName = TEXT("COM20");

	UPROPERTY(EditAnywhere, Category = "ROV|Sensores")
	int32 SerialBaudRate = 9600;

private:
	void Emit(FROVSensorOutput& Output, const FString& Sentence);
	void ServiceTcpClients();
	void CloseSocket(FSocket*& Socket);

	// ATENÇÃO: thread do listener. Só enfileira.
	bool OnTcpConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint);

	FSocket* UdpSocket = nullptr;
	FIPv4Address UdpTargetAddress;

	FSocket* TcpListenSocket = nullptr;
	TUniquePtr<FTcpListener> TcpListener;
	TQueue<FSocket*, EQueueMode::Spsc> PendingTcpClients;
	TArray<FSocket*> TcpClients;

	FROVSerialWriter* SerialWriter = nullptr; // thread que escreve na porta COM
};
