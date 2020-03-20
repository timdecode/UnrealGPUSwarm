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
		SHADER_PARAMETER(FIntVector, job_params)
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
	int threadCount = ((numItems - 1) >> 9) + 1;

	bool done = true;

	{
		FBitonicSort_sort::FParameters parameters;

		parameters.itemCount = numItems;

		parameters.comparisonBuffer = comparisonBuffer_read;
		parameters.indexBuffer = indexBuffer_write;

		unsigned int numThreadGroups = ((maxSize - 1) >> 9) + 1;

		//assert(numThreadGroups <= 1024);

		if (numThreadGroups > 1)
		{
			bDone = false;
		}

		TShaderMapRef<FBitonicSort_sort> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::Dispatch(
			commands,
			*computeShader,
			parameters,
			FIntVector(threadCount, 1, 1)
		);

		// TODO: need barrier
	}

	int presorted = 512;
	while (!bDone)
	{
		// Incremental sorting:

		bDone = true;



		// prepare thread group description data
		uint32_t numThreadGroups = 0;

		if (maxCount > (uint32_t)presorted)
		{
			if (maxCount > (uint32_t)presorted * 2)
				bDone = false;

			uint32_t pow2 = presorted;
			while (pow2 < maxCount)
				pow2 *= 2;
			numThreadGroups = pow2 >> 9;
		}

		FIntVector jab_params;

		// step-sort
		uint32_t nMergeSize = presorted * 2;
		for (uint32_t nMergeSubSize = nMergeSize >> 1; nMergeSubSize > 256; nMergeSubSize = nMergeSubSize >> 1)
		{


			job_params.x = nMergeSubSize;
			if (nMergeSubSize == nMergeSize >> 1)
			{
				job_params.y = (2 * nMergeSubSize - 1);
				job_params.z = -1;
			}
			else
			{
				job_params.y = nMergeSubSize;
				job_params.z = 1;
			}

			FBitonicSort_sortStep::FParameters sortparameters;

			sortparameters.itemCount = numItems;
			sortparameters.comparisonBuffer = comparisonBuffer_read;
			sortparameters.indexBuffer = indexBuffer_write;

			sortparameters.job_params = job_params;

			TShaderMapRef<FBitonicSort_sortStep> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				commands,
				*computeShader,
				parameters,
				FIntVector(numThreadGroups, 1, 1)
			);


			device->Barrier(&GPUBarrier::Memory(), 1, cmd);
		}

		{
			FBitonicSort_sortInner::FParameters parameters;

			parameters.job_params = job_params;
			parameters.itemCount = numItems;

			parameters.comparisonBuffer = comparisonBuffer_read;
			parameters.indexBuffer = indexBuffer_write;

			TShaderMapRef<FBitonicSort_sortInner> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				commands,
				*computeShader,
				parameters,
				FIntVector(numThreadGroups, 1, 1)
			);
		}

		presorted *= 2;
	}

	device->UnbindUAVs(0, arraysize(uavs), cmd);
	device->UnbindResources(0, arraysize(resources), cmd);


	device->EventEnd(cmd)

}
