#include "ROVPid.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Simula 60 s de um corpo 1D (massa 1, arrasto 0,5) empurrado por uma força constante de 0,2
	 * (o papel da corrente). O PID tenta levar a posição de 0 até 1. Devolve o erro final.
	 */
	float SimulateAndGetFinalError(FROVPid Pid)
	{
		const float Dt = 0.01f;
		const float Target = 1.f;
		const float Disturbance = 0.2f;
		float Position = 0.f;
		float Velocity = 0.f;

		for (float Time = 0.f; Time < 60.f; Time += Dt)
		{
			const float Command = Pid.Update(Target - Position, Velocity, Dt); // -1 a 1
			const float Acceleration = Command - 0.5f * Velocity - Disturbance;
			Velocity += Acceleration * Dt;
			Position += Velocity * Dt;
		}
		return Target - Position;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FROVPidTest, "ROVsim.PID",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FROVPidTest::RunTest(const FString& Parameters)
{
	// 1) Saturação: erro enorme não pode passar de 100% de propulsor
	{
		FROVPid Pid(10.f, 0.f, 0.f);
		TestEqual(TEXT("Saída limitada a +1"), Pid.Update(5.f, 0.f, 0.1f), 1.f);
		TestEqual(TEXT("Saída limitada a -1"), Pid.Update(-5.f, 0.f, 0.1f), -1.f);
	}

	// 2) Só o P: a força constante deixa um erro que nunca some (esperado: 0,2 / Kp = 0,2)
	const float ErrorP = SimulateAndGetFinalError(FROVPid(1.f, 0.f, 1.5f));
	TestTrue(FString::Printf(TEXT("Só P deixa erro em regime permanente (%.3f)"), ErrorP), ErrorP > 0.15f);

	// 3) Com o I: o erro vai a zero mesmo com a força constante
	const float ErrorPid = SimulateAndGetFinalError(FROVPid(1.f, 0.2f, 1.5f));
	TestTrue(FString::Printf(TEXT("PID elimina o erro (%.4f)"), ErrorPid), FMath::Abs(ErrorPid) < 0.01f);

	return true;
}

#endif