// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "GlobalShader.h"
#include "UniformBuffer.h"
#include "RHICommandList.h"

#include <atomic>

#include "ComputeShaderTestComponent.generated.h"

UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class UNREALGPUSWARM_API UComputeShaderTestComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// Sets default values for this component's properties
	UComputeShaderTestComponent();

protected:
	// Called when the game starts
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;



public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int numBoids = 1000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int numNeighbours = 32;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float neighbourDistance = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float separationDistance = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float homeInnerRadius = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float boidSpeed = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float boidSpeedVariation = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float boidRotationSpeed = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float homeUrge = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float separationUrge = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float cohesionUrge = 0.01f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float alignmentUrge = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float spawnRadius = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FIntVector gridDimensions = FIntVector(256, 256, 256);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float gridCellSize = 5.0;

	TArray<FVector4> outputPositions;

	TArray<FVector4> outputDirections;

public:
	unsigned int dualBufferCount = 0;

	// GPU side
	FStructuredBufferRHIRef _positionBuffer[2];
	FUnorderedAccessViewRHIRef _positionBufferUAV[2];     // we need a UAV for writing

	FStructuredBufferRHIRef _directionsBuffer[2];
	FUnorderedAccessViewRHIRef _directionsBufferUAV[2];

	FStructuredBufferRHIRef _newDirectionsBuffer;
	FUnorderedAccessViewRHIRef _newDirectionsBufferUAV;

	FStructuredBufferRHIRef _neighboursBuffer;
	FUnorderedAccessViewRHIRef _neighboursBufferUAV;

	FStructuredBufferRHIRef _neighboursBaseIndex;
	FUnorderedAccessViewRHIRef _neighboursBaseIndexUAV;

	FStructuredBufferRHIRef _neighboursCount;
	FUnorderedAccessViewRHIRef _neighboursCountUAV;


	// Hashed grid data structures
	FStructuredBufferRHIRef _particleIndexBuffer;
	FUnorderedAccessViewRHIRef _particleIndexBufferUAV;

	FStructuredBufferRHIRef _cellIndexBuffer;
	FUnorderedAccessViewRHIRef _cellIndexBufferUAV;

	FStructuredBufferRHIRef _cellOffsetBuffer;
	FUnorderedAccessViewRHIRef _cellOffsetBufferUAV;
};
