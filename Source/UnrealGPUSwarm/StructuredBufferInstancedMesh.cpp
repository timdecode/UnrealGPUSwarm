// Fill out your copyright notice in the Description page of Project Settings.


#include "StructuredBufferInstancedMesh.h"






class ENGINE_API FStructredBufferInstancedMeshProxy : public FPrimitiveSceneProxy
{
public:
	FStaticMeshRenderData* RenderData;


public:
	FStructredBufferInstancedMeshProxy(UStaticMeshComponent* Component, bool bForceLODsShareStaticLighting) :
		: FPrimitiveSceneProxy(InComponent, InComponent->GetStaticMesh()->GetFName())
		, RenderData(InComponent->GetStaticMesh()->RenderData.Get())

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

	virtual int32 GetNumMeshBatches() const
	{
		return 1;
	}

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			const FSceneView* View = Views[ViewIndex];

			if (IsShown(View) && (VisibilityMap & (1 << ViewIndex)))
			{
				FFrozenSceneViewMatricesGuard FrozenMatricesGuard(*const_cast<FSceneView*>(Views[ViewIndex]));

				FLODMask LODMask = GetLODMask(View);

				for (int32 LODIndex = 0; LODIndex < RenderData->LODResources.Num(); LODIndex++)
				{
					if (LODMask.ContainsLOD(LODIndex) && LODIndex >= ClampedMinLOD)
					{
						const FStaticMeshLODResources& LODModel = RenderData->LODResources[LODIndex];
						const FLODInfo& ProxyLODInfo = LODs[LODIndex];


						// Draw the static mesh sections.
						for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); SectionIndex++)
						{
							const int32 NumBatches = GetNumMeshBatches();

							for (int32 BatchIndex = 0; BatchIndex < NumBatches; BatchIndex++)
							{
								bool bSectionIsSelected = false;
								FMeshBatch& MeshElement = Collector.AllocateMesh();

								if (GetMeshElement(LODIndex, BatchIndex, SectionIndex, SDPG_World, bSectionIsSelected, true, MeshElement))
								{
									if (MeshElement.bDitheredLODTransition && LODMask.IsDithered())
									{

									}
									else
									{
										MeshElement.bDitheredLODTransition = false;
									}

									MeshElement.bCanApplyViewModeOverrides = true;
									MeshElement.bUseWireframeSelectionColoring = bSectionIsSelected;

									Collector.AddMesh(ViewIndex, MeshElement);
								}
							}
						}
					}
				}
			}
		}

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

UStaticMesh * UStructuredBufferInstancedMesh::GetStaticMesh()
{
	return StaticMesh;
}
