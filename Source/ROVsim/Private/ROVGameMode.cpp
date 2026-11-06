#include "ROVGameMode.h"

#include "ROVHUD.h"
#include "ROVPawn.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

AROVGameMode::AROVGameMode()
{
	HUDClass = AROVHUD::StaticClass();

	// O ROV é criado pelo GameMode no PlayerStart a cada Play. Assim ele sempre nasce com a
	// estrutura atual do C++, sem depender de uma instância salva no mapa (que pode guardar
	// componentes antigos - foi o que travou o ROV quando o corpo passou de "Body" para "Hull").
	DefaultPawnClass = AROVPawn::StaticClass();

	PrimaryActorTick.bCanEverTick = true;
}

void AROVGameMode::BeginPlay()
{
	Super::BeginPlay();

	bSmokeTest = FParse::Param(FCommandLine::Get(), TEXT("ROVSmokeTest"));
	if (bSmokeTest)
	{
		SmokeStartTime = GetWorld()->GetTimeSeconds();
		UE_LOG(LogROV, Display, TEXT("SMOKE TEST iniciado"));
	}
}

AROVPawn* AROVGameMode::FindROV() const
{
	return Cast<AROVPawn>(UGameplayStatics::GetPlayerPawn(this, 0));
}

void AROVGameMode::LogTelemetry(const TCHAR* Label) const
{
	const AROVPawn* ROV = FindROV();
	if (!ROV)
	{
		UE_LOG(LogROV, Warning, TEXT("SMOKE %s: jogador não está controlando um AROVPawn"), Label);
		return;
	}

	const FROVTelemetry& T = ROV->GetTelemetry();
	const FVector Pos = ROV->GetActorLocation() / 100.f;
	UE_LOG(LogROV, Display,
		TEXT("SMOKE t=%5.1f %-14s pos=(%7.1f,%7.1f,%6.1f)m modo=%s cam=%s prof=%6.2fm alt=%6.2fm rumo=%6.1f pitch=%+5.1f roll=%+5.1f vel=(%+5.2f,%+5.2f,%+5.2f)m/s yaw=%+6.1fdeg/s empuxo=(%+4.0f,%+4.0f,%+4.0f,%+4.0f)%% massa=%.0f"),
		GetWorld()->GetTimeSeconds() - SmokeStartTime, Label, Pos.X, Pos.Y, Pos.Z,
		ROV->GetControlMode() == EROVControlMode::Simulator ? TEXT("SIM ") : TEXT("DEMO"),
		ROV->GetCameraMode() == EROVCameraMode::External ? TEXT("EXT") : TEXT("COCK"),
		T.DepthM, T.AltitudeM, T.HeadingDeg, T.PitchDeg, T.RollDeg,
		T.LocalVelocityMs.X, T.LocalVelocityMs.Y, T.LocalVelocityMs.Z, T.YawRateDegS,
		T.ThrustPercent.X, T.ThrustPercent.Y, T.ThrustPercent.Z, T.YawThrustPercent, T.MassKg);
}

void AROVGameMode::TakeScreenshot(const FString& Name) const
{
	const FString File = FPaths::Combine(FPaths::ScreenShotDir(), TEXT("SmokeTest"), Name + TEXT(".png"));
	FScreenshotRequest::RequestScreenshot(File, true, false);
	UE_LOG(LogROV, Display, TEXT("SMOKE screenshot: %s"), *File);
}

void AROVGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bSmokeTest)
	{
		return;
	}

	AROVPawn* ROV = FindROV();
	if (!ROV)
	{
		return;
	}

	const float T = GetWorld()->GetTimeSeconds() - SmokeStartTime;

	// Telemetria a cada segundo
	if (T >= NextLogTime)
	{
		NextLogTime += 1.f;
		LogTelemetry(TEXT("amostra"));
	}

	// Roteiro: cada passo roda uma vez quando o tempo dele chega
	struct FStep { float Time; TFunction<void()> Action; };
	const TArray<FStep> Script = {
		{ 1.f,  [&] { ROV->SetControlMode(EROVControlMode::Simulator); ROV->SetCameraMode(EROVCameraMode::External);
		              ROV->SetPilotCommand(FVector::ZeroVector, 0.f); LogTelemetry(TEXT("SIM repouso")); } },
		{ 6.f,  [&] { TakeScreenshot(TEXT("01_sim_externa_repouso")); LogTelemetry(TEXT("SIM repouso"));
		              ROV->SetPilotCommand(FVector(1.f, 0.f, 0.f), 0.f); } },
		{ 26.f, [&] { LogTelemetry(TEXT("SIM surge fim")); TakeScreenshot(TEXT("02_sim_surge"));
		              ROV->SetPilotCommand(FVector(0.f, 0.f, 1.f), 0.f); } },
		{ 41.f, [&] { LogTelemetry(TEXT("SIM heave fim")); ROV->SetPilotCommand(FVector::ZeroVector, 1.f); } },
		{ 53.f, [&] { LogTelemetry(TEXT("SIM yaw fim")); ROV->SetPilotCommand(FVector::ZeroVector, 0.f);
		              ROV->SetCameraMode(EROVCameraMode::Cockpit); } },
		{ 56.f, [&] { TakeScreenshot(TEXT("03_sim_cockpit")); } },
		{ 57.f, [&] { ROV->SetControlMode(EROVControlMode::Demo); ROV->SetCameraMode(EROVCameraMode::External);
		              ROV->SetPilotCommand(FVector(1.f, 0.f, 0.f), 0.f); } },
		{ 62.f, [&] { LogTelemetry(TEXT("DEMO surge fim")); TakeScreenshot(TEXT("04_demo_externa"));
		              ROV->SetPilotCommand(FVector(0.f, 0.f, -1.f), 1.f); } },
		{ 66.f, [&] { LogTelemetry(TEXT("DEMO desce+gira")); ROV->SetPilotCommand(FVector(0.f, 0.f, -1.f), 0.f); } },
		// Desce até encostar no fundo: a varredura de colisão tem que segurar o ROV
		{ 72.f, [&] { LogTelemetry(TEXT("DEMO no fundo")); ROV->SetPilotCommand(FVector(0.f, 0.f, 0.4f), 0.f); } },
		{ 72.6f, [&] { ROV->SetPilotCommand(FVector::ZeroVector, 0.f); ROV->SetCameraMode(EROVCameraMode::Cockpit); } },
		{ 75.f, [&] { LogTelemetry(TEXT("DEMO cockpit")); TakeScreenshot(TEXT("05_demo_cockpit")); } },
		{ 76.f, [&] { ROV->SetCameraMode(EROVCameraMode::External); TakeScreenshot(TEXT("06_demo_externa_fundo")); } },
		{ 77.f, [&] { ROV->SetControlMode(EROVControlMode::Simulator); } },
		{ 82.f, [&] { LogTelemetry(TEXT("SIM apos troca")); TakeScreenshot(TEXT("07_sim_apos_troca")); } },
		// Estabilidade: inclina o ROV e confere se o momento do empuxo (BG) o endireita sozinho
		{ 83.f, [&] { ROV->SetActorRotation(FRotator(15.f, ROV->GetActorRotation().Yaw, 20.f), ETeleportType::TeleportPhysics);
		              LogTelemetry(TEXT("SIM inclinado")); } },
		{ 84.5f, [&] { TakeScreenshot(TEXT("08_sim_endireitando")); } },
		{ 98.f, [&] { LogTelemetry(TEXT("SIM estabilizou")); UE_LOG(LogROV, Display, TEXT("SMOKE TEST concluído"));
		              FGenericPlatformMisc::RequestExit(false, TEXT("ROVSmokeTest")); } },
	};

	while (SmokeStep < Script.Num() && T >= Script[SmokeStep].Time)
	{
		Script[SmokeStep].Action();
		++SmokeStep;
	}
}
