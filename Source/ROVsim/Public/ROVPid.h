#pragma once

#include "CoreMinimal.h"

/**
 * Controlador PID (Proporcional, Integral, Derivativo) para os pilotos automáticos.
 *
 * Struct C++ simples (sem UObject): fácil de testar isoladamente com números.
 * A saída fica entre -OutputLimit e +OutputLimit, a mesma escala do comando dos propulsores (-1 a 1).
 */
struct FROVPid
{
	float Kp = 0.f;
	float Ki = 0.f;
	float Kd = 0.f;
	float OutputLimit = 1.f;   // saturação: o propulsor não passa de 100%
	float IntegralLimit = 1.f; // anti-windup: o termo I sozinho não passa deste valor

	FROVPid() = default;
	FROVPid(float InKp, float InKi, float InKd)
		: Kp(InKp), Ki(InKi), Kd(InKd)
	{
	}

	/**
	 * Calcula a saída do controlador.
	 * @param Error         alvo - medido (ex.: 5 graus à direita do rumo desejado)
	 * @param MeasuredRate  taxa da grandeza medida (ex.: velocidade de giro), usada pelo termo D
	 * @param DeltaTime     tempo desde a última chamada, em segundos
	 */
	float Update(float Error, float MeasuredRate, float DeltaTime)
	{
		if (DeltaTime <= 0.f)
		{
			return 0.f;
		}

		// P: reage ao erro de agora
		const float P = Kp * Error;

		// I: acumula o erro no tempo; elimina o erro que sobra quando há uma força constante (corrente)
		Integral += Error * DeltaTime;
		if (Ki > 0.f)
		{
			Integral = FMath::Clamp(Integral, -IntegralLimit / Ki, IntegralLimit / Ki); // anti-windup
		}
		const float I = Ki * Integral;

		// D: freia pela taxa MEDIDA (e não pela derivada do erro): sem "chute" quando o alvo muda
		const float D = -Kd * MeasuredRate;

		return FMath::Clamp(P + I + D, -OutputLimit, OutputLimit);
	}

	/** Zera a memória (chamar ao ligar o piloto automático ou trocar de alvo) */
	void Reset()
	{
		Integral = 0.f;
	}

private:
	float Integral = 0.f;
};