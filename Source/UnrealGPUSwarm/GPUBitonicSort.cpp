// Copyright 2020 Timothy Davison, all rights reserved.

#include "GPUBitonicSort.h"

#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"
#include "Shader.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "UniformBuffer.h"
#include "RHICommandList.h"

class FBitonicSort_dispatchSort : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBoidsComputeShader);
	SHADER_USE_PARAMETER_STRUCT(FBoidsComputeShader, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int3, job_params)
		SHADER_PARAMETER(uint, itemCount) // the number of particles

		// counterBuffer
		// indirectBuffers
		SHADER_PARAMETER(float, boidSpeed)
		SHADER_PARAMETER(float, boidSpeedVariation)
		SHADER_PARAMETER(float, boidRotationSpeed)
		SHADER_PARAMETER(float, homeInnerRadius)

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


void FGPUBitonicSort::sort(
	uint32_t maxSize, 
	FStructuredBufferRHIRef comparisionBuffer_read, 
	FStructuredBufferRHIRef countBuffer_read, 
	uint32_t counterReadOffset,
    FStructuredBufferRHIRef indexBuffer_write,
    FRHICommandListImmediate commands)
{
	RHICreateUniformBuffer()
}
