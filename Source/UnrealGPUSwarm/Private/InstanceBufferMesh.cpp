// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	InstaneBufferMesh.cpp: Static mesh rendering code.
=============================================================================*/

#include "InstanceBufferMesh.h"
#include "../InstanceBufferMeshComponent.h"

#include "AI/NavigationSystemBase.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Components/LightComponent.h"
#include "Logging/TokenizedMessage.h"
#include "Logging/MessageLog.h"
#include "UnrealEngine.h"
#include "AI/NavigationSystemHelpers.h"
#include "AI/Navigation/NavCollisionBase.h"
#include "ShaderParameterUtils.h"
#include "Misc/UObjectToken.h"
#include "PhysXPublic.h"
//#include "PhysicsEngine/PhysXSupport.h"
#include "PhysicsEngine/BodySetup.h"
#include "GameFramework/WorldSettings.h"
#include "ComponentRecreateRenderStateContext.h"
#include "SceneManagement.h"
#include "Algo/Transform.h"
#include "UObject/MobileObjectVersion.h"
#include "EngineStats.h"
#include "Interfaces/ITargetPlatform.h"
#if WITH_EDITOR
#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#endif // WITH_EDITOR
#include "MeshMaterialShader.h"

#include "RayTracingInstance.h"


//#if RHI_RAYTRACING
//#include "RayTracingInstance.h"
//#endif

#include "Interfaces/ITargetPlatform.h"
#if WITH_EDITOR
#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#endif // WITH_EDITOR
#include "UObject/FortniteMainBranchObjectVersion.h"
#include "UObject/EditorObjectVersion.h"


const int32 InstancedStaticMeshMaxTexCoord = 8;

IMPLEMENT_HIT_PROXY(HInstanceBufferMeshInstance, HHitProxy);

const float TimHackMinSize = 0.000001f;
const float TimHackLODScale = 1.0f;
const float TimHackLODRange = 0.0f;

namespace InstanceBuffeStaticMeshNameSpace
{
    TAutoConsoleVariable<int32> CVarMinLOD(
        TEXT("foliage.MinLOD"),
        -1,
        TEXT("Used to discard the top LODs for performance evaluation. -1: Disable all effects of this cvar."));

    static TAutoConsoleVariable<int32> CVarRayTracingRenderInstances(
        TEXT("r.RayTracing.InstancedStaticMeshes"),
        1,
        TEXT("Include static mesh instances in ray tracing effects (default = 1 (Instances enabled in ray tracing))"));

    static TAutoConsoleVariable<int32> CVarRayTracingRenderInstancesCulling(
        TEXT("r.RayTracing.InstancedStaticMeshes.Culling"),
        1,
        TEXT("Enable culling for instances in ray tracing (default = 1 (Culling enabled))"));

    static TAutoConsoleVariable<float> CVarRayTracingInstancesCullClusterMaxRadiusMultiplier(
        TEXT("r.RayTracing.InstancedStaticMeshes.CullClusterMaxRadiusMultiplier"),
        20.0f,
        TEXT("Multiplier for the maximum instance size (default = 20cm)"));

    static TAutoConsoleVariable<float> CVarRayTracingInstancesCullClusterRadius(
        TEXT("r.RayTracing.InstancedStaticMeshes.CullClusterRadius"),
        10000.0f, // 100 m
        TEXT("Ignore instances outside of this radius in ray tracing effects (default = 10000 (100m))"));

    static TAutoConsoleVariable<float> CVarRayTracingInstancesLowScaleThreshold(
        TEXT("r.RayTracing.InstancedStaticMeshes.LowScaleRadiusThreshold"),
        50.0f, // Instances with a radius smaller than this threshold get culled after CVarRayTracingInstancesLowScaleCullRadius
        TEXT("Threshold that classifies instances as small (default = 50cm))"));

    static TAutoConsoleVariable<float> CVarRayTracingInstancesLowScaleCullRadius(
        TEXT("r.RayTracing.InstancedStaticMeshes.LowScaleCullRadius"),
        1000.0f,
        TEXT("Cull radius for small instances (default = 1000 (10m))"));
}




/** InstancedStaticMeshInstance hit proxy */
void HInstanceBufferMeshInstance::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Component);
}


FIBMInstanceBuffer::FIBMInstanceBuffer(ERHIFeatureLevel::Type InFeatureLevel)
	: FRenderResource(InFeatureLevel)
{
}

FIBMInstanceBuffer::~FIBMInstanceBuffer()
{
	CleanUp();
}

/** Delete existing resources */
void FIBMInstanceBuffer::CleanUp()
{
}


void FIBMInstanceBuffer::UpdateWithNumInstances_Concurrent(unsigned int numInstances)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FIBMInstanceBuffer_UpdateWithNumInstances_Concurrent);

	FIBMInstanceBuffer* InstanceBuffer = this;

	ENQUEUE_RENDER_COMMAND(InstanceBuffer_UpdateFromPreallocatedData)(
		[InstanceBuffer, numInstances](FRHICommandListImmediate& RHICmdList)
	{
		InstanceBuffer->UpdateWithNumInstances_RenderThread(numInstances);
	});
}

void FIBMInstanceBuffer::UpdateWithNumInstances_RenderThread(unsigned int numInstances)
{
	_numInstances = numInstances;

	UpdateRHI();
}


/**
 * Specialized assignment operator, only used when importing LOD's.  
 */
void FIBMInstanceBuffer::operator=(const FIBMInstanceBuffer &Other)
{
	checkf(0, TEXT("Unexpected assignment call"));
}

void FIBMInstanceBuffer::InitRHI()
{
	if (_numInstances > 0)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FStaticMeshInstanceBuffer_InitRHI);
		LLM_SCOPE(ELLMTag::InstancedMesh);

		// Tim: We want to write to this buffer GPU side. So it should not be static.
		auto AccessFlags = BUF_UnorderedAccess | BUF_ShaderResource; 

		uint32_t originsSize = _numInstances * uint32_t(sizeof(FVector4));
		uint32_t transformsSize = _numInstances * uint32_t(sizeof(FVector4) * 3);
		uint32_t lightmapSize = _numInstances * uint32_t(sizeof(int16) * 4); // see FStaticMeshInstanceData::FInstanceLightMapVector (which is private, hence the manual calculations here)

		CreateVertexBuffer(originsSize, AccessFlags, 16, PF_A32B32G32R32F, InstanceOriginBuffer.VertexBufferRHI, InstanceOriginSRV);
		CreateVertexBuffer(transformsSize, AccessFlags, 16, PF_A32B32G32R32F, InstanceTransformBuffer.VertexBufferRHI, InstanceTransformSRV);
		CreateVertexBuffer(lightmapSize, AccessFlags, 8, PF_R16G16B16A16_SNORM, InstanceLightmapBuffer.VertexBufferRHI, InstanceLightmapSRV);
	}
}

void FIBMInstanceBuffer::ReleaseRHI()
{
	InstanceOriginSRV.SafeRelease();
	InstanceTransformSRV.SafeRelease();
	InstanceLightmapSRV.SafeRelease();

	InstanceOriginBuffer.ReleaseRHI();
	InstanceTransformBuffer.ReleaseRHI();
	InstanceLightmapBuffer.ReleaseRHI();
}

void FIBMInstanceBuffer::InitResource()
{
	FRenderResource::InitResource();
	InstanceOriginBuffer.InitResource();
	InstanceTransformBuffer.InitResource();
	InstanceLightmapBuffer.InitResource();
}

void FIBMInstanceBuffer::ReleaseResource()
{
	FRenderResource::ReleaseResource();
	InstanceOriginBuffer.ReleaseResource();
	InstanceTransformBuffer.ReleaseResource();
	InstanceLightmapBuffer.ReleaseResource();
}

SIZE_T FIBMInstanceBuffer::GetResourceSize() const
{
	return 0;
}

void FIBMInstanceBuffer::CreateVertexBuffer(uint32_t sizeInBytes, uint32 InUsage, uint32 InStride, uint8 InFormat, FVertexBufferRHIRef& OutVertexBufferRHI, FShaderResourceViewRHIRef& OutInstanceSRV)
{
	FRHIResourceCreateInfo CreateInfo;

	OutVertexBufferRHI = RHICreateVertexBuffer(sizeInBytes, InUsage, CreateInfo);

	if (RHISupportsManualVertexFetch(GMaxRHIShaderPlatform))
	{
		OutInstanceSRV = RHICreateShaderResourceView(OutVertexBufferRHI, InStride, InFormat);
	}
}

void FIBMInstanceBuffer::BindInstanceVertexBuffer(const class FVertexFactory* VertexFactory, FInstanceBufferMeshDataType& InstancedStaticMeshData) const
{
	if (GetNumInstances() > 0 && RHISupportsManualVertexFetch(GMaxRHIShaderPlatform))
	{
		check(InstanceOriginSRV);
		check(InstanceTransformSRV);
		check(InstanceLightmapSRV);
	}

	{
		InstancedStaticMeshData.InstanceOriginSRV = InstanceOriginSRV;
		InstancedStaticMeshData.InstanceTransformSRV = InstanceTransformSRV;
		InstancedStaticMeshData.InstanceLightmapSRV = InstanceLightmapSRV;
		InstancedStaticMeshData.NumInstances = GetNumInstances();
		InstancedStaticMeshData.bInitialized = true;
	}

	{
		InstancedStaticMeshData.InstanceOriginComponent = FVertexStreamComponent(
			&InstanceOriginBuffer,
			0,
			16,
			VET_Float4,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);

		EVertexElementType TransformType = VET_Float4;
		uint32 TransformStride = 16;

		InstancedStaticMeshData.InstanceTransformComponent[0] = FVertexStreamComponent(
			&InstanceTransformBuffer,
			0 * TransformStride,
			3 * TransformStride,
			TransformType,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);
		InstancedStaticMeshData.InstanceTransformComponent[1] = FVertexStreamComponent(
			&InstanceTransformBuffer,
			1 * TransformStride,
			3 * TransformStride,
			TransformType,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);
		InstancedStaticMeshData.InstanceTransformComponent[2] = FVertexStreamComponent(
			&InstanceTransformBuffer,
			2 * TransformStride,
			3 * TransformStride,
			TransformType,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);

		InstancedStaticMeshData.InstanceLightmapAndShadowMapUVBiasComponent = FVertexStreamComponent(
			&InstanceLightmapBuffer,
			0,
			8,
			VET_Short4N,
			EVertexStreamUsage::ManualFetch | EVertexStreamUsage::Instancing
		);
	}
}




/**
 * Should we cache the material's shadertype on this platform with this vertex factory? 
 */
bool FInstanceBufferMeshVertexFactory::ShouldCompilePermutation(EShaderPlatform Platform, const class FMaterial* Material, const class FShaderType* ShaderType)
{
	return (Material->IsUsedWithInstancedStaticMeshes() || Material->IsSpecialEngineMaterial()) 
			&& FLocalVertexFactory::ShouldCompilePermutation(Platform, Material, ShaderType);
}


/**
 * Copy the data from another vertex factory
 * @param Other - factory to copy from
 */
void FInstanceBufferMeshVertexFactory::Copy(const FInstanceBufferMeshVertexFactory& Other)
{
	FInstanceBufferMeshVertexFactory* VertexFactory = this;
	const FDataType* DataCopy = &Other.Data;
	ENQUEUE_RENDER_COMMAND(FInstancedStaticMeshVertexFactoryCopyData)(
	[VertexFactory, DataCopy](FRHICommandListImmediate& RHICmdList)
	{
		VertexFactory->Data = *DataCopy;
	});
	BeginUpdateResourceRHI(this);
}

void FInstanceBufferMeshVertexFactory::InitRHI()
{
	check(HasValidFeatureLevel());
	const bool bInstanced = GRHISupportsInstancing;

#if !ALLOW_DITHERED_LOD_FOR_INSTANCED_STATIC_MESHES // position(and normal) only shaders cannot work with dithered LOD
	// If the vertex buffer containing position is not the same vertex buffer containing the rest of the data,
	// then initialize PositionStream and PositionDeclaration.
	if(Data.PositionComponent.VertexBuffer != Data.TangentBasisComponents[0].VertexBuffer)
	{
		auto AddDeclaration = [&Data](EVertexInputStreamType InputStreamType, bool bInstanced, bool bAddNormal)
		{
			FVertexDeclarationElementList StreamElements;
			StreamElements.Add(AccessPositionStreamComponent(Data.PositionComponent, 0));

			if (bAddNormal)
				StreamElements.Add(AccessPositionStreamComponent(Data.TangentBasisComponents[2], 2));

			if (bInstanced)
			{
				// toss in the instanced location stream
				StreamElements.Add(AccessPositionStreamComponent(Data.InstanceOriginComponent, 8));
				StreamElements.Add(AccessPositionStreamComponent(Data.InstanceTransformComponent[0], 9));
				StreamElements.Add(AccessPositionStreamComponent(Data.InstanceTransformComponent[1], 10));
				StreamElements.Add(AccessPositionStreamComponent(Data.InstanceTransformComponent[2], 11));
			}

			InitDeclaration(StreamElements, InputStreamType);
		};
		AddDeclaration(EVertexInputStreamType::PositionOnly, bInstanced, false);
		AddDeclaration(EVertexInputStreamType::PositionAndNormalOnly, bInstanced, true);
	}
#endif

	FVertexDeclarationElementList Elements;
	if(Data.PositionComponent.VertexBuffer != NULL)
	{
		Elements.Add(AccessStreamComponent(Data.PositionComponent,0));
	}

	// only tangent,normal are used by the stream. the binormal is derived in the shader
	uint8 TangentBasisAttributes[2] = { 1, 2 };
	for(int32 AxisIndex = 0;AxisIndex < 2;AxisIndex++)
	{
		if(Data.TangentBasisComponents[AxisIndex].VertexBuffer != NULL)
		{
			Elements.Add(AccessStreamComponent(Data.TangentBasisComponents[AxisIndex],TangentBasisAttributes[AxisIndex]));
		}
	}

	if (Data.ColorComponentsSRV == nullptr)
	{
		Data.ColorComponentsSRV = GNullColorVertexBuffer.VertexBufferSRV;
		Data.ColorIndexMask = 0;
	}

	if(Data.ColorComponent.VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(Data.ColorComponent,3));
	}
	else
	{
		//If the mesh has no color component, set the null color buffer on a new stream with a stride of 0.
		//This wastes 4 bytes of bandwidth per vertex, but prevents having to compile out twice the number of vertex factories.
		FVertexStreamComponent NullColorComponent(&GNullColorVertexBuffer, 0, 0, VET_Color, EVertexStreamUsage::ManualFetch);
		Elements.Add(AccessStreamComponent(NullColorComponent, 3));
	}

	if(Data.TextureCoordinates.Num())
	{
		const int32 BaseTexCoordAttribute = 4;
		for(int32 CoordinateIndex = 0;CoordinateIndex < Data.TextureCoordinates.Num();CoordinateIndex++)
		{
			Elements.Add(AccessStreamComponent(
				Data.TextureCoordinates[CoordinateIndex],
				BaseTexCoordAttribute + CoordinateIndex
				));
		}

		for(int32 CoordinateIndex = Data.TextureCoordinates.Num(); CoordinateIndex < (InstancedStaticMeshMaxTexCoord + 1) / 2; CoordinateIndex++)
		{
			Elements.Add(AccessStreamComponent(
				Data.TextureCoordinates[Data.TextureCoordinates.Num() - 1],
				BaseTexCoordAttribute + CoordinateIndex
				));
		}
	}

	if(Data.LightMapCoordinateComponent.VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(Data.LightMapCoordinateComponent,15));
	}
	else if(Data.TextureCoordinates.Num())
	{
		Elements.Add(AccessStreamComponent(Data.TextureCoordinates[0],15));
	}

	// toss in the instanced location stream
	check(Data.InstanceOriginComponent.VertexBuffer || !bInstanced);
	if (bInstanced && Data.InstanceOriginComponent.VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(Data.InstanceOriginComponent, 8));
	}

	check(Data.InstanceTransformComponent[0].VertexBuffer || !bInstanced);
	if (bInstanced && Data.InstanceTransformComponent[0].VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(Data.InstanceTransformComponent[0], 9));
		Elements.Add(AccessStreamComponent(Data.InstanceTransformComponent[1], 10));
		Elements.Add(AccessStreamComponent(Data.InstanceTransformComponent[2], 11));
	}

	if (bInstanced && Data.InstanceLightmapAndShadowMapUVBiasComponent.VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(Data.InstanceLightmapAndShadowMapUVBiasComponent,12));
	}

	// we don't need per-vertex shadow or lightmap rendering
	InitDeclaration(Elements);
}


FVertexFactoryShaderParameters* FInstanceBufferMeshVertexFactory::ConstructShaderParameters(EShaderFrequency ShaderFrequency)
{
	return ShaderFrequency == SF_Vertex ? new FInstanceBufferMeshVertexFactoryShaderParameters() : NULL;
}

IMPLEMENT_VERTEX_FACTORY_TYPE_EX(FInstanceBufferMeshVertexFactory,"/Engine/Private/LocalVertexFactory.ush",true,true,true,true,true,true,false);

void FInstanceBufferMeshRenderData::InitVertexFactories()
{
	const bool bInstanced = GRHISupportsInstancing;

	check(bInstanced);

	// Allocate the vertex factories for each LOD
	for (int32 LODIndex = 0; LODIndex < LODModels.Num(); LODIndex++)
	{
		VertexFactories.Add(new FInstanceBufferMeshVertexFactory(FeatureLevel));
	}

	const int32 LightMapCoordinateIndex = Component->GetStaticMesh()->LightMapCoordinateIndex;
	ENQUEUE_RENDER_COMMAND(InstancedStaticMeshRenderData_InitVertexFactories)(
		[this, LightMapCoordinateIndex, bInstanced](FRHICommandListImmediate& RHICmdList)
	{
		for (int32 LODIndex = 0; LODIndex < VertexFactories.Num(); LODIndex++)
		{
			const FStaticMeshLODResources* RenderData = &LODModels[LODIndex];

			FInstanceBufferMeshVertexFactory::FDataType Data;
			// Assign to the vertex factory for this LOD.
			FInstanceBufferMeshVertexFactory& VertexFactory = VertexFactories[LODIndex];

			RenderData->VertexBuffers.PositionVertexBuffer.BindPositionVertexBuffer(&VertexFactory, Data);
			RenderData->VertexBuffers.StaticMeshVertexBuffer.BindTangentVertexBuffer(&VertexFactory, Data);
			RenderData->VertexBuffers.StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(&VertexFactory, Data);
			if (LightMapCoordinateIndex < (int32)RenderData->VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() && LightMapCoordinateIndex >= 0)
			{
				RenderData->VertexBuffers.StaticMeshVertexBuffer.BindLightMapVertexBuffer(&VertexFactory, Data, LightMapCoordinateIndex);
			}
			RenderData->VertexBuffers.ColorVertexBuffer.BindColorVertexBuffer(&VertexFactory, Data);

			if (bInstanced)
			{
				check(PerInstanceRenderData);
				PerInstanceRenderData->InstanceBuffer.BindInstanceVertexBuffer(&VertexFactory, Data);
			}

			VertexFactory.SetData(Data);
			VertexFactory.InitResource();
		}
	});
}

FIBMPerInstanceRenderData::FIBMPerInstanceRenderData(ERHIFeatureLevel::Type InFeaureLevel)
	: ResourceSize(0)
	, InstanceBuffer(InFeaureLevel)
{
	BeginInitResource(&InstanceBuffer);
}
		
FIBMPerInstanceRenderData::~FIBMPerInstanceRenderData()
{
	// Should be always destructed on rendering thread
	InstanceBuffer.ReleaseResource();
}


UNREALGPUSWARM_API void FIBMPerInstanceRenderData::UpdateWithNumInstance(int numInstances)
{
	InstanceBuffer.UpdateWithNumInstances_Concurrent(numInstances);
}



SIZE_T FInstanceBufferMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FInstanceBufferMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_InstancedStaticMeshSceneProxy_GetMeshElements);

	const bool bSelectionRenderEnabled = GIsEditor && ViewFamily.EngineShowFlags.Selection;

	// If the first pass rendered selected instances only, we need to render the deselected instances in a second pass
	const int32 NumSelectionGroups = (bSelectionRenderEnabled && bHasSelectedInstances) ? 2 : 1;

	const FInstancingUserData* PassUserData[2] =
	{
		bHasSelectedInstances && bSelectionRenderEnabled ? &UserData_SelectedInstances : &UserData_AllInstances,
		&UserData_DeselectedInstances
	};

	bool BatchRenderSelection[2] = 
	{
		bSelectionRenderEnabled && IsSelected(),
		false
	};

	const bool bIsWireframe = ViewFamily.EngineShowFlags.Wireframe;

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			const FSceneView* View = Views[ViewIndex];

			for (int32 SelectionGroupIndex = 0; SelectionGroupIndex < NumSelectionGroups; SelectionGroupIndex++)
			{
				const int32 LODIndex = GetLOD(View);
				const FStaticMeshLODResources& LODModel = StaticMesh->RenderData->LODResources[LODIndex];

				for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); SectionIndex++)
				{
					const int32 NumBatches = GetNumMeshBatches();

					for (int32 BatchIndex = 0; BatchIndex < NumBatches; BatchIndex++)
					{
						FMeshBatch& MeshElement = Collector.AllocateMesh();

						if (GetMeshElement(LODIndex, BatchIndex, SectionIndex, GetDepthPriorityGroup(View), BatchRenderSelection[SelectionGroupIndex], true, MeshElement))
						{
							//@todo-rco this is only supporting selection on the first element
							MeshElement.Elements[0].UserData = PassUserData[SelectionGroupIndex];
							MeshElement.Elements[0].bUserDataIsColorVertexBuffer = false;
							MeshElement.bCanApplyViewModeOverrides = true;
							MeshElement.bUseSelectionOutline = BatchRenderSelection[SelectionGroupIndex];
							MeshElement.bUseWireframeSelectionColoring = BatchRenderSelection[SelectionGroupIndex];

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
}

int32 FInstanceBufferMeshSceneProxy::GetNumMeshBatches() const
{
	const bool bInstanced = GRHISupportsInstancing;

	if (bInstanced)
	{
		return 1;
	}
	else
	{
		const uint32 NumInstances = InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetNumInstances();
		const uint32 MaxInstancesPerBatch = FInstanceBufferMeshVertexFactory::NumBitsForVisibilityMask();
		const uint32 NumBatches = FMath::DivideAndRoundUp(NumInstances, MaxInstancesPerBatch);
		return NumBatches;
	}
}

int32 FInstanceBufferMeshSceneProxy::CollectOccluderElements(FOccluderElementsCollector& Collector) const
{
	if (OccluderData)
	{	
		FIBMInstanceBuffer& InstanceBuffer = InstancedRenderData.PerInstanceRenderData->InstanceBuffer;
		const int32 NumInstances = InstanceBuffer.GetNumInstances();
		
		for (int32 InstanceIndex = 0; InstanceIndex < NumInstances; ++InstanceIndex)
		{
			FMatrix InstanceToLocal;
			;	
			InstanceToLocal.M[3][3] = 1.0f;
						
			Collector.AddElements(OccluderData->VerticesSP, OccluderData->IndicesSP, InstanceToLocal * GetLocalToWorld());
		}
		
		return NumInstances;
	}
	
	return 0;
}

void FInstanceBufferMeshSceneProxy::SetupProxy(UInstanceBufferMeshComponent* InComponent)
{
	// Tim: disabled
//#if WITH_EDITOR
//	if (bHasSelectedInstances)
//	{
//		// if we have selected indices, mark scene proxy as selected.
//		SetSelection_GameThread(true);
//	}
//#endif
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

	FVector MinScale(0);
	FVector MaxScale(0);
	InComponent->GetInstancesMinMaxScale(MinScale, MaxScale);

	UserData_AllInstances.AverageInstancesScale = MinScale + (MaxScale - MinScale) / 2.0f;

	// selected only
	UserData_SelectedInstances = UserData_AllInstances;
	UserData_SelectedInstances.bRenderUnselected = false;

	// unselected only
	UserData_DeselectedInstances = UserData_AllInstances;
	UserData_DeselectedInstances.bRenderSelected = false;
}

void FInstanceBufferMeshSceneProxy::SetupInstancedMeshBatch(int32 LODIndex, int32 BatchIndex, FMeshBatch& OutMeshBatch) const
{
	const bool bInstanced = GRHISupportsInstancing;

	// we only support hardware with instancing
	check(bInstanced);

	OutMeshBatch.VertexFactory = &InstancedRenderData.VertexFactories[LODIndex];
	const uint32 NumInstances = InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetNumInstances();
	FMeshBatchElement& BatchElement0 = OutMeshBatch.Elements[0];
	BatchElement0.UserData = (void*)&UserData_AllInstances;
	BatchElement0.bUserDataIsColorVertexBuffer = false;
	BatchElement0.InstancedLODIndex = LODIndex;
	BatchElement0.UserIndex = 0;
	BatchElement0.bIsInstancedMesh = bInstanced;
	BatchElement0.PrimitiveUniformBuffer = GetUniformBuffer();

	BatchElement0.NumInstances = NumInstances;
}

void FInstanceBufferMeshSceneProxy::GetLightRelevance(const FLightSceneProxy* LightSceneProxy, bool& bDynamic, bool& bRelevant, bool& bLightMapped, bool& bShadowMapped) const
{
	FStaticMeshSceneProxy::GetLightRelevance(LightSceneProxy, bDynamic, bRelevant, bLightMapped, bShadowMapped);

	if (InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetNumInstances() == 0)
	{
		bRelevant = false;
	}
}

bool FInstanceBufferMeshSceneProxy::GetShadowMeshElement(int32 LODIndex, int32 BatchIndex, uint8 InDepthPriorityGroup, FMeshBatch& OutMeshBatch, bool bDitheredLODTransition) const
{
	if (LODIndex < InstancedRenderData.VertexFactories.Num() && FStaticMeshSceneProxy::GetShadowMeshElement(LODIndex, BatchIndex, InDepthPriorityGroup, OutMeshBatch, bDitheredLODTransition))
	{
		SetupInstancedMeshBatch(LODIndex, BatchIndex, OutMeshBatch);
		return true;
	}
	return false;
}

/** Sets up a FMeshBatch for a specific LOD and element. */
bool FInstanceBufferMeshSceneProxy::GetMeshElement(int32 LODIndex, int32 BatchIndex, int32 ElementIndex, uint8 InDepthPriorityGroup, bool bUseSelectionOutline, bool bAllowPreCulledIndices, FMeshBatch& OutMeshBatch) const
{
	if (LODIndex < InstancedRenderData.VertexFactories.Num() && FStaticMeshSceneProxy::GetMeshElement(LODIndex, BatchIndex, ElementIndex, InDepthPriorityGroup, bUseSelectionOutline, bAllowPreCulledIndices, OutMeshBatch))
	{
		SetupInstancedMeshBatch(LODIndex, BatchIndex, OutMeshBatch);
		return true;
	}
	return false;
};

/** Sets up a wireframe FMeshBatch for a specific LOD. */
bool FInstanceBufferMeshSceneProxy::GetWireframeMeshElement(int32 LODIndex, int32 BatchIndex, const FMaterialRenderProxy* WireframeRenderProxy, uint8 InDepthPriorityGroup, bool bAllowPreCulledIndices, FMeshBatch& OutMeshBatch) const
{
	if (LODIndex < InstancedRenderData.VertexFactories.Num() && FStaticMeshSceneProxy::GetWireframeMeshElement(LODIndex, BatchIndex, WireframeRenderProxy, InDepthPriorityGroup, bAllowPreCulledIndices, OutMeshBatch))
	{
		SetupInstancedMeshBatch(LODIndex, BatchIndex, OutMeshBatch);
		return true;
	}
	return false;
}

void FInstanceBufferMeshSceneProxy::GetDistancefieldAtlasData(FBox& LocalVolumeBounds, FVector2D& OutDistanceMinMax, FIntVector& OutBlockMin, FIntVector& OutBlockSize, bool& bOutBuiltAsIfTwoSided, bool& bMeshWasPlane, float& SelfShadowBias, TArray<FMatrix>& ObjectLocalToWorldTransforms, bool& bOutThrottled) const
{
	FStaticMeshSceneProxy::GetDistancefieldAtlasData(LocalVolumeBounds, OutDistanceMinMax, OutBlockMin, OutBlockSize, bOutBuiltAsIfTwoSided, bMeshWasPlane, SelfShadowBias, ObjectLocalToWorldTransforms, bOutThrottled);

	ObjectLocalToWorldTransforms.Reset();

	const uint32 NumInstances = InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetNumInstances();
	for (uint32 InstanceIndex = 0; InstanceIndex < NumInstances; InstanceIndex++)
	{
		FMatrix InstanceToLocal;

		// Tim: Hack, we need the position
		InstanceToLocal.M[3][3] = 1.0f;

		ObjectLocalToWorldTransforms.Add(InstanceToLocal * GetLocalToWorld());
	}
}

void FInstanceBufferMeshSceneProxy::GetDistanceFieldInstanceInfo(int32& NumInstances, float& BoundsSurfaceArea) const
{
	NumInstances = DistanceFieldData ? InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetNumInstances() : 0;

	if (NumInstances > 0)
	{
		FMatrix InstanceToLocal;
		const int32 InstanceIndex = 0;

		InstanceToLocal.M[3][3] = 1.0f;

		const FMatrix InstanceTransform = InstanceToLocal * GetLocalToWorld();
		const FVector AxisScales = InstanceTransform.GetScaleVector();
		const FVector BoxDimensions = RenderData->Bounds.BoxExtent * AxisScales * 2;

		BoundsSurfaceArea = 2 * BoxDimensions.X * BoxDimensions.Y
			+ 2 * BoxDimensions.Z * BoxDimensions.Y
			+ 2 * BoxDimensions.X * BoxDimensions.Z;
	}
}

HHitProxy* FInstanceBufferMeshSceneProxy::CreateHitProxies(UPrimitiveComponent* Component,TArray<TRefCountPtr<HHitProxy> >& OutHitProxies)
{
	if(InstancedRenderData.PerInstanceRenderData.IsValid() && InstancedRenderData.PerInstanceRenderData->HitProxies.Num() )
	{
		// Add any per-instance hit proxies.
		OutHitProxies += InstancedRenderData.PerInstanceRenderData->HitProxies;

		// No default hit proxy.
		return nullptr;
	}

	return FStaticMeshSceneProxy::CreateHitProxies(Component, OutHitProxies);
}

#if RHI_RAYTRACING
void FInstanceBufferMeshSceneProxy::GetDynamicRayTracingInstances(struct FRayTracingMaterialGatheringContext& Context, TArray<FRayTracingInstance>& OutRayTracingInstances)
{
	//if (!CVarRayTracingRenderInstances.GetValueOnRenderThread())
	//{
	//	return;
	//}

	//// Tim: Disabling this stuff for now
	//uint32 LOD = GetCurrentFirstLODIdx_RenderThread();
	//const int32 InstanceCount = 0;

	//if (InstanceCount == 0)
	//{
	//	return;
	//}
	////setup a 'template' for the instance first, so we aren't duplicating work
	////#dxr_todo: when multiple LODs are used, template needs to be an array of templates, probably best initialized on-demand via a lamda
	//FRayTracingInstance RayTracingInstanceTemplate;
	//RayTracingInstanceTemplate.Geometry = &RenderData->LODResources[LOD].RayTracingGeometry;

	////preallocate the worst-case to prevent an explosion of reallocs
	////#dxr_todo: possibly track used instances and reserve based on previous behavior
	//RayTracingInstanceTemplate.InstanceTransforms.Reserve(InstanceCount);


	//int32 SectionCount = InstancedRenderData.LODModels[LOD].Sections.Num();

	//for (int32 SectionIdx = 0; SectionIdx < SectionCount; ++SectionIdx)
	//{
	//	//#dxr_todo: so far we use the parent static mesh path to get material data
	//	FMeshBatch MeshBatch;
	//	FStaticMeshSceneProxy::GetMeshElement(LOD, 0, SectionIdx, 0, false, false, MeshBatch);

	//	RayTracingInstanceTemplate.Materials.Add(MeshBatch);
	//}
	//RayTracingInstanceTemplate.BuildInstanceMaskAndFlags();

	//if (CVarRayTracingRenderInstancesCulling.GetValueOnRenderThread() > 0 && RayTracingCullClusterInstances.Num() > 0)
	//{
	//	const float BVHCullRadius = CVarRayTracingInstancesCullClusterRadius.GetValueOnRenderThread();
	//	const float BVHLowScaleThreshold = CVarRayTracingInstancesLowScaleThreshold.GetValueOnRenderThread();
	//	const float BVHLowScaleRadius = CVarRayTracingInstancesLowScaleCullRadius.GetValueOnRenderThread();
	//	const bool ApplyGeneralCulling = BVHCullRadius > 0.0f;
	//	const bool ApplyLowScaleCulling = BVHLowScaleThreshold > 0.0f && BVHLowScaleRadius > 0.0f;
	//	FMatrix ToWorld = InstancedRenderData.Component->GetComponentTransform().ToMatrixWithScale();

	//	// Iterate over all culling clusters
	//	for (int32 ClusterIdx = 0; ClusterIdx < RayTracingCullClusterBoundsMin.Num(); ++ClusterIdx)
	//	{
	//		FVector VClusterBBoxSize = RayTracingCullClusterBoundsMax[ClusterIdx] - RayTracingCullClusterBoundsMin[ClusterIdx];
	//		FVector VClusterCenter = 0.5f * (RayTracingCullClusterBoundsMax[ClusterIdx] + RayTracingCullClusterBoundsMin[ClusterIdx]);
	//		FVector VToClusterCenter = VClusterCenter - Context.ReferenceView->ViewLocation;
	//		float ClusterRadius = 0.5f * VClusterBBoxSize.Size();
	//		float DistToClusterCenter = VToClusterCenter.Size();

	//		// Cull whole cluster if the bounding sphere is too far away
	//		if ((DistToClusterCenter - ClusterRadius) > BVHCullRadius && ApplyGeneralCulling)
	//		{
	//			continue;
	//		}

	//		TDoubleLinkedList< uint32 >* InstanceList = RayTracingCullClusterInstances[ClusterIdx];

	//		// Unroll instances in the current cluster into the array
	//		for (TDoubleLinkedList<uint32>::TDoubleLinkedListNode* InstancePtr = InstanceList->GetHead(); InstancePtr != nullptr; InstancePtr = InstancePtr->GetNextNode())
	//		{
	//			const uint32 Instance = InstancePtr->GetValue();

	//			if (InstancedRenderData.Component->PerInstanceSMData.IsValidIndex(Instance))
	//			{
	//				const FIBMInstanceData& InstanceData = InstancedRenderData.Component->PerInstanceSMData[Instance];
	//				FMatrix InstanceTransform = InstanceData.Transform * ToWorld;
	//				FVector InstanceLocation = InstanceTransform.TransformPosition({ 0.0f,0.0f,0.0f });
	//				FVector VToInstanceCenter = Context.ReferenceView->ViewLocation - InstanceLocation;
	//				float   DistanceToInstanceCenter = VToInstanceCenter.Size();

	//				FVector VMin, VMax, VDiag;
	//				InstancedRenderData.Component->GetLocalBounds(VMin, VMax);
	//				VMin = InstanceTransform.TransformPosition(VMin);
	//				VMax = InstanceTransform.TransformPosition(VMax);
	//				VDiag = VMax - VMin;

	//				float InstanceRadius = 0.5f * VDiag.Size();
	//				float DistanceToInstanceStart = DistanceToInstanceCenter - InstanceRadius;

	//				// Cull instance based on distance
	//				if (DistanceToInstanceStart > BVHCullRadius && ApplyGeneralCulling)
	//					continue;

	//				// Special culling for small scale objects
	//				if (InstanceRadius < BVHLowScaleThreshold && ApplyLowScaleCulling)
	//				{
	//					if (DistanceToInstanceStart > BVHLowScaleRadius)
	//						continue;
	//				}

	//				RayTracingInstanceTemplate.InstanceTransforms.Add(InstanceTransform);
	//			}
	//		}
	//	}
	//}
	//else
	//{
	//	// No culling
	//	for (int32 InstanceIdx = 0; InstanceIdx < InstanceCount; ++InstanceIdx)
	//	{
	//		if (InstancedRenderData.Component->PerInstanceSMData.IsValidIndex(InstanceIdx))
	//		{
	//			const FIBMInstanceData& InstanceData = InstancedRenderData.Component->PerInstanceSMData[InstanceIdx];
	//			FMatrix ComponentLocalToWorld = InstancedRenderData.Component->GetComponentTransform().ToMatrixWithScale();
	//			FMatrix InstanceTransform = InstanceData.Transform * ComponentLocalToWorld;

	//			RayTracingInstanceTemplate.InstanceTransforms.Add(InstanceTransform);
	//		}
	//	}
	//}

	//OutRayTracingInstances.Add(RayTracingInstanceTemplate);
}

void FInstanceBufferMeshSceneProxy::SetupRayTracingCullClusters()
{
	////#dxr_todo: select the appropriate LOD depending on Context.View
	//int32 LOD = 0;
	//if (RenderData->LODResources.Num() > LOD && RenderData->LODResources[LOD].RayTracingGeometry.IsInitialized())
	//{
	//	const float MaxClusterRadiusMultiplier = CVarRayTracingInstancesCullClusterMaxRadiusMultiplier.GetValueOnAnyThread();
	//	const int32 Batches = GetNumMeshBatches();
	//	const int32 InstanceCount = InstancedRenderData.Component->PerInstanceSMData.Num();
	//	int32 ClusterIndex = 0;
	//	FMatrix ComponentLocalToWorld = InstancedRenderData.Component->GetComponentTransform().ToMatrixWithScale();
	//	float MaxInstanceRadius = 0.0f;

	//	// Init first cluster
	//	RayTracingCullClusterInstances.Add(new TDoubleLinkedList< uint32>());
	//	RayTracingCullClusterBoundsMin.Add(FVector(MAX_FLT, MAX_FLT, MAX_FLT));
	//	RayTracingCullClusterBoundsMax.Add(FVector(-MAX_FLT, -MAX_FLT, -MAX_FLT));

	//	// Traverse instances to find maximum rarius
	//	for (int32 Instance = 0; Instance < InstanceCount; ++Instance)
	//	{
	//		if (InstancedRenderData.Component->PerInstanceSMData.IsValidIndex(Instance))
	//		{
	//			const FIBMInstanceData& InstanceData = InstancedRenderData.Component->PerInstanceSMData[Instance];
	//			FMatrix InstanceTransform = InstanceData.Transform * ComponentLocalToWorld;
	//			FVector VMin, VMax;

	//			InstancedRenderData.Component->GetLocalBounds(VMin, VMax);
	//			VMin = InstanceTransform.TransformPosition(VMin);
	//			VMax = InstanceTransform.TransformPosition(VMax);

	//			FVector VBBoxSize = VMax - VMin;

	//			MaxInstanceRadius = FMath::Max(0.5f * VBBoxSize.Size(), MaxInstanceRadius);
	//		}
	//	}

	//	float MaxClusterRadius = MaxInstanceRadius * MaxClusterRadiusMultiplier;

	//	// Build clusters
	//	for (int32 Instance = 0; Instance < InstanceCount; ++Instance)
	//	{
	//		if (InstancedRenderData.Component->PerInstanceSMData.IsValidIndex(Instance))
	//		{
	//			const FIBMInstanceData& InstanceData = InstancedRenderData.Component->PerInstanceSMData[Instance];
	//			FMatrix InstanceTransform = InstanceData.Transform * ComponentLocalToWorld;
	//			FVector InstanceLocation = InstanceTransform.TransformPosition({ 0.0f,0.0f,0.0f });
	//			FVector VMin = InstanceLocation - FVector(MaxInstanceRadius, MaxInstanceRadius, MaxInstanceRadius);
	//			FVector VMax = InstanceLocation + FVector(MaxInstanceRadius, MaxInstanceRadius, MaxInstanceRadius);
	//			bool bClusterFound = false;

	//			// Try to find suitable cluster
	//			for (int32 CandidateCluster = 0; CandidateCluster <= ClusterIndex; ++CandidateCluster)
	//			{
	//				// Build new candidate cluster bounds
	//				FVector VCandidateMin = VMin.ComponentMin(RayTracingCullClusterBoundsMin[CandidateCluster]);
	//				FVector VCandidateMax = VMax.ComponentMax(RayTracingCullClusterBoundsMax[CandidateCluster]);

	//				FVector VCandidateBBoxSize = VCandidateMax - VCandidateMin;
	//				float MaxCandidateRadius = 0.5f * VCandidateBBoxSize.Size();

	//				// If new candidate is still small enough, update current cluster
	//				if (MaxCandidateRadius <= MaxClusterRadius)
	//				{
	//					RayTracingCullClusterInstances[CandidateCluster]->AddTail(Instance);
	//					RayTracingCullClusterBoundsMin[CandidateCluster] = VCandidateMin;
	//					RayTracingCullClusterBoundsMax[CandidateCluster] = VCandidateMax;
	//					bClusterFound = true;
	//					break;
	//				}
	//			}

	//			// if we couldn't add the instance to an existing cluster create a new one
	//			if (!bClusterFound)
	//			{
	//				++ClusterIndex;
	//				RayTracingCullClusterInstances.Add(new TDoubleLinkedList< uint32>());
	//				RayTracingCullClusterInstances[ClusterIndex]->AddTail(Instance);
	//				RayTracingCullClusterBoundsMin.Add(VMin);
	//				RayTracingCullClusterBoundsMax.Add(VMax);
	//			}
	//		}
	//	}
	//}
}

#endif


/*-----------------------------------------------------------------------------
	UInstanceBufferMeshComponent
-----------------------------------------------------------------------------*/

UInstanceBufferMeshComponent::UInstanceBufferMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Mobility = EComponentMobility::Movable;
	BodyInstance.bSimulatePhysics = false;

	bDisallowMeshPaintPerInstance = true;
}

UInstanceBufferMeshComponent::UInstanceBufferMeshComponent(FVTableHelper& Helper)
	: Super(Helper)
{
}

UInstanceBufferMeshComponent::~UInstanceBufferMeshComponent()
{
	ReleasePerInstanceRenderData();
}

TStructOnScope<FActorComponentInstanceData> UInstanceBufferMeshComponent::GetComponentInstanceData() const
{
	TStructOnScope<FActorComponentInstanceData> InstanceData;

	return InstanceData;
}

void UInstanceBufferMeshComponent::ApplyComponentInstanceData(FIBMComponentInstanceData* InstancedMeshData)
{

}

void UInstanceBufferMeshComponent::SetNumInstances(int numInstances)
{
	check(numInstances > 0);

	if (numInstances == _numInstances)
		return;

	// this will trigger a rebuild of the scene proxy

	_numInstances = numInstances;

	// Force recreation of the render data
	MarkRenderStateDirty();
}

int32 UInstanceBufferMeshComponent::GetNumInstancesCurrentlyAllocated() const
{
	return PerInstanceRenderData->InstanceBuffer.GetNumInstances();
}



FPrimitiveSceneProxy* UInstanceBufferMeshComponent::CreateSceneProxy()
{
	LLM_SCOPE(ELLMTag::InstancedMesh);
	ProxySize = 0;

	// Verify that the mesh is valid before using it.
	const bool bMeshIsValid = 
		// make sure we have instances
		_numInstances > 0 &&
		// make sure we have an actual staticmesh
		GetStaticMesh() &&
		GetStaticMesh()->HasValidRenderData() &&
		// You really can't use hardware instancing on the consoles with multiple elements because they share the same index buffer. 
		// @todo: Level error or something to let LDs know this
		1;//GetStaticMesh()->LODModels(0).Elements.Num() == 1;

	if(bMeshIsValid)
	{
		check(InstancingRandomSeed != 0);
		
		CreateHitProxyData(PerInstanceRenderData->HitProxies);

		PerInstanceRenderData->UpdateWithNumInstance(_numInstances);
		
		ProxySize = PerInstanceRenderData->ResourceSize;
		return ::new FInstanceBufferMeshSceneProxy(this, GetWorld()->FeatureLevel);
	}
	else
	{
		return NULL;
	}
}

void UInstanceBufferMeshComponent::CreateHitProxyData(TArray<TRefCountPtr<HHitProxy>>& HitProxies)
{
	if (GIsEditor && bHasPerInstanceHitProxies)
	{
		//QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_CreateHitProxyData);
		//
		//int32 NumProxies = PerInstanceSMData.Num();
		//HitProxies.Empty(NumProxies);

		//for (int32 InstanceIdx = 0; InstanceIdx < NumProxies; ++InstanceIdx)
		//{
		//	HitProxies.Add(new HInstanceBufferMeshInstance(this, InstanceIdx));
		//}
	}
	else
	{
		HitProxies.Empty();
	}
}

void UInstanceBufferMeshComponent::BuildRenderData(TArray<TRefCountPtr<HHitProxy>>& OutHitProxies)
{
	LLM_SCOPE(ELLMTag::InstancedMesh);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_BuildRenderData);

	CreateHitProxyData(OutHitProxies);
}

bool UInstanceBufferMeshComponent::CanEditSimulatePhysics()
{
	return false;
}

FBoxSphereBounds UInstanceBufferMeshComponent::CalcBounds(const FTransform& BoundTransform) const
{
	return FBoxSphereBounds(BoundTransform.GetLocation(), FVector(500000.f), 1000000.f);

	//if(GetStaticMesh() && PerInstanceSMData.Num() > 0)
	//{
	//	FMatrix BoundTransformMatrix = BoundTransform.ToMatrixWithScale();

	//	FBoxSphereBounds RenderBounds = GetStaticMesh()->GetBounds();
	//	FBoxSphereBounds NewBounds = RenderBounds.TransformBy(PerInstanceSMData[0].Transform * BoundTransformMatrix);

	//	for (int32 InstanceIndex = 1; InstanceIndex < PerInstanceSMData.Num(); InstanceIndex++)
	//	{
	//		NewBounds = NewBounds + RenderBounds.TransformBy(PerInstanceSMData[InstanceIndex].Transform * BoundTransformMatrix);
	//	}

	//	return NewBounds;
	//}
	//else
	//{
	//	return FBoxSphereBounds(BoundTransform.GetLocation(), FVector::ZeroVector, 0.f);
	//}
}

#if WITH_EDITOR
void UInstanceBufferMeshComponent::GetStaticLightingInfo(FStaticLightingPrimitiveInfo& OutPrimitiveInfo, const TArray<ULightComponent*>& InRelevantLights, const FLightingBuildOptions& Options)
{
	// Tim: There are more classes we have to port to support this. We don't bake our lighting anyways.

	//if (HasValidSettingsForStaticLighting(false))
	//{
	//	// create static lighting for LOD 0
	//	int32 LightMapWidth = 0;
	//	int32 LightMapHeight = 0;
	//	GetLightMapResolution(LightMapWidth, LightMapHeight);

	//	bool bFit = false;
	//	bool bReduced = false;
	//	while (1)
	//	{
	//		const int32 OneLessThanMaximumSupportedResolution = 1 << (GMaxTextureMipCount - 2);

	//		const int32 MaxInstancesInMaxSizeLightmap = (OneLessThanMaximumSupportedResolution / LightMapWidth) * ((OneLessThanMaximumSupportedResolution / 2) / LightMapHeight);
	//		if (PerInstanceSMData.Num() > MaxInstancesInMaxSizeLightmap)
	//		{
	//			if (LightMapWidth < 4 || LightMapHeight < 4)
	//			{
	//				break;
	//			}
	//			LightMapWidth /= 2;
	//			LightMapHeight /= 2;
	//			bReduced = true;
	//		}
	//		else
	//		{
	//			bFit = true;
	//			break;
	//		}
	//	}

	//	if (!bFit)
	//	{
	//		FMessageLog("LightingResults").Message(EMessageSeverity::Error)
	//			->AddToken(FUObjectToken::Create(this))
	//			->AddToken(FTextToken::Create(NSLOCTEXT("InstancedStaticMesh", "FailedStaticLightingWarning", "The total lightmap size for this InstancedStaticMeshComponent is too big no matter how much we reduce the per-instance size, the number of mesh instances in this component must be reduced")));
	//		return;
	//	}
	//	if (bReduced)
	//	{
	//		FMessageLog("LightingResults").Message(EMessageSeverity::Warning)
	//			->AddToken(FUObjectToken::Create(this))
	//			->AddToken(FTextToken::Create(NSLOCTEXT("InstancedStaticMesh", "ReducedStaticLightingWarning", "The total lightmap size for this InstancedStaticMeshComponent was too big and it was automatically reduced. Consider reducing the component's lightmap resolution or number of mesh instances in this component")));
	//	}

	//	const int32 LightMapSize = GetWorld()->GetWorldSettings()->PackedLightAndShadowMapTextureSize;
	//	const int32 MaxInstancesInDefaultSizeLightmap = (LightMapSize / LightMapWidth) * ((LightMapSize / 2) / LightMapHeight);
	//	if (PerInstanceSMData.Num() > MaxInstancesInDefaultSizeLightmap)
	//	{
	//		FMessageLog("LightingResults").Message(EMessageSeverity::Warning)
	//			->AddToken(FUObjectToken::Create(this))
	//			->AddToken(FTextToken::Create(NSLOCTEXT("InstancedStaticMesh", "LargeStaticLightingWarning", "The total lightmap size for this InstancedStaticMeshComponent is large, consider reducing the component's lightmap resolution or number of mesh instances in this component")));
	//	}

	//	// TODO: Support separate static lighting in LODs for instanced meshes.

	//	if (!GetStaticMesh()->CanLODsShareStaticLighting())
	//	{
	//		//TODO: Detect if the UVs for all sub-LODs overlap the base LOD UVs and omit this warning if they do.
	//		FMessageLog("LightingResults").Message(EMessageSeverity::Warning)
	//			->AddToken(FUObjectToken::Create(this))
	//			->AddToken(FTextToken::Create(NSLOCTEXT("InstancedStaticMesh", "UniqueStaticLightingForLODWarning", "Instanced meshes don't yet support unique static lighting for each LOD. Lighting on LOD 1+ may be incorrect unless lightmap UVs are the same for all LODs.")));
	//	}

	//	// Force sharing LOD 0 lightmaps for now.
	//	int32 NumLODs = 1;

	//	CachedMappings.Reset(PerInstanceSMData.Num() * NumLODs);
	//	CachedMappings.AddZeroed(PerInstanceSMData.Num() * NumLODs);

	//	NumPendingLightmaps = 0;

	//	for (int32 LODIndex = 0; LODIndex < NumLODs; LODIndex++)
	//	{
	//		const FStaticMeshLODResources& LODRenderData = GetStaticMesh()->RenderData->LODResources[LODIndex];

	//		for (int32 InstanceIndex = 0; InstanceIndex < PerInstanceSMData.Num(); InstanceIndex++)
	//		{
	//			auto* StaticLightingMesh = new FStaticLightingMesh_InstancedStaticMesh(this, LODIndex, InstanceIndex, InRelevantLights);
	//			OutPrimitiveInfo.Meshes.Add(StaticLightingMesh);

	//			auto* InstancedMapping = new FStaticLightingTextureMapping_InstanceBufferMesh(this, LODIndex, InstanceIndex, StaticLightingMesh, LightMapWidth, LightMapHeight, GetStaticMesh()->LightMapCoordinateIndex, true);
	//			OutPrimitiveInfo.Mappings.Add(InstancedMapping);

	//			CachedMappings[LODIndex * PerInstanceSMData.Num() + InstanceIndex].Mapping = InstancedMapping;
	//			NumPendingLightmaps++;
	//		}

	//		// Shrink LOD texture lightmaps by half for each LOD level (minimum 4x4 px)
	//		LightMapWidth  = FMath::Max(LightMapWidth  / 2, 4);
	//		LightMapHeight = FMath::Max(LightMapHeight / 2, 4);
	//	}
	//}
}

void UInstanceBufferMeshComponent::ApplyLightMapping(FStaticLightingTextureMapping_InstanceBufferMesh* InMapping, ULevel* LightingScenario)
{
	// Tim: There are more classes we have to port to support this. We don't bake our lighting anyways.

	//static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.VirtualTexturedLightmaps"));
	//const bool bUseVirtualTextures = (CVar->GetValueOnAnyThread() != 0) && UseVirtualTexturing(GMaxRHIFeatureLevel);

	//NumPendingLightmaps--;

	//if (NumPendingLightmaps == 0)
	//{
	//	// Calculate the range of each coefficient in this light-map and repack the data to have the same scale factor and bias across all instances
	//	// TODO: Per instance scale?

	//	// generate the final lightmaps for all the mappings for this component
	//	TArray<TUniquePtr<FQuantizedLightmapData>> AllQuantizedData;
	//	for (auto& MappingInfo : CachedMappings)
	//	{
	//		FStaticLightingTextureMapping_InstanceBufferMesh* Mapping = MappingInfo.Mapping;
	//		AllQuantizedData.Add(MoveTemp(Mapping->QuantizedData));
	//	}

	//	bool bNeedsShadowMap = false;
	//	TArray<TMap<ULightComponent*, TUniquePtr<FShadowMapData2D>>> AllShadowMapData;
	//	for (auto& MappingInfo : CachedMappings)
	//	{
	//		FStaticLightingTextureMapping_InstanceBufferMesh* Mapping = MappingInfo.Mapping;
	//		bNeedsShadowMap = bNeedsShadowMap || (Mapping->ShadowMapData.Num() > 0);
	//		AllShadowMapData.Add(MoveTemp(Mapping->ShadowMapData));
	//	}

	//	UStaticMesh* ResolvedMesh = GetStaticMesh();
	//	if (LODData.Num() != ResolvedMesh->GetNumLODs())
	//	{
	//		MarkPackageDirty();
	//	}

	//	// Ensure LODData has enough entries in it, free not required.
	//	SetLODDataCount(ResolvedMesh->GetNumLODs(), ResolvedMesh->GetNumLODs());

	//	ULevel* StorageLevel = LightingScenario ? LightingScenario : GetOwner()->GetLevel();
	//	UMapBuildDataRegistry* Registry = StorageLevel->GetOrCreateMapBuildData();
	//	FMeshMapBuildData& MeshBuildData = Registry->AllocateMeshBuildData(LODData[0].MapBuildDataId, true);

	//	MeshBuildData.PerInstanceLightmapData.Empty(AllQuantizedData.Num());
	//	MeshBuildData.PerInstanceLightmapData.AddZeroed(AllQuantizedData.Num());

	//	// Create a light-map for the primitive.
	//	// When using VT, shadow map data is included with lightmap allocation
	//	const ELightMapPaddingType PaddingType = GAllowLightmapPadding ? LMPT_NormalPadding : LMPT_NoPadding;
	//	TArray<TMap<ULightComponent*, TUniquePtr<FShadowMapData2D>>> EmptyShadowMapData;
	//	TRefCountPtr<FLightMap2D> NewLightMap = FLightMap2D::AllocateInstancedLightMap(Registry, this,
	//		MoveTemp(AllQuantizedData),
	//		bUseVirtualTextures ? MoveTemp(AllShadowMapData) : MoveTemp(EmptyShadowMapData),
	//		Registry, LODData[0].MapBuildDataId, Bounds, PaddingType, LMF_Streamed);

	//	// Create a shadow-map for the primitive, only needed when not using VT
	//	TRefCountPtr<FShadowMap2D> NewShadowMap = (bNeedsShadowMap && !bUseVirtualTextures)
	//		? FShadowMap2D::AllocateInstancedShadowMap(Registry, this, MoveTemp(AllShadowMapData), Registry, LODData[0].MapBuildDataId, Bounds, PaddingType, SMF_Streamed)
	//		: nullptr;

	//	MeshBuildData.LightMap = NewLightMap;
	//	MeshBuildData.ShadowMap = NewShadowMap;

	//	// Build the list of statically irrelevant lights.
	//	// TODO: This should be stored per LOD.
	//	TSet<FGuid> RelevantLights;
	//	TSet<FGuid> PossiblyIrrelevantLights;
	//	for (auto& MappingInfo : CachedMappings)
	//	{
	//		for (const ULightComponent* Light : MappingInfo.Mapping->Mesh->RelevantLights)
	//		{
	//			// Check if the light is stored in the light-map.
	//			const bool bIsInLightMap = MeshBuildData.LightMap && MeshBuildData.LightMap->LightGuids.Contains(Light->LightGuid);

	//			// Check if the light is stored in the shadow-map.
	//			const bool bIsInShadowMap = MeshBuildData.ShadowMap && MeshBuildData.ShadowMap->LightGuids.Contains(Light->LightGuid);

	//			// If the light isn't already relevant to another mapping, add it to the potentially irrelevant list
	//			if (!bIsInLightMap && !bIsInShadowMap && !RelevantLights.Contains(Light->LightGuid))
	//			{
	//				PossiblyIrrelevantLights.Add(Light->LightGuid);
	//			}

	//			// Light is relevant
	//			if (bIsInLightMap || bIsInShadowMap)
	//			{
	//				RelevantLights.Add(Light->LightGuid);
	//				PossiblyIrrelevantLights.Remove(Light->LightGuid);
	//			}
	//		}
	//	}

	//	MeshBuildData.IrrelevantLights = PossiblyIrrelevantLights.Array();

	//	// Force recreation of the render data
	//	InstanceUpdateCmdBuffer.Edit();
	//	MarkRenderStateDirty();
	//}
}
#endif

void UInstanceBufferMeshComponent::ReleasePerInstanceRenderData()
{
	if (PerInstanceRenderData.IsValid())
	{
		typedef TSharedPtr<FIBMPerInstanceRenderData, ESPMode::ThreadSafe> FPerInstanceRenderDataPtr;

		PerInstanceRenderData->HitProxies.Empty();

		// Make shared pointer object on the heap
		FPerInstanceRenderDataPtr* CleanupRenderDataPtr = new FPerInstanceRenderDataPtr(PerInstanceRenderData);
		PerInstanceRenderData.Reset();

		FPerInstanceRenderDataPtr* InCleanupRenderDataPtr = CleanupRenderDataPtr;
		ENQUEUE_RENDER_COMMAND(FReleasePerInstanceRenderData)(
			[InCleanupRenderDataPtr](FRHICommandList& RHICmdList)
			{
				// Destroy the shared pointer object we allocated on the heap.
				// Resource will either be released here or by scene proxy on the render thread, whoever gets executed last
				delete InCleanupRenderDataPtr;
			});
	} //-V773
}

void UInstanceBufferMeshComponent::PropagateLightingScenarioChange()
{
	FComponentRecreateRenderStateContext Context(this);

	// Force recreation of the render data
	MarkRenderStateDirty();
}

void UInstanceBufferMeshComponent::GetLightAndShadowMapMemoryUsage( int32& LightMapMemoryUsage, int32& ShadowMapMemoryUsage ) const
{
	Super::GetLightAndShadowMapMemoryUsage(LightMapMemoryUsage, ShadowMapMemoryUsage);

	int32 NumInstances = GetNumInstancesCurrentlyAllocated();

	// Scale lighting demo by number of instances
	LightMapMemoryUsage *= NumInstances;
	ShadowMapMemoryUsage *= NumInstances;
}

static bool NeedRenderDataForTargetPlatform(const ITargetPlatform* TargetPlatform)
{
#if WITH_EDITOR
	const UDeviceProfile* DeviceProfile = UDeviceProfileManager::Get().FindProfile(TargetPlatform->IniPlatformName());
	if (DeviceProfile)
	{
		int32 CVarFoliageSaveRenderData = 1;
		if (DeviceProfile->GetConsolidatedCVarValue(TEXT("foliage.SaveRenderData"), CVarFoliageSaveRenderData))
		{
			return CVarFoliageSaveRenderData != 0;
		}
	}
#endif // WITH_EDITOR
	return true;
}

void UInstanceBufferMeshComponent::SerializeRenderData(FArchive& Ar)
{

}

void UInstanceBufferMeshComponent::Serialize(FArchive& Ar)
{
	LLM_SCOPE(ELLMTag::InstancedMesh);
	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FMobileObjectVersion::GUID);
	Ar.UsingCustomVersion(FFortniteMainBranchObjectVersion::GUID);
	Ar.UsingCustomVersion(FEditorObjectVersion::GUID);	
	
	bool bCooked = Ar.IsCooking();
	if (Ar.CustomVer(FFortniteMainBranchObjectVersion::GUID) >= FFortniteMainBranchObjectVersion::SerializeInstancedStaticMeshRenderData || Ar.CustomVer(FEditorObjectVersion::GUID) >= FEditorObjectVersion::SerializeInstancedStaticMeshRenderData)
	{
		Ar << bCooked;
	}

	if (bCooked && (Ar.CustomVer(FFortniteMainBranchObjectVersion::GUID) >= FFortniteMainBranchObjectVersion::SerializeInstancedStaticMeshRenderData || Ar.CustomVer(FEditorObjectVersion::GUID) >= FEditorObjectVersion::SerializeInstancedStaticMeshRenderData))
	{
		SerializeRenderData(Ar);
	}

}

void UInstanceBufferMeshComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	// We are handling the physics move below, so don't handle it at higher levels
	Super::OnUpdateTransform(UpdateTransformFlags | EUpdateTransformFlags::SkipPhysicsUpdate, Teleport);
}

bool UInstanceBufferMeshComponent::ShouldCreatePhysicsState() const
{
	return false;
}

float UInstanceBufferMeshComponent::GetTextureStreamingTransformScale() const
{
	// By default if there are no per instance data, use a scale of 1.
	// This is required because some derived class use the instancing system without filling the per instance data. (like landscape grass)
	// In those cases, we assume the instance are spreaded across the bounds with a scale of 1.
	float TransformScale = 1.f; 

	//if (PerInstanceSMData.Num() > 0)
	//{
	//	TransformScale = Super::GetTextureStreamingTransformScale();

	//	float WeightedAxisScaleSum = 0;
	//	float WeightSum = 0;

	//	for (int32 InstanceIndex = 0; InstanceIndex < PerInstanceSMData.Num(); InstanceIndex++)
	//	{
	//		const float AxisScale = PerInstanceSMData[InstanceIndex].Transform.GetMaximumAxisScale();
	//		const float Weight = AxisScale; // The weight is the axis scale since we want to weight by surface coverage.
	//		WeightedAxisScaleSum += AxisScale * Weight;
	//		WeightSum += Weight;
	//	}

	//	if (WeightSum > SMALL_NUMBER)
	//	{
	//		TransformScale *= WeightedAxisScaleSum / WeightSum;
	//	}
	//}
	return TransformScale;
}

bool UInstanceBufferMeshComponent::GetMaterialStreamingData(int32 MaterialIndex, FPrimitiveMaterialInfo& MaterialData) const
{
	// Same thing as StaticMesh but we take the full bounds to cover the instances.
	if (GetStaticMesh())
	{
		MaterialData.Material = GetMaterial(MaterialIndex);
		MaterialData.UVChannelData = GetStaticMesh()->GetUVChannelData(MaterialIndex);
		MaterialData.PackedRelativeBox = PackedRelativeBox_Identity;
	}
	return MaterialData.IsValid();
}

bool UInstanceBufferMeshComponent::BuildTextureStreamingData(ETextureStreamingBuildType BuildType, EMaterialQualityLevel::Type QualityLevel, ERHIFeatureLevel::Type FeatureLevel, TSet<FGuid>& DependentResources)
{
#if WITH_EDITORONLY_DATA // Only rebuild the data in editor 
	if (GetInstanceCount() > 0)
	{
		return Super::BuildTextureStreamingData(BuildType, QualityLevel, FeatureLevel, DependentResources);
	}
#endif
	return true;
}

void UInstanceBufferMeshComponent::GetStreamingRenderAssetInfo(FStreamingTextureLevelContext& LevelContext, TArray<FStreamingRenderAssetPrimitiveInfo>& OutStreamingRenderAssets) const
{
	// Don't only look the instance count but also if the bound is valid, as derived classes might not set PerInstanceSMData.
	if (GetInstanceCount() > 0 || Bounds.SphereRadius > 0)
	{
		return Super::GetStreamingRenderAssetInfo(LevelContext, OutStreamingRenderAssets);
	}
}



int32 UInstanceBufferMeshComponent::GetInstanceCount() const
{
	return GetNumInstancesCurrentlyAllocated();
}

void UInstanceBufferMeshComponent::SetCullDistances(int32 StartCullDistance, int32 EndCullDistance)
{
	InstanceStartCullDistance = StartCullDistance;
	InstanceEndCullDistance = EndCullDistance;
	MarkRenderStateDirty();
}


static bool ComponentRequestsCPUAccess(UInstanceBufferMeshComponent* InComponent, ERHIFeatureLevel::Type FeatureLevel)
{
	if (FeatureLevel > ERHIFeatureLevel::ES3_1)
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.GenerateMeshDistanceFields"));

		const bool bNeedsCPUAccess = (InComponent->CastShadow && InComponent->bAffectDistanceFieldLighting 
			// Distance field algorithms need access to instance data on the CPU
			&& (CVar->GetValueOnAnyThread(true) != 0 || (InComponent->GetStaticMesh() && InComponent->GetStaticMesh()->bGenerateMeshDistanceField)));

		return bNeedsCPUAccess;
	}
	return false;
}

void UInstanceBufferMeshComponent::GetInstancesMinMaxScale(FVector& MinScale, FVector& MaxScale) const
{
	MinScale = FVector(1.0f);
	MaxScale = FVector(1.0f);

	// Tim: We should read this from the GPU
}

void UInstanceBufferMeshComponent::InitPerInstanceRenderData()
{
	if (PerInstanceRenderData.IsValid())
	{
		return;
	}

	LLM_SCOPE(ELLMTag::InstancedMesh);

	// If we don't have a random seed for this instanced static mesh component yet, then go ahead and
	// generate one now.  This will be saved with the static mesh component and used for future generation
	// of random numbers for this component's instances. (Used by the PerInstanceRandom material expression)
	while (InstancingRandomSeed == 0)
	{
		InstancingRandomSeed = FMath::Rand();
	}

	UWorld* World = GetWorld();
	ERHIFeatureLevel::Type FeatureLevel = World != nullptr ? World->FeatureLevel.GetValue() : GMaxRHIFeatureLevel;

	{
		TArray<TRefCountPtr<HHitProxy>> HitProxies;
		
		CreateHitProxyData(HitProxies);
			
		PerInstanceRenderData = MakeShareable(new FIBMPerInstanceRenderData(FeatureLevel));
		PerInstanceRenderData->HitProxies = MoveTemp(HitProxies);
	}
}

void UInstanceBufferMeshComponent::OnComponentCreated()
{
	Super::OnComponentCreated();

	if (FApp::CanEverRender() && !HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		// if we are pasting/duplicating this component, it may be created with some instances already in place
		// in this case, need to ensure that the instance render data is properly created
		// We only need to only init from current data if the reorder table == per instance data, but only for the HISM Component, in the case of ISM, the reorder table is never used.
		InitPerInstanceRenderData();
	}
}

void UInstanceBufferMeshComponent::PostLoad()
{
	Super::PostLoad();

	// Has different implementation in HISMC
	OnPostLoadPerInstanceData();
}

void UInstanceBufferMeshComponent::OnPostLoadPerInstanceData()
{
	if (!HasAnyFlags(RF_ClassDefaultObject|RF_ArchetypeObject))
	{
		// create PerInstanceRenderData and pass InstanceDataBuffers ownership to it
		InitPerInstanceRenderData();
	}
}

void UInstanceBufferMeshComponent::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);

	if (PerInstanceRenderData.IsValid())
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(PerInstanceRenderData->ResourceSize); 
	}
}

void UInstanceBufferMeshComponent::BeginDestroy()
{
	ReleasePerInstanceRenderData();
	Super::BeginDestroy();
}

void UInstanceBufferMeshComponent::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	if (!HasAnyFlags(RF_ClassDefaultObject|RF_ArchetypeObject) && bDuplicateForPIE)
	{
		InitPerInstanceRenderData();		
	}
}

#if WITH_EDITOR
void UInstanceBufferMeshComponent::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent)
{
	if(PropertyChangedEvent.Property != NULL)
	{
		if (PropertyChangedEvent.Property->GetFName() == "Transform")
		{
			// Force recreation of the render data
			MarkRenderStateDirty();
		}
	}
	Super::PostEditChangeChainProperty(PropertyChangedEvent);
}

void UInstanceBufferMeshComponent::PostEditUndo()
{
	Super::PostEditUndo();

	FNavigationSystem::UpdateComponentData(*this);
}
#endif

bool UInstanceBufferMeshComponent::IsInstanceSelected(int32 InInstanceIndex) const
{
#if WITH_EDITOR
	if(SelectedInstances.IsValidIndex(InInstanceIndex))
	{
		return SelectedInstances[InInstanceIndex];
	}
#endif

	return false;
}

void UInstanceBufferMeshComponent::SelectInstance(bool bInSelected, int32 InInstanceIndex, int32 InInstanceCount)
{
#if WITH_EDITOR

#endif
}

void UInstanceBufferMeshComponent::ClearInstanceSelection()
{
#if WITH_EDITOR

#endif
}

static TAutoConsoleVariable<int32> CVarCullAllInVertexShader(
	TEXT("foliage.CullAllInVertexShader"),
	0,
	TEXT("Debugging, if this is greater than 0, cull all instances in the vertex shader."));

// Copied from from FLocalVertexFactoryShaderParametersBase because it wasn't declared part of the ENGINE_API >:
void FInstanceBufferMeshVertexFactoryShaderParameters::GetElementShaderBindingsBase(const FSceneInterface* Scene, const FSceneView* View, const FMeshMaterialShader* Shader, const EVertexInputStreamType InputStreamType, ERHIFeatureLevel::Type FeatureLevel, const FVertexFactory* VertexFactory, const FMeshBatchElement& BatchElement, FRHIUniformBuffer* VertexFactoryUniformBuffer, FMeshDrawSingleShaderBindings& ShaderBindings, FVertexInputStreamArray& VertexStreams) const
{
	const auto* LocalVertexFactory = static_cast<const FLocalVertexFactory*>(VertexFactory);

	if (LocalVertexFactory->SupportsManualVertexFetch(FeatureLevel) || UseGPUScene(GMaxRHIShaderPlatform, FeatureLevel))
	{
		if (!VertexFactoryUniformBuffer)
		{
			// No batch element override
			VertexFactoryUniformBuffer = LocalVertexFactory->GetUniformBuffer();
		}

		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLocalVertexFactoryUniformShaderParameters>(), VertexFactoryUniformBuffer);
	}

	//@todo - allow FMeshBatch to supply vertex streams (instead of requiring that they come from the vertex factory), and this userdata hack will no longer be needed for override vertex color
	if (BatchElement.bUserDataIsColorVertexBuffer)
	{
		FColorVertexBuffer* OverrideColorVertexBuffer = (FColorVertexBuffer*)BatchElement.UserData;
		check(OverrideColorVertexBuffer);

		if (!LocalVertexFactory->SupportsManualVertexFetch(FeatureLevel))
		{
			LocalVertexFactory->GetColorOverrideStream(OverrideColorVertexBuffer, VertexStreams);
		}
	}


}

void FInstanceBufferMeshVertexFactoryShaderParameters::GetElementShaderBindings(
	const class FSceneInterface* Scene,
	const FSceneView* View,
	const FMeshMaterialShader* Shader,
	const EVertexInputStreamType InputStreamType,
	ERHIFeatureLevel::Type FeatureLevel,
	const FVertexFactory* VertexFactory,
	const FMeshBatchElement& BatchElement,
	FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams
	) const
{
	const bool bInstanced = GRHISupportsInstancing;

	// Decode VertexFactoryUserData as VertexFactoryUniformBuffer
	FRHIUniformBuffer* VertexFactoryUniformBuffer = static_cast<FRHIUniformBuffer*>(BatchElement.VertexFactoryUserData);
	FInstanceBufferMeshVertexFactoryShaderParameters::GetElementShaderBindingsBase(Scene, View, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement, VertexFactoryUniformBuffer, ShaderBindings, VertexStreams);

	const FInstancingUserData* InstancingUserData = (const FInstancingUserData*)BatchElement.UserData;
	const auto* InstancedVertexFactory = static_cast<const FInstanceBufferMeshVertexFactory*>(VertexFactory);
	const int32 InstanceOffsetValue = BatchElement.UserIndex;

	if (bInstanced)
	{
		if (InstancedVertexFactory->SupportsManualVertexFetch(FeatureLevel))
		{
			if (InstancedVertexFactory->GetNumInstances() > 0)
			{
				ShaderBindings.Add(VertexFetch_InstanceOriginBufferParameter, InstancedVertexFactory->GetInstanceOriginSRV());
				ShaderBindings.Add(VertexFetch_InstanceTransformBufferParameter, InstancedVertexFactory->GetInstanceTransformSRV());
				ShaderBindings.Add(VertexFetch_InstanceLightmapBufferParameter, InstancedVertexFactory->GetInstanceLightmapSRV());
				ShaderBindings.Add(InstanceOffset, InstanceOffsetValue);
			}
			else
			{
				ensureMsgf(false, TEXT("Instanced static mesh rendered with no instances. Data initialized: %d"), InstancedVertexFactory->IsDataInitialized());
			}
		}

		if (InstanceOffsetValue > 0 && VertexStreams.Num() > 0)
		{
			VertexFactory->OffsetInstanceStreams(InstanceOffsetValue, InputStreamType, VertexStreams);
		}
	}


	if( InstancingWorldViewOriginOneParameter.IsBound() )
	{
		FVector4 InstancingViewZCompareZero(MIN_flt, MIN_flt, MAX_flt, 1.0f);
		FVector4 InstancingViewZCompareOne(MIN_flt, MIN_flt, MAX_flt, 0.0f);
		FVector4 InstancingViewZConstant(ForceInit);
		FVector4 InstancingWorldViewOriginZero(ForceInit);
		FVector4 InstancingWorldViewOriginOne(ForceInit);
		InstancingWorldViewOriginOne.W = 1.0f;
		if (InstancingUserData && BatchElement.InstancedLODRange)
		{
			int32 FirstLOD = InstancingUserData->MinLOD;

			int32 DebugMin = FMath::Min(InstanceBuffeStaticMeshNameSpace::CVarMinLOD.GetValueOnRenderThread(), InstancingUserData->MeshRenderData->LODResources.Num() - 1);
			if (DebugMin >= 0)
			{
				FirstLOD = FMath::Max(FirstLOD, DebugMin);
			}

			FBoxSphereBounds ScaledBounds = InstancingUserData->MeshRenderData->Bounds.TransformBy(FTransform(FRotator::ZeroRotator, FVector::ZeroVector, InstancingUserData->AverageInstancesScale));
			float SphereRadius = ScaledBounds.SphereRadius;
			float MinSize = View->ViewMatrices.IsPerspectiveProjection() ? TimHackMinSize : 0.0f;
			float LODScale = TimHackLODScale;
			float LODRandom = TimHackLODRange;
			float MaxDrawDistanceScale = GetCachedScalabilityCVars().ViewDistanceScale;

			if (BatchElement.InstancedLODIndex)
			{
				InstancingViewZConstant.X = -1.0f;
			}
			else
			{
				// this is the first LOD, so we don't have a fade-in region
				InstancingViewZConstant.X = 0.0f;
			}
			InstancingViewZConstant.Y = 0.0f;
			InstancingViewZConstant.Z = 1.0f;

			// now we subtract off the lower segments, since they will be incorporated 
			InstancingViewZConstant.Y -= InstancingViewZConstant.X;
			InstancingViewZConstant.Z -= InstancingViewZConstant.X + InstancingViewZConstant.Y;
			//not using W InstancingViewZConstant.W -= InstancingViewZConstant.X + InstancingViewZConstant.Y + InstancingViewZConstant.Z;

			for (int32 SampleIndex = 0; SampleIndex < 2; SampleIndex++)
			{
				FVector4& InstancingViewZCompare(SampleIndex ? InstancingViewZCompareOne : InstancingViewZCompareZero);

				float FinalCull = MAX_flt;
				if (MinSize > 0.0)
				{
					FinalCull = ComputeBoundsDrawDistance(MinSize, SphereRadius, View->ViewMatrices.GetProjectionMatrix()) * LODScale;
				}
				if (InstancingUserData->EndCullDistance > 0.0f)
				{
					FinalCull = FMath::Min(FinalCull, InstancingUserData->EndCullDistance * MaxDrawDistanceScale);
				}
				FinalCull *= MaxDrawDistanceScale;

				InstancingViewZCompare.Z = FinalCull;
				if (BatchElement.InstancedLODIndex < InstancingUserData->MeshRenderData->LODResources.Num() - 1)
				{
					float NextCut = ComputeBoundsDrawDistance(InstancingUserData->MeshRenderData->ScreenSize[BatchElement.InstancedLODIndex + 1].GetValueForFeatureLevel(FeatureLevel), SphereRadius, View->ViewMatrices.GetProjectionMatrix()) * LODScale;
					InstancingViewZCompare.Z = FMath::Min(NextCut, FinalCull);
				}

				InstancingViewZCompare.X = MIN_flt;
				if (BatchElement.InstancedLODIndex > FirstLOD)
				{
					float CurCut = ComputeBoundsDrawDistance(InstancingUserData->MeshRenderData->ScreenSize[BatchElement.InstancedLODIndex].GetValueForFeatureLevel(FeatureLevel), SphereRadius, View->ViewMatrices.GetProjectionMatrix()) * LODScale;
					if (CurCut < FinalCull)
					{
						InstancingViewZCompare.Y = CurCut;
					}
					else
					{
						// this LOD is completely removed by one of the other two factors
						InstancingViewZCompare.Y = MIN_flt;
						InstancingViewZCompare.Z = MIN_flt;
					}
				}
				else
				{
					// this is the first LOD, so we don't have a fade-in region
					InstancingViewZCompare.Y = MIN_flt;
				}
			}


			InstancingWorldViewOriginZero = View->GetTemporalLODOrigin(0);
			InstancingWorldViewOriginOne = View->GetTemporalLODOrigin(1);

			float Alpha = View->GetTemporalLODTransition();
			InstancingWorldViewOriginZero.W = 1.0f - Alpha;
			InstancingWorldViewOriginOne.W = Alpha;

			InstancingViewZCompareZero.W = LODRandom;
		}

		ShaderBindings.Add(InstancingViewZCompareZeroParameter, InstancingViewZCompareZero);
		ShaderBindings.Add(InstancingViewZCompareOneParameter, InstancingViewZCompareOne);
		ShaderBindings.Add(InstancingViewZConstantParameter, InstancingViewZConstant);
		ShaderBindings.Add(InstancingWorldViewOriginZeroParameter, InstancingWorldViewOriginZero);
		ShaderBindings.Add(InstancingWorldViewOriginOneParameter, InstancingWorldViewOriginOne);
	}

	if( InstancingFadeOutParamsParameter.IsBound() )
	{
		FVector4 InstancingFadeOutParams(MAX_flt,0.f,1.f,1.f);
		if (InstancingUserData)
		{
			const float MaxDrawDistanceScale = GetCachedScalabilityCVars().ViewDistanceScale;
			const float StartDistance = InstancingUserData->StartCullDistance * MaxDrawDistanceScale;
			const float EndDistance = InstancingUserData->EndCullDistance * MaxDrawDistanceScale;

			InstancingFadeOutParams.X = StartDistance;
			if( EndDistance > 0 )
			{
				if( EndDistance > StartDistance )
				{
					InstancingFadeOutParams.Y = 1.f / (float)(EndDistance - StartDistance);
				}
				else
				{
					InstancingFadeOutParams.Y = 1.f;
				}
			}
			else
			{
				InstancingFadeOutParams.Y = 0.f;
			}
			if (CVarCullAllInVertexShader.GetValueOnRenderThread() > 0)
			{
				InstancingFadeOutParams.Z = 0.0f;
				InstancingFadeOutParams.W = 0.0f;
			}
			else
			{
				InstancingFadeOutParams.Z = InstancingUserData->bRenderSelected ? 1.f : 0.f;
				InstancingFadeOutParams.W = InstancingUserData->bRenderUnselected ? 1.f : 0.f;
			}
		}

		ShaderBindings.Add(InstancingFadeOutParamsParameter, InstancingFadeOutParams);

	}
}

