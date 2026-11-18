#include "ROVHUD.h"

#include "ROVPawn.h"
#include "ROVUdpLinkComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"

namespace
{
	constexpr float MsToKnots = 1.f / 0.514444f;
	constexpr float LineHeight = 17.f;
	constexpr float Padding = 10.f;
	constexpr float ColumnGap = 16.f;
	constexpr float Margin = 20.f;
	constexpr float SectionGap = 10.f;
	constexpr float RightColumnWidth = 400.f; // todas as seções da direita com a mesma largura

	const FLinearColor PanelColor(0.f, 0.02f, 0.05f, 0.68f);
	const FLinearColor LabelColor(0.78f, 0.84f, 0.88f);
	const FLinearColor HintColor(0.52f, 0.60f, 0.65f);
	const FLinearColor GoodColor(0.45f, 1.f, 0.55f);
	const FLinearColor WarnColor(1.f, 0.75f, 0.3f);

	/** 16 pontos da rosa dos ventos (L = Leste, O = Oeste) */
	const TCHAR* Cardinal(float HeadingDeg)
	{
		static const TCHAR* Names[16] = {
			TEXT("N"), TEXT("NNE"), TEXT("NE"), TEXT("ENE"), TEXT("L"), TEXT("ESE"), TEXT("SE"), TEXT("SSE"),
			TEXT("S"), TEXT("SSO"), TEXT("SO"), TEXT("OSO"), TEXT("O"), TEXT("ONO"), TEXT("NO"), TEXT("NNO") };
		return Names[FMath::RoundToInt(FRotator::ClampAxis(HeadingDeg) / 22.5f) % 16];
	}
}

// =============================================================================
// Blocos de desenho
// =============================================================================

float AROVHUD::DrawSection(const FString& Title, const TArray<FHudRow>& Rows, float X, float Y, bool bAlignRight, const FLinearColor& Accent, float MinWidth)
{
	UFont* Font = GEngine->GetSmallFont();

	// Mede as três colunas para alinhar todas as linhas
	float LabelW = 0.f, ValueW = 0.f, HintW = 0.f, TitleW = 0.f, H = 0.f;
	GetTextSize(Title, TitleW, H, Font);
	for (const FHudRow& Row : Rows)
	{
		float W = 0.f;
		GetTextSize(Row.Label, W, H, Font); LabelW = FMath::Max(LabelW, W);
		GetTextSize(Row.Value, W, H, Font); ValueW = FMath::Max(ValueW, W);
		GetTextSize(Row.Hint, W, H, Font);  HintW = FMath::Max(HintW, W);
	}

	const float ContentW = FMath::Max(TitleW, LabelW + ColumnGap + ValueW + ColumnGap + HintW);
	const float Width = FMath::Max(MinWidth, ContentW + 2.f * Padding);
	const float Height = 2.f * Padding + LineHeight * (Rows.Num() + 1) + 4.f;
	const float Left = bAlignRight ? X - Width : X;

	DrawRect(PanelColor, Left, Y, Width, Height);
	DrawRect(Accent, Left, Y, 3.f, Height); // faixa colorida: identifica a seção de longe
	DrawText(Title, Accent, Left + Padding, Y + Padding, Font);

	const float ValueX = Left + Padding + LabelW + ColumnGap;
	const float HintX = ValueX + ValueW + ColumnGap;
	float RowY = Y + Padding + LineHeight + 4.f;
	for (const FHudRow& Row : Rows)
	{
		DrawText(Row.Label, LabelColor, Left + Padding, RowY, Font);
		DrawText(Row.Value, Row.ValueColor, ValueX, RowY, Font);
		DrawText(Row.Hint, HintColor, HintX, RowY, Font);
		RowY += LineHeight;
	}
	return Height;
}

float AROVHUD::DrawBars(const FString& Title, const FString& Subtitle, const TArray<FHudBar>& Bars, float RightEdge, float Y, const FLinearColor& Accent)
{
	UFont* Font = GEngine->GetSmallFont();

	constexpr float LabelW = 120.f;
	constexpr float BarW = 200.f;   // metade para cada lado do zero
	constexpr float BarH = 9.f;
	constexpr float ValueW = 50.f;
	const float Width = 2.f * Padding + LabelW + BarW + 10.f + ValueW; // = 400, a largura da coluna direita
	const float Height = 2.f * Padding + LineHeight * (Bars.Num() + 2) + 4.f;
	const float Left = RightEdge - Width;

	DrawRect(PanelColor, Left, Y, Width, Height);
	DrawRect(Accent, Left, Y, 3.f, Height);
	DrawText(Title, Accent, Left + Padding, Y + Padding, Font);
	DrawText(Subtitle, HintColor, Left + Padding, Y + Padding + LineHeight, Font);

	const float BarLeft = Left + Padding + LabelW;
	const float Center = BarLeft + BarW * 0.5f;
	float RowY = Y + Padding + 2.f * LineHeight + 4.f;
	for (const FHudBar& Bar : Bars)
	{
		const float Fraction = FMath::Clamp(Bar.Percent / 100.f, -1.f, 1.f);
		const float BarY = RowY + (LineHeight - BarH) * 0.5f - 1.f;

		DrawText(Bar.Label, LabelColor, Left + Padding, RowY, Font);
		DrawRect(FLinearColor(1.f, 1.f, 1.f, 0.08f), BarLeft, BarY, BarW, BarH);        // trilho
		const float FillW = FMath::Abs(Fraction) * BarW * 0.5f;
		DrawRect(Fraction >= 0.f ? GoodColor : WarnColor, Fraction >= 0.f ? Center : Center - FillW, BarY, FillW, BarH);
		DrawRect(FLinearColor(1.f, 1.f, 1.f, 0.6f), Center - 0.5f, BarY - 2.f, 1.f, BarH + 4.f); // marca do zero
		DrawText(FString::Printf(TEXT("%+4.0f%%"), Bar.Percent), FLinearColor::White, BarLeft + BarW + 10.f, RowY, Font);
		RowY += LineHeight;
	}
	return Height;
}

float AROVHUD::DrawStatusBar(const TArray<TPair<FString, FString>>& Items, float X, float Y, const FLinearColor& Accent)
{
	UFont* Font = GEngine->GetSmallFont();
	constexpr float ItemGap = 26.f;

	float TotalW = 0.f, W = 0.f, H = 0.f;
	for (const TPair<FString, FString>& Item : Items)
	{
		GetTextSize(Item.Key + TEXT("  ") + Item.Value, W, H, Font);
		TotalW += W + ItemGap;
	}
	const float Height = LineHeight + 2.f * 7.f;
	DrawRect(PanelColor, X, Y, TotalW - ItemGap + 2.f * Padding, Height);

	float CursorX = X + Padding;
	for (const TPair<FString, FString>& Item : Items)
	{
		DrawText(Item.Key, HintColor, CursorX, Y + 7.f, Font);
		GetTextSize(Item.Key + TEXT("  "), W, H, Font);
		DrawText(Item.Value, Accent, CursorX + W, Y + 7.f, Font);
		GetTextSize(Item.Key + TEXT("  ") + Item.Value, W, H, Font);
		CursorX += W + ItemGap;
	}
	return Height;
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

	DrawRect(PanelColor, X, Y, MaxWidth + 2.f * Padding, Lines.Num() * LineHeight + 2.f * Padding);
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		DrawText(Lines[i], TextColor, X + Padding, Y + Padding + i * LineHeight, Font);
	}
}

// =============================================================================
// HUD
// =============================================================================

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
	const FLinearColor Accent = bSim ? FLinearColor(0.35f, 0.95f, 1.f) : FLinearColor(1.f, 0.8f, 0.3f);
	const UROVUdpLinkComponent* UdpLink = ROV->FindComponentByClass<UROVUdpLinkComponent>();
	const bool bRemote = UdpLink && UdpLink->IsRemoteActive();

	// ---------------------------------------------------------------- estado
	const float StatusH = DrawStatusBar({
		{ TEXT("MODO"),     bSim ? TEXT("SIMULADOR (física real)") : TEXT("DEMONSTRAÇÃO (arcade, sem física)") },
		{ TEXT("CÂMERA"),   bCockpit ? TEXT("cockpit (vídeo do ROV)") : TEXT("externa") },
		{ TEXT("LUZES"),    ROV->AreLightsOn() ? TEXT("ligadas") : TEXT("desligadas") },
		{ TEXT("CONTROLE"), bRemote ? TEXT("REMOTO (UDP 5000)") : TEXT("local") },
	}, Margin, Margin, Accent);

	const float TopY = Margin + StatusH + SectionGap;
	float LeftY = TopY;
	float RightY = TopY;
	const float RightEdge = Canvas->ClipX - Margin;

	// ---------------------------------------------------------------- navegação
	{
		const FString Altitude = T.AltitudeM >= 0.f ? FString::Printf(TEXT("%.1f m"), T.AltitudeM) : TEXT("--");
		LeftY += DrawSection(TEXT("NAVEGAÇÃO"), {
			{ TEXT("Profundidade"), FString::Printf(TEXT("%.1f m"), T.DepthM), TEXT("abaixo da superfície") },
			{ TEXT("Altitude"),     Altitude, T.AltitudeM >= 0.f ? TEXT("distância até o fundo (altímetro)") : TEXT("fundo fora do alcance (> 100 m)") },
			{ TEXT("Rumo"),         FString::Printf(TEXT("%05.1f°  %s"), T.HeadingDeg, Cardinal(T.HeadingDeg)), TEXT("para onde a proa aponta (0° = Norte)") },
			{ TEXT("Pitch"),        FString::Printf(TEXT("%+.1f°"), T.PitchDeg), TEXT("inclinação da proa (+ = proa para cima)") },
			{ TEXT("Roll"),         FString::Printf(TEXT("%+.1f°"), T.RollDeg),  TEXT("inclinação lateral") },
		}, Margin, LeftY, false, Accent) + SectionGap;
	}

	// ---------------------------------------------------------------- velocidade
	{
		const FVector& V = T.LocalVelocityMs;
		const float Horizontal = FVector2D(V.X, V.Y).Size();
		LeftY += DrawSection(TEXT("VELOCIDADE  (em relação ao fundo)"), {
			{ TEXT("Avanço (surge)"),   FString::Printf(TEXT("%+.2f m/s"), V.X), TEXT("+ = para frente") },
			{ TEXT("Lateral (sway)"),   FString::Printf(TEXT("%+.2f m/s"), V.Y), TEXT("+ = para a direita") },
			{ TEXT("Vertical (heave)"), FString::Printf(TEXT("%+.2f m/s"), V.Z), TEXT("+ = subindo") },
			{ TEXT("Horizontal"),       FString::Printf(TEXT("%.2f m/s  (%.2f nós)"), Horizontal, Horizontal * MsToKnots), TEXT("velocidade sobre o fundo") },
			{ TEXT("Giro (yaw)"),       FString::Printf(TEXT("%+.1f °/s"), T.YawRateDegS), TEXT("+ = girando para a direita") },
		}, Margin, LeftY, false, Accent) + SectionGap;
	}

	// ---------------------------------------------------------------- propulsores
	RightY += DrawBars(
		bSim ? TEXT("PROPULSORES") : TEXT("COMANDO"),
		bSim ? TEXT("empuxo aplicado, em % do máximo") : TEXT("modo demo: comando direto, sem física"),
		{
			// FVector guarda double; a inicialização com { } não aceita reduzir para float sem cast explícito
			{ TEXT("Avanço"),   static_cast<float>(T.ThrustPercent.X) },
			{ TEXT("Lateral"),  static_cast<float>(T.ThrustPercent.Y) },
			{ TEXT("Vertical"), static_cast<float>(T.ThrustPercent.Z) },
			{ TEXT("Giro"),     T.YawThrustPercent },
		}, RightEdge, RightY, Accent) + SectionGap;

	// ---------------------------------------------------------------- piloto automático
	{
		FHudRow Heading{ TEXT("Rumo"), TEXT("desligado"), TEXT("H  /  L3  /  AUTO HEADING"), HintColor };
		if (ROV->IsAutoHeadingOn())
		{
			const float Deviation = FMath::FindDeltaAngleDegrees(ROV->GetTargetHeading(), T.HeadingDeg);
			Heading.Value = FString::Printf(TEXT("ALVO %05.1f°"), ROV->GetTargetHeading());
			Heading.Hint = FString::Printf(TEXT("desvio %+.1f°"), Deviation);
			Heading.ValueColor = GoodColor;
		}

		FHudRow Vertical{ TEXT("Vertical"), TEXT("desligado"), TEXT("J prof.  /  K alt.  /  AUTO DEPTH|ALT"), HintColor };
		if (ROV->GetVerticalAuto() == EROVVerticalAuto::Depth)
		{
			Vertical.Value = FString::Printf(TEXT("PROF. %.1f m"), ROV->GetTargetDepth());
			Vertical.Hint = FString::Printf(TEXT("desvio %+.2f m  (+ = mais fundo)"), T.DepthM - ROV->GetTargetDepth());
			Vertical.ValueColor = GoodColor;
		}
		else if (ROV->GetVerticalAuto() == EROVVerticalAuto::Altitude)
		{
			Vertical.Value = FString::Printf(TEXT("ALT. %.1f m"), ROV->GetTargetAltitude());
			Vertical.Hint = FString::Printf(TEXT("desvio %+.2f m  (+ = mais alto)"), T.AltitudeM - ROV->GetTargetAltitude());
			Vertical.ValueColor = GoodColor;
		}

		RightY += DrawSection(TEXT("PILOTO AUTOMÁTICO"), { Heading, Vertical }, RightEdge, RightY, true, Accent, RightColumnWidth) + SectionGap;
	}

	// ---------------------------------------------------------------- ambiente
	{
		const FVector Current = ROV->GetCurrentVelocityMs();
		FHudRow CurrentRow{ TEXT("Corrente"), TEXT("nenhuma"), TEXT("CURRENT <nós> <graus>  no console TCP"), HintColor };
		if (!Current.IsNearlyZero())
		{
			// atan2(Y, X) devolve o ângulo do vetor: 0 = +X (Norte), 90 = +Y (Leste)
			const float TowardDeg = FRotator::ClampAxis(FMath::RadiansToDegrees(FMath::Atan2(Current.Y, Current.X)));
			CurrentRow.Value = FString::Printf(TEXT("%.2f nós  para %03.0f° %s"), Current.Size() * MsToKnots, TowardDeg, Cardinal(TowardDeg));
			CurrentRow.Hint = TEXT("para onde a água vai");
			CurrentRow.ValueColor = WarnColor;
		}

		TArray<FHudRow> Rows = { CurrentRow };
		if (bSim)
		{
			Rows.Add({ TEXT("Submerso"), FString::Printf(TEXT("%.0f%%"), T.SubmergedFraction * 100.f), TEXT("parte do casco dentro d'água") });
			Rows.Add({ TEXT("Massa"),    FString::Printf(TEXT("%.0f kg"), T.MassKg), TEXT("Doc Ricketts (ficha da MBARI)") });
		}
		RightY += DrawSection(TEXT("AMBIENTE"), Rows, RightEdge, RightY, true, Accent, RightColumnWidth) + SectionGap;
	}

	// ---------------------------------------------------------------- ajuda
	const TArray<FString> Help = {
		TEXT("TECLADO   W/S frente/ré   A/D lateral   E/Q sobe/desce   setas giram   mouse olha   roda zoom"),
		TEXT("          C câmera   M modo   L luzes   H auto rumo   J auto prof.   K auto alt.   segurar R reset"),
		TEXT("GAMEPAD   stick esq. sobe/gira   stick dir. anda/lateral   D-pad câmera   RB/LB zoom   Y câmera   X luzes"),
		TEXT("          Menu modo   L3 auto rumo   R3 auto prof.   segurar B reset"),
	};
	DrawPanel(Help, Margin, Canvas->ClipY - (Help.Num() * LineHeight + 2.f * Padding) - Margin, HintColor);

	// ---------------------------------------------------------------- retícula do cockpit
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
