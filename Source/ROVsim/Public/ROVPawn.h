#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "ROVPawn.generated.h"

// NOVO: "forward declarations" - dizem ao compilador que essas classes existem,
// sem precisar incluir os cabeçalhos inteiros aqui (compila mais rápido).
class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;

UCLASS()
class ROVSIM_API AROVPawn : public APawn
{
	GENERATED_BODY()

public:
	AROVPawn();

protected:
	virtual void BeginPlay() override;

	// NOVO: componentes do ROV
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV")
	TObjectPtr<UStaticMeshComponent> Body;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV")
	TObjectPtr<UCameraComponent> Camera;

public:
	virtual void Tick(float DeltaTime) override;

	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
};