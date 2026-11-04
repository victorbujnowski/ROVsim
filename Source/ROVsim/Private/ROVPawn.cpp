// Fill out your copyright notice in the Description page of Project Settings.


#include "ROVPawn.h"

// Sets default values
AROVPawn::AROVPawn()
{
 	// Set this pawn to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

}

// Called when the game starts or when spawned
void AROVPawn::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void AROVPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

// Called to bind functionality to input
void AROVPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

}

