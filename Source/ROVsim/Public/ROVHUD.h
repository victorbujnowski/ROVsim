#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ROVHUD.generated.h"

/**
 * HUD de telemetria do ROV, organizado em seções como o console de um piloto:
 *
 *   barra de estado (modo, câmera, luzes, quem está no controle)
 *   NAVEGAÇÃO        profundidade, altitude, rumo, pitch, roll
 *   VELOCIDADE       avanço, lateral, vertical, giro (em relação ao fundo)
 *   PROPULSORES      barras com o empuxo de cada eixo
 *   PILOTO AUTOMÁTICO alvos e desvios
 *   AMBIENTE         corrente, parte submersa, massa
 *
 * Cada linha tem nome, valor com unidade e uma explicação curta do que o número significa.
 */
UCLASS()
class ROVSIM_API AROVHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	/** Uma linha de uma seção: "Profundidade | 37.7 m | abaixo da superfície" */
	struct FHudRow
	{
		FString Label;
		FString Value;
		FString Hint;
		FLinearColor ValueColor = FLinearColor::White;
	};

	/** Uma barra de propulsor: nome e valor de -100 a +100 (%) */
	struct FHudBar
	{
		FString Label;
		float Percent = 0.f;
	};

	/** Desenha uma seção com título e linhas alinhadas em colunas. Devolve a altura usada. */
	float DrawSection(const FString& Title, const TArray<FHudRow>& Rows, float X, float Y, bool bAlignRight, const FLinearColor& Accent, float MinWidth = 0.f);

	/** Desenha barras centradas no zero (esquerda = negativo, direita = positivo). Devolve a altura usada. */
	float DrawBars(const FString& Title, const FString& Subtitle, const TArray<FHudBar>& Bars, float RightEdge, float Y, const FLinearColor& Accent);

	/** Barra de estado do topo: pares "NOME valor". Devolve a altura usada. */
	float DrawStatusBar(const TArray<TPair<FString, FString>>& Items, float X, float Y, const FLinearColor& Accent);

	/** Painel simples de texto (ajuda de controles) */
	void DrawPanel(const TArray<FString>& Lines, float X, float Y, const FLinearColor& TextColor);
};
