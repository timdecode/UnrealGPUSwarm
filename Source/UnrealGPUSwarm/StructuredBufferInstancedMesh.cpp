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

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

};

void FStructredBufferInstancedMeshProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{

}








FPrimitiveSceneProxy* UStructuredBufferInstancedMesh::CreateSceneProxy()
{
	if (GetStaticMesh() == nullptr || GetStaticMesh()->RenderData == nullptr)
	{
		return nullptr;
	}

	FPrimitiveSceneProxy* Proxy = ::new FStructredBufferInstancedMeshProxy(this, false);

	return Proxy;
}


