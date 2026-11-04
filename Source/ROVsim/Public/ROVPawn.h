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

	// Parâmetros de empuxo (editáveis no painel Details)
	UPROPERTY(EditAnywhere, Category = "ROV|Buoyancy")
	float WaterSurfaceZ = 1000.f;      // cm - altura da superfície da água no mundo

	UPROPERTY(EditAnywhere, Category = "ROV|Buoyancy")
	float WaterDensity = 1025.f;       // kg/m³ - água do mar

	UPROPERTY(EditAnywhere, Category = "ROV|Buoyancy")
	float DisplacedVolume = 0.1f;      // m³ - volume do casco do ROV

	UPROPERTY(EditAnywhere, Category = "ROV|Buoyancy")
	float HullHeight = 100.f;          // cm - altura do casco

	void ApplyBuoyancy();

	// Parâmetros de arrasto
	UPROPERTY(EditAnywhere, Category = "ROV|Drag")
	float LinearDamping = 50.f;        // N·s/m - arrasto em baixa velocidade

	UPROPERTY(EditAnywhere, Category = "ROV|Drag")
	float DragCoefficient = 1.05f;     // Cd - adimensional (cubo ≈ 1.05)

	UPROPERTY(EditAnywhere, Category = "ROV|Drag")
	float ReferenceArea = 1.0f;        // m² - área frontal (face do cubo de 1 m)

	UPROPERTY(EditAnywhere, Category = "ROV|Drag")
	float AngularDamping = 200.f;      // N·m·s/rad - resistência à rotação

	float GetSubmergedFraction() const;
	void ApplyDrag();

public:
	virtual void Tick(float DeltaTime) override;

	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
};