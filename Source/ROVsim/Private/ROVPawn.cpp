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

}

// Called to bind functionality to input
void AROVPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

}

