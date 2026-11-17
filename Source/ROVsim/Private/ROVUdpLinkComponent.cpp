#include "ROVUdpLinkComponent.h"

#include "ROVPawn.h"
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

UROVUdpLinkComponent::UROVUdpLinkComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UROVUdpLinkComponent::BeginPlay()
{
	Super::BeginPlay();

	// Socket de comandos: escuta em todas as placas de rede (0.0.0.0) na porta CommandPort
	CommandSocket = FUdpSocketBuilder(TEXT("ROV_Commands"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(FIPv4Address::Any)
		.BoundToPort(CommandPort)
		.WithReceiveBufferSize(64 * 1024)
		.Build();

	if (!CommandSocket)
	{
		UE_LOG(LogROV, Error, TEXT("UDP: não consegui abrir a porta %d (outro programa usando?)"), CommandPort);
		return;
	}

	// Thread de recepção: acorda a cada 100 ms para checar se deve parar
	Receiver = MakeUnique<FUdpSocketReceiver>(CommandSocket, FTimespan::FromMilliseconds(100), TEXT("ROV_UdpReceiver"));
	Receiver->OnDataReceived().BindUObject(this, &UROVUdpLinkComponent::OnDataReceived);
	Receiver->Start();

	// --- Telemetria: socket só de envio (o sistema escolhe a porta de saída) ---
	TelemetrySocket = FUdpSocketBuilder(TEXT("ROV_Telemetry"))
		.AsNonBlocking()
		.Build();

	FIPv4Address Address;
	if (TelemetrySocket && FIPv4Address::Parse(TelemetryAddress, Address))
	{
		TelemetryEndpoint = FIPv4Endpoint(Address, TelemetryPort).ToInternetAddr();
		UE_LOG(LogROV, Display, TEXT("UDP: enviando telemetria para %s:%d a %.0f Hz"), *TelemetryAddress, TelemetryPort, TelemetryRateHz);
	}
	else
	{
		UE_LOG(LogROV, Error, TEXT("UDP: endereço de telemetria inválido: %s"), *TelemetryAddress);
	}

	UE_LOG(LogROV, Display, TEXT("UDP: escutando comandos na porta %d"), CommandPort);
}

void UROVUdpLinkComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Ordem importa: primeiro para a thread (ela usa o socket), depois fecha o socket
	if (Receiver)
	{
		Receiver->Stop();
		Receiver.Reset(); // destrói o receptor e espera a thread terminar
	}

	if (CommandSocket)
	{
		CommandSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(CommandSocket);
		CommandSocket = nullptr;
	}

	if (TelemetrySocket)
	{
		TelemetrySocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(TelemetrySocket);
		TelemetrySocket = nullptr;
	}
	TelemetryEndpoint.Reset();

	Super::EndPlay(EndPlayReason);
}

// THREAD DE REDE: só lê o pacote e coloca na fila
void UROVUdpLinkComponent::OnDataReceived(const FArrayReaderPtr& Data, const FIPv4Endpoint& Sender)
{
	// Bytes (UTF-8) -> texto
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data->GetData()), Data->Num());
	const FString Text(Converted.Length(), Converted.Get());

	// Texto -> objeto JSON. Pacote quebrado ou de outro tipo é simplesmente ignorado.
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return;
	}

	FString Type;
	if (!Json->TryGetStringField(TEXT("type"), Type) || Type != TEXT("cmd"))
	{
		return;
	}

	// Campos ausentes ficam em 0 (comando neutro)
	double Seq = 0.0, Surge = 0.0, Sway = 0.0, Heave = 0.0, Yaw = 0.0;
	Json->TryGetNumberField(TEXT("seq"), Seq);
	Json->TryGetNumberField(TEXT("surge"), Surge);
	Json->TryGetNumberField(TEXT("sway"), Sway);
	Json->TryGetNumberField(TEXT("heave"), Heave);
	Json->TryGetNumberField(TEXT("yaw"), Yaw);

	FROVRemoteCommand Command;
	Command.Sequence = static_cast<uint32>(Seq);
	Command.Translation = FVector(Surge, Sway, Heave);
	Command.Yaw = static_cast<float>(Yaw);

	PendingCommands.Enqueue(Command);
}

// GAME THREAD: aplica os comandos e cuida do watchdog
void UROVUdpLinkComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AROVPawn* ROV = Cast<AROVPawn>(GetOwner());
	if (!ROV)
	{
		return;
	}

	// 1) Esvazia a fila
	bool bGotCommand = false;
	FROVRemoteCommand Command;
	while (PendingCommands.Dequeue(Command))
	{
		// UDP não garante ordem: pacote mais velho que o último aplicado é descartado.
		// (Se o link estava parado, aceita qualquer seq: o cliente pode ter reiniciado a contagem.)
		if (bRemoteActive && Command.Sequence <= LastAppliedSequence)
		{
			continue;
		}

		LastAppliedSequence = Command.Sequence;
		ROV->SetPilotCommand(Command.Translation, Command.Yaw);
		bGotCommand = true;
	}

	// 2) Watchdog (failsafe)
	if (bGotCommand)
	{
		TimeSinceLastCommand = 0.f;
		if (!bRemoteActive)
		{
			bRemoteActive = true;
			UE_LOG(LogROV, Display, TEXT("UDP: controle remoto ATIVO"));
		}
	}
	else
	{
		TimeSinceLastCommand += DeltaTime;
		if (bRemoteActive && TimeSinceLastCommand > CommandTimeout)
		{
			bRemoteActive = false;
			ROV->SetPilotCommand(FVector::ZeroVector, 0.f); // para os propulsores
			UE_LOG(LogROV, Warning, TEXT("UDP: sem comandos há %.1f s - propulsores zerados (failsafe)"), TimeSinceLastCommand);
		}
	}

	// 3) Telemetria na frequência configurada
	if (TelemetryRateHz > 0.f)
	{
		TelemetryAccumulator += DeltaTime;
		const float Period = 1.f / TelemetryRateHz;
		if (TelemetryAccumulator >= Period)
		{
			// Fmod em vez de zerar: guarda a "sobra" e mantém a média em 10 Hz mesmo com FPS variável
			TelemetryAccumulator = FMath::Fmod(TelemetryAccumulator, Period);
			SendTelemetry();
		}
	}
}

void UROVUdpLinkComponent::SendTelemetry()
{
	const AROVPawn* ROV = Cast<AROVPawn>(GetOwner());
	if (!ROV || !TelemetrySocket || !TelemetryEndpoint.IsValid())
	{
		return;
	}

	const FROVTelemetry& T = ROV->GetTelemetry();
	const FVector PositionM = ROV->GetActorLocation() / 100.f; // cm -> m

	// Vetor -> array JSON [x, y, z]
	auto MakeArray = [](const FVector& V)
		{
			TArray<TSharedPtr<FJsonValue>> Array;
			Array.Add(MakeShared<FJsonValueNumber>(V.X));
			Array.Add(MakeShared<FJsonValueNumber>(V.Y));
			Array.Add(MakeShared<FJsonValueNumber>(V.Z));
			return Array;
		};

	// Monta o objeto JSON campo a campo
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("type"), TEXT("telemetry"));
	Json->SetNumberField(TEXT("seq"), ++TelemetrySequence);
	Json->SetNumberField(TEXT("t"), GetWorld()->GetTimeSeconds());
	Json->SetStringField(TEXT("mode"), ROV->GetControlMode() == EROVControlMode::Simulator ? TEXT("SIM") : TEXT("DEMO"));
	Json->SetBoolField(TEXT("remote"), bRemoteActive);
	Json->SetArrayField(TEXT("pos"), MakeArray(PositionM));
	Json->SetNumberField(TEXT("depth"), T.DepthM);
	Json->SetNumberField(TEXT("altitude"), T.AltitudeM);
	Json->SetNumberField(TEXT("heading"), T.HeadingDeg);
	Json->SetNumberField(TEXT("pitch"), T.PitchDeg);
	Json->SetNumberField(TEXT("roll"), T.RollDeg);
	Json->SetArrayField(TEXT("vel"), MakeArray(T.LocalVelocityMs));
	Json->SetNumberField(TEXT("yawRate"), T.YawRateDegS);

	// Objeto -> texto compacto (sem espaços nem quebras de linha: pacote menor)
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Json, Writer);

	// Texto -> bytes UTF-8 -> rede
	const FTCHARToUTF8 Utf8(*Text);
	int32 BytesSent = 0;
	TelemetrySocket->SendTo(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), BytesSent, *TelemetryEndpoint);
}