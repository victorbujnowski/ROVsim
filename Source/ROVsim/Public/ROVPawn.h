#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "ROVPawn.generated.h"

// "Forward declarations": dizem ao compilador que essas classes existem,
// sem precisar incluir os cabeçalhos inteiros aqui (compila mais rápido).
class UBoxComponent;
class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class USpotLightComponent;
class UInputMappingContext;
class UInputAction;
struct FInputActionValue;

// Categoria de log própria: filtrar por "LogROV" no Output Log
ROVSIM_API DECLARE_LOG_CATEGORY_EXTERN(LogROV, Log, All);

/** Como o ROV é controlado */
UENUM(BlueprintType)
enum class EROVControlMode : uint8
{
	// Física completa: massa, empuxo, arrasto, massa adicional, atraso dos propulsores (ficha MBARI)
	Simulator UMETA(DisplayName = "Simulador"),
	// Controle arcade: sem física, velocidade direta, ROV sempre nivelado
	Demo UMETA(DisplayName = "Demonstracao")
};

/** Qual câmera está ativa */
UENUM(BlueprintType)
enum class EROVCameraMode : uint8
{
	External UMETA(DisplayName = "Externa (Spring Arm)"),
	Cockpit UMETA(DisplayName = "Cockpit (1a pessoa)")
};

/** Telemetria calculada a cada frame (lida pelo HUD e pelo smoke test) */
USTRUCT(BlueprintType)
struct FROVTelemetry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ROV") float DepthM = 0.f;          // m abaixo da superfície
	UPROPERTY(BlueprintReadOnly, Category = "ROV") float AltitudeM = -1.f;      // m até o fundo (-1 = fora do alcance)
	UPROPERTY(BlueprintReadOnly, Category = "ROV") float HeadingDeg = 0.f;      // 0..360 (+X = Norte, +Y = Leste)
	UPROPERTY(BlueprintReadOnly, Category = "ROV") float PitchDeg = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ROV") float RollDeg = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ROV") FVector LocalVelocityMs = FVector::ZeroVector; // X surge, Y sway, Z heave
	UPROPERTY(BlueprintReadOnly, Category = "ROV") float YawRateDegS = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ROV") float SubmergedFraction = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ROV") FVector ThrustPercent = FVector::ZeroVector;   // -100..100 por eixo
	UPROPERTY(BlueprintReadOnly, Category = "ROV") float YawThrustPercent = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ROV") float MassKg = 0.f;
};

/**
 * ROV "Doc Ricketts" (MBARI) - veículo operado remotamente.
 *
 * Corpo físico: caixa (UBoxComponent) no tamanho real. O modelo 3D é só visual.
 * Dois modos de controle (Simulador / Demonstração) e duas câmeras (externa / cockpit).
 */
UCLASS()
class ROVSIM_API AROVPawn : public APawn
{
	GENERATED_BODY()

public:
	AROVPawn();

	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	// --- API pública (usada pelo input, pelo HUD e pelo smoke test) ---
	UFUNCTION(BlueprintCallable, Category = "ROV")
	void SetControlMode(EROVControlMode NewMode);

	UFUNCTION(BlueprintCallable, Category = "ROV")
	void SetCameraMode(EROVCameraMode NewMode);

	UFUNCTION(BlueprintCallable, Category = "ROV")
	void SetLightsOn(bool bOn);

	/** Comando do piloto, de -1 a 1 por eixo (X surge, Y sway, Z heave) + yaw */
	UFUNCTION(BlueprintCallable, Category = "ROV")
	void SetPilotCommand(FVector Translation, float Yaw);

	EROVControlMode GetControlMode() const { return ControlMode; }
	EROVCameraMode GetCameraMode() const { return CameraMode; }
	bool AreLightsOn() const { return bLightsOn; }
	const FROVTelemetry& GetTelemetry() const { return Telemetry; }

protected:
	virtual void BeginPlay() override;
	virtual void NotifyControllerChanged() override;

	// =====================================================================
	// Componentes
	// =====================================================================

	/** Corpo físico: caixa no tamanho real do Doc Ricketts (3,66 x 1,83 x 2,13 m) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<UBoxComponent> Hull;

	/** Modelo 3D (só visual, sem colisão) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<UStaticMeshComponent> VisualMesh;

	/** Braço da câmera externa (orbita com o mouse, zoom na roda, colisão e lag) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<UCameraComponent> ExternalCamera;

	/** Câmera de piloto, na frente do ROV (como a câmera de vídeo do veículo real) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<UCameraComponent> CockpitCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<USpotLightComponent> LightLeft;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<USpotLightComponent> LightRight;

	// =====================================================================
	// Estado inicial
	// =====================================================================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ROV|Modo")
	EROVControlMode ControlMode = EROVControlMode::Simulator;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ROV|Modo")
	EROVCameraMode CameraMode = EROVCameraMode::External;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ROV|Modo")
	bool bLightsOn = true;

	// =====================================================================
	// Ambiente
	// =====================================================================

	UPROPERTY(EditAnywhere, Category = "ROV|Agua")
	float WaterSurfaceZ = 1000.f;      // cm - altura da superfície da água no mundo

	UPROPERTY(EditAnywhere, Category = "ROV|Agua")
	float WaterDensity = 1025.f;       // kg/m³ - água do mar

	// =====================================================================
	// Simulador (valores da ficha da MBARI; "estimado" = não publicado)
	// =====================================================================

	/** Volume deslocado. Massa 4763 kg -> 4,66 m³ deixa ~13 kg de flutuação positiva (padrão de segurança de ROVs) */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Empuxo")
	float DisplacedVolume = 4.66f;     // m³

	/** Distância do centro de carena acima do centro de gravidade (BG). Gera o momento que endireita o ROV. */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Empuxo")
	float BuoyancyCenterHeight = 20.f; // cm (estimado)

	/** Coeficiente de arrasto por eixo do ROV (X surge, Y sway, Z heave) */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Arrasto")
	FVector DragCoefficients = FVector(1.05f, 1.05f, 1.05f);

	/** Área projetada por eixo: frontal (L x A), lateral (C x A) e vertical (C x L) */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Arrasto")
	FVector ProjectedAreas = FVector(3.90f, 7.80f, 6.70f); // m²

	/** Arrasto linear por eixo (domina em baixa velocidade) */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Arrasto")
	FVector LinearDamping = FVector(500.f, 500.f, 500.f); // N·s/m

	/** Amortecimento angular (roll, pitch, yaw) */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Arrasto")
	FVector AngularDamping = FVector(8000.f, 8000.f, 6000.f); // N·m·s/rad

	/** Massa adicional (água arrastada junto) como fração da massa, por eixo */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Massa adicional")
	FVector AddedMassRatio = FVector(0.4f, 0.7f, 0.9f); // estimado

	/** Inércia adicional, como fração da inércia, por eixo de rotação */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Massa adicional")
	FVector AddedInertiaRatio = FVector(0.5f, 0.5f, 0.5f); // estimado

	/** Empuxo máximo dos propulsores por eixo: surge 650 kgf e heave 550 kgf (MBARI), sway estimado */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Propulsores")
	FVector MaxThrust = FVector(6375.f, 4000.f, 5394.f); // N

	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Propulsores")
	float MaxYawTorque = 3000.f;       // N·m (estimado)

	/** Constante de tempo dos propulsores hidráulicos (atraso entre comando e empuxo) */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Propulsores")
	float ThrusterTimeConstant = 0.25f; // s

	// =====================================================================
	// Demonstração (arcade)
	// =====================================================================

	/** Velocidade máxima por eixo (frente, lado, vertical) */
	UPROPERTY(EditAnywhere, Category = "ROV|Demonstracao")
	FVector DemoMaxSpeed = FVector(8.f, 5.f, 5.f); // m/s

	UPROPERTY(EditAnywhere, Category = "ROV|Demonstracao")
	float DemoYawRate = 90.f;          // °/s

	/** Quão rápido a velocidade alcança o comando (maior = mais "seco") */
	UPROPERTY(EditAnywhere, Category = "ROV|Demonstracao")
	float DemoResponsiveness = 4.f;    // 1/s

	// =====================================================================
	// Câmeras
	// =====================================================================

	UPROPERTY(EditAnywhere, Category = "ROV|Camera")
	float DesiredArmLength = 900.f;    // cm

	UPROPERTY(EditAnywhere, Category = "ROV|Camera")
	float MinArmLength = 500.f;

	UPROPERTY(EditAnywhere, Category = "ROV|Camera")
	float MaxArmLength = 3000.f;

	UPROPERTY(EditAnywhere, Category = "ROV|Camera")
	float ZoomStep = 150.f;            // cm por "clique" da roda

	/** Graus por unidade de movimento do mouse na câmera de cockpit (pan/tilt).
	 *  2.5 = mesma sensibilidade da câmera externa (escala de input legada do projeto). */
	UPROPERTY(EditAnywhere, Category = "ROV|Camera")
	float CockpitLookSensitivity = 2.5f;

	UPROPERTY(EditAnywhere, Category = "ROV|Camera")
	float CockpitPanLimit = 100.f;     // ° para cada lado

	UPROPERTY(EditAnywhere, Category = "ROV|Camera")
	float CockpitTiltMin = -70.f;

	UPROPERTY(EditAnywhere, Category = "ROV|Camera")
	float CockpitTiltMax = 30.f;

private:
	// --- Simulação ---
	void TickSimulator(float DeltaTime);
	void TickDemo(float DeltaTime);
	void UpdateCameras(float DeltaTime);
	void UpdateTelemetry();
	float GetSubmergedFraction() const;
	void ApplyCameraLimits();

	// --- Input (criado em C++, sem depender de assets) ---
	void BuildInputMappings();
	void OnSurge(const FInputActionValue& Value);
	void OnSway(const FInputActionValue& Value);
	void OnHeave(const FInputActionValue& Value);
	void OnYaw(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnZoom(const FInputActionValue& Value);
	void OnToggleCamera();
	void OnToggleMode();
	void OnToggleLights();

	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> InputContext;
	UPROPERTY(Transient) TObjectPtr<UInputAction> SurgeAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> SwayAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> HeaveAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> YawAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> LookAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ZoomAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ToggleCameraAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ToggleModeAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ToggleLightsAction;

	// --- Estado ---
	FVector PilotTranslation = FVector::ZeroVector; // comando do piloto, -1..1
	float PilotYaw = 0.f;
	FVector FilteredThrust = FVector::ZeroVector;   // comando depois do atraso dos propulsores
	float FilteredYaw = 0.f;
	FVector DemoVelocityMs = FVector::ZeroVector;   // velocidade no modo demonstração (mundo)
	float DemoYawRateDeg = 0.f;
	float LastCameraYaw = 0.f;
	FRotator CockpitPanTilt = FRotator::ZeroRotator;
	FROVTelemetry Telemetry;
};
