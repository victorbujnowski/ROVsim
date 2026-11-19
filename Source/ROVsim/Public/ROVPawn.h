#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "ROVPid.h"
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
class UROVUdpLinkComponent;
class UROVTcpCommandComponent;
class UROVSensorEmulatorComponent;
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

/**
 * Fase de um piloto automático, do ponto de vista do piloto:
 *   AJUSTANDO  o piloto está mexendo no comando: ele manda e o alvo acompanha
 *   FREANDO    o piloto soltou: o auto só freia o movimento
 *   SEGURANDO  o movimento parou: o alvo ficou gravado e o PID o mantém
 */
UENUM(BlueprintType)
enum class EROVAutoPhase : uint8
{
	Off UMETA(DisplayName = "Desligado"),
	Adjusting UMETA(DisplayName = "Ajustando"),
	Braking UMETA(DisplayName = "Freando"),
	Holding UMETA(DisplayName = "Segurando"),
	NoBottom UMETA(DisplayName = "Sem fundo") // auto altitude sem fundo no alcance do altímetro
};

/** Piloto automático vertical: profundidade OU altitude (os dois usam o heave, então são exclusivos) */
UENUM(BlueprintType)
enum class EROVVerticalAuto : uint8
{
	Off UMETA(DisplayName = "Desligado"),
	Depth UMETA(DisplayName = "Profundidade"),
	Altitude UMETA(DisplayName = "Altitude")
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

	/** Define a corrente: velocidade em nós e direção para onde a água vai (0 = Norte, 90 = Leste) */
	UFUNCTION(BlueprintCallable, Category = "ROV")
	void SetCurrent(float SpeedKnots, float TowardDeg);

	/** Velocidade da corrente no mundo, em m/s */
	FVector GetCurrentVelocityMs() const;

	/** Liga/desliga o auto heading. Ao ligar, segura o rumo atual. */
	UFUNCTION(BlueprintCallable, Category = "ROV|Autos")
	void SetAutoHeading(bool bEnable);

	/** Liga o auto heading com um rumo alvo (graus, 0 = Norte, 90 = Leste) */
	UFUNCTION(BlueprintCallable, Category = "ROV|Autos")
	void SetTargetHeading(float HeadingDeg);

	bool IsAutoHeadingOn() const { return bAutoHeading; }
	float GetTargetHeading() const { return TargetHeadingDeg; }

	/** Auto depth: segura a profundidade atual (desliga o auto altitude) */
	UFUNCTION(BlueprintCallable, Category = "ROV|Autos")
	void SetAutoDepth(bool bEnable);

	/** Liga o auto depth com uma profundidade alvo, em metros */
	UFUNCTION(BlueprintCallable, Category = "ROV|Autos")
	void SetTargetDepth(float DepthM);

	/** Auto altitude: segura a distância atual até o fundo (desliga o auto depth) */
	UFUNCTION(BlueprintCallable, Category = "ROV|Autos")
	void SetAutoAltitude(bool bEnable);

	/** Liga o auto altitude com uma altitude alvo, em metros */
	UFUNCTION(BlueprintCallable, Category = "ROV|Autos")
	void SetTargetAltitude(float AltitudeM);

	EROVVerticalAuto GetVerticalAuto() const { return VerticalAuto; }
	float GetTargetDepth() const { return TargetDepthM; }
	float GetTargetAltitude() const { return TargetAltitudeM; }

	/** Em que fase cada piloto automático está agora (mostrado no HUD) */
	EROVAutoPhase GetHeadingPhase() const;
	EROVAutoPhase GetVerticalPhase() const;

	/** Volta o ROV para onde ele nasceu, parado e com os propulsores zerados */
	UFUNCTION(BlueprintCallable, Category = "ROV")
	void ResetToStart();

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

	/** Controle externo por UDP (comandos na porta 5000, telemetria na 5001) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<UROVUdpLinkComponent> UdpLink;

	/** Comandos discretos por TCP (porta 5002): modo, luzes, câmera, reset */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<UROVTcpCommandComponent> TcpCommands;

	/** Sensores emulados em NMEA: girobússola e atitude (UDP), DVL (TCP), profundímetro e altímetro (COM) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ROV|Componentes")
	TObjectPtr<UROVSensorEmulatorComponent> Sensors;

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

	/** Velocidade da corrente (1 nó = 0,514 m/s) */
	UPROPERTY(EditAnywhere, Category = "ROV|Agua", meta = (ClampMin = "0.0"))
	float CurrentSpeedKnots = 0.f;

	/** Direção PARA ONDE a corrente vai (convenção oceanográfica). 0 = Norte (+X), 90 = Leste (+Y) */
	UPROPERTY(EditAnywhere, Category = "ROV|Agua")
	float CurrentTowardDeg = 0.f;

	// =====================================================================
	// Simulador (valores da ficha da MBARI; "estimado" = não publicado)
	// =====================================================================

	/** Volume deslocado. Massa 4763 kg -> 4,66 m³ deixa ~13 kg de flutuação positiva (padrão de segurança de ROVs) */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Empuxo")
	float DisplacedVolume = 4.66f;     // m³

	/** Distância do centro de carena acima do centro de gravidade (BG). Gera o momento que endireita o ROV. */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Empuxo")
	float BuoyancyCenterHeight = 20.f; // cm (estimado)

	/**
	 * Posição no eixo X (para frente) onde a força LATERAL da água atua, em relação ao centro de massa.
	 * Negativo = atrás: a corrente lateral gira a proa para dentro da corrente, como um cata-vento.
	 * Em ROVs reais isso vem do formato assimétrico e do cabo umbilical preso na popa.
	 */
	UPROPERTY(EditAnywhere, Category = "ROV|Simulador|Arrasto")
	float LateralCenterOfPressureX = -30.f; // cm (estimado)

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

	/** Velocidade da câmera com o D-pad do gamepad */
	UPROPERTY(EditAnywhere, Category = "ROV|Camera")
	float GamepadLookSpeed = 90.f;     // °/s

private:
	// --- Simulação ---
	void TickSimulator(float DeltaTime);
	void TickDemo(float DeltaTime);
	void UpdateCameras(float DeltaTime);
	void UpdateTelemetry();
	void UpdateAutopilots(float DeltaTime);
	void UpdateAutoHeading(float DeltaTime);
	void UpdateAutoVertical(float DeltaTime);
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
	void OnLookPad(const FInputActionValue& Value);
	void OnResetHeld();
	void OnToggleAutoHeading();
	void OnToggleAutoDepth();
	void OnToggleAutoAltitude();

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
	UPROPERTY(Transient) TObjectPtr<UInputAction> LookPadAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ResetAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> AutoHeadingAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> AutoDepthAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> AutoAltitudeAction;

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

	// --- Pilotos automáticos ---
	// Rumo: PD (sem I). Kp = 0,08 por grau, Kd = 0,10 por grau/s. Ganhos ajustados simulando o yaw do ROV.
	FROVPid HeadingPid = FROVPid(0.08f, 0.f, 0.10f);
	bool bAutoHeading = false;
	float TargetHeadingDeg = 0.f;
	float YawCommand = 0.f; // o yaw que vai para os propulsores: do piloto ou do auto heading

	// Vertical: PID com I (a flutuação positiva é uma força constante). Ganhos ajustados simulando o heave;
	// o limite do I (0,1) é configurado no construtor.
	FROVPid DepthPid = FROVPid(0.8f, 0.08f, 1.5f);
	EROVVerticalAuto VerticalAuto = EROVVerticalAuto::Off;
	float TargetDepthM = 0.f;
	float TargetAltitudeM = 0.f;
	FVector TranslationCommand = FVector::ZeroVector; // surge, sway e heave que vão para os propulsores

	// "Segura onde parar": depois que o piloto solta (ou ao ligar o auto), freia antes de gravar o alvo
	bool bHeadingSettling = false;
	bool bVerticalSettling = false;

	FTransform StartTransform; // onde o ROV nasceu (usado pelo RESET)
};
