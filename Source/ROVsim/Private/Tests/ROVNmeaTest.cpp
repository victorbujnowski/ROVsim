#include "ROVNmea.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FROVNmeaTest, "ROVsim.NMEA",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FROVNmeaTest::RunTest(const FString& Parameters)
{
	// Vetor de referência conferido à mão: XOR de "HEHDT,45.0,T" = 0x1E
	TestEqual(TEXT("Checksum de HEHDT,45.0,T"), static_cast<int32>(ROVNmea::Checksum(TEXT("HEHDT,45.0,T"))), 0x1E);
	TestEqual(TEXT("Frase completa"), ROVNmea::Sentence(TEXT("HEHDT,45.0,T")), FString(TEXT("$HEHDT,45.0,T*1E\r\n")));

	// Validação de frases recebidas
	TestTrue(TEXT("Frase certa é válida"), ROVNmea::IsValid(TEXT("$HEHDT,45.0,T*1E\r\n")));
	TestTrue(TEXT("Checksum em minúsculas é aceito"), ROVNmea::IsValid(TEXT("$HEHDT,45.0,T*1e")));
	TestFalse(TEXT("Checksum errado é recusado"), ROVNmea::IsValid(TEXT("$HEHDT,45.0,T*1F")));
	TestFalse(TEXT("Um dígito trocado no dado é detectado"), ROVNmea::IsValid(TEXT("$HEHDT,46.0,T*1E")));
	TestFalse(TEXT("Sem '$' é recusado"), ROVNmea::IsValid(TEXT("HEHDT,45.0,T*1E")));
	TestFalse(TEXT("Sem checksum é recusado"), ROVNmea::IsValid(TEXT("$HEHDT,45.0,T")));

	// Toda frase gerada tem que passar na própria validação
	TestTrue(TEXT("Frase gerada é válida"), ROVNmea::IsValid(ROVNmea::Sentence(TEXT("VDVBW,1.20,-0.35,A,1.10,-0.30,A"))));

	return true;
}

#endif
