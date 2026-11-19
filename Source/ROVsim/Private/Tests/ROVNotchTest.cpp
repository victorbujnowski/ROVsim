#include "ROVNotch.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Passa uma senoide (+ um valor constante) pelo notch e devolve a maior variação na saída depois de assentar */
	double FilteredSwing(double SignalOmega, double NotchOmega, double Offset)
	{
		FROVNotchFilter Filter;
		const double Dt = 1.0 / 120.0; // 120 FPS
		double Min = TNumericLimits<double>::Max(), Max = TNumericLimits<double>::Lowest();
		for (double Time = 0.0; Time < 400.0; Time += Dt) // 400 s: cabem 2 períodos inteiros da deriva de 120 s depois do transitório
		{
			const double Out = Filter.Process(Offset + FMath::Sin(SignalOmega * Time), NotchOmega, Dt);
			if (Time > 160.0) // ignora o transitório inicial
			{
				Min = FMath::Min(Min, Out);
				Max = FMath::Max(Max, Out);
			}
		}
		return 0.5 * (Max - Min); // amplitude que sobrou (a da entrada é 1)
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FROVNotchTest, "ROVsim.Notch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FROVNotchTest::RunTest(const FString& Parameters)
{
	const double Wave = 2.0 * PI / 10.0; // swell de 10 s

	// 1) A frequência da onda é apagada (sobra menos de 2%)
	TestTrue(TEXT("Onda de 10 s apagada"), FilteredSwing(Wave, Wave, 0.0) < 0.02);

	// 2) Movimento lento (deriva, período de 2 min) passa quase inteiro
	TestTrue(TEXT("Deriva lenta passa"), FilteredSwing(2.0 * PI / 120.0, Wave, 0.0) > 0.95);

	// 3) Um valor constante (ROV parado fora do alvo) passa sem mudar
	{
		FROVNotchFilter Filter;
		double Out = 0.0;
		for (int32 i = 0; i < 2000; ++i)
		{
			Out = Filter.Process(5.0, Wave, 1.0 / 60.0);
		}
		TestTrue(TEXT("Constante passa sem mudar"), FMath::Abs(Out - 5.0) < 1e-6);
	}

	// 4) Frequência 0 = sem filtro
	{
		FROVNotchFilter Filter;
		Filter.Process(1.0, 0.0, 0.01);
		TestEqual(TEXT("Sem filtro devolve a entrada"), Filter.Process(3.0, 0.0, 0.01), 3.0);
	}
	return true;
}

#endif
