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

#include "GPUBitonicSort.h"


// Some useful links
// -----------------
// [Enqueue render commands using lambdas](https://github.com/EpicGames/UnrealEngine/commit/41f6b93892dcf626a5acc155f7d71c756a5624b0)
//
// [Useful tutorial on Unreal compute shaders](https://github.com/Temaran/UE4ShaderPluginDemo)


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
		SHADER_PARAMETER(float, homeInnerRadius)
		SHADER_PARAMETER(float, separationDistance)
		SHADER_PARAMETER(float, neighbourhoodDistance)

		SHADER_PARAMETER(float, homeUrge)
		SHADER_PARAMETER(float, separationUrge)
		SHADER_PARAMETER(float, cohesionUrge)
		SHADER_PARAMETER(float, alignmentUrge)


		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, positions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, directions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float3>, newDirections)


		SHADER_PARAMETER(uint32, numParticles)
		SHADER_PARAMETER(float, cellSizeReciprocal)
		SHADER_PARAMETER(uint32, cellOffsetBufferSize)
		SHADER_PARAMETER(FIntVector, gridDimensions)

		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, particleIndexBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, cellIndexBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, cellOffsetBuffer)
		
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FBoidsComputeShader, "/ComputeShaderPlugin/Boid.usf", "GridNeighboursBoidUpdate", SF_Compute);


class FBoids_integratePosition_CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoids_integratePosition_CS);
	SHADER_USE_PARAMETER_STRUCT(FBoids_integratePosition_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, dt)
		SHADER_PARAMETER(float, totalTime)
		SHADER_PARAMETER(float, boidSpeed)
		SHADER_PARAMETER(float, boidSpeedVariation)
		SHADER_PARAMETER(float, boidRotationSpeed)
		SHADER_PARAMETER(uint32, numParticles)

		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, positions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, directions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float3>, newDirections)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FBoids_integratePosition_CS, "/ComputeShaderPlugin/Boid.usf", "IntegrateBoidPosition", SF_Compute);






class FBoids_rearrangePositions_CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoids_rearrangePositions_CS);
	SHADER_USE_PARAMETER_STRUCT(FBoids_rearrangePositions_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, numParticles)

		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, positions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, directions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, positions_other)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, directions_other)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float3>, particleIndexBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FBoids_rearrangePositions_CS, "/ComputeShaderPlugin/Boid.usf", "rearrangePositions", SF_Compute);





class FHashedGrid_createUnsortedList_CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHashedGrid_createUnsortedList_CS);
	SHADER_USE_PARAMETER_STRUCT(FHashedGrid_createUnsortedList_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, numParticles)
		SHADER_PARAMETER(float, cellSizeReciprocal)
		SHADER_PARAMETER(uint32, cellOffsetBufferSize)
		SHADER_PARAMETER(FIntVector, gridDimensions)

		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, positions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, particleIndexBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, cellIndexBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHashedGrid_createUnsortedList_CS, "/ComputeShaderPlugin/HashedGrid.usf", "createUnsortedList", SF_Compute);




class FHashedGrid_createOffsetList_CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHashedGrid_createOffsetList_CS);
	SHADER_USE_PARAMETER_STRUCT(FHashedGrid_createOffsetList_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, numParticles)
		SHADER_PARAMETER(float, cellSizeReciprocal)
		SHADER_PARAMETER(uint32, cellOffsetBufferSize)
		SHADER_PARAMETER(FIntVector, gridDimensions)

		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, particleIndexBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, cellIndexBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, cellOffsetBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHashedGrid_createOffsetList_CS, "/ComputeShaderPlugin/HashedGrid.usf", "createOffsetList", SF_Compute);

class FHashedGrid_resetCellOffsetBuffer_CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHashedGrid_resetCellOffsetBuffer_CS);
	SHADER_USE_PARAMETER_STRUCT(FHashedGrid_resetCellOffsetBuffer_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, cellOffsetBufferSize)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, cellOffsetBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHashedGrid_resetCellOffsetBuffer_CS, "/ComputeShaderPlugin/HashedGrid.usf", "resetCellOffsetBuffer", SF_Compute);





class FNeighboursComputeShader : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNeighboursComputeShader);
	SHADER_USE_PARAMETER_STRUCT(FNeighboursComputeShader, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, numNeighbours)
		SHADER_PARAMETER(float, neighbourDistance)

		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, positions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, neigbhours)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, neighboursBaseIndex)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, neighboursCount)

		SHADER_PARAMETER(uint32, numParticles)
		SHADER_PARAMETER(uint32, cellOffsetBufferSize)
		SHADER_PARAMETER(FIntVector, gridDimensions)
		SHADER_PARAMETER(float, cellSizeReciprocal)

		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, particleIndexBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, cellIndexBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, cellOffsetBuffer)

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
		TResourceArray<FVector4> resourceArray;
		const FVector4 zero(0.0f);
		resourceArray.Init(zero, numBoids);



		for (FVector4& position : resourceArray)
		{
			position = rng.GetUnitVector() * rng.GetFraction() * spawnRadius;
		}

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		const size_t size = sizeof(FVector4);

		for( int i = 0; i < 2; ++i )
		{
			_positionBuffer[i] = RHICreateStructuredBuffer(size, size * numBoids, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
			_positionBufferUAV[i] = RHICreateUnorderedAccessView(_positionBuffer[i], false, false);
		}
	}
    
	// directions
	{
		TResourceArray<FVector4> resourceArray;
		const FVector4 zero(0.0f);

		resourceArray.Init(zero, numBoids);


		for (FVector4& position : resourceArray)
		{
			position = rng.GetUnitVector();
		}

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		const size_t size = sizeof(FVector4);

		for( int i = 0; i < 2; ++i )
		{
			_directionsBuffer[i] = RHICreateStructuredBuffer(size, size * numBoids, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
			_directionsBufferUAV[i] = RHICreateUnorderedAccessView(_directionsBuffer[i], false, false);
		}


		_newDirectionsBuffer = RHICreateStructuredBuffer(size, size * numBoids, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
		_newDirectionsBufferUAV = RHICreateUnorderedAccessView(_newDirectionsBuffer, false, false);
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

	// particleIndexBuffer
	{
		TResourceArray<uint32_t> resourceArray;
		resourceArray.Init(0, numBoids);

		for( int i = 0; i < numBoids; ++i )
			resourceArray[i] = i;

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		const size_t size = sizeof(uint32_t);

		_particleIndexBuffer = RHICreateStructuredBuffer(size, size * numBoids, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
		_particleIndexBufferUAV = RHICreateUnorderedAccessView(_particleIndexBuffer, false, false);
	}

	// cellIndexBuffer
	{
		TResourceArray<uint32_t> resourceArray;
		resourceArray.Init(0, numBoids);

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		const size_t size = sizeof(uint32_t);

		_cellIndexBuffer = RHICreateStructuredBuffer(size, size * numBoids, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
		_cellIndexBufferUAV = RHICreateUnorderedAccessView(_cellIndexBuffer, false, false);
	}
	
	// cellOffsetBuffer
	{
		const size_t size = sizeof(uint32_t);
		const size_t gridSize = gridDimensions.X * gridDimensions.Y * gridDimensions.Z;

		TResourceArray<uint32_t> resourceArray;
		resourceArray.Init(0, gridSize);

		FRHIResourceCreateInfo createInfo;
		createInfo.ResourceArray = &resourceArray;

		_cellOffsetBuffer = RHICreateStructuredBuffer(size, gridSize * size, BUF_UnorderedAccess | BUF_ShaderResource, createInfo);
		_cellOffsetBufferUAV = RHICreateUnorderedAccessView(_cellOffsetBuffer, false, false);
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

static FIntVector groupSize(int numElements)
{
	const int threadCount = 256;

	int count = ((numElements - 1) / threadCount) + 1;

	return FIntVector(count, 1, 1);
}

// Called every frame
void UComputeShaderTestComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	float totalTime = GetOwner()->GetWorld()->TimeSeconds;
	const float dt = FMath::Min(1.0f / 60.0f, DeltaTime);

	ENQUEUE_RENDER_COMMAND(FComputeShaderRunner)(
	[&, totalTime, dt](FRHICommandListImmediate& RHICommands)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_UComputeShaderTestComponent_TickComponent);

		const uint32_t cellOffsetBufferSize = gridDimensions.X * gridDimensions.Y * gridDimensions.Z;

		auto& positionsBufferUAV = _positionBufferUAV[dualBufferCount];
		auto& directionsBufferUAV = _directionsBufferUAV[dualBufferCount];

		// calculate the unsorted cell index buffer
		{
			FHashedGrid_createUnsortedList_CS::FParameters parameters;
			parameters.numParticles = numBoids;
			parameters.cellSizeReciprocal = 1.0f / gridCellSize;
			parameters.cellOffsetBufferSize = cellOffsetBufferSize;
			parameters.gridDimensions = gridDimensions;
			parameters.positions = positionsBufferUAV;
			parameters.particleIndexBuffer = _particleIndexBufferUAV;
			parameters.cellIndexBuffer = _cellIndexBufferUAV;


			TShaderMapRef<FHashedGrid_createUnsortedList_CS> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				RHICommands,
				*computeShader,
				parameters,
				groupSize(numBoids)
			);


			RHICommands.TransitionResource(
				EResourceTransitionAccess::ERWBarrier,
				EResourceTransitionPipeline::EGfxToCompute,
				_cellIndexBufferUAV
			);
		}

		// sort the cell index buffer
		{
		 	FGPUBitonicSort gpuBitonicSort;

		 	gpuBitonicSort.sort(
				numBoids,
				numBoids,
				_cellIndexBufferUAV,
				_particleIndexBufferUAV,
				RHICommands
			);

			RHICommands.TransitionResource(
				EResourceTransitionAccess::ERWBarrier,
				EResourceTransitionPipeline::EGfxToCompute,
				_particleIndexBufferUAV
			);


		}

		// reset the cell offset buffer
		{
			FHashedGrid_resetCellOffsetBuffer_CS::FParameters parameters;
			parameters.cellOffsetBufferSize = cellOffsetBufferSize;
			parameters.cellOffsetBuffer = _cellOffsetBufferUAV;

			TShaderMapRef<FHashedGrid_resetCellOffsetBuffer_CS> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				RHICommands,
				*computeShader,
				parameters,
				groupSize(cellOffsetBufferSize)
			);

		}

		// build the cell offset buffer
		{
			FHashedGrid_createOffsetList_CS::FParameters parameters;
			parameters.numParticles = numBoids;
			parameters.cellSizeReciprocal = 1.0f / gridCellSize;
			parameters.cellOffsetBufferSize = cellOffsetBufferSize;
			parameters.gridDimensions = gridDimensions;

			parameters.particleIndexBuffer = _particleIndexBufferUAV;
			parameters.cellIndexBuffer = _cellIndexBufferUAV;
			parameters.cellOffsetBuffer = _cellOffsetBufferUAV;


			TShaderMapRef<FHashedGrid_createOffsetList_CS> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				RHICommands,
				*computeShader,
				parameters,
				groupSize(numBoids)
			);


		}

		// find our neighbours
		{

			
			// FNeighboursComputeShader::FParameters parameters;
			// parameters.numNeighbours = numNeighbours;
			// parameters.neighbourDistance = neighbourDistance;

			// parameters.neigbhours = _neighboursBufferUAV;
			// parameters.neighboursBaseIndex = _neighboursBaseIndexUAV;
			// parameters.neighboursCount = _neighboursCountUAV;

			// parameters.numParticles = numBoids;
			// parameters.cellSizeReciprocal = 1.0f / gridCellSize;
			// parameters.cellOffsetBufferSize = cellOffsetBufferSize;
			// parameters.gridDimensions = gridDimensions;

			// parameters.positions = positionBufferUAV;
			// parameters.cellOffsetBuffer = _cellOffsetBufferUAV;
			// parameters.cellIndexBuffer = _cellIndexBufferUAV;
			// parameters.particleIndexBuffer = _particleIndexBufferUAV;


			// TShaderMapRef<FNeighboursComputeShader> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			// FComputeShaderUtils::Dispatch(
			// 	RHICommands,
			// 	*computeShader,
			// 	parameters,
			// 	groupSize(numBoids)
			// );
		}

		if (false)
		{
			TArray<uint32> cellIndexBuffer;
			cellIndexBuffer.Init(0, numBoids);

			TArray<uint32> particleIndexBuffer;
			particleIndexBuffer.Init(0, numBoids);

			TArray<uint32> cellOffsetBuffer;
			cellOffsetBuffer.Init(0, cellOffsetBufferSize);

			uint8* cellIndexData = (uint8*)RHILockStructuredBuffer(_cellIndexBuffer, 0, numBoids * sizeof(uint32_t), RLM_ReadOnly);
			FMemory::Memcpy(cellIndexBuffer.GetData(), cellIndexData, numBoids * sizeof(uint32_t));
			RHIUnlockStructuredBuffer(_cellIndexBuffer);

			uint8* particleIndexData = (uint8*)RHILockStructuredBuffer(_particleIndexBuffer, 0, numBoids * sizeof(uint32_t), RLM_ReadOnly);
			FMemory::Memcpy(particleIndexBuffer.GetData(), particleIndexData, numBoids * sizeof(uint32_t));
			RHIUnlockStructuredBuffer(_particleIndexBuffer);

			uint8* cellOffsetData = (uint8*)RHILockStructuredBuffer(_cellOffsetBuffer, 0, cellOffsetBufferSize * sizeof(uint32_t), RLM_ReadOnly);
			FMemory::Memcpy(cellOffsetBuffer.GetData(), cellOffsetData, cellOffsetBufferSize * sizeof(uint32_t));
			RHIUnlockStructuredBuffer(_cellOffsetBuffer);
		}


		// execute the main compute shader
		{
			FBoidsComputeShader::FParameters parameters;
			parameters.dt = dt;
			parameters.totalTime = totalTime;
			parameters.separationDistance = separationDistance;
			parameters.boidSpeed = boidSpeed;
			parameters.boidSpeedVariation = boidSpeedVariation;
			parameters.separationDistance = separationDistance;
			parameters.boidRotationSpeed = boidRotationSpeed;
			parameters.homeInnerRadius = homeInnerRadius;
			parameters.neighbourhoodDistance = neighbourDistance;

			parameters.homeUrge = homeUrge;
			parameters.separationUrge = separationUrge;
			parameters.cohesionUrge = cohesionUrge;
			parameters.alignmentUrge = alignmentUrge;

			parameters.numParticles = numBoids;
			parameters.cellSizeReciprocal = 1.0f / gridCellSize;
			parameters.cellOffsetBufferSize = cellOffsetBufferSize;
			parameters.gridDimensions = gridDimensions;

			parameters.positions = positionsBufferUAV;
			parameters.directions = directionsBufferUAV;
			parameters.newDirections = _newDirectionsBufferUAV;
			parameters.cellOffsetBuffer = _cellOffsetBufferUAV;
			parameters.cellIndexBuffer = _cellIndexBufferUAV;
			parameters.particleIndexBuffer = _particleIndexBufferUAV;


			TShaderMapRef<FBoidsComputeShader> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				RHICommands,
				*computeShader,
				parameters,
				groupSize(numBoids)
			);
		}

		// integrate positions
		{
			FBoids_integratePosition_CS::FParameters parameters;
			parameters.dt = dt;
			parameters.totalTime = totalTime;
			parameters.boidSpeed = boidSpeed;
			parameters.boidSpeedVariation = boidSpeedVariation;

			parameters.numParticles = numBoids;

			parameters.positions = positionsBufferUAV;
			parameters.directions = directionsBufferUAV;
			parameters.newDirections = _newDirectionsBufferUAV;

			TShaderMapRef<FBoids_integratePosition_CS> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				RHICommands,
				*computeShader,
				parameters,
				groupSize(numBoids)
			);
		}

		// rearrange positions for better cache-coherence on the next run
		{
			FBoids_rearrangePositions_CS::FParameters parameters;

			parameters.positions = positionsBufferUAV;
			parameters.directions = directionsBufferUAV;

			parameters.positions_other = _positionBufferUAV[(dualBufferCount + 1) % 2];
			parameters.directions_other = _directionsBufferUAV[(dualBufferCount + 1) % 2];

			parameters.particleIndexBuffer = _particleIndexBufferUAV;
			parameters.numParticles = numBoids;

			TShaderMapRef<FBoids_rearrangePositions_CS> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				RHICommands,
				*computeShader,
				parameters,
				groupSize(numBoids)
			); 

			// read back the data
			// positions
			{
				uint8* data = (uint8*)RHILockStructuredBuffer(_positionBuffer[dualBufferCount], 0, numBoids * sizeof(FVector4), RLM_ReadOnly);
				FMemory::Memcpy(outputPositions.GetData(), data, numBoids * sizeof(FVector4));
				RHIUnlockStructuredBuffer(_positionBuffer[dualBufferCount]);
			}

			// directions
			{
				uint8* data = (uint8*)RHILockStructuredBuffer(_directionsBuffer[dualBufferCount], 0, numBoids * sizeof(FVector4), RLM_ReadOnly);
				FMemory::Memcpy(outputDirections.GetData(), data, numBoids * sizeof(FVector4));
				RHIUnlockStructuredBuffer(_directionsBuffer[dualBufferCount]);
			}

			// rotate our buffers
			dualBufferCount = (dualBufferCount + 1) % 2;
		}
	});
}



