// Fill out your copyright notice in the Description page of Project Settings.


#include "ComputeShaderTestComponent.h"

#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"
#include "Shader.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "UniformBuffer.h"
#include "RHICommandList.h"


// Some useful links
// -----------------
// [Enqueue render commands using lambdas](https://github.com/EpicGames/UnrealEngine/commit/41f6b93892dcf626a5acc155f7d71c756a5624b0)
//
// [Useful tutorial on Unreal compute shaders](https://github.com/Temaran/UE4ShaderPluginDemo)


IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FComputeShaderVariableParameters, "CSVariables");

class FBoidsComputeShader : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoidsComputeShader);
	SHADER_USE_PARAMETER_STRUCT(FBoidsComputeShader, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, dt)
		SHADER_PARAMETER(float, totalTime)
		SHADER_PARAMETER(float, boidSpeed)
		SHADER_PARAMETER(float, boidSpeedVariation)
		SHADER_PARAMETER(float, boidRotationSpeed)

		SHADER_PARAMETER(float, separationDistance)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float3>, positions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float3>, directions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, neigbhours)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, neighboursBaseIndex)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, neighboursCount)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FBoidsComputeShader, "/ComputeShaderPlugin/Boid.usf", "MainComputeShader", SF_Compute);

class FNeighboursComputeShader : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNeighboursComputeShader);
	SHADER_USE_PARAMETER_STRUCT(FNeighboursComputeShader, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, numBoids)
		SHADER_PARAMETER(uint32, numNeighbours)
		SHADER_PARAMETER(float, neighbourDistance)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float3>, positions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, neigbhours)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, neighboursBaseIndex)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, neighboursCount)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FNeighboursComputeShader, "/ComputeShaderPlugin/Neighbours.usf", "MainComputeShader", SF_Compute);


// Sets default values for this component's properties
UComputeShaderTestComponent::UComputeShaderTestComponent() 
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;

	// ...
}


// Called when the game starts
void UComputeShaderTestComponent::BeginPlay()
{
	Super::BeginPlay();

    FRHICommandListImmediate& RHICommands = GRHICommandList.GetImmediateCommandList();

	FRandomStream rng;

	// positions
	{
		TResourceArray<FVector> resourceArray;
		resourceArray.Init(FVector::ZeroVector, numBoids);


		for (FVector& position : resourceArray)
		{
			position = rng.GetUnitVector() * rng.GetFraction() * spawnRadius;
		}

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		const size_t size = sizeof(FVector);

		_positionBuffer = RHICreateStructuredBuffer(size, size * numBoids, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
		_positionBufferUAV = RHICreateUnorderedAccessView(_positionBuffer, false, false);
	}
    
	// directions
	{
		TResourceArray<FVector> resourceArray;
		resourceArray.Init(FVector::ZeroVector, numBoids);


		for (FVector& position : resourceArray)
		{
			position = rng.GetUnitVector();
		}

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		const size_t size = sizeof(FVector);

		_directionsBuffer = RHICreateStructuredBuffer(size, size * numBoids, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
		_directionsBufferUAV = RHICreateUnorderedAccessView(_directionsBuffer, false, false);
	}

	// neighbours
	{
		TResourceArray<uint32_t> resourceArray;
		resourceArray.Init(0, numBoids * numNeighbours);

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		const size_t size = sizeof(uint32_t);

		_neighboursBuffer = RHICreateStructuredBuffer(size, size * numBoids * numNeighbours, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
		_neighboursBufferUAV = RHICreateUnorderedAccessView(_neighboursBuffer, false, false);
	}

	// neighboursBaseIndex
	{
		TResourceArray<uint32_t> resourceArray;
		resourceArray.Init(0, numBoids);

		for (int i = 0; i < numBoids; ++i)
		{
			resourceArray[i] = i * numNeighbours;
		}

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		const size_t size = sizeof(uint32_t);

		_neighboursBaseIndex = RHICreateStructuredBuffer(size, size * numBoids, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
		_neighboursBaseIndexUAV = RHICreateUnorderedAccessView(_neighboursBaseIndex, false, false);
	}

	// neighboursCount
	{
		TResourceArray<uint32_t> resourceArray;
		resourceArray.Init(0, numBoids);

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		const size_t size = sizeof(uint32_t);

		_neighboursCount = RHICreateStructuredBuffer(size, size * numBoids, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
		_neighboursCountUAV = RHICreateUnorderedAccessView(_neighboursCount, false, false);
	}

	if (outputPositions.Num() != numBoids)
	{
		const FVector zero(0.0f);
		outputPositions.Init(zero, numBoids);
	}

	if (outputDirections.Num() != numBoids)
	{
		const FVector zero(0.0f);
		outputDirections.Init(zero, numBoids);
	}
}

// Called every frame
void UComputeShaderTestComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	float totalTime = GetOwner()->GetWorld()->TimeSeconds;

	FComputeShaderVariableParameters paramaters;
	paramaters.boidSpeed = 10.0f;
	paramaters.boidSpeedVariation = 1.0f;
	paramaters.rotationSpeed = 40.0f;
	paramaters.dt = DeltaTime;
	paramaters.totalTime = totalTime;
	paramaters.neighbourDistance = neighbourDistance;
	paramaters.separationDistance = separationDistance;
	paramaters.numNeighbours = numNeighbours;
	paramaters.numBoids = numBoids;

	ENQUEUE_RENDER_COMMAND(FComputeShaderRunner)(
	[&, paramaters, totalTime, DeltaTime](FRHICommandListImmediate& RHICommands)
	{
		// find our neighbours
		{
			//TShaderMapRef<FNeighboursUpdateComputeShaderDeclaration> neighbourCS(GetGlobalShaderMap(ERHIFeatureLevel::SM5));


			//FRHIComputeShader * rhiComputeShader = neighbourCS->GetComputeShader();

			//RHICommands.SetUAVParameter(rhiComputeShader, neighbourCS->positions.GetBaseIndex(), _positionBufferUAV);

			//RHICommands.SetUAVParameter(rhiComputeShader, neighbourCS->neigbhours.GetBaseIndex(), _neighboursBufferUAV);
			//RHICommands.SetUAVParameter(rhiComputeShader, neighbourCS->neighboursBaseIndex.GetBaseIndex(), _neighboursBaseIndexUAV);
			//RHICommands.SetUAVParameter(rhiComputeShader, neighbourCS->neighboursCount.GetBaseIndex(), _neighboursCountUAV);

			//auto variablesBuffer = TUniformBufferRef<FComputeShaderVariableParameters>::
			//	CreateUniformBufferImmediate(paramaters, UniformBuffer_SingleDraw);

			//auto variablesBufferParameter = neighbourCS->GetUniformBufferParameter<FComputeShaderVariableParameters>();

			//SetUniformBufferParameter(
			//	RHICommands,
			//	rhiComputeShader,
			//	variablesBufferParameter,
			//	variablesBuffer);


			//RHICommands.SetComputeShader(rhiComputeShader);

			//DispatchComputeShader(RHICommands, *neighbourCS, 256, 1, 1);




			RHICommands.TransitionResource(
				EResourceTransitionAccess::ERWBarrier,
				EResourceTransitionPipeline::EGfxToCompute, 
				_positionBufferUAV
			);
			
			FNeighboursComputeShader::FParameters parameters;
			parameters.numBoids = numBoids;
			parameters.numNeighbours = numNeighbours;
			parameters.neighbourDistance = neighbourDistance;

			parameters.positions = _positionBufferUAV;
			parameters.neigbhours = _neighboursBufferUAV;
			parameters.neighboursBaseIndex = _neighboursBaseIndexUAV;
			parameters.neighboursCount = _neighboursCountUAV;


			TShaderMapRef<FNeighboursComputeShader> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				RHICommands,
				*computeShader,
				parameters,
				FIntVector(256, 1, 1)
			);
		}

		// execute the main compute shader
		{
			RHICommands.TransitionResource(
				EResourceTransitionAccess::ERWBarrier,
				EResourceTransitionPipeline::EGfxToCompute,
				_positionBufferUAV
			);

			FBoidsComputeShader::FParameters parameters;
			parameters.dt = DeltaTime;
			parameters.totalTime = totalTime;
			parameters.separationDistance = separationDistance;
			parameters.boidSpeed = boidSpeed;
			parameters.boidSpeedVariation = boidSpeedVariation;
			parameters.separationDistance = separationDistance;
			parameters.boidRotationSpeed = boidRotationSpeed;

			parameters.positions = _positionBufferUAV;
			parameters.directions = _directionsBufferUAV;
			parameters.neigbhours = _neighboursBufferUAV;
			parameters.neighboursBaseIndex = _neighboursBaseIndexUAV;
			parameters.neighboursCount = _neighboursCountUAV;


			TShaderMapRef<FBoidsComputeShader> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				RHICommands,
				*computeShader,
				parameters,
				FIntVector(256, 1, 1)
			);

			/*TShaderMapRef<FComputeShaderDeclaration> mainCS(GetGlobalShaderMap(ERHIFeatureLevel::SM5));


			FRHIComputeShader * rhiComputeShader = mainCS->GetComputeShader();

			RHICommands.SetUAVParameter(rhiComputeShader, mainCS->positions.GetBaseIndex(), _positionBufferUAV);
			RHICommands.SetUAVParameter(rhiComputeShader, mainCS->directions.GetBaseIndex(), _directionsBufferUAV);

			RHICommands.SetUAVParameter(rhiComputeShader, mainCS->neigbhours.GetBaseIndex(), _neighboursBufferUAV);
			RHICommands.SetUAVParameter(rhiComputeShader, mainCS->neighboursBaseIndex.GetBaseIndex(), _neighboursBaseIndexUAV);
			RHICommands.SetUAVParameter(rhiComputeShader, mainCS->neighboursCount.GetBaseIndex(), _neighboursCountUAV);


			auto variablesBuffer = TUniformBufferRef<FComputeShaderVariableParameters>::
				CreateUniformBufferImmediate(paramaters, UniformBuffer_SingleDraw);

			auto variablesBufferParameter = mainCS->GetUniformBufferParameter<FComputeShaderVariableParameters>();

			SetUniformBufferParameter(
				RHICommands,
				rhiComputeShader,
				variablesBufferParameter,
				variablesBuffer);


			RHICommands.SetComputeShader(rhiComputeShader);

			DispatchComputeShader(RHICommands, *mainCS, 256, 1, 1);*/

			// read back the data
			// positions
			{
				uint8* data = (uint8*)RHILockStructuredBuffer(_positionBuffer, 0, numBoids * sizeof(FVector), RLM_ReadOnly);
				FMemory::Memcpy(outputPositions.GetData(), data, numBoids * sizeof(FVector));
				RHIUnlockStructuredBuffer(_positionBuffer);
			}

			// directions
			{
				uint8* data = (uint8*)RHILockStructuredBuffer(_directionsBuffer, 0, numBoids * sizeof(FVector), RLM_ReadOnly);
				FMemory::Memcpy(outputDirections.GetData(), data, numBoids * sizeof(FVector));
				RHIUnlockStructuredBuffer(_directionsBuffer);
			}
		}
	});
}



