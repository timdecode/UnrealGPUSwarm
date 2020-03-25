// Fill out your copyright notice in the Description page of Project Settings.


#include "StructuredBufferInstancedMesh.h"







class UNREALGPUSWARM_API FStructredBufferInstancedMeshProxy : public FStaticMeshSceneProxy
{



public:
	FStructredBufferInstancedMeshProxy(UStaticMeshComponent* InComponent, bool bForceLODsShareStaticLighting) 
		: FStaticMeshSceneProxy(InComponent, bForceLODsShareStaticLighting)
	{

	}

	virtual ~FStructredBufferInstancedMeshProxy()
	{

	}

	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}
};









FPrimitiveSceneProxy* UStructuredBufferInstancedMesh::CreateSceneProxy()
{
	if (GetStaticMesh() == nullptr || GetStaticMesh()->RenderData == nullptr)
	{
		return nullptr;
	}

	FPrimitiveSceneProxy* Proxy = ::new FStructredBufferInstancedMeshProxy(this, false);

	return Proxy;
}


