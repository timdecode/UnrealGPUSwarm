// Fill out your copyright notice in the Description page of Project Settings.


#include "ComputeShaderTestComponent.h"

#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"

// Some useful links
// -----------------
// [Enqueue render commands using lambdas](https://github.com/EpicGames/UnrealEngine/commit/41f6b93892dcf626a5acc155f7d71c756a5624b0)
//

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FComputeShaderVariableParameters, "CSVariables");


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
}

// Called every frame
void UComputeShaderTestComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);



	ENQUEUE_RENDER_COMMAND(FComputeShaderRunner)(
	[&](FRHICommandListImmediate& RHICommands)
	{
		TShaderMapRef<FComputeShaderDeclaration> cs(GetGlobalShaderMap(ERHIFeatureLevel::SM5));

		FRHIComputeShader * rhiComputeShader = cs->GetComputeShader();

		RHICommands.SetUAVParameter(rhiComputeShader, cs->positions.GetBaseIndex(), _positionBufferUAV);
		RHICommands.SetUAVParameter(rhiComputeShader, cs->directions.GetBaseIndex(), _directionsBufferUAV);

		RHICommands.SetUAVParameter(rhiComputeShader, cs->neigbhours.GetBaseIndex(), _neighboursBufferUAV);
		RHICommands.SetUAVParameter(rhiComputeShader, cs->neighboursBaseIndex.GetBaseIndex(), _neighboursBaseIndexUAV);
		RHICommands.SetUAVParameter(rhiComputeShader, cs->neighboursCount.GetBaseIndex(), _neighboursCountUAV);

		FComputeShaderVariableParameters paramaters;
		paramaters.boidSpeed = 2.0f;
		paramaters.boidSpeedVariation = 1.0f;
		paramaters.dt = DeltaTime;
		paramaters.totalTime = GetOwner()->GetWorld()->TimeSeconds;
		paramaters.neighbourDistance = 5.0f;

		auto variablesBuffer = TUniformBufferRef<FComputeShaderVariableParameters>::
			CreateUniformBufferImmediate(paramaters, UniformBuffer_SingleDraw);

		auto variablesBufferParameter = cs->GetUniformBufferParameter<FComputeShaderVariableParameters>();

		SetUniformBufferParameter(
			RHICommands,
			rhiComputeShader,
			variablesBufferParameter,
			variablesBuffer);


		RHICommands.SetComputeShader(rhiComputeShader);

		DispatchComputeShader(RHICommands, *cs, 256, 1, 1);

		// read back the data
		uint8* data = (uint8*)RHILockStructuredBuffer(_positionBuffer, 0, numBoids * sizeof(FVector), RLM_ReadOnly);
		FMemory::Memcpy(outputPositions.GetData(), data, numBoids * sizeof(FVector));		

		RHIUnlockStructuredBuffer(_positionBuffer);
	});
}

FComputeShaderDeclaration::FComputeShaderDeclaration(const ShaderMetaType::CompiledShaderInitializerType& Initializer) : FGlobalShader(Initializer)
{
	positions.Bind(Initializer.ParameterMap, TEXT("positions"));
	directions.Bind(Initializer.ParameterMap, TEXT("directions"));
	neigbhours.Bind(Initializer.ParameterMap, TEXT("neigbhours"));
	neighboursBaseIndex.Bind(Initializer.ParameterMap, TEXT("neighboursBaseIndex"));
	neighboursCount.Bind(Initializer.ParameterMap, TEXT("neighboursCount"));

}

void FComputeShaderDeclaration::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.CompilerFlags.Add(CFLAG_StandardOptimization);
}

IMPLEMENT_SHADER_TYPE(, FComputeShaderDeclaration, TEXT("/ComputeShaderPlugin/Boid.usf"), TEXT("MainComputeShader"), SF_Compute);
