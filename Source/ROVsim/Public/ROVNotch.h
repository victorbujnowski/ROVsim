#pragma once

#include "CoreMinimal.h"

/**
 * Filtro "notch" (rejeita-faixa) de 2ª ordem: apaga UMA frequência e deixa o resto passar.
 *
 * É o "filtro de ondas" (wave filter) do station keeping: o swell faz o ROV ir e voltar a cada
 * período da onda. Se o controle de posição visse esse movimento, ficaria invertendo os propulsores
 * a cada onda (gastando energia e hidráulico sem conseguir parar o ROV). O notch na frequência
 * da onda tira esse vaivém da medida; a deriva lenta (corrente) passa inteira e é corrigida.
 *
 * Fórmulas do "Audio EQ Cookbook" (R. Bristow-Johnson), recalculadas a cada passo porque o
 * tempo de frame varia. Usa double: com 120 FPS, 1 - cos(ω·dt) fica em ~1e-5 e o float perderia precisão.
 */
struct FROVNotchFilter
{
	/** Largura do notch: menor = mais largo (apaga mais frequências vizinhas, mas atrasa mais o sinal) */
	double Q = 1.0;

	/**
	 * Filtra uma amostra.
	 * @param In          valor medido agora
	 * @param OmegaRadS   frequência a apagar (rad/s). 0 = sem filtro (a saída é a entrada)
	 * @param DeltaTime   tempo desde a amostra anterior (s)
	 */
	double Process(double In, double OmegaRadS, double DeltaTime)
	{
		const double W0 = OmegaRadS * DeltaTime; // frequência em radianos por amostra
		if (!bStarted || OmegaRadS <= 0.0 || DeltaTime <= 0.0 || W0 >= PI)
		{
			// Sem filtro (ou primeira amostra): a memória fica igual à entrada, como se estivesse parado há tempo
			X1 = X2 = Y1 = Y2 = In;
			bStarted = true;
			return In;
		}

		const double CosW = FMath::Cos(W0);
		const double Alpha = FMath::Sin(W0) / (2.0 * Q);
		const double A0 = 1.0 + Alpha;
		// Numerador: zeros exatamente em ω0 (ganho zero lá). Denominador: polos perto, para o "buraco" ser estreito.
		const double Out = (In - 2.0 * CosW * X1 + X2 + 2.0 * CosW * Y1 - (1.0 - Alpha) * Y2) / A0;

		X2 = X1; X1 = In;
		Y2 = Y1; Y1 = Out;
		return Out;
	}

	void Reset() { bStarted = false; }

private:
	bool bStarted = false;
	double X1 = 0.0, X2 = 0.0; // entradas anteriores
	double Y1 = 0.0, Y2 = 0.0; // saídas anteriores
};
