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
}

// Called to bind functionality to input
void AROVPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

}

void AROVPawn::ApplyBuoyancy()
{
	// Profundidade do centro do ROV (positivo = abaixo da superfície), em cm
	const float Depth = WaterSurfaceZ - Body->GetComponentLocation().Z;

	// Fração do casco submersa: 0 = fora d'água, 1 = totalmente submerso
	const float SubmergedFraction = FMath::Clamp((Depth + HullHeight * 0.5f) / HullHeight, 0.f, 1.f);

	// Gravidade do mundo vem em cm/s² (negativa); convertemos para m/s²
	const float Gravity = FMath::Abs(GetWorld()->GetGravityZ()) / 100.f;

	// Arquimedes: F = rho * V * g  (em Newtons)
	const float BuoyancyNewtons = WaterDensity * DisplacedVolume * SubmergedFraction * Gravity;

	// Newtons -> unidades da Unreal (kg·cm/s²) e aplica para cima
	Body->AddForce(FVector(0.f, 0.f, BuoyancyNewtons * 100.f));
}
