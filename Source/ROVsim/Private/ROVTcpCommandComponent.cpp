#include "ROVTcpCommandComponent.h"

#include "ROVPawn.h"
#include "Common/TcpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

UROVTcpCommandComponent::UROVTcpCommandComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UROVTcpCommandComponent::BeginPlay()
{
	Super::BeginPlay();

	// Socket de escuta: fica "atendendo o telefone" na porta CommandPort.
	// Criado aqui (e não dentro do listener) para sabermos na hora se a porta abriu.
	ListenSocket = FTcpSocketBuilder(TEXT("ROV_TcpListen"))
		.AsReusable()
		.BoundToEndpoint(FIPv4Endpoint(FIPv4Address::Any, CommandPort))
		.Listening(4) // até 4 conexões esperando para serem aceitas
		.Build();

	if (!ListenSocket)
	{
		UE_LOG(LogROV, Error, TEXT("TCP: não consegui abrir a porta %d (outro programa usando?)"), CommandPort);
		return;
	}

	// O listener roda numa thread própria, porque esperar conexão (Accept) é bloqueante
	Listener = MakeUnique<FTcpListener>(*ListenSocket, FTimespan::FromMilliseconds(100));
	Listener->OnConnectionAccepted().BindUObject(this, &UROVTcpCommandComponent::OnConnectionAccepted);

	UE_LOG(LogROV, Display, TEXT("TCP: aguardando conexões na porta %d"), CommandPort);
}

void UROVTcpCommandComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 1) Para a thread do listener
	Listener.Reset();

	// 2) Fecha as conexões (as aceitas e as que ainda estavam na fila)
	FROVPendingConnection Pending;
	while (PendingConnections.Dequeue(Pending))
	{
		CloseSocket(Pending.Socket);
	}
	for (FROVTcpClient& Client : Clients)
	{
		CloseSocket(Client.Socket);
	}
	Clients.Empty();

	// 3) Fecha o socket de escuta (o listener não fecha um socket que não é dele)
	CloseSocket(ListenSocket);

	Super::EndPlay(EndPlayReason);
}

// THREAD DO LISTENER: não mexe no ROV nem na lista de clientes, só enfileira
bool UROVTcpCommandComponent::OnConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& ClientEndpoint)
{
	FROVPendingConnection Pending;
	Pending.Socket = ClientSocket;
	Pending.Endpoint = ClientEndpoint;
	PendingConnections.Enqueue(Pending);
	return true; // true = "fico com este socket" (com false, o listener o fecharia)
}

// GAME THREAD
void UROVTcpCommandComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// 1) Adota as conexões novas
	FROVPendingConnection Pending;
	while (PendingConnections.Dequeue(Pending))
	{
		Pending.Socket->SetNonBlocking(true); // Recv no game thread nunca pode ficar esperando
		FROVTcpClient& Client = Clients.AddDefaulted_GetRef();
		Client.Socket = Pending.Socket;
		Client.Endpoint = Pending.Endpoint;

		UE_LOG(LogROV, Display, TEXT("TCP: cliente conectado (%s)"), *Client.Endpoint.ToString());
		SendLine(Client.Socket, TEXT("OK ROVsim pronto. Comandos: PING, STATUS, MODE SIM|DEMO, LIGHTS ON|OFF, CAMERA EXT|COCKPIT, RESET, CURRENT, AUTO"));
	}

	// 2) Lê cada cliente. De trás para frente, para poder remover quem desconectou.
	for (int32 Index = Clients.Num() - 1; Index >= 0; --Index)
	{
		bool bDisconnected = false;
		ReadClient(Clients[Index], bDisconnected);

		if (bDisconnected)
		{
			UE_LOG(LogROV, Display, TEXT("TCP: cliente desconectou (%s)"), *Clients[Index].Endpoint.ToString());
			CloseSocket(Clients[Index].Socket);
			Clients.RemoveAtSwap(Index);
		}
	}
}

void UROVTcpCommandComponent::ReadClient(FROVTcpClient& Client, bool& bOutDisconnected)
{
	// a) Lê tudo que chegou até agora: pode ser meia linha, uma linha ou várias
	uint8 Chunk[1024];
	int32 BytesRead = 0;
	while (true)
	{
		if (!Client.Socket->Recv(Chunk, sizeof(Chunk), BytesRead))
		{
			bOutDisconnected = true; // o cliente fechou a conexão (ou ela caiu)
			return;
		}
		if (BytesRead == 0)
		{
			break; // não chegou mais nada por enquanto
		}
		Client.Buffer.Append(Chunk, BytesRead);
	}

	// b) FRAMING: executa cada linha completa (terminada em '\n')
	int32 NewLineIndex = INDEX_NONE;
	while (Client.Buffer.Find(static_cast<uint8>('\n'), NewLineIndex))
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Client.Buffer.GetData()), NewLineIndex);
		const FString Line = FString(Converted.Length(), Converted.Get()).TrimStartAndEnd(); // tira espaços e '\r'
		Client.Buffer.RemoveAt(0, NewLineIndex + 1); // descarta a linha já usada (com o '\n')

		if (!Line.IsEmpty())
		{
			SendLine(Client.Socket, ExecuteCommand(Line));
		}
	}

	// c) Proteção: um cliente mandando bytes sem nunca mandar '\n' não pode encher a memória
	if (Client.Buffer.Num() > 4096)
	{
		SendLine(Client.Socket, TEXT("ERR linha muito longa"));
		bOutDisconnected = true;
	}
}

FString UROVTcpCommandComponent::ExecuteCommand(const FString& Line)
{
	AROVPawn* ROV = Cast<AROVPawn>(GetOwner());
	if (!ROV)
	{
		return TEXT("ERR sem ROV");
	}

	UE_LOG(LogROV, Display, TEXT("TCP: comando \"%s\""), *Line);

	// "mode demo" -> ["MODE", "DEMO"]
	TArray<FString> Words;
	Line.ToUpper().ParseIntoArrayWS(Words);
	const FString& Command = Words[0];
	const FString Argument = Words.Num() > 1 ? Words[1] : FString();

	if (Command == TEXT("PING"))
	{
		return TEXT("OK PONG");
	}

	if (Command == TEXT("MODE"))
	{
		if (Argument == TEXT("SIM")) { ROV->SetControlMode(EROVControlMode::Simulator); return TEXT("OK MODE SIM"); }
		if (Argument == TEXT("DEMO")) { ROV->SetControlMode(EROVControlMode::Demo);      return TEXT("OK MODE DEMO"); }
		return TEXT("ERR use MODE SIM ou MODE DEMO");
	}

	if (Command == TEXT("LIGHTS"))
	{
		if (Argument == TEXT("ON")) { ROV->SetLightsOn(true);  return TEXT("OK LIGHTS ON"); }
		if (Argument == TEXT("OFF")) { ROV->SetLightsOn(false); return TEXT("OK LIGHTS OFF"); }
		return TEXT("ERR use LIGHTS ON ou LIGHTS OFF");
	}

	if (Command == TEXT("CAMERA"))
	{
		if (Argument == TEXT("EXT")) { ROV->SetCameraMode(EROVCameraMode::External); return TEXT("OK CAMERA EXT"); }
		if (Argument == TEXT("COCKPIT")) { ROV->SetCameraMode(EROVCameraMode::Cockpit);  return TEXT("OK CAMERA COCKPIT"); }
		return TEXT("ERR use CAMERA EXT ou CAMERA COCKPIT");
	}

	if (Command == TEXT("RESET"))
	{
		ROV->ResetToStart();
		return TEXT("OK RESET");
	}

	if (Command == TEXT("STATUS"))
	{
		const FROVTelemetry& T = ROV->GetTelemetry();
		return FString::Printf(TEXT("OK MODE %s LIGHTS %s CAMERA %s DEPTH %.1f HEADING %.1f"),
			ROV->GetControlMode() == EROVControlMode::Simulator ? TEXT("SIM") : TEXT("DEMO"),
			ROV->AreLightsOn() ? TEXT("ON") : TEXT("OFF"),
			ROV->GetCameraMode() == EROVCameraMode::External ? TEXT("EXT") : TEXT("COCKPIT"),
			T.DepthM, T.HeadingDeg);
	}
	if (Command == TEXT("CURRENT"))
	{
		// CURRENT <nós> <direção para onde vai, em graus>    ex.: CURRENT 1 0  |  CURRENT 0 desliga
		float Knots = 0.f;
		float Toward = 0.f;
		const bool bValid = Words.Num() >= 2
			&& FCString::IsNumeric(*Words[1]) && LexTryParseString(Knots, *Words[1])
			&& (Words.Num() < 3 || (FCString::IsNumeric(*Words[2]) && LexTryParseString(Toward, *Words[2])));
		if (!bValid || Knots < 0.f) // velocidade negativa não existe: para inverter, use a direção + 180
		{
			return TEXT("ERR use CURRENT <nos> <direcao_graus>  (ex.: CURRENT 1.5 90)");
		}

		ROV->SetCurrent(Knots, Toward);
		return FString::Printf(TEXT("OK CURRENT %.2f %.0f"), Knots, FRotator::ClampAxis(Toward));
	}

	if (Command == TEXT("AUTO"))
	{
		// AUTO HEADING|DEPTH|ALT  ON|OFF|<valor>     ou     AUTO OFF (desliga todos)
		if (Argument == TEXT("OFF"))
		{
			ROV->SetAutoHeading(false);
			ROV->SetAutoDepth(false); // desliga o vertical (profundidade ou altitude)
			return TEXT("OK AUTO OFF");
		}

		const FString Value = Words.Num() > 2 ? Words[2] : FString();
		float Number = 0.f;
		const bool bNumber = FCString::IsNumeric(*Value) && LexTryParseString(Number, *Value);

		if (Argument == TEXT("HEADING"))
		{
			if (Value == TEXT("ON"))  { ROV->SetAutoHeading(true);  return TEXT("OK AUTO HEADING ON"); }
			if (Value == TEXT("OFF")) { ROV->SetAutoHeading(false); return TEXT("OK AUTO HEADING OFF"); }
			if (bNumber)
			{
				ROV->SetTargetHeading(Number);
				return FString::Printf(TEXT("OK AUTO HEADING %.1f"), ROV->GetTargetHeading()); // o valor APLICADO
			}
		}
		else if (Argument == TEXT("DEPTH"))
		{
			if (Value == TEXT("ON"))  { ROV->SetAutoDepth(true);  return TEXT("OK AUTO DEPTH ON"); }
			if (Value == TEXT("OFF")) { ROV->SetAutoDepth(false); return TEXT("OK AUTO DEPTH OFF"); }
			if (bNumber)
			{
				ROV->SetTargetDepth(Number);
				return FString::Printf(TEXT("OK AUTO DEPTH %.1f"), ROV->GetTargetDepth());
			}
		}
		else if (Argument == TEXT("ALT"))
		{
			if (Value == TEXT("ON"))
			{
				ROV->SetAutoAltitude(true);
				return ROV->GetVerticalAuto() == EROVVerticalAuto::Altitude
					? TEXT("OK AUTO ALT ON")
					: TEXT("ERR fundo fora do alcance do altimetro");
			}
			if (Value == TEXT("OFF")) { ROV->SetAutoAltitude(false); return TEXT("OK AUTO ALT OFF"); }
			if (bNumber)
			{
				ROV->SetTargetAltitude(Number);
				return FString::Printf(TEXT("OK AUTO ALT %.1f"), ROV->GetTargetAltitude());
			}
		}

		return TEXT("ERR use AUTO HEADING|DEPTH|ALT ON|OFF|<valor>  ou  AUTO OFF");
	}


	return FString::Printf(TEXT("ERR comando desconhecido: %s"), *Command);
}

void UROVTcpCommandComponent::SendLine(FSocket* Socket, const FString& Line)
{
	const FString WithNewLine = Line + TEXT("\n"); // o '\n' é o "fim de mensagem" do protocolo
	const FTCHARToUTF8 Utf8(*WithNewLine);
	int32 BytesSent = 0;
	Socket->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), BytesSent);
}

void UROVTcpCommandComponent::CloseSocket(FSocket*& Socket)
{
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
}