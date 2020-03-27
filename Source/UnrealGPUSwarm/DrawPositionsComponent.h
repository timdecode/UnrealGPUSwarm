// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DrawPositionsComponent.generated.h"


UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class UNREALGPUSWARM_API UDrawPositionsComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	// Sets default values for this component's properties
	UDrawPositionsComponent();

protected:
	// Called when the game starts
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	void _initISMC();
	void _updateInstanceBuffers();
	void _updateInstanceTransforms();

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float size = 0.02f;

protected:
	TArray<FTransform> _instanceTransforms;

	FUnorderedAccessViewRHIRef _positionsUAV; // float4
	FUnorderedAccessViewRHIRef _transformsUAV; // float4x4
};
