#include "ROVPawn.h"

// NOVO: agora sim incluímos os cabeçalhos completos
#include "Components/StaticMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "GameFramework/PlayerController.h"

AROVPawn::AROVPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	// Corpo: cria o componente e o torna a raiz do Actor
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	RootComponent = Body;

	// Usa o cubo que vem com a engine como malha provisória
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		Body->SetStaticMesh(CubeMesh.Object);
	}

	// Física: o corpo passa a ser simulado pelo Chaos
	Body->SetSimulatePhysics(true);
	Body->SetMassOverrideInKg(NAME_None, 100.f, true);

	// Braço da câmera, preso ao corpo
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(Body);
	SpringArm->TargetArmLength = 400.f;
	SpringArm->SetRelativeRotation(FRotator(-20.f, 0.f, 0.f));
	SpringArm->bDoCollisionTest = false;

	// Câmera na ponta do braço
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm);

	// O jogador 0 controla este Pawn automaticamente
	AutoPossessPlayer = EAutoReceiveInput::Player0;
}
void AROVPawn::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void AROVPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	ApplyBuoyancy();
	ApplyDrag();
	ApplyThrusters();
}

// Called to bind functionality to input
void AROVPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		return;
	}

	// Triggered: enquanto a tecla está pressionada | Completed: quando solta (valor volta a 0)
	Input->BindAction(SurgeAction, ETriggerEvent::Triggered, this, &AROVPawn::OnSurge);
	Input->BindAction(SurgeAction, ETriggerEvent::Completed, this, &AROVPawn::OnSurge);
	Input->BindAction(SwayAction, ETriggerEvent::Triggered, this, &AROVPawn::OnSway);
	Input->BindAction(SwayAction, ETriggerEvent::Completed, this, &AROVPawn::OnSway);
	Input->BindAction(HeaveAction, ETriggerEvent::Triggered, this, &AROVPawn::OnHeave);
	Input->BindAction(HeaveAction, ETriggerEvent::Completed, this, &AROVPawn::OnHeave);
	Input->BindAction(YawAction, ETriggerEvent::Triggered, this, &AROVPawn::OnYaw);
	Input->BindAction(YawAction, ETriggerEvent::Completed, this, &AROVPawn::OnYaw);
}

float AROVPawn::GetSubmergedFraction() const
{
	// Profundidade do centro do ROV (positivo = abaixo da superfície), em cm
	const float Depth = WaterSurfaceZ - Body->GetComponentLocation().Z;

	// 0 = fora d'água, 1 = totalmente submerso
	return FMath::Clamp((Depth + HullHeight * 0.5f) / HullHeight, 0.f, 1.f);
}

void AROVPawn::ApplyBuoyancy()
{
	const float SubmergedFraction = GetSubmergedFraction();

	// Gravidade do mundo vem em cm/s² (negativa); convertemos para m/s²
	const float Gravity = FMath::Abs(GetWorld()->GetGravityZ()) / 100.f;

	// Arquimedes: F = rho * V * g  (em Newtons)
	const float BuoyancyNewtons = WaterDensity * DisplacedVolume * SubmergedFraction * Gravity;

	// Newtons -> unidades da Unreal (kg·cm/s²) e aplica para cima
	Body->AddForce(FVector(0.f, 0.f, BuoyancyNewtons * 100.f));
}

void AROVPawn::ApplyDrag()
{
	const float Submerged = GetSubmergedFraction();
	if (Submerged <= 0.f)
	{
		return; // fora d'água: sem arrasto hidrodinâmico
	}

	// --- Arrasto linear (translação) ---
	// Velocidade da Unreal vem em cm/s; convertemos para m/s
	const FVector Velocity = Body->GetPhysicsLinearVelocity() / 100.f;
	const float Speed = Velocity.Size();

	const float QuadraticCoef = 0.5f * WaterDensity * DragCoefficient * ReferenceArea; // kg/m
	const FVector DragNewtons = -(LinearDamping * Velocity + QuadraticCoef * Speed * Velocity) * Submerged;

	Body->AddForce(DragNewtons * 100.f); // N -> kg·cm/s²

	// --- Arrasto angular (rotação) ---
	const FVector AngularVelocity = Body->GetPhysicsAngularVelocityInRadians(); // rad/s
	const FVector DragTorque = -AngularDamping * AngularVelocity * Submerged;   // N·m

	Body->AddTorqueInRadians(DragTorque * 10000.f); // N·m -> kg·cm²/s²
}

void AROVPawn::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	// Ativa o mapeamento de teclas do ROV para o jogador que o controla
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(InputMapping, 0);
		}
	}
}

void AROVPawn::OnSurge(const FInputActionValue& Value) { ThrustCommand.X = Value.Get<float>(); }
void AROVPawn::OnSway(const FInputActionValue& Value) { ThrustCommand.Y = Value.Get<float>(); }
void AROVPawn::OnHeave(const FInputActionValue& Value) { ThrustCommand.Z = Value.Get<float>(); }
void AROVPawn::OnYaw(const FInputActionValue& Value) { YawCommand = Value.Get<float>(); }

void AROVPawn::ApplyThrusters()
{
	if (GetSubmergedFraction() <= 0.f)
	{
		return; // propulsor fora d'água não empurra
	}

	// Força no referencial do PRÓPRIO ROV (local): X = frente, Y = direita, Z = cima
	const FVector LocalForceN(
		ThrustCommand.X * MaxSurgeThrust,
		ThrustCommand.Y * MaxSwayThrust,
		ThrustCommand.Z * MaxHeaveThrust);

	// Converte de local para mundo usando o quatérnio de orientação do ROV
	const FQuat Orientation = Body->GetComponentQuat();
	const FVector WorldForceN = Orientation.RotateVector(LocalForceN);
	Body->AddForce(WorldForceN * 100.f); // N -> kg·cm/s²

	// Torque de guinada (yaw) em torno do eixo Z local do ROV
	const FVector WorldTorqueNm = Orientation.RotateVector(FVector(0.f, 0.f, YawCommand * MaxYawTorque));
	Body->AddTorqueInRadians(WorldTorqueNm * 10000.f); // N·m -> kg·cm²/s²
}