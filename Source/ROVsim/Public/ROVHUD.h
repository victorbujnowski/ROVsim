#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ROVHUD.generated.h"

/**
 * HUD de telemetria do ROV: modo, câmera, profundidade, altitude, rumo,
 * atitude, velocidades, propulsores e a ajuda de controles.
 */
UCLASS()
class ROVSIM_API AROVHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawPanel(const TArray<FString>& Lines, float X, float Y, const FLinearColor& TextColor);
};
