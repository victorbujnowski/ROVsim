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
#include "InputTriggers.h"
#include "InputActionValue.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "ROVUdpLinkComponent.h"
#include "ROVTcpCommandComponent.h"
#include "ROVSensorEmulatorComponent.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

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

	// Anti-windup do auto depth/altitude: segurar a flutuação de 13 kg exige só ~2,4% do empuxo vertical.
	// Limitar o I a 10% dá folga para isso sem deixá-lo acumular durante uma descida longa.
	DepthPid.IntegralLimit = 0.1f;

	// Auto heading: o I só precisa cobrir o torque do cata-vento (~14% com 1 nó de lado); 30% dá folga sem acumular demais
	HeadingPid.IntegralLimit = 0.3f;

	// Station keeping: saída em frações de MaxThrust.Y (4000 N). Limite 1,6 = 6375 N, o empuxo de surge.
	// O I pode chegar a 0,8 (3200 N): 1 nó de corrente de lado pede ~1400 N, de frente ~800 N.
	for (FROVPid* Pid : { &NorthPid, &EastPid })
	{
		Pid->OutputLimit = 1.6f;
		Pid->IntegralLimit = 0.8f;
	}

	// Link de rede: componente sem posição no espaço, por isso não tem SetupAttachment
	UdpLink = CreateDefaultSubobject<UROVUdpLinkComponent>(TEXT("UdpLink"));
	TcpCommands = CreateDefaultSubobject<UROVTcpCommandComponent>(TEXT("TcpCommands"));
	Sensors = CreateDefaultSubobject<UROVSensorEmulatorComponent>(TEXT("Sensors"));

	// Neve marinha: o asset NS_MarineSnow é criado pelo commandlet ROVCreateMarineSnow (módulo ROVsimTools).
	// Fica presa na câmera ativa (ver SetCameraMode): os flocos nascem numa caixa em volta de quem olha
	// e ficam parados no mundo, então passam pela câmera quando o ROV anda - é a melhor pista visual de
	// movimento debaixo d'água, onde quase não há referência. Brilham mais perto dos faróis.
	MarineSnow = CreateDefaultSubobject<UNiagaraComponent>(TEXT("MarineSnow"));
	MarineSnow->SetupAttachment(ExternalCamera);
	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> SnowSystem(TEXT("/Game/ROV/FX/NS_MarineSnow.NS_MarineSnow"));
	if (SnowSystem.Succeeded())
	{
		MarineSnow->SetAsset(SnowSystem.Object);
	}

	// Quem cria e possui o ROV é o AROVGameMode (no PlayerStart), por isso não há AutoPossess aqui.
}

void AROVPawn::BeginPlay()
{
	Super::BeginPlay();

	LastCameraYaw = GetActorRotation().Yaw;
	StartTransform = GetActorTransform();
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

	// A neve acompanha a câmera ativa
	MarineSnow->AttachToComponent(bExternal ? ExternalCamera.Get() : CockpitCamera.Get(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);

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

void AROVPawn::ResetToStart()
{
	// Zera os comandos e o estado interno dos dois modos
	SetPilotCommand(FVector::ZeroVector, 0.f);
	FilteredThrust = FVector::ZeroVector;
	FilteredYaw = 0.f;
	DemoVelocityMs = FVector::ZeroVector;
	DemoYawRateDeg = 0.f;

	// TeleportPhysics: move o corpo físico junto, sem gerar velocidade pelo "salto"
	SetActorTransform(StartTransform, false, nullptr, ETeleportType::TeleportPhysics);
	if (Hull->IsSimulatingPhysics())
	{
		Hull->SetPhysicsLinearVelocity(FVector::ZeroVector);
		Hull->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
	}
	LastCameraYaw = GetActorRotation().Yaw;

	// O salto confundiria o filtro de ondas; e o station keeping passa a segurar o ponto inicial
	for (FROVNotchFilter* Filter : { &WaveFilterPosN, &WaveFilterPosE, &WaveFilterVelN, &WaveFilterVelE })
	{
		Filter->Reset();
	}
	if (bPositionHold)
	{
		SetTargetPosition(StartTransform.GetLocation().X / ROVUnits::CmPerM, StartTransform.GetLocation().Y / ROVUnits::CmPerM);
	}

	UE_LOG(LogROV, Display, TEXT("ROV reiniciado na posição inicial"));
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

	// Pilotos automáticos decidem o comando ANTES de a física usá-lo
	UpdateAutopilots(DeltaTime);

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
	// Com swell, a superfície sobe e desce (η): perto da superfície o casco sai e entra na água
	const FVector Location = Hull->GetComponentLocation();
	const float HalfHeight = Hull->GetScaledBoxExtent().Z;
	const float CenterDepth = WaterSurfaceZ + GetSwellElevationM(Location) * ROVUnits::CmPerM - Location.Z;
	return FMath::Clamp((CenterDepth + HalfHeight) / (2.f * HalfHeight), 0.f, 1.f);
}

FVector AROVPawn::GetCurrentVelocityMs() const
{
	// Nós -> m/s, e direção -> vetor horizontal (+X = Norte, +Y = Leste, igual ao rumo do HUD)
	const float SpeedMs = CurrentSpeedKnots * 0.514444f;
	const float DirectionRad = FMath::DegreesToRadians(CurrentTowardDeg);
	return FVector(FMath::Cos(DirectionRad), FMath::Sin(DirectionRad), 0.f) * SpeedMs;
}

void AROVPawn::SetCurrent(float SpeedKnots, float TowardDeg)
{
	CurrentSpeedKnots = FMath::Max(0.f, SpeedKnots);
	CurrentTowardDeg = FRotator::ClampAxis(TowardDeg); // qualquer ângulo vira 0..360
	UE_LOG(LogROV, Display, TEXT("Corrente: %.2f nós para %.0f graus"), CurrentSpeedKnots, CurrentTowardDeg);
}

// =============================================================================
// Swell (teoria linear de ondas de Airy, águas profundas)
//
//   ω = 2π/T          frequência angular (rad/s)
//   k = ω²/g          número de onda (rad/m)  ->  λ = 2π/k = g·T²/2π
//   a = H/2           amplitude
//   θ = k·(x·dx + y·dy) − ω·t     fase no ponto (dx, dy = direção de propagação)
//
// Cada partícula de água anda em CÍRCULO (órbita) de raio a·e^(−k·z): a velocidade
// horizontal é a·ω·e^(−kz)·cos θ e a vertical a·ω·e^(−kz)·sin θ. O fator e^(−kz) faz
// a onda "sumir" com a profundidade: a meio comprimento de onda já sobra só 4%.
// "Águas profundas" vale quando o fundo está a mais de λ/2 (fora isso as órbitas viram elipses).
// =============================================================================

namespace ROVSwell
{
	/** Número de onda k = ω²/g, em rad/m */
	float WaveNumber(float PeriodS, float Gravity)
	{
		const float Omega = 2.f * PI / PeriodS;
		return Omega * Omega / Gravity;
	}
}

void AROVPawn::SetSwell(float HeightM, float PeriodS, float FromDeg)
{
	// Ligando agora (estava desligado): começa a rampa de entrada
	if (!IsSwellOn() && HeightM > 0.f && GetWorld())
	{
		SwellStartTime = GetWorld()->GetTimeSeconds();
	}

	SwellPeriodS = FMath::Clamp(PeriodS, 2.f, 30.f);
	// Uma onda real quebra quando fica íngreme demais: altura máxima ≈ 1/7 do comprimento (limite de Stokes)
	const float MaxHeight = GetSwellWavelengthM() / 7.f;
	SwellHeightM = FMath::Clamp(HeightM, 0.f, MaxHeight);
	SwellFromDeg = FRotator::ClampAxis(FromDeg);
	UE_LOG(LogROV, Display, TEXT("Swell: %.2f m, %.1f s, vindo de %.0f graus (λ %.0f m)"),
		SwellHeightM, SwellPeriodS, SwellFromDeg, GetSwellWavelengthM());
}

float AROVPawn::GetSwellWavelengthM() const
{
	const float Gravity = FMath::Abs(GetWorld() ? GetWorld()->GetGravityZ() : -980.f) / ROVUnits::CmPerM;
	return 2.f * PI / ROVSwell::WaveNumber(SwellPeriodS, Gravity);
}

float AROVPawn::GetSwellAttenuation(float DepthM) const
{
	const float K = 2.f * PI / GetSwellWavelengthM();
	return FMath::Exp(-K * FMath::Max(0.f, DepthM)); // acima da superfície média, sem amplificar
}

void AROVPawn::GetSwellKinematics(const FVector& LocationCm, FVector& OutVelocityMs, FVector& OutAccelerationMs2) const
{
	OutVelocityMs = FVector::ZeroVector;
	OutAccelerationMs2 = FVector::ZeroVector;
	if (!IsSwellOn() || !GetWorld())
	{
		return;
	}

	const float Omega = 2.f * PI / SwellPeriodS;
	const float K = 2.f * PI / GetSwellWavelengthM();
	const float Amplitude = 0.5f * SwellHeightM * GetSwellRamp();

	// "De onde vem" + 180° = "para onde vai" (direção de propagação)
	const float FromRad = FMath::DegreesToRadians(SwellFromDeg);
	const FVector2D Direction(-FMath::Cos(FromRad), -FMath::Sin(FromRad));

	const FVector2D PositionM(LocationCm.X / ROVUnits::CmPerM, LocationCm.Y / ROVUnits::CmPerM);
	const float DepthM = (WaterSurfaceZ - LocationCm.Z) / ROVUnits::CmPerM;
	const float Theta = K * FVector2D::DotProduct(PositionM, Direction) - Omega * GetWorld()->GetTimeSeconds();

	const float Speed = Amplitude * Omega * GetSwellAttenuation(DepthM); // raio da órbita × ω
	const float CosT = FMath::Cos(Theta);
	const float SinT = FMath::Sin(Theta);

	// Velocidade orbital: horizontal na direção de propagação, vertical defasada 90°
	OutVelocityMs = FVector(Direction.X * Speed * CosT, Direction.Y * Speed * CosT, Speed * SinT);
	// Aceleração = derivada no tempo (θ diminui com t, por isso dθ/dt = −ω)
	OutAccelerationMs2 = FVector(Direction.X * Speed * Omega * SinT, Direction.Y * Speed * Omega * SinT, -Speed * Omega * CosT);

	// + aceleração CONVECTIVA (u·∇)u: a partícula de água muda de lugar dentro da onda, e lá a velocidade
	// é outra. Em onda de Airy isso vale ∇(½|u|²) = k·|u|² para CIMA (|u| só depende da profundidade).
	// Sem este termo, o ROV que acompanha a órbita sente em média uma força para baixo e afunda
	// devagar - coisa que a água de verdade não faz (o movimento vertical médio é zero).
	OutAccelerationMs2.Z += K * Speed * Speed;
}

float AROVPawn::GetSwellElevationM(const FVector& LocationCm) const
{
	if (!IsSwellOn() || !GetWorld())
	{
		return 0.f;
	}
	const float Omega = 2.f * PI / SwellPeriodS;
	const float K = 2.f * PI / GetSwellWavelengthM();
	const float FromRad = FMath::DegreesToRadians(SwellFromDeg);
	const FVector2D Direction(-FMath::Cos(FromRad), -FMath::Sin(FromRad));
	const FVector2D PositionM(LocationCm.X / ROVUnits::CmPerM, LocationCm.Y / ROVUnits::CmPerM);
	const float Theta = K * FVector2D::DotProduct(PositionM, Direction) - Omega * GetWorld()->GetTimeSeconds();
	return 0.5f * SwellHeightM * GetSwellRamp() * FMath::Cos(Theta);
}

float AROVPawn::GetSwellRamp() const
{
	if (!IsSwellOn() || !GetWorld())
	{
		return 0.f;
	}
	// Ligar a onda de uma vez faz a água sair do repouso para a velocidade máxima num instante:
	// o ROV, que estava parado, fica com uma diferença de velocidade que só o arrasto desfaz, devagar
	// (constante de tempo ~20 s) - ele "deriva" por dezenas de segundos. Simuladores de ondas sobem a
	// amplitude aos poucos (ramp-up); aqui em 2 períodos, com curva suave (smoothstep: começa e termina sem tranco).
	const float Progress = (GetWorld()->GetTimeSeconds() - SwellStartTime) / (2.f * SwellPeriodS);
	return FMath::SmoothStep(0.f, 1.f, Progress);
}

FVector AROVPawn::GetWaterVelocityMs(const FVector& LocationCm) const
{
	FVector SwellVelocity, SwellAcceleration;
	GetSwellKinematics(LocationCm, SwellVelocity, SwellAcceleration);
	return GetCurrentVelocityMs() + SwellVelocity;
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
	// O arrasto depende da velocidade RELATIVA À ÁGUA. Com corrente, a água também se move:
	// um ROV parado no fundo com 1 nó de corrente "sente" 1 nó de água vindo contra ele.
	// Com swell, a água ainda gira em órbitas: soma-se a velocidade orbital no ponto onde o ROV está.
	FVector SwellVelocityMs, SwellAccelerationWorld;
	GetSwellKinematics(Hull->GetComponentLocation(), SwellVelocityMs, SwellAccelerationWorld);
	const FVector RelativeVelocityWorld = Hull->GetPhysicsLinearVelocity() / CmPerM - (GetCurrentVelocityMs() + SwellVelocityMs); // m/s
	const FVector VelocityLocal = Orientation.UnrotateVector(RelativeVelocityWorld);
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

	// Efeito cata-vento: a força lateral da água não passa pelo centro de massa, e sim pelo
	// centro de pressão (CP), deslocado no eixo X. Torque = r × F, com r = (x_CP, 0, 0) e F = (0, Fy, 0)
	// -> Tz = x_CP * Fy. Com o CP atrás do centro de massa (x negativo), a proa gira para dentro da corrente.
	DampingTorqueLocal.Z += (LateralCenterOfPressureX / CmPerM) * DragLocal.Y;

	// Inércia da água acelerando (termo de inércia da equação de Morison). A corrente é constante,
	// mas a água do swell ACELERA: para isso, a pressão em volta do ROV muda de um lado para o outro,
	// e essa diferença empurra o casco (força de Froude-Krylov = ρ·V·a_água). A água que o ROV
	// arrasta junto (massa adicional) também é acelerada: + m_a·a_água. Sem este termo, só o
	// arrasto puxaria o ROV, e ele ficaria quase parado enquanto a água oscila em volta - errado:
	// um corpo com flutuação quase neutra se move junto com a água da onda.
	const FVector SwellAccelerationLocal = Orientation.UnrotateVector(SwellAccelerationWorld);
	FVector WaveInertiaLocal = FVector::ZeroVector;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const double FroudeKrylov = WaterDensity * DisplacedVolume;   // kg (massa da água deslocada)
		const double AddedMass = AddedMassRatio[Axis] * Mass;         // kg
		WaveInertiaLocal[Axis] = (FroudeKrylov + AddedMass) * SwellAccelerationLocal[Axis] * Submerged; // N
	}

	// --- 4) Propulsores, com atraso de resposta (filtro de 1ª ordem) ---
	const float Alpha = 1.f - FMath::Exp(-DeltaTime / FMath::Max(ThrusterTimeConstant, KINDA_SMALL_NUMBER));
	FilteredThrust += (TranslationCommand - FilteredThrust) * Alpha;
	FilteredYaw += (YawCommand - FilteredYaw) * Alpha;

	// Hélice fora d'água não empurra (ventilação): empuxo proporcional à parte submersa
	const FVector ThrustLocal = FilteredThrust * MaxThrust * Submerged;                  // N (por eixo)
	const FVector YawTorqueLocal(0.f, 0.f, FilteredYaw * MaxYawTorque * Submerged);    // N·m

	// --- 5) Soma no referencial local e divide pela massa/inércia adicional ---
	// Acelerar um corpo na água também acelera a água em volta dele: na prática a massa
	// "efetiva" é maior. Dividir a força por (1 + razão) dá a mesma aceleração que m + m_a.
	FVector ForceLocal = Orientation.UnrotateVector(ForceWorld) + DragLocal + WaveInertiaLocal + ThrustLocal;
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
	const FVector TargetVelocity = Heading.RotateVector(TranslationCommand * DemoMaxSpeed);
	DemoVelocityMs += (TargetVelocity - DemoVelocityMs) * Blend;
	DemoYawRateDeg += (YawCommand * DemoYawRate - DemoYawRateDeg) * Blend;

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
	Telemetry.ThrustPercent = (bSim ? FilteredThrust : TranslationCommand) * 100.f;
	Telemetry.YawThrustPercent = (bSim ? FilteredYaw : YawCommand) * 100.f;

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

	// Mapeia um eixo analógico do gamepad: zona morta + curva de resposta
	auto MapStick = [this](UInputAction* Action, const FKey& Key, bool bNegate)
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(Action, Key);

		// Zona morta: ignora os primeiros 15% (o stick nunca volta exatamente ao zero)
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(InputContext);
		DeadZone->Type = EDeadZoneType::Axial;
		DeadZone->LowerThreshold = 0.15f;
		Mapping.Modifiers.Add(DeadZone);

		// Curva quadrática: precisão perto do centro, potência total no fim do curso
		UInputModifierResponseCurveExponential* Curve = NewObject<UInputModifierResponseCurveExponential>(InputContext);
		Curve->CurveExponent = FVector(2.f, 2.f, 1.f);
		Mapping.Modifiers.Add(Curve);

		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));
		}
	};

	SurgeAction = MakeAction(TEXT("IA_Surge_Runtime"), EInputActionValueType::Axis1D);
	Map(SurgeAction, EKeys::W, false);
	Map(SurgeAction, EKeys::S, true);
	MapStick(SurgeAction, EKeys::Gamepad_RightY, false);

	SwayAction = MakeAction(TEXT("IA_Sway_Runtime"), EInputActionValueType::Axis1D);
	Map(SwayAction, EKeys::D, false);
	Map(SwayAction, EKeys::A, true);
	MapStick(SwayAction, EKeys::Gamepad_RightX, false);

	HeaveAction = MakeAction(TEXT("IA_Heave_Runtime"), EInputActionValueType::Axis1D);
	Map(HeaveAction, EKeys::E, false);
	Map(HeaveAction, EKeys::Q, true);
	MapStick(HeaveAction, EKeys::Gamepad_LeftY, false);

	YawAction = MakeAction(TEXT("IA_Yaw_Runtime"), EInputActionValueType::Axis1D);
	Map(YawAction, EKeys::Right, false);
	Map(YawAction, EKeys::Left, true);
	MapStick(YawAction, EKeys::Gamepad_LeftX, false);

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
	// Gamepad: RB aproxima, LB afasta. "Pressed" dispara uma vez por aperto (um passo de zoom).
	for (const FKey& Key : { EKeys::Gamepad_RightShoulder, EKeys::Gamepad_LeftShoulder })
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(ZoomAction, Key);
		Mapping.Triggers.Add(NewObject<UInputTriggerPressed>(InputContext));
		if (Key == EKeys::Gamepad_LeftShoulder)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));
		}
	}

	ToggleCameraAction = MakeAction(TEXT("IA_ToggleCamera_Runtime"), EInputActionValueType::Boolean);
	Map(ToggleCameraAction, EKeys::C, false);
	Map(ToggleCameraAction, EKeys::Gamepad_FaceButton_Top, false);

	ToggleModeAction = MakeAction(TEXT("IA_ToggleMode_Runtime"), EInputActionValueType::Boolean);
	Map(ToggleModeAction, EKeys::M, false);
	Map(ToggleModeAction, EKeys::Gamepad_Special_Right, false);

	ToggleLightsAction = MakeAction(TEXT("IA_ToggleLights_Runtime"), EInputActionValueType::Boolean);
	Map(ToggleLightsAction, EKeys::L, false);
	Map(ToggleLightsAction, EKeys::Gamepad_FaceButton_Left, false);

	// Auto heading: H no teclado, clique no stick esquerdo (L3) no gamepad
	AutoHeadingAction = MakeAction(TEXT("IA_AutoHeading_Runtime"), EInputActionValueType::Boolean);
	Map(AutoHeadingAction, EKeys::H, false);
	Map(AutoHeadingAction, EKeys::Gamepad_LeftThumbstick, false);

	// Auto depth: J no teclado, clique no stick direito (R3). Auto altitude: K no teclado.
	AutoDepthAction = MakeAction(TEXT("IA_AutoDepth_Runtime"), EInputActionValueType::Boolean);
	Map(AutoDepthAction, EKeys::J, false);
	Map(AutoDepthAction, EKeys::Gamepad_RightThumbstick, false);
	AutoAltitudeAction = MakeAction(TEXT("IA_AutoAltitude_Runtime"), EInputActionValueType::Boolean);
	Map(AutoAltitudeAction, EKeys::K, false);
	// Station keeping: P no teclado
	AutoPositionAction = MakeAction(TEXT("IA_AutoPosition_Runtime"), EInputActionValueType::Boolean);
	Map(AutoPositionAction, EKeys::P, false);

	// D-pad: gira a câmera. Direita/esquerda já caem no eixo X;
	// cima/baixo passam pelo "Swizzle", que troca X por Y.
	LookPadAction = MakeAction(TEXT("IA_LookPad_Runtime"), EInputActionValueType::Axis2D);
	Map(LookPadAction, EKeys::Gamepad_DPad_Right, false);
	Map(LookPadAction, EKeys::Gamepad_DPad_Left, true);
	for (const FKey& Key : { EKeys::Gamepad_DPad_Up, EKeys::Gamepad_DPad_Down })
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(LookPadAction, Key);
		Mapping.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(InputContext)); // padrão YXZ: X vira Y
		if (Key == EKeys::Gamepad_DPad_Down)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));
		}
	}

	// Reset: segurar B / Círculo (ou R no teclado) por 1 segundo
	ResetAction = MakeAction(TEXT("IA_Reset_Runtime"), EInputActionValueType::Boolean);
	for (const FKey& Key : { EKeys::Gamepad_FaceButton_Right, EKeys::R })
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(ResetAction, Key);
		UInputTriggerHold* Hold = NewObject<UInputTriggerHold>(InputContext);
		Hold->HoldTimeThreshold = 1.f;
		Hold->bIsOneShot = true; // dispara uma vez, e não a cada frame enquanto segura
		Mapping.Triggers.Add(Hold);
	}
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
	Input->BindAction(LookPadAction, ETriggerEvent::Triggered, this, &AROVPawn::OnLookPad);
	// Com o gatilho "Hold", Triggered só acontece depois de segurar o botão pelo tempo definido
	Input->BindAction(ResetAction, ETriggerEvent::Triggered, this, &AROVPawn::OnResetHeld);
	Input->BindAction(AutoHeadingAction, ETriggerEvent::Started, this, &AROVPawn::OnToggleAutoHeading);
	Input->BindAction(AutoDepthAction, ETriggerEvent::Started, this, &AROVPawn::OnToggleAutoDepth);
	Input->BindAction(AutoAltitudeAction, ETriggerEvent::Started, this, &AROVPawn::OnToggleAutoAltitude);
	Input->BindAction(AutoPositionAction, ETriggerEvent::Started, this, &AROVPawn::OnToggleAutoPosition);
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

bool AROVPawn::IsLocalPilotingBlocked() const
{
	// Com o controle remoto ativo (comandos UDP chegando), quem pilota é o console remoto. Teclado e gamepad
	// ficam bloqueados para o ROV não receber duas ordens ao mesmo tempo (o último a escrever ganharia,
	// e o comando "pularia" entre os dois). A câmera continua livre: olhar não muda o veículo.
	// Quando os comandos param (watchdog de 0,5 s), o controle volta sozinho para o local.
	return UdpLink && UdpLink->IsRemoteActive();
}

void AROVPawn::OnSurge(const FInputActionValue& Value) { if (IsLocalPilotingBlocked()) return; PilotTranslation.X = FMath::Clamp(Value.Get<float>(), -1.f, 1.f); }
void AROVPawn::OnSway(const FInputActionValue& Value) { if (IsLocalPilotingBlocked()) return; PilotTranslation.Y = FMath::Clamp(Value.Get<float>(), -1.f, 1.f); }
void AROVPawn::OnHeave(const FInputActionValue& Value) { if (IsLocalPilotingBlocked()) return; PilotTranslation.Z = FMath::Clamp(Value.Get<float>(), -1.f, 1.f); }
void AROVPawn::OnYaw(const FInputActionValue& Value) { if (IsLocalPilotingBlocked()) return; PilotYaw = FMath::Clamp(Value.Get<float>(), -1.f, 1.f); }

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
	if (IsLocalPilotingBlocked()) { return; }
	SetControlMode(ControlMode == EROVControlMode::Simulator ? EROVControlMode::Demo : EROVControlMode::Simulator);
}

void AROVPawn::OnToggleLights()
{
	if (IsLocalPilotingBlocked()) { return; }
	SetLightsOn(!bLightsOn);
}

void AROVPawn::OnLookPad(const FInputActionValue& Value)
{
	// D-pad é digital (0 ou 1). Multiplicar pelo tempo do frame transforma em "graus por segundo":
	// a câmera gira na mesma velocidade com 30 ou 120 FPS.
	const FVector2D Step = Value.Get<FVector2D>() * GamepadLookSpeed * GetWorld()->GetDeltaSeconds();

	if (CameraMode == EROVCameraMode::External)
	{
		if (Controller)
		{
			FRotator Rotation = Controller->GetControlRotation();
			Rotation.Yaw += Step.X;
			Rotation.Pitch = FMath::ClampAngle(Rotation.Pitch + Step.Y, -80.f, 45.f);
			Controller->SetControlRotation(Rotation);
		}
	}
	else
	{
		CockpitPanTilt.Yaw = FMath::Clamp(CockpitPanTilt.Yaw + Step.X, -CockpitPanLimit, CockpitPanLimit);
		CockpitPanTilt.Pitch = FMath::Clamp(CockpitPanTilt.Pitch + Step.Y, CockpitTiltMin, CockpitTiltMax);
	}
}

void AROVPawn::OnResetHeld()
{
	if (IsLocalPilotingBlocked()) { return; }
	ResetToStart();
}

// =============================================================================
// Pilotos automáticos
// =============================================================================

void AROVPawn::SetAutoHeading(bool bEnable)
{
	bAutoHeading = bEnable;
	TargetHeadingDeg = FRotator::ClampAxis(GetActorRotation().Yaw);
	bHeadingSettling = bEnable; // se estiver girando, primeiro freia e segura onde parar
	HeadingPid.Reset();
	UE_LOG(LogROV, Display, TEXT("Auto heading %s (alvo %.1f graus)"),
		bEnable ? TEXT("LIGADO") : TEXT("DESLIGADO"), TargetHeadingDeg);
}

void AROVPawn::SetTargetHeading(float HeadingDeg)
{
	bAutoHeading = true;
	TargetHeadingDeg = FRotator::ClampAxis(HeadingDeg);
	bHeadingSettling = false; // alvo explícito: vai direto para ele
	HeadingPid.Reset();
	UE_LOG(LogROV, Display, TEXT("Auto heading LIGADO (alvo %.1f graus)"), TargetHeadingDeg);
}

void AROVPawn::OnToggleAutoHeading()
{
	if (IsLocalPilotingBlocked()) { return; }
	SetAutoHeading(!bAutoHeading);
}

void AROVPawn::UpdateAutoHeading(float DeltaTime)
{
	// Sem auto: o yaw vem direto do piloto
	YawCommand = PilotYaw;
	if (!bAutoHeading)
	{
		return;
	}

	const float CurrentHeading = GetActorRotation().Yaw;

	// Piloto mexendo no yaw: ele manda. Quando soltar, o auto primeiro freia o giro.
	if (FMath::Abs(PilotYaw) > 0.05f)
	{
		bHeadingSettling = true;
		HeadingPid.Reset();
		return;
	}

	// "Segura onde parar" (mesma ideia do auto depth): só freia, e grava o rumo quando o giro quase parar
	if (bHeadingSettling)
	{
		TargetHeadingDeg = FRotator::ClampAxis(CurrentHeading);
		if (FMath::Abs(Telemetry.YawRateDegS) > 1.f) // graus/s
		{
			YawCommand = HeadingPid.Update(0.f, Telemetry.YawRateDegS, DeltaTime);
			return;
		}
		bHeadingSettling = false;
		HeadingPid.Reset();
	}

	// Erro pelo caminho mais curto: de 350° para 10° são +20°, e não -340°
	const float ErrorDeg = FMath::FindDeltaAngleDegrees(CurrentHeading, TargetHeadingDeg);
	YawCommand = HeadingPid.Update(ErrorDeg, Telemetry.YawRateDegS, DeltaTime);
}

void AROVPawn::UpdateAutopilots(float DeltaTime)
{
	UpdateAutoHeading(DeltaTime);
	UpdateAutoVertical(DeltaTime);   // começa copiando o comando do piloto; mexe só no heave
	UpdateAutoPosition(DeltaTime);   // depois dele: mexe só em surge e sway
}

void AROVPawn::UpdateAutoVertical(float DeltaTime)
{
	// Sem auto: o comando vem direto do piloto
	TranslationCommand = PilotTranslation;

	// Filtro de ondas (o mesmo notch do station keeping): o swell sobe e desce o ROV a cada período.
	// O auto vertical deve segurar a profundidade MÉDIA e deixar o ROV acompanhar a órbita da água;
	// sem o filtro, o heave inverte a cada meia onda (gasta propulsor e nem consegue cancelar a onda).
	// Roda sempre, mesmo com o auto desligado, para o filtro já estar "aquecido" quando ligar.
	const double WaveOmega = IsSwellOn() ? 2.0 * PI / SwellPeriodS : 0.0;
	const float RawVerticalSpeedMs = Hull->IsSimulatingPhysics()
		? Hull->GetPhysicsLinearVelocity().Z / 100.f
		: DemoVelocityMs.Z;
	const float FilteredDepthM = WaveFilterDepth.Process(Telemetry.DepthM, WaveOmega, DeltaTime);
	const float VerticalSpeedMs = WaveFilterVerticalSpeed.Process(RawVerticalSpeedMs, WaveOmega, DeltaTime); // + = subindo
	float FilteredAltitudeM = Telemetry.AltitudeM;
	if (Telemetry.AltitudeM >= 0.f)
	{
		FilteredAltitudeM = WaveFilterAltitude.Process(Telemetry.AltitudeM, WaveOmega, DeltaTime);
	}
	else
	{
		WaveFilterAltitude.Reset(); // sem fundo no alcance: recomeça quando o altímetro voltar
	}

	// Ganhos por situação ("gain scheduling"). O notch tira ganho e atrasa o sinal perto da frequência
	// da onda; se o controle tentar responder nessa faixa, o laço fica instável (testado: ciclo de
	// ±1,8 m a cada ~14 s). Regra de DP: com filtro de ondas, a banda do controle fica bem ABAIXO da
	// onda. Sem swell: ωn ≈ √(0,8·0,6) ≈ 0,69 rad/s (rápido). Com swell: ωn ≈ √(0,2·0,6) ≈ 0,35 rad/s,
	// metade da onda de 10 s (0,63 rad/s); 0,6 m/s² é a aceleração vertical por unidade de comando.
	const bool bWaveGains = WaveOmega > 0.0;
	DepthPid.Kp = bWaveGains ? 0.2f : 0.8f;
	DepthPid.Ki = bWaveGains ? 0.02f : 0.08f;
	DepthPid.Kd = bWaveGains ? 1.0f : 1.5f;

	if (VerticalAuto == EROVVerticalAuto::Off)
	{
		return;
	}

	const bool bDepth = (VerticalAuto == EROVVerticalAuto::Depth);

	// Altímetro sem fundo no alcance: não há o que seguir, o piloto continua no controle
	if (!bDepth && Telemetry.AltitudeM < 0.f)
	{
		return;
	}

	// Piloto mexendo no heave: ele manda. Quando soltar, o auto primeiro freia (ver abaixo).
	if (FMath::Abs(PilotTranslation.Z) > 0.05f)
	{
		bVerticalSettling = true;
		DepthPid.Reset();
		return;
	}

	// Daqui para baixo, tudo usa a medida FILTRADA (profundidade, altitude e velocidade sem o vaivém da onda)

	// "Segura onde parar": logo depois de o piloto soltar (ou de ligar o auto com o ROV em movimento),
	// o auto só FREIA, com erro zero (apenas o termo D age), e o alvo acompanha a posição.
	// Quando a velocidade quase zera, o alvo fica gravado ali. Sem isso, o alvo seria o ponto
	// em que o piloto soltou, e a inércia levaria o ROV além dele para depois voltar.
	if (bVerticalSettling)
	{
		TargetDepthM = FilteredDepthM;
		TargetAltitudeM = FilteredAltitudeM;
		if (FMath::Abs(VerticalSpeedMs) > 0.05f) // m/s
		{
			TranslationCommand.Z = DepthPid.Update(0.f, VerticalSpeedMs, DeltaTime);
			return;
		}
		bVerticalSettling = false;
		DepthPid.Reset();
	}

	// Erro "para cima": quanto o ROV precisa subir (positivo) ou descer (negativo).
	// A profundidade cresce para baixo, por isso o sinal é invertido em relação à altitude.
	const float ErrorUpM = bDepth
		? FilteredDepthM - TargetDepthM
		: TargetAltitudeM - FilteredAltitudeM;

	TranslationCommand.Z = DepthPid.Update(ErrorUpM, VerticalSpeedMs, DeltaTime);
}

void AROVPawn::SetAutoDepth(bool bEnable)
{
	VerticalAuto = bEnable ? EROVVerticalAuto::Depth : EROVVerticalAuto::Off;
	TargetDepthM = Telemetry.DepthM;
	bVerticalSettling = bEnable; // se estiver subindo/descendo, primeiro freia e segura onde parar
	DepthPid.Reset();
	UE_LOG(LogROV, Display, TEXT("Auto depth %s (alvo %.1f m)"), bEnable ? TEXT("LIGADO") : TEXT("DESLIGADO"), TargetDepthM);
}

void AROVPawn::SetTargetDepth(float DepthM)
{
	VerticalAuto = EROVVerticalAuto::Depth;
	TargetDepthM = FMath::Max(0.f, DepthM);
	bVerticalSettling = false; // alvo explícito: vai direto para ele
	DepthPid.Reset();
	UE_LOG(LogROV, Display, TEXT("Auto depth LIGADO (alvo %.1f m)"), TargetDepthM);
}

void AROVPawn::SetAutoAltitude(bool bEnable)
{
	if (bEnable && Telemetry.AltitudeM < 0.f)
	{
		UE_LOG(LogROV, Warning, TEXT("Auto altitude: fundo fora do alcance do altímetro"));
		return;
	}
	VerticalAuto = bEnable ? EROVVerticalAuto::Altitude : EROVVerticalAuto::Off;
	TargetAltitudeM = Telemetry.AltitudeM;
	bVerticalSettling = bEnable; // se estiver subindo/descendo, primeiro freia e segura onde parar
	DepthPid.Reset();
	UE_LOG(LogROV, Display, TEXT("Auto altitude %s (alvo %.1f m)"), bEnable ? TEXT("LIGADO") : TEXT("DESLIGADO"), TargetAltitudeM);
}

void AROVPawn::SetTargetAltitude(float AltitudeM)
{
	VerticalAuto = EROVVerticalAuto::Altitude;
	TargetAltitudeM = FMath::Max(0.5f, AltitudeM); // nunca mirar encostado no fundo
	bVerticalSettling = false; // alvo explícito: vai direto para ele
	DepthPid.Reset();
	UE_LOG(LogROV, Display, TEXT("Auto altitude LIGADO (alvo %.1f m)"), TargetAltitudeM);
}

EROVAutoPhase AROVPawn::GetHeadingPhase() const
{
	if (!bAutoHeading)
	{
		return EROVAutoPhase::Off;
	}
	if (FMath::Abs(PilotYaw) > 0.05f)
	{
		return EROVAutoPhase::Adjusting;
	}
	return bHeadingSettling ? EROVAutoPhase::Braking : EROVAutoPhase::Holding;
}

EROVAutoPhase AROVPawn::GetVerticalPhase() const
{
	if (VerticalAuto == EROVVerticalAuto::Off)
	{
		return EROVAutoPhase::Off;
	}
	if (VerticalAuto == EROVVerticalAuto::Altitude && Telemetry.AltitudeM < 0.f)
	{
		return EROVAutoPhase::NoBottom;
	}
	if (FMath::Abs(PilotTranslation.Z) > 0.05f)
	{
		return EROVAutoPhase::Adjusting;
	}
	return bVerticalSettling ? EROVAutoPhase::Braking : EROVAutoPhase::Holding;
}

void AROVPawn::OnToggleAutoDepth()
{
	if (IsLocalPilotingBlocked()) { return; }
	SetAutoDepth(VerticalAuto != EROVVerticalAuto::Depth);
}

void AROVPawn::OnToggleAutoAltitude()
{
	if (IsLocalPilotingBlocked()) { return; }
	SetAutoAltitude(VerticalAuto != EROVVerticalAuto::Altitude);
}

void AROVPawn::OnToggleAutoPosition()
{
	if (IsLocalPilotingBlocked()) { return; }
	SetPositionHold(!bPositionHold);
}

// =============================================================================
// Station keeping (manter posição)
//
// Mesma receita dos outros autos, agora no plano horizontal:
//   1) ONDE ESTOU: aqui é a posição real do simulador. Num ROV de verdade viria do USBL (posição
//      acústica medida pelo navio) somado ao DVL (velocidade sobre o fundo).
//   2) FILTRO DE ONDAS: tira da medida o vaivém do swell (notch na frequência da onda).
//   3) PID por eixo do mundo (Norte, Leste): o I acumula até igualar a força da corrente.
//   4) Gira a força pedida do mundo para o casco e divide pelo empuxo de cada eixo.
// =============================================================================

void AROVPawn::SetPositionHold(bool bEnable)
{
	bPositionHold = bEnable;
	TargetPositionM = FilteredPositionM;
	bPositionSettling = bEnable; // se estiver andando, primeiro freia e segura onde parar
	PositionSettlingTime = 0.f;
	NorthPid.Reset();
	EastPid.Reset();
	UE_LOG(LogROV, Display, TEXT("Station keeping %s"), bEnable ? TEXT("LIGADO") : TEXT("DESLIGADO"));
}

void AROVPawn::SetTargetPosition(float NorthM, float EastM)
{
	bPositionHold = true;
	TargetPositionM = FVector2D(NorthM, EastM);
	bPositionSettling = false; // alvo explícito: vai direto para ele
	NorthPid.Reset();
	EastPid.Reset();
	UE_LOG(LogROV, Display, TEXT("Station keeping LIGADO (alvo N %.1f m, L %.1f m)"), NorthM, EastM);
}

FVector2D AROVPawn::GetPositionOffsetM() const
{
	const FVector Location = GetActorLocation() / ROVUnits::CmPerM;
	return FVector2D(Location.X, Location.Y) - TargetPositionM;
}

EROVAutoPhase AROVPawn::GetPositionPhase() const
{
	if (!bPositionHold)
	{
		return EROVAutoPhase::Off;
	}
	if (FVector2D(PilotTranslation.X, PilotTranslation.Y).Size() > 0.05f)
	{
		return EROVAutoPhase::Adjusting;
	}
	return bPositionSettling ? EROVAutoPhase::Braking : EROVAutoPhase::Holding;
}

void AROVPawn::UpdateAutoPosition(float DeltaTime)
{
	using namespace ROVUnits;

	// 1) e 2) Medida filtrada. Roda SEMPRE (mesmo com o auto desligado), para o filtro já estar
	// "aquecido" quando o piloto ligar. A frequência do notch vem do swell configurado; num DP real
	// ela é estimada do próprio movimento medido (o pico do espectro das ondas).
	const FVector Location = GetActorLocation() / CmPerM;
	const FVector Velocity = Hull->IsSimulatingPhysics() ? Hull->GetPhysicsLinearVelocity() / CmPerM : DemoVelocityMs;
	const double WaveOmega = IsSwellOn() ? 2.0 * PI / SwellPeriodS : 0.0;
	FilteredPositionM.X = WaveFilterPosN.Process(Location.X, WaveOmega, DeltaTime);
	FilteredPositionM.Y = WaveFilterPosE.Process(Location.Y, WaveOmega, DeltaTime);
	FilteredVelocityMs.X = WaveFilterVelN.Process(Velocity.X, WaveOmega, DeltaTime);
	FilteredVelocityMs.Y = WaveFilterVelE.Process(Velocity.Y, WaveOmega, DeltaTime);

	if (!bPositionHold)
	{
		return;
	}

	// Piloto mexendo em surge ou sway: ele manda. Quando soltar, o auto primeiro freia.
	if (FVector2D(PilotTranslation.X, PilotTranslation.Y).Size() > 0.05f)
	{
		bPositionSettling = true;
		PositionSettlingTime = 0.f;
		NorthPid.Reset();
		EastPid.Reset();
		return;
	}

	// 3) PID no mundo. Durante a frenagem o erro é zero (só o D age) e o alvo acompanha a posição.
	FVector2D ErrorM = TargetPositionM - FilteredPositionM;
	if (bPositionSettling)
	{
		TargetPositionM = FilteredPositionM;
		ErrorM = FVector2D::ZeroVector;
		PositionSettlingTime += DeltaTime;
		// Parou (sem contar o vaivém da onda) ou já freou 3 s. Com corrente, só o D empata com ela
		// numa velocidade que nunca chega a zero; a partir daí o P e o I trazem o ROV de volta.
		if (FilteredVelocityMs.Size() < 0.05f || PositionSettlingTime > 3.f)
		{
			bPositionSettling = false;
			NorthPid.Reset();
			EastPid.Reset();
		}
	}
	const float NorthCommand = NorthPid.Update(ErrorM.X, FilteredVelocityMs.X, DeltaTime);
	const float EastCommand = EastPid.Update(ErrorM.Y, FilteredVelocityMs.Y, DeltaTime);

	// 4) Força pedida no mundo (N) -> referencial do casco. Só o rumo importa (pitch e roll ficam perto de zero).
	const FVector ForceWorldN = FVector(NorthCommand, EastCommand, 0.f) * MaxThrust.Y;
	const FVector ForceBodyN = FRotator(0.f, GetActorRotation().Yaw, 0.f).UnrotateVector(ForceWorldN);
	TranslationCommand.X = FMath::Clamp(ForceBodyN.X / MaxThrust.X, -1.f, 1.f);
	TranslationCommand.Y = FMath::Clamp(ForceBodyN.Y / MaxThrust.Y, -1.f, 1.f);
}
