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

class FBitonicSort_sort : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBitonicSort_sort);
	SHADER_USE_PARAMETER_STRUCT(FBitonicSort_sort, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int3, job_params)
		SHADER_PARAMETER(uint, itemCount) // the number of particles

		SHADER_PARAMETER_UAV(StructuredBuffer<float>, comparisonBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, indexBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FBitonicSort_sortInner : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBitonicSort_sortInner);
	SHADER_USE_PARAMETER_STRUCT(FBitonicSort_sortInner, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int3, job_params)
		SHADER_PARAMETER(uint, itemCount) // the number of particles

		SHADER_PARAMETER_UAV(StructuredBuffer<float>, comparisonBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, indexBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FBitonicSort_sortStep : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBitonicSort_sortStep);
	SHADER_USE_PARAMETER_STRUCT(FBitonicSort_sortStep, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int3, job_params)
		SHADER_PARAMETER(uint, itemCount) // the number of particles

		SHADER_PARAMETER_UAV(StructuredBuffer<float>, comparisonBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, indexBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};


void FGPUBitonicSort::sort(
	uint32_t maxSize, 
	uint32_t numItems,
	FStructuredBufferRHIRef comparisonBuffer_read, 
    FStructuredBufferRHIRef indexBuffer_write,
    FRHICommandListImmediate& commands)
{
	{
		FBitonicSort_sort::FParameters parameters;

		parameters.itemCount = numItems;

		parameters.comparisonBuffer = comparisonBuffer_read;
		parameters.indexBuffer = indexBuffer_write;

		TShaderMapRef<FBitonicSort_sort> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::Dispatch(
			commands,
			*computeShader,
			parameters,
			FIntVector(numItems, 1, 1)
		);
	}
}
