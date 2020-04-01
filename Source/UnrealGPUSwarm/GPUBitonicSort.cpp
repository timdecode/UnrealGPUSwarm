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
		SHADER_PARAMETER(FIntVector, job_params)
		SHADER_PARAMETER(uint32, itemCount) // the number of particles


		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, comparisonBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, indexBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FBitonicSort_sort, "/ComputeShaderPlugin/BitonicSort_sort.usf", "BitonicSort_sort", SF_Compute);





class FBitonicSort_sortInner : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBitonicSort_sortInner);
	SHADER_USE_PARAMETER_STRUCT(FBitonicSort_sortInner, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, job_params)
		SHADER_PARAMETER(uint32, itemCount) // the number of particles

		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, comparisonBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, indexBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FBitonicSort_sortInner, "/ComputeShaderPlugin/BitonicSort_sortInner.usf", "BitonicSort_sortInner", SF_Compute);






class FBitonicSort_sortStep : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FBitonicSort_sortStep);
	SHADER_USE_PARAMETER_STRUCT(FBitonicSort_sortStep, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, job_params)
		SHADER_PARAMETER(uint32, itemCount) // the number of particles

		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, comparisonBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, indexBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::ES3_1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FBitonicSort_sortStep, "/ComputeShaderPlugin/BitonicSort_sortStep.usf", "BitonicSort_sortStep", SF_Compute);






void FGPUBitonicSort::sort(
	uint32_t maxCount, 
	uint32_t numItems,
	FUnorderedAccessViewRHIRef comparisonBuffer_read,
	FUnorderedAccessViewRHIRef indexBuffer_write,
    FRHICommandListImmediate& commands)
{
	int threadCount = ((numItems - 1) >> 9) + 1;

	bool done = true;

	{
		FBitonicSort_sort::FParameters parameters;

		parameters.itemCount = numItems;

		parameters.comparisonBuffer = comparisonBuffer_read;
		parameters.indexBuffer = indexBuffer_write;

		unsigned int numThreadGroups = ((maxCount - 1) >> 9) + 1;

		//assert(numThreadGroups <= 1024);

		if (numThreadGroups > 1)
		{
			done = false;
		}

		TShaderMapRef<FBitonicSort_sort> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::Dispatch(
			commands,
			*computeShader,
			parameters,
			FIntVector(threadCount, 1, 1)
		);
	}

	int presorted = 512;
	while (!done)
	{
		// Incremental sorting:

		done = true;



		// prepare thread group description data
		uint32_t numThreadGroups = 0;

		if (maxCount > (uint32_t)presorted)
		{
			if (maxCount > (uint32_t)presorted * 2)
				done = false;

			uint32_t pow2 = presorted;
			while (pow2 < maxCount)
				pow2 *= 2;
			numThreadGroups = pow2 >> 9;
		}

		FIntVector job_params;

		// step-sort
		uint32_t nMergeSize = presorted * 2;
		for (uint32_t nMergeSubSize = nMergeSize >> 1; nMergeSubSize > 256; nMergeSubSize = nMergeSubSize >> 1)
		{


			job_params.X = nMergeSubSize;
			if (nMergeSubSize == nMergeSize >> 1)
			{
				job_params.Y = (2 * nMergeSubSize - 1);
				job_params.Z = -1;
			}
			else
			{
				job_params.Y = nMergeSubSize;
				job_params.Z = 1;
			}

			FBitonicSort_sortStep::FParameters parameters;

			parameters.itemCount = numItems;
			parameters.job_params = job_params;

			parameters.comparisonBuffer = comparisonBuffer_read;
			parameters.indexBuffer = indexBuffer_write;


			TShaderMapRef<FBitonicSort_sortStep> computeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::Dispatch(
				commands,
				*computeShader,
				parameters,
				FIntVector(numThreadGroups, 1, 1)
			);
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
}
