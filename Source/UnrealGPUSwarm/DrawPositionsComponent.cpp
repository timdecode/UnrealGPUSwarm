// Fill out your copyright notice in the Description page of Project Settings.


#include "DrawPositionsComponent.h"

#include "InstanceBufferMeshComponent.h"

#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"
#include "Shader.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "UniformBuffer.h"
#include "RHICommandList.h"
#include "Private/InstanceBufferMesh.h"

#include "ComputeShaderTestComponent.h"









class FBoids_copyPositions_CS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoids_copyPositions_CS);
	SHADER_USE_PARAMETER_STRUCT(FBoids_copyPositions_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, numParticles)

		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, positions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, positions_other)

		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, directions)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float4x4>, transforms_other)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FBoids_copyPositions_CS, "/ComputeShaderPlugin/CopyPositions.usf", "copyPositions", SF_Compute);























// Sets default values for this component's properties
UDrawPositionsComponent::UDrawPositionsComponent()
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;

	// ...
}


// Called when the game starts
void UDrawPositionsComponent::BeginPlay()
{
	Super::BeginPlay();

	_initISMC();
}


// Called every frame
void UDrawPositionsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);


	_updateInstanceBuffers();
}

void UDrawPositionsComponent::_initISMC()
{
	UInstanceBufferMeshComponent * ismc = GetOwner()->FindComponentByClass<UInstanceBufferMeshComponent>();

	if (!ismc) return;

	ismc->SetSimulatePhysics(false);

	ismc->SetMobility(EComponentMobility::Movable);
	ismc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ismc->SetCanEverAffectNavigation(false);
	//ismc->UseDynamicInstanceBuffer = true;
	//ismc->KeepInstanceBufferCPUAccess = true;
	ismc->SetCollisionProfileName(TEXT("NoCollision"));
}

static FIntVector groupSize(int numElements)
{
	const int threadCount = 256;

	int count = ((numElements - 1) / threadCount) + 1;

	return FIntVector(count, 1, 1);
}


void UDrawPositionsComponent::_updateInstanceBuffers()
{
	UInstanceBufferMeshComponent * ismc = GetOwner()->FindComponentByClass<UInstanceBufferMeshComponent>();

	if (!ismc) return;

	UComputeShaderTestComponent * boidsComponent = GetOwner()->FindComponentByClass<UComputeShaderTestComponent>();

	if (!boidsComponent) return;

	// resize up/down the ismc
	int toAdd = FMath::Max(0, boidsComponent->numBoids - ismc->GetInstanceCount());
	int toRemove = FMath::Max(0, ismc->GetInstanceCount() - boidsComponent->numBoids);

	FTransform transform = FTransform::Identity;

	transform.SetScale3D(FVector(size));

	for (int i = 0; i < toAdd; ++i)
	{


		ismc->AddInstance(transform);
	}

	for (int i = 0; i < toRemove; ++i)
		ismc->RemoveInstance(ismc->GetInstanceCount() - 1);

	// directly update the buffer
	if (toAdd == 0)
	{
		auto renderData = ismc->PerInstanceRenderData;

		auto originSRV = renderData->InstanceBuffer.InstanceOriginSRV.GetReference();

		int numParticles = boidsComponent->numBoids;

		if (!_positionsUAV)
		{
			FRHIVertexBuffer * positionsVertexBuffer = renderData->InstanceBuffer.InstanceOriginBuffer.VertexBufferRHI.GetReference();

			uint8 theFormat = PF_A32B32G32R32F;;

			_positionsUAV = RHICreateUnorderedAccessView(positionsVertexBuffer, theFormat);
		}
		
		


		FBoids_copyPositions_CS::FParameters parameters;
		parameters.positions = boidsComponent->currentPositionsBuffer();
		parameters.positions_other = _positionsUAV;
		parameters.numParticles = numParticles;

		ENQUEUE_RENDER_COMMAND(FComputeShaderRunner)(
			[&, parameters, numParticles](FRHICommandListImmediate& RHICommands)
		{


			TShaderMapRef<FBoids_copyPositions_CS> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				RHICommands,
				*computeShader,
				parameters,
				groupSize(numParticles)
			);


		});
	}
	else
		ismc->MarkRenderStateDirty();
}

void UDrawPositionsComponent::_updateInstanceTransforms()
{
	UInstanceBufferMeshComponent * ismc = GetOwner()->FindComponentByClass<UInstanceBufferMeshComponent>();

	if (!ismc) return;

	UComputeShaderTestComponent * boidsComponent = GetOwner()->FindComponentByClass<UComputeShaderTestComponent>();

	if (!boidsComponent) return;

	TArray<FVector4>& positions = boidsComponent->outputPositions;
	TArray<FVector4>& directions = boidsComponent->outputDirections;

	// resize up/down the ismc
	int toAdd = FMath::Max(0, positions.Num() - ismc->GetInstanceCount());
	int toRemove = FMath::Max(0, ismc->GetInstanceCount() - positions.Num());

	for (int i = 0; i < toAdd; ++i)
		ismc->AddInstance(FTransform::Identity);
	for (int i = 0; i < toRemove; ++i)
		ismc->RemoveInstance(ismc->GetInstanceCount() - 1);

	// update the transforms
	_instanceTransforms.SetNum(positions.Num());

	for (int i = 0; i < positions.Num(); ++i)
	{
		FTransform& transform = _instanceTransforms[i];

		transform.SetTranslation(FVector(positions[i]));
		transform.SetScale3D(FVector(size));

		FQuat quat = FQuat::FindBetweenVectors(FVector::UpVector, FVector(directions[i]));
		transform.SetRotation(quat);
	}

	ismc->BatchUpdateInstancesTransforms(0, _instanceTransforms, false, false, true);

	if (toAdd == 0)
		ismc->MarkRenderDynamicDataDirty();
	else
		ismc->MarkRenderStateDirty();
}

