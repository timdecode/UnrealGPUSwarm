// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "GlobalShader.h"
#include "UniformBuffer.h"
#include "RHICommandList.h"

#include <atomic>

#include "ComputeShaderTestComponent.generated.h"


BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FComputeShaderVariableParameters, )
SHADER_PARAMETER(float, boidSpeed)
SHADER_PARAMETER(float, boidSpeedVariation)
SHADER_PARAMETER(float, rotationSpeed)
SHADER_PARAMETER(float, dt)
SHADER_PARAMETER(float, totalTime)
SHADER_PARAMETER(float, separationDistance)
SHADER_PARAMETER(float, neighbourDistance)
SHADER_PARAMETER(int, numNeighbours)
SHADER_PARAMETER(int, numBoids)
END_GLOBAL_SHADER_PARAMETER_STRUCT()


//class FComputeShaderDeclaration : public FGlobalShader
//{
//	DECLARE_SHADER_TYPE(FComputeShaderDeclaration, Global);
//
//	FComputeShaderDeclaration() {}
//
//	explicit FComputeShaderDeclaration(const ShaderMetaType::CompiledShaderInitializerType& Initializer);
//
//	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
//		return GetMaxSupportedFeatureLevel(Parameters.Platform) >= ERHIFeatureLevel::SM5;
//	};
//
//	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
//
//	virtual bool Serialize(FArchive& Ar) override
//	{
//		bool bShaderHasOutdatedParams = FGlobalShader::Serialize(Ar);
//
//		Ar << positions;
//		Ar << directions;
//
//		Ar << neigbhours;
//		Ar << neighboursBaseIndex;
//		Ar << neighboursCount;
//
//		return bShaderHasOutdatedParams;
//	}
//
//public:
//	FShaderResourceParameter positions;
//	FShaderResourceParameter directions;
//
//	FShaderResourceParameter neigbhours;
//	FShaderResourceParameter neighboursBaseIndex;
//	FShaderResourceParameter neighboursCount;
//};
//
//class FNeighboursUpdateComputeShaderDeclaration : public FGlobalShader
//{
//	DECLARE_SHADER_TYPE(FNeighboursUpdateComputeShaderDeclaration, Global);
//
//	FNeighboursUpdateComputeShaderDeclaration() {}
//
//	explicit FNeighboursUpdateComputeShaderDeclaration(const ShaderMetaType::CompiledShaderInitializerType& Initializer);
//
//	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
//		return GetMaxSupportedFeatureLevel(Parameters.Platform) >= ERHIFeatureLevel::SM5;
//	};
//
//	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
//
//	virtual bool Serialize(FArchive& Ar) override
//	{
//		bool bShaderHasOutdatedParams = FGlobalShader::Serialize(Ar);
//
//		Ar << positions;
//
//		Ar << neigbhours;
//		Ar << neighboursBaseIndex;
//		Ar << neighboursCount;
//
//		return bShaderHasOutdatedParams;
//	}
//
//public:
//	FShaderResourceParameter positions;
//
//	FShaderResourceParameter neigbhours;
//	FShaderResourceParameter neighboursBaseIndex;
//	FShaderResourceParameter neighboursCount;
//};

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
	float boidSpeed = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float boidSpeedVariation = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float boidRotationSpeed = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float spawnRadius = 600.0f;

	TArray<FVector> outputPositions;

	TArray<FVector> outputDirections;

protected:
	// GPU side
	FStructuredBufferRHIRef _positionBuffer;
	FUnorderedAccessViewRHIRef _positionBufferUAV;     // we need a UAV for writing

	FStructuredBufferRHIRef _directionsBuffer;
	FUnorderedAccessViewRHIRef _directionsBufferUAV;


	FStructuredBufferRHIRef _neighboursBuffer;
	FUnorderedAccessViewRHIRef _neighboursBufferUAV;

	FStructuredBufferRHIRef _neighboursBaseIndex;
	FUnorderedAccessViewRHIRef _neighboursBaseIndexUAV;

	FStructuredBufferRHIRef _neighboursCount;
	FUnorderedAccessViewRHIRef _neighboursCountUAV;
};
