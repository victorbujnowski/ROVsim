#include "ROVHUD.h"

#include "ROVPawn.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"

namespace
{
	constexpr float MsToKnots = 1.f / 0.514444f;
	constexpr float LineHeight = 18.f;
	constexpr float Padding = 10.f;
}

void AROVHUD::DrawPanel(const TArray<FString>& Lines, float X, float Y, const FLinearColor& TextColor)
{
	UFont* Font = GEngine->GetSmallFont();

	float MaxWidth = 0.f;
	for (const FString& Line : Lines)
	{
		float W = 0.f, H = 0.f;
		GetTextSize(Line, W, H, Font);
		MaxWidth = FMath::Max(MaxWidth, W);
	}

	// Fundo semitransparente para o texto ficar legível sobre a água
	DrawRect(FLinearColor(0.f, 0.02f, 0.05f, 0.6f), X, Y, MaxWidth + 2.f * Padding, Lines.Num() * LineHeight + 2.f * Padding);

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		DrawText(Lines[i], TextColor, X + Padding, Y + Padding + i * LineHeight, Font);
	}
}

void AROVHUD::DrawHUD()
{
	Super::DrawHUD();

	const AROVPawn* ROV = Cast<AROVPawn>(GetOwningPawn());
	if (!ROV || !Canvas)
	{
		return;
	}

	const FROVTelemetry& T = ROV->GetTelemetry();
	const bool bSim = ROV->GetControlMode() == EROVControlMode::Simulator;
	const bool bCockpit = ROV->GetCameraMode() == EROVCameraMode::Cockpit;

	const FVector& V = T.LocalVelocityMs;
	const float HorizontalSpeed = FVector2D(V.X, V.Y).Size();
	const FString Altitude = T.AltitudeM >= 0.f ? FString::Printf(TEXT("%6.1f m"), T.AltitudeM) : TEXT("   --  ");

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("MODO %s   |   CAMERA %s   |   LUZES %s"),
		bSim ? TEXT("SIMULADOR") : TEXT("DEMONSTRACAO"),
		bCockpit ? TEXT("COCKPIT") : TEXT("EXTERNA"),
		ROV->AreLightsOn() ? TEXT("ON") : TEXT("OFF")));
	Lines.Add(FString::Printf(TEXT("Profundidade %7.1f m      Altitude %s"), T.DepthM, *Altitude));
	Lines.Add(FString::Printf(TEXT("Rumo %5.1f deg      Pitch %+5.1f deg      Roll %+5.1f deg"), T.HeadingDeg, T.PitchDeg, T.RollDeg));
	Lines.Add(FString::Printf(TEXT("Velocidade %5.2f m/s  (%4.2f nos)      Yaw %+6.1f deg/s"), HorizontalSpeed, HorizontalSpeed * MsToKnots, T.YawRateDegS));
	Lines.Add(FString::Printf(TEXT("Surge %+5.2f   Sway %+5.2f   Heave %+5.2f   m/s"), V.X, V.Y, V.Z));
	Lines.Add(FString::Printf(TEXT("%s  Surge %+4.0f%%  Sway %+4.0f%%  Heave %+4.0f%%  Yaw %+4.0f%%"),
		bSim ? TEXT("Propulsores") : TEXT("Comando    "),
		T.ThrustPercent.X, T.ThrustPercent.Y, T.ThrustPercent.Z, T.YawThrustPercent));
	if (bSim)
	{
		Lines.Add(FString::Printf(TEXT("Massa %.0f kg      Submerso %3.0f%%"), T.MassKg, T.SubmergedFraction * 100.f));
	}

	DrawPanel(Lines, 20.f, 20.f, bSim ? FLinearColor(0.4f, 1.f, 1.f) : FLinearColor(1.f, 0.85f, 0.3f));

	// Ajuda de controles no rodapé
	const TArray<FString> Help = {
		TEXT("W/S frente/re   A/D lateral   E/Q sobe/desce   Setas esq/dir giram"),
		TEXT("Mouse olha   Roda zoom   C camera   M modo   L luzes")
	};
	DrawPanel(Help, 20.f, Canvas->ClipY - (Help.Num() * LineHeight + 2.f * Padding) - 20.f, FLinearColor(0.85f, 0.85f, 0.85f));

	// Retícula central na câmera de cockpit
	if (bCockpit)
	{
		const float CX = Canvas->ClipX * 0.5f;
		const float CY = Canvas->ClipY * 0.5f;
		const FLinearColor Reticle(0.4f, 1.f, 1.f, 0.7f);
		DrawLine(CX - 20.f, CY, CX - 6.f, CY, Reticle, 1.5f);
		DrawLine(CX + 6.f, CY, CX + 20.f, CY, Reticle, 1.5f);
		DrawLine(CX, CY - 20.f, CX, CY - 6.f, Reticle, 1.5f);
		DrawLine(CX, CY + 6.f, CX, CY + 20.f, Reticle, 1.5f);
	}
}
