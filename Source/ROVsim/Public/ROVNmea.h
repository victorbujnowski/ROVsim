#pragma once

#include "CoreMinimal.h"

/**
 * Funções do protocolo NMEA 0183, o formato de texto usado por sensores de navegação.
 *
 * Uma frase:   $HEHDT,45.0,T*1E\r\n
 *              |  |            | +-- fim de linha: CR LF
 *              |  |            +---- checksum: XOR de todos os caracteres entre '$' e '*', em hexadecimal
 *              |  +----------------- tipo da frase (HDT = rumo verdadeiro) e campos separados por vírgula
 *              +-------------------- "talker": quem fala (HE = girobússola). Frases que começam com P são proprietárias
 */
namespace ROVNmea
{
	/** XOR de todos os caracteres do corpo (o texto entre '$' e '*') */
	inline uint8 Checksum(const FString& Body)
	{
		uint8 Sum = 0;
		for (const TCHAR Character : Body)
		{
			Sum ^= static_cast<uint8>(Character);
		}
		return Sum;
	}

	/** Monta a frase completa a partir do corpo: "$" + corpo + "*" + checksum + CR LF */
	inline FString Sentence(const FString& Body)
	{
		return FString::Printf(TEXT("$%s*%02X\r\n"), *Body, Checksum(Body));
	}

	/** Confere se uma frase recebida está bem formada e com o checksum certo */
	inline bool IsValid(const FString& Line)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		int32 StarIndex = INDEX_NONE;
		if (!Trimmed.StartsWith(TEXT("$")) || !Trimmed.FindLastChar(TEXT('*'), StarIndex) || StarIndex + 3 != Trimmed.Len())
		{
			return false;
		}
		const FString Body = Trimmed.Mid(1, StarIndex - 1);
		const FString Hex = Trimmed.Mid(StarIndex + 1, 2);
		return FString::Printf(TEXT("%02X"), Checksum(Body)) == Hex.ToUpper();
	}
}
