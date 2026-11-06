#include "ROVPawn.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "UObject/ConstructorHelpers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "InputActionValue.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY(LogROV);

namespace ROVUnits
{
	// A Unreal trabalha em centímetros; a física do ROV é escrita em SI (metros, Newtons).
	constexpr float CmPerM = 100.f;
	constexpr float NewtonToUnreal = 100.f;        // N   -> kg·cm/s²
	constexpr float NewtonMeterToUnreal = 10000.f; // N·m -> kg·cm²/s²
}

// =============================================================================
// Construção
// =============================================================================

AROVPawn::AROVPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	// As forças são calculadas ANTES do passo de física de cada frame
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	// --- Corpo físico ---
	// Variável e componente se chamam "Hull" (antes era "Body", um StaticMesh): com um nome novo,
	// a Unreal não tenta carregar os dados antigos salvos no mapa num componente de outro tipo.
	Hull = CreateDefaultSubobject<UBoxComponent>(TEXT("Hull"));
	RootComponent = Hull;
	Hull->SetBoxExtent(FVector(183.f, 91.5f, 106.5f)); // metade de 3,66 x 1,83 x 2,13 m, em cm
	Hull->SetCollisionProfileName(TEXT("PhysicsActor")); // caixas vêm como "gatilho"; isto faz colidir
	Hull->SetSimulatePhysics(true);
	Hull->SetCanEverAffectNavigation(false);

	// Massa da ficha da MBARI (10.500 lb). Só grava o valor: recalcular massa no construtor gera erro.
	Hull->BodyInstance.SetMassOverride(4763.f, true);

	// A gravidade é aplicada por nós em TickSimulator (junto com as outras forças), para que a
	// massa adicional da água também afete a queda. Por isso a gravidade da engine fica desligada.
	Hull->BodyInstance.bEnableGravity = false;

	// Amortecimento genérico da engine desligado: todo o arrasto vem do modelo hidrodinâmico
	Hull->BodyInstance.LinearDamping = 0.f;
	Hull->BodyInstance.AngularDamping = 0.f;

	// O Chaos "adormece" corpos lentos e zera a velocidade deles. Um ROV parado na água
	// ainda sofre forças pequenas (a flutuação positiva o faz subir ~0,2 m/s), então
	// desligamos o sono deste corpo (limite de sono multiplicado por zero).
	Hull->BodyInstance.SleepFamily = ESleepFamily::Custom;
	Hull->BodyInstance.CustomSleepThresholdMultiplier = 0.f;

	// --- Modelo visual ---
	VisualMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(Hull);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VisualMesh->SetCanEverAffectNavigation(false);
	// O modelo foi feito com a frente em +Y; -90° em Yaw coloca a frente em +X (direção do surge)
	VisualMesh->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	// Centraliza o modelo na caixa (o pivô do modelo não fica no centro dele)
	VisualMesh->SetRelativeLocation(FVector(0.f, 0.f, -100.f));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> RovMesh(TEXT("/Game/ROV/DocRicketts/SM_DocRicketts.SM_DocRicketts"));
	if (RovMesh.Succeeded())
	{
		VisualMesh->SetStaticMesh(RovMesh.Object);
	}

	// --- Câmera externa (spring arm) ---
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(Hull);
	SpringArm->TargetArmLength = DesiredArmLength;
	SpringArm->TargetOffset = FVector(0.f, 0.f, 120.f); // mira um pouco acima do centro do ROV
	SpringArm->bUsePawnControlRotation = true;          // o mouse orbita a câmera
	SpringArm->bDoCollisionTest = true;                  // encurta o braço se bater no fundo/rochas
	SpringArm->ProbeSize = 15.f;
	SpringArm->ProbeChannel = ECC_Camera;
	SpringArm->bEnableCameraLag = true;                  // a câmera "persegue" o ROV com suavidade
	SpringArm->CameraLagSpeed = 5.f;
	SpringArm->CameraLagMaxDistance = 400.f;
	SpringArm->bEnableCameraRotationLag = true;
	SpringArm->CameraRotationLagSpeed = 8.f;

	ExternalCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ExternalCamera"));
	ExternalCamera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	ExternalCamera->bUsePawnControlRotation = false; // quem gira é o braço

	// --- Câmera de cockpit (câmera de vídeo do ROV, na frente, com pan/tilt) ---
	CockpitCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("CockpitCamera"));
	CockpitCamera->SetupAttachment(Hull);
	CockpitCamera->SetRelativeLocation(FVector(190.f, 0.f, 10.f));
	CockpitCamera->SetFieldOfView(90.f);
	CockpitCamera->bUsePawnControlRotation = false;
	CockpitCamera->SetAutoActivate(false);

	// --- Faróis ---
	// Ficam no alto e nas laterais, longe da câmera, apontando para frente e para baixo.
	// Luz saindo perto da lente ilumina as partículas logo à frente dela e "lava" a imagem
	// (backscatter) - por isso ROVs reais afastam as luzes da câmera.
	auto MakeLight = [this](const TCHAR* Name, float SideOffset)
	{
		USpotLightComponent* Light = CreateDefaultSubobject<USpotLightComponent>(Name);
		Light->SetupAttachment(Hull);
		Light->SetRelativeLocation(FVector(160.f, SideOffset, 95.f));
		Light->SetRelativeRotation(FRotator(-25.f, 0.f, 0.f));
		Light->IntensityUnits = ELightUnits::Candelas;
		Light->Intensity = 1500.f;
		Light->AttenuationRadius = 4000.f;
		Light->InnerConeAngle = 15.f;
		Light->OuterConeAngle = 30.f;
		Light->VolumetricScatteringIntensity = 0.5f; // cone visível na água, sem ofuscar a câmera
		return Light;
	};
	LightLeft = MakeLight(TEXT("LightLeft"), -90.f);
	LightRight = MakeLight(TEXT("LightRight"), 90.f);

	// Quem cria e possui o ROV é o AROVGameMode (no PlayerStart), por isso não há AutoPossess aqui.
}

void AROVPawn::BeginPlay()
{
	Super::BeginPlay();

	LastCameraYaw = GetActorRotation().Yaw;
	SpringArm->TargetArmLength = DesiredArmLength;

	// Aplica o estado inicial escolhido no painel Details
	const EROVControlMode InitialMode = ControlMode;
	ControlMode = EROVControlMode::Simulator; // o corpo nasce simulando
	SetControlMode(InitialMode);
	SetCameraMode(CameraMode);
	SetLightsOn(bLightsOn);
}

// =============================================================================
// Modos
// =============================================================================

void AROVPawn::SetControlMode(EROVControlMode NewMode)
{
	using namespace ROVUnits;
	const bool bPhysicsWasOn = Hull->IsSimulatingPhysics();
	ControlMode = NewMode;

	if (NewMode == EROVControlMode::Demo)
	{
		if (bPhysicsWasOn)
		{
			// Mantém o movimento atual para a troca ser suave
			DemoVelocityMs = Hull->GetPhysicsLinearVelocity() / CmPerM;
			DemoYawRateDeg = FMath::RadiansToDegrees(Hull->GetPhysicsAngularVelocityInRadians().Z);
			Hull->SetSimulatePhysics(false);
		}
		// No modo demonstração o ROV fica sempre nivelado: só existe rotação em yaw
		SetActorRotation(FRotator(0.f, GetActorRotation().Yaw, 0.f));
	}
	else
	{
		if (!bPhysicsWasOn)
		{
			Hull->SetSimulatePhysics(true);
			Hull->SetPhysicsLinearVelocity(DemoVelocityMs * CmPerM);
			Hull->SetPhysicsAngularVelocityInRadians(FVector(0.f, 0.f, FMath::DegreesToRadians(DemoYawRateDeg)));
		}
		// Propulsores partem do comando atual (sem "tranco" na troca)
		FilteredThrust = PilotTranslation;
		FilteredYaw = PilotYaw;
	}

	UE_LOG(LogROV, Display, TEXT("Modo de controle: %s"),
		NewMode == EROVControlMode::Simulator ? TEXT("SIMULADOR") : TEXT("DEMONSTRACAO"));
}

void AROVPawn::SetCameraMode(EROVCameraMode NewMode)
{
	CameraMode = NewMode;
	const bool bExternal = (NewMode == EROVCameraMode::External);

	// A Unreal usa a primeira câmera ATIVA do Pawn como ponto de vista
	ExternalCamera->SetActive(bExternal);
	CockpitCamera->SetActive(!bExternal);

	if (bExternal)
	{
		// Volta a câmera para trás do ROV, olhando levemente para baixo
		if (Controller)
		{
			Controller->SetControlRotation(FRotator(-15.f, GetActorRotation().Yaw, 0.f));
		}
	}
	else
	{
		CockpitPanTilt = FRotator::ZeroRotator; // câmera de piloto começa olhando para frente
		CockpitCamera->SetRelativeRotation(CockpitPanTilt);
	}
}

void AROVPawn::SetLightsOn(bool bOn)
{
	bLightsOn = bOn;
	LightLeft->SetVisibility(bOn);
	LightRight->SetVisibility(bOn);
}

void AROVPawn::SetPilotCommand(FVector Translation, float Yaw)
{
	PilotTranslation = Translation.BoundToCube(1.f);
	PilotYaw = FMath::Clamp(Yaw, -1.f, 1.f);
}

// =============================================================================
// Tick
// =============================================================================

void AROVPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (DeltaTime <= 0.f)
	{
		return;
	}

	if (ControlMode == EROVControlMode::Simulator)
	{
		TickSimulator(DeltaTime);
	}
	else
	{
		TickDemo(DeltaTime);
	}

	UpdateCameras(DeltaTime);
	UpdateTelemetry();
}

float AROVPawn::GetSubmergedFraction() const
{
	// 0 = fora d'água, 1 = totalmente submerso (aproximação pela altura do centro da caixa)
	const float HalfHeight = Hull->GetScaledBoxExtent().Z;
	const float CenterDepth = WaterSurfaceZ - Hull->GetComponentLocation().Z;
	return FMath::Clamp((CenterDepth + HalfHeight) / (2.f * HalfHeight), 0.f, 1.f);
}

/**
 * Modo SIMULADOR: soma todas as forças e momentos em SI e aplica no corpo.
 *
 *   Peso  + Empuxo (no centro de carena) + Arrasto (linear + quadrático, por eixo)
 *   + Propulsores (com atraso de 1ª ordem), tudo dividido pela massa/inércia adicional.
 *
 * O arrasto e a massa adicional são calculados no referencial do PRÓPRIO ROV, porque
 * cada eixo tem área, coeficiente e massa adicional diferentes (modelo diagonal de Fossen).
 */
void AROVPawn::TickSimulator(float DeltaTime)
{
	using namespace ROVUnits;

	if (!Hull->IsSimulatingPhysics())
	{
		return;
	}
	// Sem a gravidade da engine, o Chaos pode "adormecer" o corpo parado; mantemos acordado
	Hull->WakeRigidBody();

	const FQuat Orientation = Hull->GetComponentQuat();
	const float Submerged = GetSubmergedFraction();
	const float Gravity = FMath::Abs(GetWorld()->GetGravityZ()) / CmPerM; // m/s²
	const float Mass = Hull->GetMass();                                    // kg

	// --- 1) Peso, no centro de gravidade ---
	FVector ForceWorld(0.f, 0.f, -Mass * Gravity); // N
	FVector TorqueWorld = FVector::ZeroVector;    // N·m

	// --- 2) Empuxo de Arquimedes, no centro de carena (BG acima do CG) ---
	// Como o empuxo puxa para cima num ponto acima do CG, qualquer inclinação gera um
	// momento que endireita o ROV (é o que mantém um ROV real estável em pitch e roll).
	const FVector BuoyancyForce(0.f, 0.f, WaterDensity * DisplacedVolume * Submerged * Gravity);
	const FVector LeverArmM = Orientation.RotateVector(FVector(0.f, 0.f, BuoyancyCenterHeight)) / CmPerM;
	ForceWorld += BuoyancyForce;
	TorqueWorld += FVector::CrossProduct(LeverArmM, BuoyancyForce);

	// --- 3) Hidrodinâmica no referencial do ROV ---
	const FVector VelocityLocal = Orientation.UnrotateVector(Hull->GetPhysicsLinearVelocity() / CmPerM); // m/s
	const FVector OmegaLocal = Orientation.UnrotateVector(Hull->GetPhysicsAngularVelocityInRadians());   // rad/s

	FVector DragLocal = FVector::ZeroVector;
	FVector DampingTorqueLocal = FVector::ZeroVector;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const double V = VelocityLocal[Axis];
		const double QuadraticCoef = 0.5 * WaterDensity * DragCoefficients[Axis] * ProjectedAreas[Axis]; // kg/m
		DragLocal[Axis] = -(LinearDamping[Axis] * V + QuadraticCoef * FMath::Abs(V) * V) * Submerged;
		DampingTorqueLocal[Axis] = -AngularDamping[Axis] * OmegaLocal[Axis] * Submerged;
	}

	// --- 4) Propulsores, com atraso de resposta (filtro de 1ª ordem) ---
	const float Alpha = 1.f - FMath::Exp(-DeltaTime / FMath::Max(ThrusterTimeConstant, KINDA_SMALL_NUMBER));
	FilteredThrust += (PilotTranslation - FilteredThrust) * Alpha;
	FilteredYaw += (PilotYaw - FilteredYaw) * Alpha;

	// Hélice fora d'água não empurra (ventilação): empuxo proporcional à parte submersa
	const FVector ThrustLocal = FilteredThrust * MaxThrust * Submerged;                  // N (por eixo)
	const FVector YawTorqueLocal(0.f, 0.f, FilteredYaw * MaxYawTorque * Submerged);    // N·m

	// --- 5) Soma no referencial local e divide pela massa/inércia adicional ---
	// Acelerar um corpo na água também acelera a água em volta dele: na prática a massa
	// "efetiva" é maior. Dividir a força por (1 + razão) dá a mesma aceleração que m + m_a.
	FVector ForceLocal = Orientation.UnrotateVector(ForceWorld) + DragLocal + ThrustLocal;
	FVector TorqueLocal = Orientation.UnrotateVector(TorqueWorld) + DampingTorqueLocal + YawTorqueLocal;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		ForceLocal[Axis] /= (1.0 + AddedMassRatio[Axis] * Submerged);
		TorqueLocal[Axis] /= (1.0 + AddedInertiaRatio[Axis] * Submerged);
	}

	Hull->AddForce(Orientation.RotateVector(ForceLocal) * NewtonToUnreal);
	Hull->AddTorqueInRadians(Orientation.RotateVector(TorqueLocal) * NewtonMeterToUnreal);
}

/**
 * Modo DEMONSTRAÇÃO: sem física. O comando vira velocidade direta (suavizada),
 * o ROV fica nivelado e se move com varredura de colisão (não atravessa o fundo).
 */
void AROVPawn::TickDemo(float DeltaTime)
{
	using namespace ROVUnits;

	const float Blend = 1.f - FMath::Exp(-DemoResponsiveness * DeltaTime);
	const FRotator Heading(0.f, GetActorRotation().Yaw, 0.f);

	// Comando local (frente/lado/vertical) -> velocidade alvo no mundo
	const FVector TargetVelocity = Heading.RotateVector(PilotTranslation * DemoMaxSpeed);
	DemoVelocityMs += (TargetVelocity - DemoVelocityMs) * Blend;
	DemoYawRateDeg += (PilotYaw * DemoYawRate - DemoYawRateDeg) * Blend;

	// Rotação: só yaw
	SetActorRotation(FRotator(0.f, Heading.Yaw + DemoYawRateDeg * DeltaTime, 0.f));

	// Translação com varredura: para no fundo/rochas e desliza ao longo delas
	FVector Delta = DemoVelocityMs * CmPerM * DeltaTime;

	// Não deixa o ROV sair da água (centro no máximo na superfície)
	const float CurrentZ = GetActorLocation().Z;
	if (CurrentZ + Delta.Z > WaterSurfaceZ)
	{
		Delta.Z = FMath::Max(0.f, WaterSurfaceZ - CurrentZ);
		DemoVelocityMs.Z = FMath::Min(DemoVelocityMs.Z, 0.0);
	}

	FHitResult Hit;
	AddActorWorldOffset(Delta, true, &Hit);
	if (Hit.bStartPenetrating)
	{
		// Começou o frame dentro de algo (ex.: girou encostado numa rocha): empurra para fora
		AddActorWorldOffset(Hit.Normal * (Hit.PenetrationDepth + 1.f), false);
	}
	else if (Hit.bBlockingHit)
	{
		const FVector Remaining = Delta * (1.f - Hit.Time);
		AddActorWorldOffset(FVector::VectorPlaneProject(Remaining, Hit.Normal), true);
		DemoVelocityMs = FVector::VectorPlaneProject(DemoVelocityMs, Hit.Normal);
	}
}

void AROVPawn::UpdateCameras(float DeltaTime)
{
	// O spring arm usa a rotação de controle (mouse). Somamos a ela o quanto o ROV girou
	// neste frame: a câmera acompanha o rumo do ROV, mas o mouse continua orbitando.
	const float CurrentYaw = GetActorRotation().Yaw;
	const float YawDelta = FMath::FindDeltaAngleDegrees(LastCameraYaw, CurrentYaw);
	LastCameraYaw = CurrentYaw;

	if (Controller && CameraMode == EROVCameraMode::External)
	{
		FRotator ControlRotation = Controller->GetControlRotation();
		ControlRotation.Yaw += YawDelta;
		Controller->SetControlRotation(ControlRotation);
	}

	// Zoom suave
	SpringArm->TargetArmLength = FMath::FInterpTo(SpringArm->TargetArmLength, DesiredArmLength, DeltaTime, 8.f);

	// Pan/tilt da câmera de cockpit
	CockpitCamera->SetRelativeRotation(CockpitPanTilt);
}

void AROVPawn::UpdateTelemetry()
{
	using namespace ROVUnits;

	const FVector Location = GetActorLocation();
	const FRotator Rotation = GetActorRotation();

	Telemetry.DepthM = (WaterSurfaceZ - Location.Z) / CmPerM;
	Telemetry.HeadingDeg = FRotator::ClampAxis(Rotation.Yaw);
	Telemetry.PitchDeg = Rotation.Pitch;
	Telemetry.RollDeg = Rotation.Roll;
	Telemetry.SubmergedFraction = GetSubmergedFraction();

	// GetMass() só funciona com a física ligada (no modo demo ela está desligada)
	const bool bPhysics = Hull->IsSimulatingPhysics();
	if (bPhysics)
	{
		Telemetry.MassKg = Hull->GetMass();
	}
	const FVector VelocityWorld = bPhysics ? Hull->GetPhysicsLinearVelocity() / CmPerM : DemoVelocityMs;
	Telemetry.LocalVelocityMs = GetActorQuat().UnrotateVector(VelocityWorld);
	Telemetry.YawRateDegS = bPhysics
		? FMath::RadiansToDegrees(Hull->GetPhysicsAngularVelocityInRadians().Z)
		: DemoYawRateDeg;

	const bool bSim = (ControlMode == EROVControlMode::Simulator);
	Telemetry.ThrustPercent = (bSim ? FilteredThrust : PilotTranslation) * 100.f;
	Telemetry.YawThrustPercent = (bSim ? FilteredYaw : PilotYaw) * 100.f;

	// Altímetro: raio para baixo até 100 m, medido a partir da base do casco
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ROVAltimeter), false, this);
	const FVector End = Location - FVector(0.f, 0.f, 100.f * CmPerM);
	Telemetry.AltitudeM = GetWorld()->LineTraceSingleByChannel(Hit, Location, End, ECC_Visibility, Params)
		? (Hit.Distance - Hull->GetScaledBoxExtent().Z) / CmPerM
		: -1.f;
}

// =============================================================================
// Input (Enhanced Input criado em C++)
// =============================================================================

void AROVPawn::BuildInputMappings()
{
	if (InputContext)
	{
		return;
	}

	InputContext = NewObject<UInputMappingContext>(this, TEXT("IMC_ROV_Runtime"));

	auto MakeAction = [this](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = Type;
		return Action;
	};

	// Mapeia uma tecla; bNegate inverte o sinal (ex.: S = surge negativo)
	auto Map = [this](UInputAction* Action, const FKey& Key, bool bNegate)
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(Action, Key);
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));
		}
	};

	SurgeAction = MakeAction(TEXT("IA_Surge_Runtime"), EInputActionValueType::Axis1D);
	Map(SurgeAction, EKeys::W, false);
	Map(SurgeAction, EKeys::S, true);
	Map(SurgeAction, EKeys::Gamepad_LeftY, false);

	SwayAction = MakeAction(TEXT("IA_Sway_Runtime"), EInputActionValueType::Axis1D);
	Map(SwayAction, EKeys::D, false);
	Map(SwayAction, EKeys::A, true);
	Map(SwayAction, EKeys::Gamepad_LeftX, false);

	HeaveAction = MakeAction(TEXT("IA_Heave_Runtime"), EInputActionValueType::Axis1D);
	Map(HeaveAction, EKeys::E, false);
	Map(HeaveAction, EKeys::Q, true);
	Map(HeaveAction, EKeys::Gamepad_RightShoulder, false);
	Map(HeaveAction, EKeys::Gamepad_LeftShoulder, true);

	YawAction = MakeAction(TEXT("IA_Yaw_Runtime"), EInputActionValueType::Axis1D);
	Map(YawAction, EKeys::Right, false);
	Map(YawAction, EKeys::Left, true);
	Map(YawAction, EKeys::Gamepad_RightTriggerAxis, false);
	Map(YawAction, EKeys::Gamepad_LeftTriggerAxis, true);

	// Mouse: Y invertido para "mouse para cima = olhar para cima"
	LookAction = MakeAction(TEXT("IA_Look_Runtime"), EInputActionValueType::Axis2D);
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(LookAction, EKeys::Mouse2D);
		UInputModifierNegate* NegateY = NewObject<UInputModifierNegate>(InputContext);
		NegateY->bX = false;
		NegateY->bY = true;
		NegateY->bZ = false;
		Mapping.Modifiers.Add(NegateY);
	}

	ZoomAction = MakeAction(TEXT("IA_Zoom_Runtime"), EInputActionValueType::Axis1D);
	Map(ZoomAction, EKeys::MouseWheelAxis, false);

	ToggleCameraAction = MakeAction(TEXT("IA_ToggleCamera_Runtime"), EInputActionValueType::Boolean);
	Map(ToggleCameraAction, EKeys::C, false);
	Map(ToggleCameraAction, EKeys::Gamepad_FaceButton_Top, false);

	ToggleModeAction = MakeAction(TEXT("IA_ToggleMode_Runtime"), EInputActionValueType::Boolean);
	Map(ToggleModeAction, EKeys::M, false);
	Map(ToggleModeAction, EKeys::Gamepad_Special_Right, false);

	ToggleLightsAction = MakeAction(TEXT("IA_ToggleLights_Runtime"), EInputActionValueType::Boolean);
	Map(ToggleLightsAction, EKeys::L, false);
	Map(ToggleLightsAction, EKeys::Gamepad_FaceButton_Left, false);
}

void AROVPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogROV, Error, TEXT("O projeto não está usando o Enhanced Input Component"));
		return;
	}

	BuildInputMappings();

	// Eixos: Triggered enquanto pressiona, Completed ao soltar (valor volta a 0)
	Input->BindAction(SurgeAction, ETriggerEvent::Triggered, this, &AROVPawn::OnSurge);
	Input->BindAction(SurgeAction, ETriggerEvent::Completed, this, &AROVPawn::OnSurge);
	Input->BindAction(SwayAction, ETriggerEvent::Triggered, this, &AROVPawn::OnSway);
	Input->BindAction(SwayAction, ETriggerEvent::Completed, this, &AROVPawn::OnSway);
	Input->BindAction(HeaveAction, ETriggerEvent::Triggered, this, &AROVPawn::OnHeave);
	Input->BindAction(HeaveAction, ETriggerEvent::Completed, this, &AROVPawn::OnHeave);
	Input->BindAction(YawAction, ETriggerEvent::Triggered, this, &AROVPawn::OnYaw);
	Input->BindAction(YawAction, ETriggerEvent::Completed, this, &AROVPawn::OnYaw);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &AROVPawn::OnLook);
	Input->BindAction(ZoomAction, ETriggerEvent::Triggered, this, &AROVPawn::OnZoom);

	// Botões: Started dispara uma vez por toque
	Input->BindAction(ToggleCameraAction, ETriggerEvent::Started, this, &AROVPawn::OnToggleCamera);
	Input->BindAction(ToggleModeAction, ETriggerEvent::Started, this, &AROVPawn::OnToggleMode);
	Input->BindAction(ToggleLightsAction, ETriggerEvent::Started, this, &AROVPawn::OnToggleLights);
}

void AROVPawn::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		BuildInputMappings();
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(InputContext, 0);
		}

		ApplyCameraLimits();
		PC->SetControlRotation(FRotator(-15.f, GetActorRotation().Yaw, 0.f));
		LastCameraYaw = GetActorRotation().Yaw;
	}
}

void AROVPawn::ApplyCameraLimits()
{
	// Limita o quanto a câmera externa pode olhar para cima/baixo
	if (const APlayerController* PC = Cast<APlayerController>(Controller))
	{
		if (PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->ViewPitchMin = -80.f;
			PC->PlayerCameraManager->ViewPitchMax = 45.f;
		}
	}
}

void AROVPawn::OnSurge(const FInputActionValue& Value) { PilotTranslation.X = FMath::Clamp(Value.Get<float>(), -1.f, 1.f); }
void AROVPawn::OnSway(const FInputActionValue& Value) { PilotTranslation.Y = FMath::Clamp(Value.Get<float>(), -1.f, 1.f); }
void AROVPawn::OnHeave(const FInputActionValue& Value) { PilotTranslation.Z = FMath::Clamp(Value.Get<float>(), -1.f, 1.f); }
void AROVPawn::OnYaw(const FInputActionValue& Value) { PilotYaw = FMath::Clamp(Value.Get<float>(), -1.f, 1.f); }

void AROVPawn::OnLook(const FInputActionValue& Value)
{
	const FVector2D Look = Value.Get<FVector2D>();

	if (CameraMode == EROVCameraMode::External)
	{
		// Orbita o spring arm
		AddControllerYawInput(Look.X);
		AddControllerPitchInput(Look.Y);
	}
	else
	{
		// Pan/tilt da câmera de vídeo, com os limites mecânicos de uma câmera real
		CockpitPanTilt.Yaw = FMath::Clamp(CockpitPanTilt.Yaw + Look.X * CockpitLookSensitivity, -CockpitPanLimit, CockpitPanLimit);
		CockpitPanTilt.Pitch = FMath::Clamp(CockpitPanTilt.Pitch - Look.Y * CockpitLookSensitivity, CockpitTiltMin, CockpitTiltMax);
	}
}

void AROVPawn::OnZoom(const FInputActionValue& Value)
{
	DesiredArmLength = FMath::Clamp(DesiredArmLength - Value.Get<float>() * ZoomStep, MinArmLength, MaxArmLength);
}

void AROVPawn::OnToggleCamera()
{
	SetCameraMode(CameraMode == EROVCameraMode::External ? EROVCameraMode::Cockpit : EROVCameraMode::External);
}

void AROVPawn::OnToggleMode()
{
	SetControlMode(ControlMode == EROVControlMode::Simulator ? EROVControlMode::Demo : EROVControlMode::Simulator);
}

void AROVPawn::OnToggleLights()
{
	SetLightsOn(!bLightsOn);
}
