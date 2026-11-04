#include "ROVPawn.h"

// NOVO: agora sim incluímos os cabeçalhos completos
#include "Components/StaticMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "UObject/ConstructorHelpers.h"

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
}

// Called to bind functionality to input
void AROVPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

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