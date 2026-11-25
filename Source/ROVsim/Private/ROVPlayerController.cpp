#include "ROVPlayerController.h"

#include "ROVPawn.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

AROVPlayerController::AROVPlayerController()
{
	// Atenção: NÃO desligar bAutoManageActiveCameraTarget. É ele que põe a câmera do PILOTO no ROV quando
	// o controller o possui; desligado, o host ficava com a tela preta (câmera presa no controller).
	// Para o observador (sem pawn), o PlayerTick abaixo aponta a câmera para o ROV a cada frame.

	// Aceita "-ROVObserverTest" (60 s) ou "-ROVObserverTest=90". Atenção: FParse::Param só reconhece a
	// opção SOZINHA; com "=valor" quem lê é o FParse::Value.
	bObserverTest = FParse::Param(FCommandLine::Get(), TEXT("ROVObserverTest"))
		|| FParse::Value(FCommandLine::Get(), TEXT("ROVObserverTest="), ObserverTestDuration);
}

AROVPawn* AROVPlayerController::FindROV() const
{
	if (AROVPawn* Own = Cast<AROVPawn>(GetPawn()))
	{
		return Own;
	}
	// Observador: o ROV é uma cópia (simulated proxy) que a rede criou neste mundo
	for (TActorIterator<AROVPawn> It(GetWorld()); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AROVPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	if (!IsLocalController() || !IsObserver())
	{
		return;
	}

	// O ROV do servidor pode chegar alguns frames depois de conectar: tenta até achar
	AROVPawn* ROV = FindROV();
	if (ROV && GetViewTarget() != ROV)
	{
		SetViewTarget(ROV); // a câmera usa a câmera ativa do ROV (a externa, atrás dele)
	}
	// (O servidor normalmente já mandou este alvo de câmera; o SetViewTarget acima é a garantia)
	if (ROV && !bAnnouncedObserver)
	{
		bAnnouncedObserver = true;
		UE_LOG(LogROV, Display, TEXT("OBSERVADOR: recebendo o ROV do servidor (%s), câmera nele: %s"),
			*ROV->GetName(), GetViewTarget() == ROV ? TEXT("sim") : TEXT("não"));
	}

	if (bObserverTest)
	{
		ObserverTime += DeltaTime;
		TickObserverTest(ROV);
	}
}

void AROVPlayerController::TickObserverTest(AROVPawn* ROV)
{
	if (ObserverTime >= NextObserverLog)
	{
		NextObserverLog += 2.f;
		if (!ROV)
		{
			UE_LOG(LogROV, Display, TEXT("OBSERVER t=%5.1f sem ROV ainda"), ObserverTime);
		}
		else
		{
			// Mesmos campos do log do servidor: comparando os dois logs no mesmo horário, os valores têm que bater
			const FROVTelemetry& T = ROV->GetTelemetry();
			const FVector Pos = ROV->GetActorLocation() / 100.f;
			UE_LOG(LogROV, Display,
				TEXT("OBSERVER t=%5.1f pos=(%7.1f,%7.1f,%6.1f)m prof=%6.2fm rumo=%6.1f vel=(%+5.2f,%+5.2f,%+5.2f)m/s luzes=%s autos=%d/%d/%d corrente=%.1fkn swell=%.1fm"),
				ObserverTime, Pos.X, Pos.Y, Pos.Z, T.DepthM, T.HeadingDeg,
				T.LocalVelocityMs.X, T.LocalVelocityMs.Y, T.LocalVelocityMs.Z,
				ROV->AreLightsOn() ? TEXT("ON") : TEXT("OFF"),
				static_cast<int32>(ROV->GetHeadingPhase()), static_cast<int32>(ROV->GetVerticalPhase()),
				static_cast<int32>(ROV->GetPositionPhase()),
				ROV->GetCurrentVelocityMs().Size() / 0.514444f, ROV->GetSwellHeightM());
		}
	}

	// Screenshots em 1/3 e 2/3 do teste
	if (ROV && ObserverScreenshots < 2 && ObserverTime >= ObserverTestDuration * (ObserverScreenshots + 1) / 3.f)
	{
		++ObserverScreenshots;
		const FString File = FPaths::Combine(FPaths::ScreenShotDir(), TEXT("ObserverTest"),
			FString::Printf(TEXT("observador_%d.png"), ObserverScreenshots));
		FScreenshotRequest::RequestScreenshot(File, true, false);
		UE_LOG(LogROV, Display, TEXT("OBSERVER screenshot: %s"), *File);
	}

	if (ObserverTime >= ObserverTestDuration)
	{
		UE_LOG(LogROV, Display, TEXT("OBSERVER TEST concluído"));
		bObserverTest = false;
		FGenericPlatformMisc::RequestExit(false, TEXT("ROVObserverTest"));
	}
}
