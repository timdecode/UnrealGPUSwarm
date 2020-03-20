// Copyright 2020 Timothy Davison, all rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "GPUBitonicSort.generated.h"

USTRUCT(BlueprintType)
struct UNREALGPUSWARM_API FGPUBitonicSort
{
	GENERATED_BODY()

public:
	// Sort data on the GPU with bitonic sorting.
	void sort(
		uint32_t maxSize,
		uint32_t numItems,
		FUnorderedAccessViewRHIRef comparisionBuffer_read,
		FUnorderedAccessViewRHIRef indexBuffer_write,
		FRHICommandListImmediate& commands
	);
};
