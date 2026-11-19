#include "ROVSensorEmulatorComponent.h"

#include "ROVNmea.h"
#include "ROVPawn.h"
#include "Common/TcpSocketBuilder.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include <atomic>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	constexpr float MsToKnots = 1.f / 0.514444f;
}

// =============================================================================
// Escritor serial: uma thread só para escrever na porta COM
// =============================================================================

/**
 * Escrever numa porta serial é bloqueante: a 9600 baud, cada caractere leva ~1 ms, e se o outro lado
 * não estiver lendo a escrita pode esperar o timeout. Por isso o game thread só ENFILEIRA as frases,
 * e esta thread as escreve. (É a mesma ideia da recepção UDP, ao contrário.)
 */
class FROVSerialWriter : public FRunnable
{
public:
	/** Abre a porta e liga a thread. Devolve nullptr se a porta não existir ou estiver em uso. */
	static FROVSerialWriter* Open(const FString& PortName, int32 BaudRate)
	{
#if PLATFORM_WINDOWS
		// "\\.\COM20": o prefixo é obrigatório para COM10 em diante
		const FString Path = FString(TEXT("\\\\.\\")) + PortName;
		HANDLE Handle = ::CreateFileW(*Path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (Handle == INVALID_HANDLE_VALUE)
		{
			return nullptr;
		}

		// 8N1: 8 bits de dados, sem paridade, 1 bit de parada (o padrão NMEA 0183)
		DCB Config = {};
		Config.DCBlength = sizeof(DCB);
		::GetCommState(Handle, &Config);
		Config.BaudRate = static_cast<DWORD>(BaudRate);
		Config.ByteSize = 8;
		Config.Parity = NOPARITY;
		Config.StopBits = ONESTOPBIT;
		::SetCommState(Handle, &Config);

		// Uma escrita nunca espera mais que 200 ms (se ninguém estiver lendo do outro lado)
		COMMTIMEOUTS Timeouts = {};
		Timeouts.WriteTotalTimeoutConstant = 200;
		::SetCommTimeouts(Handle, &Timeouts);

		return new FROVSerialWriter(Handle);
#else
		return nullptr;
#endif
	}

	virtual ~FROVSerialWriter() override
	{
		Stop();
		if (Thread)
		{
			Thread->WaitForCompletion();
			delete Thread;
		}
#if PLATFORM_WINDOWS
		::CloseHandle(static_cast<HANDLE>(PortHandle));
#endif
		FPlatformProcess::ReturnSynchEventToPool(WakeUp);
	}

	/** Chamado no game thread: só enfileira e acorda a thread */
	void Write(const FString& Line)
	{
		const FTCHARToUTF8 Utf8(*Line);
		Pending.Enqueue(TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
		WakeUp->Trigger();
	}

	virtual uint32 Run() override
	{
		while (!bStopping)
		{
			WakeUp->Wait(100); // dorme até ter frase para escrever (ou 100 ms)
			TArray<uint8> Data;
			while (Pending.Dequeue(Data))
			{
#if PLATFORM_WINDOWS
				DWORD Written = 0;
				::WriteFile(static_cast<HANDLE>(PortHandle), Data.GetData(), static_cast<DWORD>(Data.Num()), &Written, nullptr);
#endif
			}
		}
		return 0;
	}

	virtual void Stop() override
	{
		bStopping = true;
		WakeUp->Trigger();
	}

private:
	explicit FROVSerialWriter(void* InHandle)
		: PortHandle(InHandle)
	{
		WakeUp = FPlatformProcess::GetSynchEventFromPool(false);
		Thread = FRunnableThread::Create(this, TEXT("ROV_SerialWriter"));
	}

	void* PortHandle = nullptr;
	FRunnableThread* Thread = nullptr;
	FEvent* WakeUp = nullptr;
	TQueue<TArray<uint8>, EQueueMode::Spsc> Pending;
	std::atomic<bool> bStopping { false }; // lido pela thread, escrito pelo game thread
};

// =============================================================================
// Componente
// =============================================================================

UROVSensorEmulatorComponent::UROVSensorEmulatorComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	auto MakeOutput = [](EROVSensorKind Kind, EROVSensorTransport Transport, int32 UdpPort, float RateHz)
	{
		FROVSensorOutput Output;
		Output.Kind = Kind;
		Output.Transport = Transport;
		Output.UdpPort = UdpPort;
		Output.RateHz = RateHz;
		return Output;
	};

	// Taxas típicas de cada instrumento real
	Outputs = {
		MakeOutput(EROVSensorKind::Gyro,      EROVSensorTransport::Udp,    10001, 10.f),
		MakeOutput(EROVSensorKind::Attitude,  EROVSensorTransport::Udp,    10002, 20.f),
		MakeOutput(EROVSensorKind::Dvl,       EROVSensorTransport::Tcp,    0,     5.f),
		// Seriais: a porta UDP é o RESERVA, usado enquanto a COM não existir
		MakeOutput(EROVSensorKind::Depth,     EROVSensorTransport::Serial, 10003, 2.f),
		MakeOutput(EROVSensorKind::Altimeter, EROVSensorTransport::Serial, 10004, 5.f),
	};
}

void UROVSensorEmulatorComponent::BeginPlay()
{
	Super::BeginPlay();

	// --- UDP: um socket só de envio, para todas as portas UDP ---
	UdpSocket = FUdpSocketBuilder(TEXT("ROV_SensorsUdp")).AsNonBlocking().Build();
	if (!FIPv4Address::Parse(UdpAddress, UdpTargetAddress))
	{
		UE_LOG(LogROV, Error, TEXT("Sensores: endereço UDP inválido: %s"), *UdpAddress);
	}

	// --- TCP: servidor que aceita conexões e transmite o fluxo para todos os clientes ---
	TcpListenSocket = FTcpSocketBuilder(TEXT("ROV_SensorsTcp"))
		.AsReusable()
		.BoundToEndpoint(FIPv4Endpoint(FIPv4Address::Any, TcpPort))
		.Listening(4)
		.Build();
	if (TcpListenSocket)
	{
		TcpListener = MakeUnique<FTcpListener>(*TcpListenSocket, FTimespan::FromMilliseconds(100));
		TcpListener->OnConnectionAccepted().BindUObject(this, &UROVSensorEmulatorComponent::OnTcpConnectionAccepted);
	}
	else
	{
		UE_LOG(LogROV, Error, TEXT("Sensores: não consegui abrir a porta TCP %d"), TcpPort);
	}

	// --- Serial: só abre se algum sensor usa COM ---
	const bool bNeedsSerial = Outputs.ContainsByPredicate([](const FROVSensorOutput& Output)
	{
		return Output.bEnabled && Output.Transport == EROVSensorTransport::Serial;
	});
	if (bNeedsSerial)
	{
		SerialWriter = FROVSerialWriter::Open(SerialPortName, SerialBaudRate);
		if (SerialWriter)
		{
			UE_LOG(LogROV, Display, TEXT("Sensores: porta %s aberta a %d baud"), *SerialPortName, SerialBaudRate);
		}
		else
		{
			UE_LOG(LogROV, Warning, TEXT("Sensores: porta %s indisponível. Os sensores seriais saem pela porta UDP reserva de cada um, até a COM existir."), *SerialPortName);
		}
	}

	UE_LOG(LogROV, Display, TEXT("Sensores: UDP para %s, TCP na porta %d"), *UdpAddress, TcpPort);
}

void UROVSensorEmulatorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	delete SerialWriter; // para a thread e fecha a porta
	SerialWriter = nullptr;

	TcpListener.Reset();
	FSocket* Pending = nullptr;
	while (PendingTcpClients.Dequeue(Pending))
	{
		CloseSocket(Pending);
	}
	for (FSocket*& Client : TcpClients)
	{
		CloseSocket(Client);
	}
	TcpClients.Empty();
	CloseSocket(TcpListenSocket);
	CloseSocket(UdpSocket);

	Super::EndPlay(EndPlayReason);
}

bool UROVSensorEmulatorComponent::OnTcpConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint)
{
	PendingTcpClients.Enqueue(ClientSocket);
	return true;
}

void UROVSensorEmulatorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	ServiceTcpClients();

	// Cada sensor no seu ritmo: acumula o tempo e envia quando passa o período
	for (FROVSensorOutput& Output : Outputs)
	{
		if (!Output.bEnabled || Output.RateHz <= 0.f)
		{
			continue;
		}
		Output.Accumulator += DeltaTime;
		const float Period = 1.f / Output.RateHz;
		if (Output.Accumulator >= Period)
		{
			Output.Accumulator = FMath::Fmod(Output.Accumulator, Period);
			Emit(Output, BuildSentence(Output.Kind));
		}
	}
}

void UROVSensorEmulatorComponent::ServiceTcpClients()
{
	// Adota as conexões novas
	FSocket* NewClient = nullptr;
	while (PendingTcpClients.Dequeue(NewClient))
	{
		NewClient->SetNonBlocking(true);
		TcpClients.Add(NewClient);
		UE_LOG(LogROV, Display, TEXT("Sensores: cliente TCP conectado (%d no total)"), TcpClients.Num());
	}

	// Detecta quem desconectou (o Recv devolve false quando o outro lado fecha)
	for (int32 Index = TcpClients.Num() - 1; Index >= 0; --Index)
	{
		uint8 Discard[256];
		int32 BytesRead = 0;
		if (!TcpClients[Index]->Recv(Discard, sizeof(Discard), BytesRead))
		{
			CloseSocket(TcpClients[Index]);
			TcpClients.RemoveAtSwap(Index);
			UE_LOG(LogROV, Display, TEXT("Sensores: cliente TCP desconectou (%d restantes)"), TcpClients.Num());
		}
	}
}

void UROVSensorEmulatorComponent::Emit(FROVSensorOutput& Output, const FString& Sentence)
{
	if (Sentence.IsEmpty())
	{
		return;
	}

	const FTCHARToUTF8 Utf8(*Sentence);
	const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
	const int32 Size = Utf8.Length();
	bool bSent = false;

	// Sensor serial sem porta COM: usa a porta UDP reserva (se tiver uma configurada)
	const bool bSerialFallback = Output.Transport == EROVSensorTransport::Serial && !SerialWriter && Output.UdpPort > 0;
	const EROVSensorTransport Transport = bSerialFallback ? EROVSensorTransport::Udp : Output.Transport;

	switch (Transport)
	{
	case EROVSensorTransport::Udp:
		if (UdpSocket)
		{
			// Uma frase por datagrama: quem lê recebe frases inteiras, sem precisar de framing
			const TSharedRef<FInternetAddr> Target = FIPv4Endpoint(UdpTargetAddress, Output.UdpPort).ToInternetAddr();
			int32 BytesSent = 0;
			bSent = UdpSocket->SendTo(Bytes, Size, BytesSent, *Target);
		}
		break;

	case EROVSensorTransport::Tcp:
		// O mesmo fluxo vai para todos os clientes conectados. O "\r\n" no fim é o framing do NMEA.
		for (FSocket* Client : TcpClients)
		{
			int32 BytesSent = 0;
			bSent |= Client->Send(Bytes, Size, BytesSent);
		}
		break;

	case EROVSensorTransport::Serial:
		if (SerialWriter)
		{
			SerialWriter->Write(Sentence);
			bSent = true;
		}
		break;
	}

	if (bSent)
	{
		++Output.SentCount;
		Output.LastSentence = Sentence.TrimEnd();
	}
}

FString UROVSensorEmulatorComponent::BuildSentence(EROVSensorKind Kind) const
{
	const AROVPawn* ROV = Cast<AROVPawn>(GetOwner());
	if (!ROV)
	{
		return FString();
	}
	const FROVTelemetry& T = ROV->GetTelemetry();

	switch (Kind)
	{
	case EROVSensorKind::Gyro:
		// HDT = rumo verdadeiro (True heading), em graus
		return ROVNmea::Sentence(FString::Printf(TEXT("HEHDT,%.1f,T"), T.HeadingDeg));

	case EROVSensorKind::Attitude:
		// Formato de atitude da Teledyne RDI: pitch, roll e rumo, em graus
		return ROVNmea::Sentence(FString::Printf(TEXT("PRDID,%+.2f,%+.2f,%.2f"), T.PitchDeg, T.RollDeg, T.HeadingDeg));

	case EROVSensorKind::Dvl:
	{
		// VBW = velocidade dupla, em nós, no referencial do ROV (longitudinal = frente, transversal = boreste):
		// em relação à ÁGUA (o que o ROV "sente") e em relação ao FUNDO (o que ele realmente percorre).
		// Com corrente, os dois são diferentes; é assim que um DVL real ajuda a estimar a corrente.
		const FVector GroundWorld = ROV->GetActorQuat().RotateVector(T.LocalVelocityMs);
		// A água = corrente + órbita do swell no ponto do ROV
		const FVector Water = ROV->GetActorQuat().UnrotateVector(GroundWorld - ROV->GetWaterVelocityMs(ROV->GetActorLocation())) * MsToKnots;
		const FVector Ground = T.LocalVelocityMs * MsToKnots;
		return ROVNmea::Sentence(FString::Printf(TEXT("VDVBW,%.2f,%.2f,A,%.2f,%.2f,A"), Water.X, Water.Y, Ground.X, Ground.Y));
	}

	case EROVSensorKind::Depth:
		// Frase proprietária do ROVsim ("P" + código do fabricante RSM): profundidade abaixo da superfície, em metros
		return ROVNmea::Sentence(FString::Printf(TEXT("PRSMD,%.2f,M"), T.DepthM));

	case EROVSensorKind::Altimeter:
		// DPT = profundidade da água abaixo do transdutor (para um altímetro de ROV, a distância até o fundo).
		// Sem fundo no alcance, o campo vai vazio, como manda o NMEA para dado indisponível.
		return T.AltitudeM >= 0.f
			? ROVNmea::Sentence(FString::Printf(TEXT("SDDPT,%.2f,0.0"), T.AltitudeM))
			: ROVNmea::Sentence(TEXT("SDDPT,,0.0"));
	}
	return FString();
}

void UROVSensorEmulatorComponent::CloseSocket(FSocket*& Socket)
{
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
}
