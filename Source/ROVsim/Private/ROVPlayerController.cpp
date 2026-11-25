#include "ROVPlayerController.h"

#include "ROVPawn.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraTypes.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "InputActionValue.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

AROVPlayerController::AROVPlayerController()
{
	// Atenção: NÃO desligar bAutoManageActiveCameraTarget. É ele que põe a câmera do PILOTO no ROV quando
	// o controller o possui; desligado, o host ficava com a tela preta (câmera presa no controller).
	// Para o observador (sem pawn), o PlayerTick abaixo escolhe o alvo da câmera a cada frame.

	// Aceita "-ROVObserverTest" (60 s) ou "-ROVObserverTest=90". Atenção: FParse::Param só reconhece a
	// opção SOZINHA; com "=valor" quem lê é o FParse::Value.
	bObserverTest = FParse::Param(FCommandLine::Get(), TEXT("ROVObserverTest"))
		|| FParse::Value(FCommandLine::Get(), TEXT("ROVObserverTest="), ObserverTestDuration);
	bNetTrace = FParse::Param(FCommandLine::Get(), TEXT("ROVNetTrace"));
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

// =============================================================================
// Câmera livre do observador
// =============================================================================

void AROVPlayerController::BuildObserverInput()
{
	if (ObserverContext)
	{
		return;
	}
	// Mesmo esquema do ROVPawn: Enhanced Input criado em C++. Este contexto só é ATIVADO para o
	// observador (ver PlayerTick); o piloto continua usando o contexto do pawn.
	ObserverContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Observer_Runtime"));

	ObserverLookAction = NewObject<UInputAction>(this, TEXT("IA_ObserverLook_Runtime"));
	ObserverLookAction->ValueType = EInputActionValueType::Axis2D;
	{
		// Mouse: Y invertido ("mouse para cima = olhar para cima"), igual à câmera do piloto
		FEnhancedActionKeyMapping& Mouse = ObserverContext->MapKey(ObserverLookAction, EKeys::Mouse2D);
		UInputModifierNegate* NegateY = NewObject<UInputModifierNegate>(ObserverContext);
		NegateY->bX = false;
		NegateY->bY = true;
		NegateY->bZ = false;
		Mouse.Modifiers.Add(NegateY);

		// Gamepad: stick direito, com zona morta (o stick nunca volta exatamente ao zero)
		FEnhancedActionKeyMapping& Stick = ObserverContext->MapKey(ObserverLookAction, EKeys::Gamepad_Right2D);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(ObserverContext);
		DeadZone->LowerThreshold = 0.15f;
		Stick.Modifiers.Add(DeadZone);
	}

	ObserverZoomAction = NewObject<UInputAction>(this, TEXT("IA_ObserverZoom_Runtime"));
	ObserverZoomAction->ValueType = EInputActionValueType::Axis1D;
	ObserverContext->MapKey(ObserverZoomAction, EKeys::MouseWheelAxis);

	ObserverViewAction = NewObject<UInputAction>(this, TEXT("IA_ObserverView_Runtime"));
	ObserverViewAction->ValueType = EInputActionValueType::Boolean;
	ObserverContext->MapKey(ObserverViewAction, EKeys::C);
	ObserverContext->MapKey(ObserverViewAction, EKeys::Gamepad_FaceButton_Top);
}

void AROVPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	BuildObserverInput();
	if (UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent))
	{
		Input->BindAction(ObserverLookAction, ETriggerEvent::Triggered, this, &AROVPlayerController::OnObserverLook);
		Input->BindAction(ObserverZoomAction, ETriggerEvent::Triggered, this, &AROVPlayerController::OnObserverZoom);
		Input->BindAction(ObserverViewAction, ETriggerEvent::Started, this, &AROVPlayerController::OnObserverToggleView);
	}
}

void AROVPlayerController::OnObserverLook(const FInputActionValue& Value)
{
	if (!IsObserver() || ObserverView != EROVObserverView::Orbit)
	{
		return;
	}
	const FVector2D Look = Value.Get<FVector2D>();
	OrbitYawOffset = FRotator::NormalizeAxis(OrbitYawOffset + Look.X * 2.5f);
	OrbitPitch = FMath::Clamp(OrbitPitch + Look.Y * 2.5f, -80.f, 45.f);
}

void AROVPlayerController::OnObserverZoom(const FInputActionValue& Value)
{
	if (IsObserver())
	{
		OrbitDistance = FMath::Clamp(OrbitDistance - Value.Get<float>() * 150.f, 400.f, 3000.f);
	}
}

void AROVPlayerController::OnObserverToggleView()
{
	if (IsObserver())
	{
		SetObserverView(ObserverView == EROVObserverView::Orbit ? EROVObserverView::RovCamera : EROVObserverView::Orbit);
	}
}

void AROVPlayerController::SetObserverView(EROVObserverView NewView)
{
	ObserverView = NewView;
	// A câmera ativa do ROV é LOCAL (não vai pela rede): trocar aqui não muda a tela do piloto
	if (AROVPawn* ROV = FindROV())
	{
		ROV->SetCameraMode(NewView == EROVObserverView::RovCamera ? EROVCameraMode::Cockpit : EROVCameraMode::External);
	}
}

void AROVPlayerController::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	// Quando o alvo da câmera é este controller (modo órbita do observador), a engine pergunta aqui
	// onde fica a câmera. É o mesmo cálculo de um spring arm, feito à mão.
	AROVPawn* ROV = IsObserver() ? FindROV() : nullptr;
	if (!ROV || ObserverView != EROVObserverView::Orbit)
	{
		Super::CalcCamera(DeltaTime, OutResult);
		return;
	}

	// Ponto que a câmera olha: um pouco acima do centro do ROV. Seguido com suavização: esconde
	// pequenas correções da rede (posição que chega atrasada e é ajustada).
	const FVector Target = ROV->GetActorLocation() + FVector(0.f, 0.f, 120.f);
	SmoothedPivot = bPivotInitialized ? FMath::VInterpTo(SmoothedPivot, Target, DeltaTime, 10.f) : Target;
	bPivotInitialized = true;

	// Órbita relativa ao rumo do ROV: se ele gira, a câmera gira junto (como a do piloto)
	const FRotator OrbitRotation(OrbitPitch, ROV->GetActorRotation().Yaw + OrbitYawOffset, 0.f);
	FVector CameraLocation = SmoothedPivot - OrbitRotation.Vector() * OrbitDistance;

	// Colisão: se houver fundo/rocha no caminho, a câmera chega mais perto (não atravessa o cenário)
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ROVObserverCamera), false, ROV);
	if (GetWorld()->SweepSingleByChannel(Hit, SmoothedPivot, CameraLocation, FQuat::Identity, ECC_Camera,
		FCollisionShape::MakeSphere(15.f), Params))
	{
		CameraLocation = Hit.Location;
	}

	OutResult.Location = CameraLocation;
	OutResult.Rotation = OrbitRotation;
	OutResult.FOV = 90.f;
}

void AROVPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	AROVPawn* ROV = FindROV();

	// Trilha de posição para comparar host e observador (tanto o piloto quanto o observador podem gravar)
	if (bNetTrace && ROV && IsLocalController())
	{
		const float Now = GetWorld()->GetRealTimeSeconds();
		if (Now >= NextTraceTime)
		{
			NextTraceTime = Now + 0.1f;
			const FVector Pos = ROV->GetActorLocation() / 100.f;
			UE_LOG(LogROV, Display, TEXT("NETTRACE %s pos=(%.3f,%.3f,%.3f)"),
				IsObserver() ? TEXT("OBS") : TEXT("HOST"), Pos.X, Pos.Y, Pos.Z);
		}
	}

	if (!IsLocalController() || !IsObserver())
	{
		return;
	}

	// O ROV do servidor pode chegar alguns frames depois de conectar: tenta até achar.
	// Órbita: a câmera é este controller (CalcCamera acima). Câmera do ROV: o próprio ROV.
	if (ROV)
	{
		AActor* WantedTarget = ObserverView == EROVObserverView::Orbit ? static_cast<AActor*>(this) : ROV;
		if (GetViewTarget() != WantedTarget)
		{
			SetViewTarget(WantedTarget);
		}
	}

	if (ROV && !bAnnouncedObserver)
	{
		bAnnouncedObserver = true;
		// Ativa os controles de câmera do observador
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(ObserverContext, 0);
		}
		UE_LOG(LogROV, Display, TEXT("OBSERVADOR: recebendo o ROV do servidor (%s); câmera livre em órbita"), *ROV->GetName());
	}

	if (bObserverTest)
	{
		ObserverTime += DeltaTime;
		TickObserverTest(ROV, DeltaTime);
	}
}

// =============================================================================
// Teste do observador
// =============================================================================

void AROVPlayerController::TickObserverTest(AROVPawn* ROV, float DeltaTime)
{
	// "Saltos": a cada frame, quanto o ROV andou além do que a velocidade dele explica.
	// Com a rede boa e a suavização funcionando, fica perto de zero; perda de pacotes sem
	// suavização aparece como o ROV "teletransportando" alguns centímetros ou metros.
	if (ROV)
	{
		const FVector Position = ROV->GetActorLocation() / 100.f;
		// Ao entrar, a cópia nasce num lugar e a suavização da física a "puxa" até a posição real
		// (1,5 m -> 0 em ~0,5 s no teste). Isso é a entrada do jogador, não falta de suavidade: ignora 3 s.
		RovSeenTime += DeltaTime;
		if (bHasLastPosition && DeltaTime > 0.f && RovSeenTime > 3.f)
		{
			const float Moved = (Position - LastObservedPosition).Size();
			const float Expected = (ROV->GetVelocity() / 100.f).Size() * DeltaTime;
			const float Jump = FMath::Max(0.f, Moved - Expected);
			float& MaxJump = ROV->GetControlMode() == EROVControlMode::Simulator ? MaxJumpSimM : MaxJumpDemoM;
			MaxJump = FMath::Max(MaxJump, Jump);
		}
		LastObservedPosition = Position;
		bHasLastPosition = true;

		// A câmera dá uma volta lenta em torno do ROV durante o teste (testa a órbita)
		if (ObserverView == EROVObserverView::Orbit)
		{
			OrbitYawOffset = FRotator::NormalizeAxis(OrbitYawOffset + 10.f * DeltaTime);
		}
	}

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

	// Na metade do teste, troca para a câmera do ROV. (Não no mesmo frame da foto: a captura só é
	// gravada no frame seguinte, e sairia já com a câmera trocada.)
	if (ObserverView == EROVObserverView::Orbit && ObserverTime >= ObserverTestDuration * 0.5f)
	{
		SetObserverView(EROVObserverView::RovCamera);
	}

	// Screenshots em 1/3 (câmera livre) e 2/3 (câmera do ROV) do teste
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
		// Critério: no simulador o ROV não pode "pular" mais que 25 cm num frame, mesmo com rede ruim.
		// O modo demonstração (sem física) não tem suavização: só registramos, para comparar.
		UE_LOG(LogROV, Display, TEXT("OBSERVER CHECK suavidade: %s (maior salto %.3f m no simulador, %.3f m no demo; limite 0,25 m)"),
			MaxJumpSimM < 0.25f ? TEXT("PASSOU") : TEXT("FALHOU"), MaxJumpSimM, MaxJumpDemoM);
		UE_LOG(LogROV, Display, TEXT("OBSERVER TEST concluído"));
		bObserverTest = false;
		FGenericPlatformMisc::RequestExit(false, TEXT("ROVObserverTest"));
	}
}
