// Fill out your copyright notice in the Description page of Project Settings.


#include "DrawPositionsComponent.h"

#include "ComputeShaderTestComponent.h"

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

	_updateInstanceTransforms();
}

void UDrawPositionsComponent::_initISMC()
{
	UInstancedStaticMeshComponent * ismc = GetOwner()->FindComponentByClass<UInstancedStaticMeshComponent>();

	if (!ismc) return;

	ismc->SetSimulatePhysics(false);

	ismc->SetMobility(EComponentMobility::Movable);
	ismc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ismc->SetCanEverAffectNavigation(false);
	//ismc->UseDynamicInstanceBuffer = true;
	//ismc->KeepInstanceBufferCPUAccess = true;
	ismc->SetCollisionProfileName(TEXT("NoCollision"));
}

void UDrawPositionsComponent::_updateInstanceTransforms()
{
	UInstancedStaticMeshComponent * ismc = GetOwner()->FindComponentByClass<UInstancedStaticMeshComponent>();

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

	ismc->BatchUpdateInstancesTransforms(0, _instanceTransforms, false, true, true);
}

















struct FTimInstancingUserData
{
	class FInstancedStaticMeshRenderData* RenderData;
	class FStaticMeshRenderData* MeshRenderData;

	int32 StartCullDistance;
	int32 EndCullDistance;

	int32 MinLOD;

	bool bRenderSelected;
	bool bRenderUnselected;
	FVector AverageInstancesScale;
};



/**
 * A vertex factory for instanced static meshes
 */
struct FTimInstancedStaticMeshVertexFactory : public FLocalVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FTimInstancedStaticMeshVertexFactory);
public:
	FTimInstancedStaticMeshVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FLocalVertexFactory(InFeatureLevel, "FTimInstancedStaticMeshVertexFactory")
	{
	}

	struct FDataType : public FInstancedStaticMeshDataType, public FLocalVertexFactory::FDataType
	{
	};

	/**
	 * Should we cache the material's shadertype on this platform with this vertex factory?
	 */
	static bool ShouldCompilePermutation(EShaderPlatform Platform, const class FMaterial* Material, const class FShaderType* ShaderType);

	/**
	 * Modify compile environment to enable instancing
	 * @param OutEnvironment - shader compile environment to modify
	 */
	static void ModifyCompilationEnvironment(const FVertexFactoryType* Type, EShaderPlatform Platform, const FMaterial* Material, FShaderCompilerEnvironment& OutEnvironment)
	{
		const bool ContainsManualVertexFetch = OutEnvironment.GetDefinitions().Contains("MANUAL_VERTEX_FETCH");
		if (!ContainsManualVertexFetch && RHISupportsManualVertexFetch(Platform))
		{
			OutEnvironment.SetDefine(TEXT("MANUAL_VERTEX_FETCH"), TEXT("1"));
		}

		OutEnvironment.SetDefine(TEXT("USE_INSTANCING"), TEXT("1"));
		if (IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5))
		{
			OutEnvironment.SetDefine(TEXT("USE_DITHERED_LOD_TRANSITION_FOR_INSTANCED"), ALLOW_DITHERED_LOD_FOR_INSTANCED_STATIC_MESHES);
		}
		else
		{
			// On mobile dithered LOD transition has to be explicitly enabled in material and project settings
			OutEnvironment.SetDefine(TEXT("USE_DITHERED_LOD_TRANSITION_FOR_INSTANCED"), Material->IsDitheredLODTransition() && ALLOW_DITHERED_LOD_FOR_INSTANCED_STATIC_MESHES);
		}

		FLocalVertexFactory::ModifyCompilationEnvironment(Type, Platform, Material, OutEnvironment);
	}

	/**
	 * An implementation of the interface used by TSynchronizedResource to update the resource with new data from the game thread.
	 */
	void SetData(const FDataType& InData)
	{
		FLocalVertexFactory::Data = InData;
		Data = InData;
		UpdateRHI();
	}

	/**
	 * Copy the data from another vertex factory
	 * @param Other - factory to copy from
	 */
	void Copy(const FTimInstancedStaticMeshVertexFactory& Other);

	// FRenderResource interface.
	virtual void InitRHI() override;

	static FVertexFactoryShaderParameters* ConstructShaderParameters(EShaderFrequency ShaderFrequency);

	/** Make sure we account for changes in the signature of GetStaticBatchElementVisibility() */
	static CONSTEXPR uint32 NumBitsForVisibilityMask()
	{
		return 8 * sizeof(decltype(((FInstancedStaticMeshVertexFactory*)nullptr)->GetStaticBatchElementVisibility(FSceneView(FSceneViewInitOptions()), nullptr)));
	}

	/**
	* Get a bitmask representing the visibility of each FMeshBatch element.
	*/
	virtual uint64 GetStaticBatchElementVisibility(const class FSceneView& View, const struct FMeshBatch* Batch, const void* ViewCustomData = nullptr) const override
	{
		const uint32 NumBits = NumBitsForVisibilityMask();
		const uint32 NumElements = FMath::Min((uint32)Batch->Elements.Num(), NumBits);
		return NumElements == NumBits ? ~0ULL : (1ULL << (uint64)NumElements) - 1ULL;
	}
#if ALLOW_DITHERED_LOD_FOR_INSTANCED_STATIC_MESHES
	virtual bool SupportsNullPixelShader() const override { return false; }
#endif

	inline bool IsDataInitialized() const
	{
		return Data.bInitialized;
	}

	inline uint32 GetNumInstances() const
	{
		return Data.NumInstances;
	}

	inline FRHIShaderResourceView* GetInstanceOriginSRV() const
	{
		return Data.InstanceOriginSRV;
	}

	inline FRHIShaderResourceView* GetInstanceTransformSRV() const
	{
		return Data.InstanceTransformSRV;
	}

	inline FRHIShaderResourceView* GetInstanceLightmapSRV() const
	{
		return Data.InstanceLightmapSRV;
	}

private:
	FDataType Data;
};


class FDrawComputeShaderInstancesProxy final : public FPrimitiveSceneProxy
{
public:
	FTimInstancingUserData UserData_AllInstances;

	UTimDrawPositions * _drawPositions;

public:
	FDrawComputeShaderInstancesProxy(UTimDrawPositions* InComponent, ERHIFeatureLevel::Type InFeatureLevel)
		: FStaticMeshSceneProxy(InComponent, true)
		, _drawPositions(InComponent)
	{
		bVFRequiresPrimitiveUniformBuffer = true;
		SetupProxy(InComponent);


	}

	void SetupProxy(UInstancedStaticMeshComponent* InComponent)
	{
		// Make sure all the materials are okay to be rendered as an instanced mesh.
		for (int32 LODIndex = 0; LODIndex < LODs.Num(); LODIndex++)
		{
			FStaticMeshSceneProxy::FLODInfo& LODInfo = LODs[LODIndex];
			for (int32 SectionIndex = 0; SectionIndex < LODInfo.Sections.Num(); SectionIndex++)
			{
				FStaticMeshSceneProxy::FLODInfo::FSectionInfo& Section = LODInfo.Sections[SectionIndex];
				if (!Section.Material->CheckMaterialUsage_Concurrent(MATUSAGE_InstancedStaticMeshes))
				{
					Section.Material = UMaterial::GetDefaultMaterial(MD_Surface);
				}
			}
		}

		const bool bInstanced = GRHISupportsInstancing;

		// Copy the parameters for LOD - all instances
		UserData_AllInstances.MeshRenderData = InComponent->GetStaticMesh()->RenderData.Get();
		UserData_AllInstances.StartCullDistance = InComponent->InstanceStartCullDistance;
		UserData_AllInstances.EndCullDistance = InComponent->InstanceEndCullDistance;
		UserData_AllInstances.MinLOD = ClampedMinLOD;
		UserData_AllInstances.bRenderSelected = true;
		UserData_AllInstances.bRenderUnselected = true;
		UserData_AllInstances.RenderData = bInstanced ? nullptr : &InstancedRenderData;

		// hacked
		FVector MinScale(0.f);
		FVector MaxScale(1.0);
		/*InComponent->GetInstancesMinMaxScale(MinScale, MaxScale);*/

		UserData_AllInstances.AverageInstancesScale = MinScale + (MaxScale - MinScale) / 2.0f;

		// selected only
		UserData_SelectedInstances = UserData_AllInstances;
		UserData_SelectedInstances.bRenderUnselected = false;

		// unselected only
		UserData_DeselectedInstances = UserData_AllInstances;
		UserData_DeselectedInstances.bRenderSelected = false;
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FTimDrawPositions_GetMeshElements);

		const bool bSelectionRenderEnabled = false;

		// If the first pass rendered selected instances only, we need to render the deselected instances in a second pass
		const FInstancingUserData* PassUserData = &UserData_AllInstances;

		bool isSelected = false;

		const bool bIsWireframe = ViewFamily.EngineShowFlags.Wireframe;

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (VisibilityMap & (1 << ViewIndex))
			{
				const FSceneView* View = Views[ViewIndex];
				
				const int32 LODIndex = GetLOD(View);
				const FStaticMeshLODResources& LODModel = StaticMesh->RenderData->LODResources[LODIndex];

				for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); SectionIndex++)
				{
					const int32 NumBatches = 1;

					for (int32 BatchIndex = 0; BatchIndex < NumBatches; BatchIndex++)
					{
						FMeshBatch& MeshElement = Collector.AllocateMesh();

						if (GetMeshElement(LODIndex, BatchIndex, SectionIndex, GetDepthPriorityGroup(View), isSelected, true, MeshElement))
						{
							MeshElement.Elements[0].UserData = PassUserData;
							MeshElement.Elements[0].bUserDataIsColorVertexBuffer = false;
							MeshElement.bCanApplyViewModeOverrides = true;
							MeshElement.bUseSelectionOutline = false;
							MeshElement.bUseWireframeSelectionColoring = false;

							if (View->bRenderFirstInstanceOnly)
							{
								for (int32 ElementIndex = 0; ElementIndex < MeshElement.Elements.Num(); ElementIndex++)
								{
									MeshElement.Elements[ElementIndex].NumInstances = FMath::Min<uint32>(MeshElement.Elements[ElementIndex].NumInstances, 1);
								}
							}

							Collector.AddMesh(ViewIndex, MeshElement);
							INC_DWORD_STAT_BY(STAT_StaticMeshTriangles, MeshElement.GetNumPrimitives());
						}
					}
				}
			}
		}
	}

	virtual bool GetMeshElement(int32 LODIndex, int32 BatchIndex, int32 ElementIndex, uint8 InDepthPriorityGroup, bool bUseSelectionOutline, bool bAllowPreCulledIndices, FMeshBatch& OutMeshBatch) const override;
	{
		if (LODIndex < InstancedRenderData.VertexFactories.Num() && FStaticMeshSceneProxy::GetMeshElement(LODIndex, BatchIndex, ElementIndex, InDepthPriorityGroup, bUseSelectionOutline, bAllowPreCulledIndices, OutMeshBatch))
		{
			SetupInstancedMeshBatch(LODIndex, BatchIndex, OutMeshBatch);
			return true;
		}
		return false;
	};

	void SetupInstancedMeshBatch(int32 LODIndex, int32 BatchIndex, FMeshBatch& OutMeshBatch) const;
	{
		const bool bInstanced = GRHISupportsInstancing;
		OutMeshBatch.VertexFactory = &InstancedRenderData.VertexFactories[LODIndex];
		const uint32 NumInstances = InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetNumInstances();
		FMeshBatchElement& BatchElement0 = OutMeshBatch.Elements[0];
		BatchElement0.UserData = (void*)&UserData_AllInstances;
		BatchElement0.bUserDataIsColorVertexBuffer = false;
		BatchElement0.InstancedLODIndex = LODIndex;
		BatchElement0.UserIndex = 0;
		BatchElement0.bIsInstancedMesh = bInstanced;
		BatchElement0.PrimitiveUniformBuffer = GetUniformBuffer();

		if (bInstanced)
		{
			BatchElement0.NumInstances = NumInstances;
		}
		else
		{

		}
	}
};




FPrimitiveSceneProxy* UTimDrawPositions::CreateSceneProxy()
{
	return new FTimDrawPositions(this);
}

