#include "ROVGameMode.h"

#include "ROVHUD.h"
#include "ROVPawn.h"
#include "ROVPlayerController.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

AROVGameMode::AROVGameMode()
{
	HUDClass = AROVHUD::StaticClass();

	// O ROV é criado pelo GameMode no PlayerStart a cada Play. Assim ele sempre nasce com a
	// estrutura atual do C++, sem depender de uma instância salva no mapa (que pode guardar
	// componentes antigos - foi o que travou o ROV quando o corpo passou de "Body" para "Hull").
	DefaultPawnClass = AROVPawn::StaticClass();
	PlayerControllerClass = AROVPlayerController::StaticClass();

	PrimaryActorTick.bCanEverTick = true;
}

void AROVGameMode::BeginPlay()
{
	Super::BeginPlay();

	bSmokeTest = FParse::Param(FCommandLine::Get(), TEXT("ROVSmokeTest"));
	if (bSmokeTest)
	{
		SmokeStartTime = GetWorld()->GetTimeSeconds();
		UE_LOG(LogROV, Display, TEXT("SMOKE TEST iniciado"));
	}
}

void AROVGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// Já existe um ROV (o do piloto): este jogador entra como OBSERVADOR, sem pawn. O normal da Unreal
	// seria criar um pawn para cada jogador, e cada um teria o seu ROV.
	for (TActorIterator<AROVPawn> It(GetWorld()); It; ++It)
	{
		UE_LOG(LogROV, Display, TEXT("Multiplayer: %s entrou como OBSERVADOR"), *GetNameSafe(NewPlayer));
		NewPlayer->SetViewTarget(*It); // no servidor também: define de onde ele "vê" (relevância de rede)
		return;
	}
	UE_LOG(LogROV, Display, TEXT("Multiplayer: %s é o PILOTO (recebe o ROV)"), *GetNameSafe(NewPlayer));
	Super::HandleStartingNewPlayer_Implementation(NewPlayer);
}

AROVPawn* AROVGameMode::FindROV() const
{
	return Cast<AROVPawn>(UGameplayStatics::GetPlayerPawn(this, 0));
}

void AROVGameMode::LogTelemetry(const TCHAR* Label) const
{
	const AROVPawn* ROV = FindROV();
	if (!ROV)
	{
		UE_LOG(LogROV, Warning, TEXT("SMOKE %s: jogador não está controlando um AROVPawn"), Label);
		return;
	}

	const FROVTelemetry& T = ROV->GetTelemetry();
	const FVector Pos = ROV->GetActorLocation() / 100.f;
	UE_LOG(LogROV, Display,
		TEXT("SMOKE t=%5.1f %-14s pos=(%7.1f,%7.1f,%6.1f)m modo=%s cam=%s prof=%6.2fm alt=%6.2fm rumo=%6.1f pitch=%+5.1f roll=%+5.1f vel=(%+5.2f,%+5.2f,%+5.2f)m/s yaw=%+6.1fdeg/s empuxo=(%+4.0f,%+4.0f,%+4.0f,%+4.0f)%% massa=%.0f"),
		GetWorld()->GetTimeSeconds() - SmokeStartTime, Label, Pos.X, Pos.Y, Pos.Z,
		ROV->GetControlMode() == EROVControlMode::Simulator ? TEXT("SIM ") : TEXT("DEMO"),
		ROV->GetCameraMode() == EROVCameraMode::External ? TEXT("EXT") : TEXT("COCK"),
		T.DepthM, T.AltitudeM, T.HeadingDeg, T.PitchDeg, T.RollDeg,
		T.LocalVelocityMs.X, T.LocalVelocityMs.Y, T.LocalVelocityMs.Z, T.YawRateDegS,
		T.ThrustPercent.X, T.ThrustPercent.Y, T.ThrustPercent.Z, T.YawThrustPercent, T.MassKg);
}

void AROVGameMode::TakeScreenshot(const FString& Name) const
{
	const FString File = FPaths::Combine(FPaths::ScreenShotDir(), TEXT("SmokeTest"), Name + TEXT(".png"));
	FScreenshotRequest::RequestScreenshot(File, true, false);
	UE_LOG(LogROV, Display, TEXT("SMOKE screenshot: %s"), *File);
}

AROVGameMode::FOscillation AROVGameMode::AnalyzeOscillation(const TArray<FVector2D>& Samples, double OmegaRadS)
{
	FOscillation Result;
	const int32 N = Samples.Num();
	if (N < 8)
	{
		return Result;
	}

	// 1) Ajuste por mínimos quadrados de RETA + SENOIDE juntas:  z(t) = A + B·t + C·cos ωt + D·sin ωt
	//    Ajustar só a reta erra: uma senoide numa janela finita "parece" inclinada (até ±0,02 m/s com
	//    0,8 m de amplitude e 3 períodos). Ajustando as duas juntas, B mede só a deriva de verdade.
	//    Equações normais: (Xᵀ X)·p = Xᵀ z, com X = [1, t, cos ωt, sin ωt] (4x4), resolvidas por Gauss.
	const double T0 = Samples[0].X; // tempo relativo ao início: números menores, conta mais precisa
	double M[4][5] = {};
	for (const FVector2D& S : Samples)
	{
		const double T = S.X - T0;
		const double Row[4] = { 1.0, T, FMath::Cos(OmegaRadS * T), FMath::Sin(OmegaRadS * T) };
		for (int32 i = 0; i < 4; ++i)
		{
			for (int32 j = 0; j < 4; ++j) { M[i][j] += Row[i] * Row[j]; }
			M[i][4] += Row[i] * S.Y;
		}
	}
	for (int32 Col = 0; Col < 4; ++Col) // eliminação de Gauss com pivô parcial
	{
		int32 Pivot = Col;
		for (int32 r = Col + 1; r < 4; ++r) { if (FMath::Abs(M[r][Col]) > FMath::Abs(M[Pivot][Col])) { Pivot = r; } }
		for (int32 c = 0; c < 5; ++c) { Swap(M[Col][c], M[Pivot][c]); }
		for (int32 r = 0; r < 4; ++r)
		{
			if (r == Col || FMath::Abs(M[Col][Col]) < 1e-12) { continue; }
			const double F = M[r][Col] / M[Col][Col];
			for (int32 c = Col; c < 5; ++c) { M[r][c] -= F * M[Col][c]; }
		}
	}
	double P[4];
	for (int32 i = 0; i < 4; ++i) { P[i] = FMath::Abs(M[i][i]) > 1e-12 ? M[i][4] / M[i][i] : 0.0; }
	const double A = P[0], B = P[1];

	double SumZ = 0;
	for (const FVector2D& S : Samples) { SumZ += S.Y; }
	Result.MeanDepthM = static_cast<float>(SumZ / N);
	Result.DriftMs = static_cast<float>(B);
	Result.AmplitudeM = static_cast<float>(FMath::Sqrt(P[2] * P[2] + P[3] * P[3])); // amplitude da senoide ajustada

	// 2) Período MEDIDO de forma independente (o ajuste acima supõe ω): cruzamentos do zero do sinal sem a deriva
	double FirstCrossing = 0, LastCrossing = 0, Previous = 0;
	for (int32 i = 0; i < N; ++i)
	{
		const double Residual = Samples[i].Y - (A + B * (Samples[i].X - T0));
		if (i > 0 && Previous < 0.0 && Residual >= 0.0)
		{
			// Interpola entre as duas amostras o instante exato em que o sinal passou pelo zero
			const double Crossing = Samples[i - 1].X + (Samples[i].X - Samples[i - 1].X) * (-Previous / (Residual - Previous));
			if (Result.Crossings == 0) { FirstCrossing = Crossing; }
			LastCrossing = Crossing;
			++Result.Crossings;
		}
		Previous = Residual;
	}
	Result.PeriodS = Result.Crossings > 1 ? static_cast<float>((LastCrossing - FirstCrossing) / (Result.Crossings - 1)) : 0.f;
	return Result;
}

void AROVGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bSmokeTest)
	{
		return;
	}

	AROVPawn* ROV = FindROV();
	if (!ROV)
	{
		return;
	}

	const float T = GetWorld()->GetTimeSeconds() - SmokeStartTime;

	if (bSwellSampling)
	{
		SwellSamples.Add(FVector2D(T, ROV->GetTelemetry().DepthM));
	}
	if (bHoldSampling)
	{
		HoldOffsets.Add(ROV->GetPositionOffsetM());
		HoldThrust.Add(FVector2D(ROV->GetTelemetry().ThrustPercent.X, ROV->GetTelemetry().ThrustPercent.Y));
		HoldVertical.Add(FVector2D(ROV->GetTelemetry().ThrustPercent.Z,
			FMath::FindDeltaAngleDegrees(ROV->GetTargetHeading(), ROV->GetTelemetry().HeadingDeg)));
	}

	// Telemetria a cada segundo
	if (T >= NextLogTime)
	{
		NextLogTime += 1.f;
		LogTelemetry(TEXT("amostra"));
	}

	// Roteiro: cada passo roda uma vez quando o tempo dele chega
	struct FStep { float Time; TFunction<void()> Action; };
	const TArray<FStep> Script = {
		{ 1.f,  [&] { ROV->SetControlMode(EROVControlMode::Simulator); ROV->SetCameraMode(EROVCameraMode::External);
		              ROV->SetPilotCommand(FVector::ZeroVector, 0.f); LogTelemetry(TEXT("SIM repouso")); } },
		{ 6.f,  [&] { TakeScreenshot(TEXT("01_sim_externa_repouso")); LogTelemetry(TEXT("SIM repouso"));
		              ROV->SetPilotCommand(FVector(1.f, 0.f, 0.f), 0.f); } },
		{ 26.f, [&] { LogTelemetry(TEXT("SIM surge fim")); TakeScreenshot(TEXT("02_sim_surge"));
		              ROV->SetPilotCommand(FVector(0.f, 0.f, 1.f), 0.f); } },
		{ 41.f, [&] { LogTelemetry(TEXT("SIM heave fim")); ROV->SetPilotCommand(FVector::ZeroVector, 1.f); } },
		{ 53.f, [&] { LogTelemetry(TEXT("SIM yaw fim")); ROV->SetPilotCommand(FVector::ZeroVector, 0.f);
		              ROV->SetCameraMode(EROVCameraMode::Cockpit); } },
		{ 56.f, [&] { TakeScreenshot(TEXT("03_sim_cockpit")); } },
		{ 57.f, [&] { ROV->SetControlMode(EROVControlMode::Demo); ROV->SetCameraMode(EROVCameraMode::External);
		              ROV->SetPilotCommand(FVector(1.f, 0.f, 0.f), 0.f); } },
		{ 62.f, [&] { LogTelemetry(TEXT("DEMO surge fim")); TakeScreenshot(TEXT("04_demo_externa"));
		              ROV->SetPilotCommand(FVector(0.f, 0.f, -1.f), 1.f); } },
		{ 66.f, [&] { LogTelemetry(TEXT("DEMO desce+gira")); ROV->SetPilotCommand(FVector(0.f, 0.f, -1.f), 0.f); } },
		// Desce até encostar no fundo: a varredura de colisão tem que segurar o ROV
		{ 72.f, [&] { LogTelemetry(TEXT("DEMO no fundo")); ROV->SetPilotCommand(FVector(0.f, 0.f, 0.4f), 0.f); } },
		{ 72.6f, [&] { ROV->SetPilotCommand(FVector::ZeroVector, 0.f); ROV->SetCameraMode(EROVCameraMode::Cockpit); } },
		{ 75.f, [&] { LogTelemetry(TEXT("DEMO cockpit")); TakeScreenshot(TEXT("05_demo_cockpit")); } },
		{ 76.f, [&] { ROV->SetCameraMode(EROVCameraMode::External); TakeScreenshot(TEXT("06_demo_externa_fundo")); } },
		{ 77.f, [&] { ROV->SetControlMode(EROVControlMode::Simulator); } },
		{ 82.f, [&] { LogTelemetry(TEXT("SIM apos troca")); TakeScreenshot(TEXT("07_sim_apos_troca")); } },
		// Estabilidade: inclina o ROV e confere se o momento do empuxo (BG) o endireita sozinho
		{ 83.f, [&] { ROV->SetActorRotation(FRotator(15.f, ROV->GetActorRotation().Yaw, 20.f), ETeleportType::TeleportPhysics);
		              LogTelemetry(TEXT("SIM inclinado")); } },
		{ 84.5f, [&] { TakeScreenshot(TEXT("08_sim_endireitando")); } },
		{ 98.f, [&] { LogTelemetry(TEXT("SIM estabilizou")); } },
		// Corrente: 1 nó para o Norte, sem comando. Depois de 60 s o ROV tem que derivar junto com a água.
		{ 99.f, [&] { ROV->SetPilotCommand(FVector::ZeroVector, 0.f); ROV->SetCurrent(1.f, 0.f);
					  LogTelemetry(TEXT("SIM corrente on")); } },
		{ 159.f, [&] {
			const FVector GroundVelocity = ROV->GetVelocity() / 100.f;   // m/s em relação ao fundo
			const FVector Expected = ROV->GetCurrentVelocityMs();
			const float Error = FVector2D(GroundVelocity - Expected).Size(); // só no plano horizontal
			const bool bPassed = Error < 0.05f;                             // tolerância de 5 cm/s
			UE_LOG(LogROV, Display, TEXT("SMOKE CHECK corrente: %s (deriva %.3f m/s, esperado %.3f m/s, erro %.3f m/s)"),
				bPassed ? TEXT("PASSOU") : TEXT("FALHOU"), FVector2D(GroundVelocity).Size(), Expected.Size(), Error);
			ROV->SetCurrent(0.f, 0.f);
		} },
		// Autos: rumo 45 graus e 3 m mais fundo, com 1 nó de corrente para o Leste.
		// Critério do plano: rumo ±2 graus e profundidade ±0,2 m.
		{ 160.f, [&] { ROV->SetCurrent(1.f, 90.f);
		               ROV->SetTargetHeading(45.f);
		               ROV->SetTargetDepth(ROV->GetTelemetry().DepthM + 3.f);
		               LogTelemetry(TEXT("SIM autos on")); } },
		{ 175.f, [&] { TakeScreenshot(TEXT("09_sim_autos_corrente")); } },
		{ 190.f, [&] {
			const FROVTelemetry& T = ROV->GetTelemetry();
			const float HeadingError = FMath::Abs(FMath::FindDeltaAngleDegrees(T.HeadingDeg, ROV->GetTargetHeading()));
			const float DepthError = FMath::Abs(T.DepthM - ROV->GetTargetDepth());
			UE_LOG(LogROV, Display, TEXT("SMOKE CHECK auto heading: %s (erro %.2f graus)"),
				HeadingError < 2.f ? TEXT("PASSOU") : TEXT("FALHOU"), HeadingError);
			UE_LOG(LogROV, Display, TEXT("SMOKE CHECK auto depth: %s (erro %.3f m)"),
				DepthError < 0.2f ? TEXT("PASSOU") : TEXT("FALHOU"), DepthError);
			ROV->SetTargetAltitude(3.f); // troca o vertical de profundidade para altitude
		} },
		{ 220.f, [&] {
			const float AltitudeError = FMath::Abs(ROV->GetTelemetry().AltitudeM - ROV->GetTargetAltitude());
			UE_LOG(LogROV, Display, TEXT("SMOKE CHECK auto altitude: %s (erro %.3f m)"),
				AltitudeError < 0.2f ? TEXT("PASSOU") : TEXT("FALHOU"), AltitudeError);
			TakeScreenshot(TEXT("10_sim_auto_altitude"));
			ROV->SetAutoHeading(false);
			ROV->SetAutoDepth(false);
			ROV->SetCurrent(0.f, 0.f);
		} },
		// Swell: 2 m / 10 s vindo do Norte. Mede a oscilação vertical do ROV solto (sem auto depth)
		// no fundo (~34 m) e perto da superfície (~8 m). A teoria linear diz que o movimento cai com
		// e^(-k·z): a razão entre as duas amplitudes tem que ser e^(-k·Δz), e o período o da onda.
		// Comando vertical fixo de -2,5% compensa a flutuação positiva ((1025·4,66 − 4763)·9,81 ≈ 130 N
		// de 5394 N): sem ele o ROV subiria ~0,13 m/s durante a medição. A deriva que sobrar é removida
		// na análise (reta ajustada por mínimos quadrados).
		{ 221.f, [&] { ROV->SetSwell(2.f, 10.f, 0.f); ROV->SetPilotCommand(FVector(0.f, 0.f, -0.025f), 0.f);
		               LogTelemetry(TEXT("SIM swell on")); } },
		{ 246.f, [&] { SwellSamples.Reset(); bSwellSampling = true; } },
		{ 276.f, [&] {
			bSwellSampling = false;
			SwellDeep = AnalyzeOscillation(SwellSamples, 2.0 * PI / ROV->GetSwellPeriodS());
			UE_LOG(LogROV, Display, TEXT("SMOKE swell fundo: prof. média %.2f m, período %.2f s, amplitude %.3f m, deriva %+.3f m/s (%d cruzamentos)"),
				SwellDeep.MeanDepthM, SwellDeep.PeriodS, SwellDeep.AmplitudeM, SwellDeep.DriftMs, SwellDeep.Crossings);
			ROV->SetPilotCommand(FVector::ZeroVector, 0.f);
			ROV->SetTargetDepth(8.f); // sobe para perto da superfície (o auto aguenta o swell no caminho)
		} },
		{ 281.f, [&] { TakeScreenshot(TEXT("11_sim_swell_subindo")); } },
		{ 326.f, [&] { LogTelemetry(TEXT("SIM swell 8 m")); ROV->SetAutoDepth(false);
		               ROV->SetPilotCommand(FVector(0.f, 0.f, -0.025f), 0.f); } },
		{ 331.f, [&] { SwellSamples.Reset(); bSwellSampling = true; } },
		{ 361.f, [&] {
			bSwellSampling = false;
			const FOscillation Near = AnalyzeOscillation(SwellSamples, 2.0 * PI / ROV->GetSwellPeriodS());
			UE_LOG(LogROV, Display, TEXT("SMOKE swell perto: prof. média %.2f m, período %.2f s, amplitude %.3f m, deriva %+.3f m/s (%d cruzamentos)"),
				Near.MeanDepthM, Near.PeriodS, Near.AmplitudeM, Near.DriftMs, Near.Crossings);
			// Pendência corrigida: o ROV afundava 0,03-0,08 m/s durante a medição. Era um transitório (onda ligada de
			// uma vez; ROV solto pelo auto depth que lutava contra a onda). Com a rampa e o filtro, a deriva some.
			UE_LOG(LogROV, Display, TEXT("SMOKE CHECK swell sem deriva: %s (fundo %+.3f m/s, perto %+.3f m/s; limite 0,02)"),
				FMath::Abs(SwellDeep.DriftMs) < 0.02f && FMath::Abs(Near.DriftMs) < 0.02f ? TEXT("PASSOU") : TEXT("FALHOU"), SwellDeep.DriftMs, Near.DriftMs);

			const float Wavelength = ROV->GetSwellWavelengthM();
			const float K = 2.f * PI / Wavelength;
			const float ExpectedRatio = FMath::Exp(-K * (SwellDeep.MeanDepthM - Near.MeanDepthM)); // fundo / perto
			const float MeasuredRatio = Near.AmplitudeM > 0.f ? SwellDeep.AmplitudeM / Near.AmplitudeM : 0.f;
			const float ExpectedNear = 0.5f * ROV->GetSwellHeightM() * ROV->GetSwellAttenuation(Near.MeanDepthM);
			const float Period = ROV->GetSwellPeriodS();

			const bool bPeriodOk = FMath::Abs(Near.PeriodS - Period) < 0.1f * Period && FMath::Abs(SwellDeep.PeriodS - Period) < 0.1f * Period;
			const bool bRatioOk = Near.AmplitudeM > SwellDeep.AmplitudeM && FMath::Abs(MeasuredRatio / ExpectedRatio - 1.f) < 0.2f;
			const bool bAmplitudeOk = FMath::Abs(Near.AmplitudeM / ExpectedNear - 1.f) < 0.25f;
			UE_LOG(LogROV, Display,
				TEXT("SMOKE CHECK swell: %s (período %.2f/%.2f s, esperado %.1f s; razão fundo/perto %.3f, esperado e^(-kΔz) = %.3f; amplitude perto %.3f m, teoria %.3f m; λ %.0f m)"),
				bPeriodOk && bRatioOk && bAmplitudeOk ? TEXT("PASSOU") : TEXT("FALHOU"),
				Near.PeriodS, SwellDeep.PeriodS, Period, MeasuredRatio, ExpectedRatio, Near.AmplitudeM, ExpectedNear, Wavelength);

			TakeScreenshot(TEXT("12_sim_swell_superficie"));
			ROV->SetSwell(0.f, 10.f, 0.f);
			ROV->SetPilotCommand(FVector::ZeroVector, 0.f);
		} },
		// Station keeping: 1 nó de corrente do Sul para o Norte, proa para o Leste (corrente de LADO,
		// o pior caso: área lateral maior e sway mais fraco) e swell de 2 m / 10 s vindo do Sul, a 15 m.
		// Sem o auto, o ROV derivaria ~20 m nos 40 s de medição.
		{ 362.f, [&] { ROV->SetPilotCommand(FVector::ZeroVector, 0.f);
		               ROV->SetCurrent(1.f, 0.f); ROV->SetSwell(2.f, 10.f, 180.f);
		               ROV->SetTargetHeading(90.f); ROV->SetTargetDepth(15.f); ROV->SetPositionHold(true);
		               LogTelemetry(TEXT("SIM station keeping on")); } },
		{ 392.f, [&] { TakeScreenshot(TEXT("13_sim_station_keeping")); } },
		{ 422.f, [&] { HoldOffsets.Reset(); HoldThrust.Reset(); HoldVertical.Reset(); bHoldSampling = true; } },
		{ 462.f, [&] {
			bHoldSampling = false;
			const int32 N = FMath::Max(HoldOffsets.Num(), 1);

			// Média do desvio (o ROV está PARADO no alvo, em média?) e maior desvio (inclui a órbita da onda)
			FVector2D MeanOffset = FVector2D::ZeroVector, MeanThrust = FVector2D::ZeroVector;
			double MaxOffset = 0.0;
			for (int32 i = 0; i < HoldOffsets.Num(); ++i)
			{
				MeanOffset += HoldOffsets[i] / N;
				MeanThrust += HoldThrust[i] / N;
				MaxOffset = FMath::Max(MaxOffset, HoldOffsets[i].Size());
			}
			// Quanto o empuxo varia em volta da média: com o filtro de ondas, os propulsores NÃO correm atrás de cada onda
			double ThrustVariance = 0.0;
			for (const FVector2D& Thrust : HoldThrust)
			{
				ThrustVariance += (Thrust - MeanThrust).SizeSquared() / N;
			}
			const double ThrustStd = FMath::Sqrt(ThrustVariance);
			const double OrbitRadius = 0.5 * ROV->GetSwellHeightM() * ROV->GetSwellAttenuation(ROV->GetTelemetry().DepthM);

			const bool bPassed = MeanOffset.Size() < 0.3 && MaxOffset < OrbitRadius + 0.6 && ThrustStd < 10.0;

			// Auto depth com filtro de ondas: o heave NÃO pode ficar correndo atrás da onda (antes oscilava ±48%).
			// Auto heading com I: o erro de rumo médio tem que ir a zero mesmo com o cata-vento (antes ~2°).
			double HeaveMean = 0.0, HeadingMean = 0.0, HeaveVariance = 0.0;
			for (const FVector2D& V : HoldVertical) { HeaveMean += V.X / N; HeadingMean += V.Y / N; }
			for (const FVector2D& V : HoldVertical) { HeaveVariance += FMath::Square(V.X - HeaveMean) / N; }
			const double HeaveStd = FMath::Sqrt(HeaveVariance);
			UE_LOG(LogROV, Display, TEXT("SMOKE CHECK filtro de ondas no auto depth: %s (variação do heave %.1f%%, média %+.1f%%)"),
				HeaveStd < 10.0 ? TEXT("PASSOU") : TEXT("FALHOU"), HeaveStd, HeaveMean);
			UE_LOG(LogROV, Display, TEXT("SMOKE CHECK auto heading com corrente de lado: %s (erro médio de rumo %+.2f graus)"),
				FMath::Abs(HeadingMean) < 0.5 ? TEXT("PASSOU") : TEXT("FALHOU"), HeadingMean);
			UE_LOG(LogROV, Display,
				TEXT("SMOKE CHECK station keeping: %s (desvio médio %.3f m, máximo %.2f m com órbita da onda de %.2f m; empuxo médio surge %+.1f%% sway %+.1f%%, variação %.1f%%; sem o auto derivaria %.0f m)"),
				bPassed ? TEXT("PASSOU") : TEXT("FALHOU"), MeanOffset.Size(), MaxOffset, OrbitRadius,
				MeanThrust.X, MeanThrust.Y, ThrustStd, ROV->GetCurrentVelocityMs().Size() * 40.f);

			ROV->SetPositionHold(false); ROV->SetAutoHeading(false); ROV->SetAutoDepth(false);
			ROV->SetCurrent(0.f, 0.f); ROV->SetSwell(0.f, 10.f, 0.f);
		} },
		{ 463.f, [&] { UE_LOG(LogROV, Display, TEXT("SMOKE TEST concluído"));
		               FGenericPlatformMisc::RequestExit(false, TEXT("ROVSmokeTest")); } },
	};

	while (SmokeStep < Script.Num() && T >= Script[SmokeStep].Time)
	{
		Script[SmokeStep].Action();
		++SmokeStep;
	}
}
