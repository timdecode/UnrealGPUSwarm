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


/** InstancedStaticMeshInstance hit proxy */
void HInstanceBufferMeshInstance::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Component);
}

FIBMInstanceUpdateCmdBuffer::FIBMInstanceUpdateCmdBuffer()
	: NumAdds(0)
	, NumEdits(0)
{
}

void FIBMInstanceUpdateCmdBuffer::HideInstance(int32 RenderIndex)
{
	check(RenderIndex >= 0);

	FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
	Cmd.InstanceIndex = RenderIndex;
	Cmd.Type = FIBMInstanceUpdateCmdBuffer::Hide;

	Edit();
}

void FIBMInstanceUpdateCmdBuffer::AddInstance(const FMatrix& InTransform)
{
	FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
	Cmd.InstanceIndex = INDEX_NONE;
	Cmd.Type = FIBMInstanceUpdateCmdBuffer::Add;
	Cmd.XForm = InTransform;

	NumAdds++;
	Edit();
}

void FIBMInstanceUpdateCmdBuffer::UpdateInstance(int32 RenderIndex, const FMatrix& InTransform)
{
	FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
	Cmd.InstanceIndex = RenderIndex;
	Cmd.Type = FIBMInstanceUpdateCmdBuffer::Update;
	Cmd.XForm = InTransform;

	Edit();
}

void FIBMInstanceUpdateCmdBuffer::SetEditorData(int32 RenderIndex, const FColor& Color, bool bSelected)
{
	FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
	Cmd.InstanceIndex = RenderIndex;
	Cmd.Type = FIBMInstanceUpdateCmdBuffer::EditorData;
	Cmd.HitProxyColor = Color;
	Cmd.bSelected = bSelected;

	Edit();
}

void FIBMInstanceUpdateCmdBuffer::SetLightMapData(int32 RenderIndex, const FVector2D& LightmapUVBias)
{
	// We only support 1 command to update lightmap/shadowmap
	bool CommandExist = false;

	for (FInstanceUpdateCommand& Cmd : Cmds)
	{
		if (Cmd.Type == FIBMInstanceUpdateCmdBuffer::LightmapData && Cmd.InstanceIndex == RenderIndex)
		{
			CommandExist = true;
			Cmd.LightmapUVBias = LightmapUVBias;
			break;
		}
	}

	if (!CommandExist)
	{
		FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
		Cmd.InstanceIndex = RenderIndex;
		Cmd.Type = FIBMInstanceUpdateCmdBuffer::LightmapData;
		Cmd.LightmapUVBias = LightmapUVBias;
	}

	Edit();
}

void FIBMInstanceUpdateCmdBuffer::SetShadowMapData(int32 RenderIndex, const FVector2D& ShadowmapUVBias)
{
	// We only support 1 command to update lightmap/shadowmap
	bool CommandExist = false;

	for (FInstanceUpdateCommand& Cmd : Cmds)
	{
		if (Cmd.Type == FIBMInstanceUpdateCmdBuffer::LightmapData && Cmd.InstanceIndex == RenderIndex)
		{
			CommandExist = true;
			Cmd.ShadowmapUVBias = ShadowmapUVBias;
			break;
		}
	}

	if (!CommandExist)
	{
		FInstanceUpdateCommand& Cmd = Cmds.AddDefaulted_GetRef();
		Cmd.InstanceIndex = RenderIndex;
		Cmd.Type = FIBMInstanceUpdateCmdBuffer::LightmapData;
		Cmd.ShadowmapUVBias = ShadowmapUVBias;
	}

	Edit();
}

void FIBMInstanceUpdateCmdBuffer::ResetInlineCommands()
{
	Cmds.Empty();
	NumAdds = 0;
}

void FIBMInstanceUpdateCmdBuffer::Edit()
{
	NumEdits++;
}

void FIBMInstanceUpdateCmdBuffer::Reset()
{
	Cmds.Empty();
	NumAdds = 0;
	NumEdits = 0;
}

FIBMInstanceBuffer::FIBMInstanceBuffer(ERHIFeatureLevel::Type InFeatureLevel, bool InRequireCPUAccess)
	: FRenderResource(InFeatureLevel)
	, RequireCPUAccess(InRequireCPUAccess)
{
}

FIBMInstanceBuffer::~FIBMInstanceBuffer()
{
	CleanUp();
}

/** Delete existing resources */
void FIBMInstanceBuffer::CleanUp()
{
	InstanceData.Reset();
}

void FIBMInstanceBuffer::InitFromPreallocatedData(FIBMStaticMeshInstanceData& Other)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FStaticMeshInstanceBuffer_InitFromPreallocatedData);

	InstanceData = MakeShared<FIBMStaticMeshInstanceData, ESPMode::ThreadSafe>();
	FMemory::Memswap(&Other, InstanceData.Get(), sizeof(FIBMStaticMeshInstanceData));
	InstanceData->SetAllowCPUAccess(RequireCPUAccess);
}

void FIBMInstanceBuffer::UpdateFromCommandBuffer_Concurrent(FIBMInstanceUpdateCmdBuffer& CmdBuffer)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FStaticMeshInstanceBuffer_UpdateFromCommandBuffer_Concurrent);
	
	FIBMInstanceBuffer* InstanceBuffer = this; 
	FIBMInstanceUpdateCmdBuffer* NewCmdBuffer = new FIBMInstanceUpdateCmdBuffer();
	FMemory::Memswap(&CmdBuffer, NewCmdBuffer, sizeof(FIBMInstanceUpdateCmdBuffer));
	
	// leave NumEdits unchanged in commandbuffer
	CmdBuffer.NumEdits = NewCmdBuffer->NumEdits; 
	CmdBuffer.ResetInlineCommands();
		
	ENQUEUE_RENDER_COMMAND(InstanceBuffer_UpdateFromPreallocatedData)(
		[InstanceBuffer, NewCmdBuffer](FRHICommandListImmediate& RHICmdList)
		{
			InstanceBuffer->UpdateFromCommandBuffer_RenderThread(*NewCmdBuffer);
			delete NewCmdBuffer;
		});
}

void FIBMInstanceBuffer::UpdateFromCommandBuffer_RenderThread(FIBMInstanceUpdateCmdBuffer& CmdBuffer)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FStaticMeshInstanceBuffer_UpdateFromCommandBuffer_RenderThread);
	
	int32 NumCommands = CmdBuffer.NumInlineCommands();
	int32 NumAdds = CmdBuffer.NumAdds;
	int32 AddIndex = INDEX_NONE;

	if (NumAdds > 0)
	{
		AddIndex = InstanceData->GetNumInstances();
		int32 NewNumInstances = NumAdds + InstanceData->GetNumInstances();
		InstanceData->AllocateInstances(NewNumInstances, GIsEditor ? EResizeBufferFlags::AllowSlackOnGrow | EResizeBufferFlags::AllowSlackOnReduce : EResizeBufferFlags::None, false); // In Editor always permit overallocation, to prevent too much realloc
	}

	for (int32 i = 0; i < NumCommands; ++i)
	{
		const auto& Cmd = CmdBuffer.Cmds[i];
		switch (Cmd.Type)
		{
		case FIBMInstanceUpdateCmdBuffer::Add:
			InstanceData->SetInstance(AddIndex++, Cmd.XForm, 0);
			break;
		case FIBMInstanceUpdateCmdBuffer::Hide:
			InstanceData->NullifyInstance(Cmd.InstanceIndex);
			break;
		case FIBMInstanceUpdateCmdBuffer::Update:
			InstanceData->SetInstance(Cmd.InstanceIndex, Cmd.XForm, 0);
			break;
		case FIBMInstanceUpdateCmdBuffer::EditorData:
			InstanceData->SetInstanceEditorData(Cmd.InstanceIndex, Cmd.HitProxyColor, Cmd.bSelected);
			break;
		case FIBMInstanceUpdateCmdBuffer::LightmapData:
			InstanceData->SetInstanceLightMapData(Cmd.InstanceIndex, Cmd.LightmapUVBias, Cmd.ShadowmapUVBias);
			break;
		default:
			check(false);
		}
	}

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
	check(InstanceData);
	if (InstanceData->GetNumInstances() > 0)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FStaticMeshInstanceBuffer_InitRHI);
		LLM_SCOPE(ELLMTag::InstancedMesh);

		// Tim: We want to write to this buffer GPU side. So it should not be static.
		auto AccessFlags = BUF_UnorderedAccess | BUF_ShaderResource; // BUF_Static;
		CreateVertexBuffer(InstanceData->GetOriginResourceArray(), AccessFlags, 16, PF_A32B32G32R32F, InstanceOriginBuffer.VertexBufferRHI, InstanceOriginSRV);
		CreateVertexBuffer(InstanceData->GetTransformResourceArray(), AccessFlags, InstanceData->GetTranslationUsesHalfs() ? 8 : 16, InstanceData->GetTranslationUsesHalfs() ? PF_FloatRGBA : PF_A32B32G32R32F, InstanceTransformBuffer.VertexBufferRHI, InstanceTransformSRV);
		CreateVertexBuffer(InstanceData->GetLightMapResourceArray(), AccessFlags, 8, PF_R16G16B16A16_SNORM, InstanceLightmapBuffer.VertexBufferRHI, InstanceLightmapSRV);
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
	if (InstanceData && InstanceData->GetNumInstances() > 0)
	{
		return InstanceData->GetResourceSize();
	}
	return 0;
}

void FIBMInstanceBuffer::CreateVertexBuffer(FResourceArrayInterface* InResourceArray, uint32 InUsage, uint32 InStride, uint8 InFormat, FVertexBufferRHIRef& OutVertexBufferRHI, FShaderResourceViewRHIRef& OutInstanceSRV)
{
	check(InResourceArray);
	check(InResourceArray->GetResourceDataSize() > 0);

	// TODO: possibility over allocated the vertex buffer when we support partial update for when working in the editor
	FRHIResourceCreateInfo CreateInfo(InResourceArray);
	OutVertexBufferRHI = RHICreateVertexBuffer(InResourceArray->GetResourceDataSize(), InUsage, CreateInfo);
	
	if (RHISupportsManualVertexFetch(GMaxRHIShaderPlatform))
	{
		OutInstanceSRV = RHICreateShaderResourceView(OutVertexBufferRHI, InStride, InFormat);
	}
}

void FIBMInstanceBuffer::BindInstanceVertexBuffer(const class FVertexFactory* VertexFactory, FInstanceBufferMeshDataType& InstancedStaticMeshData) const
{
	if (InstanceData->GetNumInstances() && RHISupportsManualVertexFetch(GMaxRHIShaderPlatform))
	{
		check(InstanceOriginSRV);
		check(InstanceTransformSRV);
		check(InstanceLightmapSRV);
	}

	{
		InstancedStaticMeshData.InstanceOriginSRV = InstanceOriginSRV;
		InstancedStaticMeshData.InstanceTransformSRV = InstanceTransformSRV;
		InstancedStaticMeshData.InstanceLightmapSRV = InstanceLightmapSRV;
		InstancedStaticMeshData.NumInstances = InstanceData->GetNumInstances();
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

		EVertexElementType TransformType = InstanceData->GetTranslationUsesHalfs() ? VET_Half4 : VET_Float4;
		uint32 TransformStride = InstanceData->GetTranslationUsesHalfs() ? 8 : 16;

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


void FIBMStaticMeshInstanceData::Serialize(FArchive& Ar)
{
	Ar << NumInstances;

	if (Ar.IsLoading())
	{
		AllocateBuffers(NumInstances);
	}

	InstanceOriginData->Serialize(Ar);
	InstanceLightmapData->Serialize(Ar);


	InstanceTransformData->Serialize(Ar);

	if (Ar.IsLoading())
	{
		InstanceOriginDataPtr = InstanceOriginData->GetDataPointer();
		InstanceLightmapDataPtr = InstanceLightmapData->GetDataPointer();
		InstanceTransformDataPtr = InstanceTransformData->GetDataPointer();
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
IMPLEMENT_VERTEX_FACTORY_TYPE_EX(FEmulatedInstanceBufferMeshVertexFactory,"/Engine/Private/LocalVertexFactory.ush",true,true,true,true,true,true,false);

void FInstanceBufferMeshRenderData::InitVertexFactories()
{
	const bool bInstanced = GRHISupportsInstancing;

	// Allocate the vertex factories for each LOD
	for (int32 LODIndex = 0; LODIndex < LODModels.Num(); LODIndex++)
	{
		VertexFactories.Add(bInstanced ? new FInstanceBufferMeshVertexFactory(FeatureLevel) : new FEmulatedInstanceBufferMeshVertexFactory(FeatureLevel));
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

FIBMPerInstanceRenderData::FIBMPerInstanceRenderData(FIBMStaticMeshInstanceData& Other, ERHIFeatureLevel::Type InFeaureLevel, bool InRequireCPUAccess)
	: ResourceSize(InRequireCPUAccess ? Other.GetResourceSize() : 0)
	, InstanceBuffer(InFeaureLevel, InRequireCPUAccess)
{
	InstanceBuffer.InitFromPreallocatedData(Other);
	InstanceBuffer_GameThread = InstanceBuffer.InstanceData;

	BeginInitResource(&InstanceBuffer);
}
		
FIBMPerInstanceRenderData::~FIBMPerInstanceRenderData()
{
	InstanceBuffer_GameThread.Reset();
	// Should be always destructed on rendering thread
	InstanceBuffer.ReleaseResource();
}

void FIBMPerInstanceRenderData::UpdateFromPreallocatedData(FIBMStaticMeshInstanceData& InOther)
{
	InstanceBuffer.RequireCPUAccess = (InOther.GetOriginResourceArray()->GetAllowCPUAccess() || InOther.GetTransformResourceArray()->GetAllowCPUAccess() || InOther.GetLightMapResourceArray()->GetAllowCPUAccess()) ? true : InstanceBuffer.RequireCPUAccess;
	ResourceSize = InstanceBuffer.RequireCPUAccess ? InOther.GetResourceSize() : 0;

	InOther.SetAllowCPUAccess(InstanceBuffer.RequireCPUAccess);

	InstanceBuffer_GameThread = MakeShared<FIBMStaticMeshInstanceData, ESPMode::ThreadSafe>();
	FMemory::Memswap(&InOther, InstanceBuffer_GameThread.Get(), sizeof(FIBMInstanceData));

	typedef TSharedPtr<FIBMStaticMeshInstanceData, ESPMode::ThreadSafe> FStaticMeshInstanceDataPtr;

	FStaticMeshInstanceDataPtr InInstanceBufferDataPtr = InstanceBuffer_GameThread;
	FIBMInstanceBuffer* InInstanceBuffer = &InstanceBuffer;
	ENQUEUE_RENDER_COMMAND(FInstanceBuffer_UpdateFromPreallocatedData)(
		[InInstanceBufferDataPtr, InInstanceBuffer](FRHICommandListImmediate& RHICmdList)
		{
			InInstanceBuffer->InstanceData = InInstanceBufferDataPtr;
			InInstanceBuffer->UpdateRHI();
		}
	);
}

void FIBMPerInstanceRenderData::UpdateFromCommandBuffer(FIBMInstanceUpdateCmdBuffer& CmdBuffer)
{
	InstanceBuffer.UpdateFromCommandBuffer_Concurrent(CmdBuffer);
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
			InstanceBuffer.GetInstanceTransform(InstanceIndex, InstanceToLocal);	
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
		const uint32 MaxInstancesPerBatch = FInstanceBufferMeshVertexFactory::NumBitsForVisibilityMask();
		const uint32 NumBatches = FMath::DivideAndRoundUp(NumInstances, MaxInstancesPerBatch);
		uint32 InstanceIndex = BatchIndex * MaxInstancesPerBatch;
		uint32 NumInstancesThisBatch = FMath::Min(NumInstances - InstanceIndex, MaxInstancesPerBatch);
				
		if (NumInstancesThisBatch > 0)
		{
			OutMeshBatch.Elements.Reserve(NumInstancesThisBatch);
						
			// BatchElement0 is already inside the array; but Reserve() might have shifted it
			OutMeshBatch.Elements[0].UserIndex = InstanceIndex;
			--NumInstancesThisBatch;
			++InstanceIndex;

			// Add remaining BatchElements 1..n-1
			while (NumInstancesThisBatch > 0)
			{
				auto* NewBatchElement = new(OutMeshBatch.Elements) FMeshBatchElement();
				*NewBatchElement = BatchElement0;
				NewBatchElement->UserIndex = InstanceIndex;
				++InstanceIndex;
				--NumInstancesThisBatch;
			}
		}
	}
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
		InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetInstanceTransform(InstanceIndex, InstanceToLocal);	
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
		InstancedRenderData.PerInstanceRenderData->InstanceBuffer.GetInstanceTransform(InstanceIndex, InstanceToLocal);
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
	if (!CVarRayTracingRenderInstances.GetValueOnRenderThread())
	{
		return;
	}

	uint32 LOD = GetCurrentFirstLODIdx_RenderThread();
	const int32 InstanceCount = InstancedRenderData.Component->PerInstanceSMData.Num();

	if (InstanceCount == 0)
	{
		return;
	}
	//setup a 'template' for the instance first, so we aren't duplicating work
	//#dxr_todo: when multiple LODs are used, template needs to be an array of templates, probably best initialized on-demand via a lamda
	FRayTracingInstance RayTracingInstanceTemplate;
	RayTracingInstanceTemplate.Geometry = &RenderData->LODResources[LOD].RayTracingGeometry;

	//preallocate the worst-case to prevent an explosion of reallocs
	//#dxr_todo: possibly track used instances and reserve based on previous behavior
	RayTracingInstanceTemplate.InstanceTransforms.Reserve(InstanceCount);


	int32 SectionCount = InstancedRenderData.LODModels[LOD].Sections.Num();

	for (int32 SectionIdx = 0; SectionIdx < SectionCount; ++SectionIdx)
	{
		//#dxr_todo: so far we use the parent static mesh path to get material data
		FMeshBatch MeshBatch;
		FStaticMeshSceneProxy::GetMeshElement(LOD, 0, SectionIdx, 0, false, false, MeshBatch);

		RayTracingInstanceTemplate.Materials.Add(MeshBatch);
	}
	RayTracingInstanceTemplate.BuildInstanceMaskAndFlags();

	if (CVarRayTracingRenderInstancesCulling.GetValueOnRenderThread() > 0 && RayTracingCullClusterInstances.Num() > 0)
	{
		const float BVHCullRadius = CVarRayTracingInstancesCullClusterRadius.GetValueOnRenderThread();
		const float BVHLowScaleThreshold = CVarRayTracingInstancesLowScaleThreshold.GetValueOnRenderThread();
		const float BVHLowScaleRadius = CVarRayTracingInstancesLowScaleCullRadius.GetValueOnRenderThread();
		const bool ApplyGeneralCulling = BVHCullRadius > 0.0f;
		const bool ApplyLowScaleCulling = BVHLowScaleThreshold > 0.0f && BVHLowScaleRadius > 0.0f;
		FMatrix ToWorld = InstancedRenderData.Component->GetComponentTransform().ToMatrixWithScale();

		// Iterate over all culling clusters
		for (int32 ClusterIdx = 0; ClusterIdx < RayTracingCullClusterBoundsMin.Num(); ++ClusterIdx)
		{
			FVector VClusterBBoxSize = RayTracingCullClusterBoundsMax[ClusterIdx] - RayTracingCullClusterBoundsMin[ClusterIdx];
			FVector VClusterCenter = 0.5f * (RayTracingCullClusterBoundsMax[ClusterIdx] + RayTracingCullClusterBoundsMin[ClusterIdx]);
			FVector VToClusterCenter = VClusterCenter - Context.ReferenceView->ViewLocation;
			float ClusterRadius = 0.5f * VClusterBBoxSize.Size();
			float DistToClusterCenter = VToClusterCenter.Size();

			// Cull whole cluster if the bounding sphere is too far away
			if ((DistToClusterCenter - ClusterRadius) > BVHCullRadius && ApplyGeneralCulling)
			{
				continue;
			}

			TDoubleLinkedList< uint32 >* InstanceList = RayTracingCullClusterInstances[ClusterIdx];

			// Unroll instances in the current cluster into the array
			for (TDoubleLinkedList<uint32>::TDoubleLinkedListNode* InstancePtr = InstanceList->GetHead(); InstancePtr != nullptr; InstancePtr = InstancePtr->GetNextNode())
			{
				const uint32 Instance = InstancePtr->GetValue();

				if (InstancedRenderData.Component->PerInstanceSMData.IsValidIndex(Instance))
				{
					const FIBMInstanceData& InstanceData = InstancedRenderData.Component->PerInstanceSMData[Instance];
					FMatrix InstanceTransform = InstanceData.Transform * ToWorld;
					FVector InstanceLocation = InstanceTransform.TransformPosition({ 0.0f,0.0f,0.0f });
					FVector VToInstanceCenter = Context.ReferenceView->ViewLocation - InstanceLocation;
					float   DistanceToInstanceCenter = VToInstanceCenter.Size();

					FVector VMin, VMax, VDiag;
					InstancedRenderData.Component->GetLocalBounds(VMin, VMax);
					VMin = InstanceTransform.TransformPosition(VMin);
					VMax = InstanceTransform.TransformPosition(VMax);
					VDiag = VMax - VMin;

					float InstanceRadius = 0.5f * VDiag.Size();
					float DistanceToInstanceStart = DistanceToInstanceCenter - InstanceRadius;

					// Cull instance based on distance
					if (DistanceToInstanceStart > BVHCullRadius && ApplyGeneralCulling)
						continue;

					// Special culling for small scale objects
					if (InstanceRadius < BVHLowScaleThreshold && ApplyLowScaleCulling)
					{
						if (DistanceToInstanceStart > BVHLowScaleRadius)
							continue;
					}

					RayTracingInstanceTemplate.InstanceTransforms.Add(InstanceTransform);
				}
			}
		}
	}
	else
	{
		// No culling
		for (int32 InstanceIdx = 0; InstanceIdx < InstanceCount; ++InstanceIdx)
		{
			if (InstancedRenderData.Component->PerInstanceSMData.IsValidIndex(InstanceIdx))
			{
				const FIBMInstanceData& InstanceData = InstancedRenderData.Component->PerInstanceSMData[InstanceIdx];
				FMatrix ComponentLocalToWorld = InstancedRenderData.Component->GetComponentTransform().ToMatrixWithScale();
				FMatrix InstanceTransform = InstanceData.Transform * ComponentLocalToWorld;

				RayTracingInstanceTemplate.InstanceTransforms.Add(InstanceTransform);
			}
		}
	}

	OutRayTracingInstances.Add(RayTracingInstanceTemplate);
}

void FInstanceBufferMeshSceneProxy::SetupRayTracingCullClusters()
{
	//#dxr_todo: select the appropriate LOD depending on Context.View
	int32 LOD = 0;
	if (RenderData->LODResources.Num() > LOD && RenderData->LODResources[LOD].RayTracingGeometry.IsInitialized())
	{
		const float MaxClusterRadiusMultiplier = CVarRayTracingInstancesCullClusterMaxRadiusMultiplier.GetValueOnAnyThread();
		const int32 Batches = GetNumMeshBatches();
		const int32 InstanceCount = InstancedRenderData.Component->PerInstanceSMData.Num();
		int32 ClusterIndex = 0;
		FMatrix ComponentLocalToWorld = InstancedRenderData.Component->GetComponentTransform().ToMatrixWithScale();
		float MaxInstanceRadius = 0.0f;

		// Init first cluster
		RayTracingCullClusterInstances.Add(new TDoubleLinkedList< uint32>());
		RayTracingCullClusterBoundsMin.Add(FVector(MAX_FLT, MAX_FLT, MAX_FLT));
		RayTracingCullClusterBoundsMax.Add(FVector(-MAX_FLT, -MAX_FLT, -MAX_FLT));

		// Traverse instances to find maximum rarius
		for (int32 Instance = 0; Instance < InstanceCount; ++Instance)
		{
			if (InstancedRenderData.Component->PerInstanceSMData.IsValidIndex(Instance))
			{
				const FIBMInstanceData& InstanceData = InstancedRenderData.Component->PerInstanceSMData[Instance];
				FMatrix InstanceTransform = InstanceData.Transform * ComponentLocalToWorld;
				FVector VMin, VMax;

				InstancedRenderData.Component->GetLocalBounds(VMin, VMax);
				VMin = InstanceTransform.TransformPosition(VMin);
				VMax = InstanceTransform.TransformPosition(VMax);

				FVector VBBoxSize = VMax - VMin;

				MaxInstanceRadius = FMath::Max(0.5f * VBBoxSize.Size(), MaxInstanceRadius);
			}
		}

		float MaxClusterRadius = MaxInstanceRadius * MaxClusterRadiusMultiplier;

		// Build clusters
		for (int32 Instance = 0; Instance < InstanceCount; ++Instance)
		{
			if (InstancedRenderData.Component->PerInstanceSMData.IsValidIndex(Instance))
			{
				const FIBMInstanceData& InstanceData = InstancedRenderData.Component->PerInstanceSMData[Instance];
				FMatrix InstanceTransform = InstanceData.Transform * ComponentLocalToWorld;
				FVector InstanceLocation = InstanceTransform.TransformPosition({ 0.0f,0.0f,0.0f });
				FVector VMin = InstanceLocation - FVector(MaxInstanceRadius, MaxInstanceRadius, MaxInstanceRadius);
				FVector VMax = InstanceLocation + FVector(MaxInstanceRadius, MaxInstanceRadius, MaxInstanceRadius);
				bool bClusterFound = false;

				// Try to find suitable cluster
				for (int32 CandidateCluster = 0; CandidateCluster <= ClusterIndex; ++CandidateCluster)
				{
					// Build new candidate cluster bounds
					FVector VCandidateMin = VMin.ComponentMin(RayTracingCullClusterBoundsMin[CandidateCluster]);
					FVector VCandidateMax = VMax.ComponentMax(RayTracingCullClusterBoundsMax[CandidateCluster]);

					FVector VCandidateBBoxSize = VCandidateMax - VCandidateMin;
					float MaxCandidateRadius = 0.5f * VCandidateBBoxSize.Size();

					// If new candidate is still small enough, update current cluster
					if (MaxCandidateRadius <= MaxClusterRadius)
					{
						RayTracingCullClusterInstances[CandidateCluster]->AddTail(Instance);
						RayTracingCullClusterBoundsMin[CandidateCluster] = VCandidateMin;
						RayTracingCullClusterBoundsMax[CandidateCluster] = VCandidateMax;
						bClusterFound = true;
						break;
					}
				}

				// if we couldn't add the instance to an existing cluster create a new one
				if (!bClusterFound)
				{
					++ClusterIndex;
					RayTracingCullClusterInstances.Add(new TDoubleLinkedList< uint32>());
					RayTracingCullClusterInstances[ClusterIndex]->AddTail(Instance);
					RayTracingCullClusterBoundsMin.Add(VMin);
					RayTracingCullClusterBoundsMax.Add(VMax);
				}
			}
		}
	}
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
#if WITH_EDITOR
	InstanceData.InitializeAs<FIBMComponentInstanceData>(this);
	FIBMComponentInstanceData* StaticMeshInstanceData = InstanceData.Cast<FIBMComponentInstanceData>();

	// Fill in info (copied from UStaticMeshComponent::GetComponentInstanceData)
	StaticMeshInstanceData->CachedStaticLighting.Transform = GetComponentTransform();

	for (const FStaticMeshComponentLODInfo& LODDataEntry : LODData)
	{
		StaticMeshInstanceData->CachedStaticLighting.MapBuildDataIds.Add(LODDataEntry.MapBuildDataId);
	}

	// Back up per-instance lightmap/shadowmap info
	StaticMeshInstanceData->PerInstanceSMData = PerInstanceSMData;

	// Back up instance selection
	StaticMeshInstanceData->SelectedInstances = SelectedInstances;

	// Back up random seed
	StaticMeshInstanceData->InstancingRandomSeed = InstancingRandomSeed;
#endif
	return InstanceData;
}

void UInstanceBufferMeshComponent::ApplyComponentInstanceData(FIBMComponentInstanceData* InstancedMeshData)
{
#if WITH_EDITOR
	check(InstancedMeshData);

	if (GetStaticMesh() != InstancedMeshData->StaticMesh)
	{
		return;
	}

	bool bMatch = false;

	// Check for any instance having moved as that would invalidate static lighting
	if (PerInstanceSMData.Num() == InstancedMeshData->PerInstanceSMData.Num() &&
		InstancedMeshData->CachedStaticLighting.Transform.Equals(GetComponentTransform()))
	{
		bMatch = true;

		for (int32 InstanceIndex = 0; InstanceIndex < PerInstanceSMData.Num(); ++InstanceIndex)
		{
			if (PerInstanceSMData[InstanceIndex].Transform != InstancedMeshData->PerInstanceSMData[InstanceIndex].Transform)
			{
				bMatch = false;
				break;
			}
		}
	}

	// Restore static lighting if appropriate
	if (bMatch)
	{
		const int32 NumLODLightMaps = InstancedMeshData->CachedStaticLighting.MapBuildDataIds.Num();
		SetLODDataCount(NumLODLightMaps, NumLODLightMaps);

		for (int32 i = 0; i < NumLODLightMaps; ++i)
		{
			LODData[i].MapBuildDataId = InstancedMeshData->CachedStaticLighting.MapBuildDataIds[i];
		}

		PerInstanceSMData = InstancedMeshData->PerInstanceSMData;
	}

	SelectedInstances = InstancedMeshData->SelectedInstances;

	InstancingRandomSeed = InstancedMeshData->InstancingRandomSeed;

	// Force recreation of the render data
	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();
#endif
}

FPrimitiveSceneProxy* UInstanceBufferMeshComponent::CreateSceneProxy()
{
	LLM_SCOPE(ELLMTag::InstancedMesh);
	ProxySize = 0;

	// Verify that the mesh is valid before using it.
	const bool bMeshIsValid = 
		// make sure we have instances
		PerInstanceSMData.Num() > 0 &&
		// make sure we have an actual staticmesh
		GetStaticMesh() &&
		GetStaticMesh()->HasValidRenderData() &&
		// You really can't use hardware instancing on the consoles with multiple elements because they share the same index buffer. 
		// @todo: Level error or something to let LDs know this
		1;//GetStaticMesh()->LODModels(0).Elements.Num() == 1;

	if(bMeshIsValid)
	{
		check(InstancingRandomSeed != 0);
		
		// if instance data was modified, update GPU copy
		// generally happens only in editor 
		if (InstanceUpdateCmdBuffer.NumTotalCommands() != 0)
		{
			InstanceUpdateCmdBuffer.Reset();

			FIBMStaticMeshInstanceData RenderInstanceData = FIBMStaticMeshInstanceData();
			BuildRenderData(RenderInstanceData, PerInstanceRenderData->HitProxies);
			PerInstanceRenderData->UpdateFromPreallocatedData(RenderInstanceData);
		}
		
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
		QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_CreateHitProxyData);
		
		int32 NumProxies = PerInstanceSMData.Num();
		HitProxies.Empty(NumProxies);

		for (int32 InstanceIdx = 0; InstanceIdx < NumProxies; ++InstanceIdx)
		{
			HitProxies.Add(new HInstanceBufferMeshInstance(this, InstanceIdx));
		}
	}
	else
	{
		HitProxies.Empty();
	}
}

void UInstanceBufferMeshComponent::BuildRenderData(FIBMStaticMeshInstanceData& OutData, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies)
{
	LLM_SCOPE(ELLMTag::InstancedMesh);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_BuildRenderData);

	CreateHitProxyData(OutHitProxies);
	
	int32 NumInstances = PerInstanceSMData.Num();
	if (NumInstances == 0)
	{
		return;
	}
	
	OutData.AllocateInstances(NumInstances, GIsEditor ? EResizeBufferFlags::AllowSlackOnGrow | EResizeBufferFlags::AllowSlackOnReduce : EResizeBufferFlags::None, true); // In Editor always permit overallocation, to prevent too much realloc

	const FMeshMapBuildData* MeshMapBuildData = nullptr;
	if (LODData.Num() > 0)
	{
		MeshMapBuildData = GetMeshMapBuildData(LODData[0], false);
	}
	
	check(InstancingRandomSeed != 0);
	FRandomStream RandomStream = FRandomStream(InstancingRandomSeed);
	
	for (int32 Index = 0; Index < NumInstances; ++Index)
	{
		int32 RenderIndex = InstanceReorderTable.IsValidIndex(Index) ? InstanceReorderTable[Index] : Index;
		if (RenderIndex == INDEX_NONE) 
		{
			// could be skipped by density settings
			continue;
		}
			
		const FIBMInstanceData& InstanceData = PerInstanceSMData[Index];
		FVector2D LightmapUVBias = FVector2D(-1.0f, -1.0f);
		FVector2D ShadowmapUVBias = FVector2D(-1.0f, -1.0f);

		if (MeshMapBuildData != nullptr && MeshMapBuildData->PerInstanceLightmapData.IsValidIndex(Index))
		{
			LightmapUVBias = MeshMapBuildData->PerInstanceLightmapData[Index].LightmapUVBias;
			ShadowmapUVBias = MeshMapBuildData->PerInstanceLightmapData[Index].ShadowmapUVBias;
		}
	
		OutData.SetInstance(RenderIndex, InstanceData.Transform, RandomStream.GetFraction(), LightmapUVBias, ShadowmapUVBias);

#if WITH_EDITOR
		if (GIsEditor)
		{
			// Record if the instance is selected
			FColor HitProxyColor(ForceInit);
			bool bSelected = SelectedInstances.IsValidIndex(Index) && SelectedInstances[Index];

			if (OutHitProxies.IsValidIndex(Index))
			{
				HitProxyColor = OutHitProxies[Index]->Id.GetColor();
			}

			OutData.SetInstanceEditorData(RenderIndex, HitProxyColor, bSelected);
		}
#endif
	}
}

void UInstanceBufferMeshComponent::InitInstanceBody(int32 InstanceIdx, FBodyInstance* InstanceBodyInstance)
{
	if (!GetStaticMesh())
	{
		UE_LOG(LogStaticMesh, Warning, TEXT("Unabled to create a body instance for %s in Actor %s. No StaticMesh set."), *GetName(), GetOwner() ? *GetOwner()->GetName() : TEXT("?"));
		return;
	}

	check(InstanceIdx < PerInstanceSMData.Num());
	check(InstanceIdx < InstanceBodies.Num());
	check(InstanceBodyInstance);

	UBodySetup* BodySetup = GetBodySetup();
	check(BodySetup);

	// Get transform of the instance
	FTransform InstanceTransform = FTransform(PerInstanceSMData[InstanceIdx].Transform) * GetComponentTransform();
	
	InstanceBodyInstance->CopyBodyInstancePropertiesFrom(&BodyInstance);
	InstanceBodyInstance->InstanceBodyIndex = InstanceIdx; // Set body index 

	// make sure we never enable bSimulatePhysics for ISMComps
	InstanceBodyInstance->bSimulatePhysics = false;

#if WITH_PHYSX
	// Create physics body instance.
	InstanceBodyInstance->bAutoWeld = false;	//We don't support this for instanced meshes.
	InstanceBodyInstance->InitBody(BodySetup, InstanceTransform, this, GetWorld()->GetPhysicsScene(), nullptr);
#endif //WITH_PHYSX
}

void UInstanceBufferMeshComponent::CreateAllInstanceBodies()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_CreateAllInstanceBodies);

	const int32 NumBodies = PerInstanceSMData.Num();
	check(InstanceBodies.Num() == 0);

	if (UBodySetup* BodySetup = GetBodySetup())
	{
		FPhysScene* PhysScene = GetWorld()->GetPhysicsScene();

		if (!BodyInstance.GetOverrideWalkableSlopeOnInstance())
		{
			BodyInstance.SetWalkableSlopeOverride(BodySetup->WalkableSlopeOverride, false);
		}

		InstanceBodies.SetNumUninitialized(NumBodies);

		// Sanitized array does not contain any nulls
		TArray<FBodyInstance*> InstanceBodiesSanitized;
		InstanceBodiesSanitized.Reserve(NumBodies);

		TArray<FTransform> Transforms;
	    Transforms.Reserve(NumBodies);
	    for (int32 i = 0; i < NumBodies; ++i)
	    {
			const FTransform InstanceTM = FTransform(PerInstanceSMData[i].Transform) * GetComponentTransform();
			if (InstanceTM.GetScale3D().IsNearlyZero())
			{
				InstanceBodies[i] = nullptr;
			}
			else
			{
				FBodyInstance* Instance = new FBodyInstance;

				InstanceBodiesSanitized.Add(Instance);
				InstanceBodies[i] = Instance;
				Instance->CopyBodyInstancePropertiesFrom(&BodyInstance);
				Instance->InstanceBodyIndex = i; // Set body index 
				Instance->bAutoWeld = false;

				// make sure we never enable bSimulatePhysics for ISMComps
				Instance->bSimulatePhysics = false;

				if (Mobility == EComponentMobility::Movable)
				{
					Instance->InitBody(BodySetup, InstanceTM, this, PhysScene, nullptr );
				}
				else
				{
					Transforms.Add(InstanceTM);
				}
			}
	    }

		if (InstanceBodiesSanitized.Num() > 0 && Mobility != EComponentMobility::Movable)
		{
			FBodyInstance::InitStaticBodies(InstanceBodiesSanitized, Transforms, BodySetup, this, GetWorld()->GetPhysicsScene());
		}
	}
	else
	{
		// In case we get into some bad state where the BodySetup is invalid but bPhysicsStateCreated is true,
		// issue a warning and add nullptrs to InstanceBodies.
		UE_LOG(LogStaticMesh, Warning, TEXT("Instance Static Mesh Component unable to create InstanceBodies!"));
		InstanceBodies.AddZeroed(NumBodies);
	}
}

void UInstanceBufferMeshComponent::ClearAllInstanceBodies()
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_UInstancedStaticMeshComponent_ClearAllInstanceBodies);
	for (int32 i = 0; i < InstanceBodies.Num(); i++)
	{
		if (InstanceBodies[i])
		{
			InstanceBodies[i]->TermBody();
			delete InstanceBodies[i];
		}
	}

	InstanceBodies.Empty();
}


void UInstanceBufferMeshComponent::OnCreatePhysicsState()
{
	check(InstanceBodies.Num() == 0);

	FPhysScene* PhysScene = GetWorld()->GetPhysicsScene();

	if (!PhysScene)
	{
		return;
	}

	// Create all the bodies.
	CreateAllInstanceBodies();

	USceneComponent::OnCreatePhysicsState();
}

void UInstanceBufferMeshComponent::OnDestroyPhysicsState()
{
	USceneComponent::OnDestroyPhysicsState();

	// Release all physics representations
	ClearAllInstanceBodies();
}

bool UInstanceBufferMeshComponent::CanEditSimulatePhysics()
{
	// if instancedstaticmeshcomponent, we will never allow it
	return false;
}

FBoxSphereBounds UInstanceBufferMeshComponent::CalcBounds(const FTransform& BoundTransform) const
{
	return FBoxSphereBounds(BoundTransform.GetLocation(), FVector(1000.0f), 10000.f);

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
	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();
}

void UInstanceBufferMeshComponent::GetLightAndShadowMapMemoryUsage( int32& LightMapMemoryUsage, int32& ShadowMapMemoryUsage ) const
{
	Super::GetLightAndShadowMapMemoryUsage(LightMapMemoryUsage, ShadowMapMemoryUsage);

	int32 NumInstances = PerInstanceSMData.Num();

	// Scale lighting demo by number of instances
	LightMapMemoryUsage *= NumInstances;
	ShadowMapMemoryUsage *= NumInstances;
}

// Deprecated version of PerInstanceSMData
struct FInstancedStaticMeshInstanceData_DEPRECATED
{
	FMatrix Transform;
	FVector2D LightmapUVBias;
	FVector2D ShadowmapUVBias;
	
	friend FArchive& operator<<(FArchive& Ar, FInstancedStaticMeshInstanceData_DEPRECATED& InstanceData)
	{
		// @warning BulkSerialize: FIBMInstanceData is serialized as memory dump
		Ar << InstanceData.Transform << InstanceData.LightmapUVBias << InstanceData.ShadowmapUVBias;
		return Ar;
	}
};

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
	if (Ar.IsLoading())
	{
		uint64 RenderDataSizeBytes = 0;
		Ar << RenderDataSizeBytes; // TODO: can skip serialization if we know that data will be discarded

		if (RenderDataSizeBytes > 0)
		{
			InstanceDataBuffers = MakeUnique<FIBMStaticMeshInstanceData>();
			InstanceDataBuffers->Serialize(Ar);
		}
	}
	else if (Ar.IsSaving())
	{
		uint64 RenderDataSizePos = Ar.Tell();
		
		// write render data size, will write real size later
		uint64 RenderDataSizeBytes = 0;
		Ar << RenderDataSizeBytes;

		bool bSaveRenderData = NeedRenderDataForTargetPlatform(Ar.CookingTarget());
		if (bSaveRenderData && !HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			uint64 RenderDataPos = Ar.Tell();

			if (PerInstanceSMData.Num() > 0)
			{
				check(PerInstanceRenderData.IsValid());

				// This will usually happen when having a BP adding instance through the construct script
				if (PerInstanceRenderData->InstanceBuffer.GetNumInstances() != PerInstanceSMData.Num() || InstanceUpdateCmdBuffer.NumTotalCommands() > 0)
				{
					InstanceUpdateCmdBuffer.Reset();

					FIBMStaticMeshInstanceData RenderInstanceData = FIBMStaticMeshInstanceData();
					BuildRenderData(RenderInstanceData, PerInstanceRenderData->HitProxies);
					PerInstanceRenderData->UpdateFromPreallocatedData(RenderInstanceData);
					MarkRenderStateDirty();
				}
			}
		
			if (PerInstanceRenderData.IsValid())
			{
				if (PerInstanceRenderData->InstanceBuffer_GameThread && PerInstanceRenderData->InstanceBuffer_GameThread->GetNumInstances() > 0)
				{
					int32 NumInstances = PerInstanceRenderData->InstanceBuffer_GameThread->GetNumInstances();

					// Clear editor data for the cooked data
					for (int32 Index = 0; Index < NumInstances; ++Index)
					{
						int32 RenderIndex = InstanceReorderTable.IsValidIndex(Index) ? InstanceReorderTable[Index] : Index;
						if (RenderIndex == INDEX_NONE)
						{
							// could be skipped by density settings
							continue;
						}

						PerInstanceRenderData->InstanceBuffer_GameThread->ClearInstanceEditorData(RenderIndex);
					}

					PerInstanceRenderData->InstanceBuffer_GameThread->Serialize(Ar);

#if WITH_EDITOR
					// Restore back the state we were in
					TArray<TRefCountPtr<HHitProxy>> HitProxies;
					CreateHitProxyData(HitProxies);

					for (int32 Index = 0; Index < NumInstances; ++Index)
					{
						int32 RenderIndex = InstanceReorderTable.IsValidIndex(Index) ? InstanceReorderTable[Index] : Index;
						if (RenderIndex == INDEX_NONE)
						{
							// could be skipped by density settings
							continue;
						}

						// Record if the instance is selected
						FColor HitProxyColor(ForceInit);
						bool bSelected = SelectedInstances.IsValidIndex(Index) && SelectedInstances[Index];

						if (HitProxies.IsValidIndex(Index))
						{
							HitProxyColor = HitProxies[Index]->Id.GetColor();
						}

						PerInstanceRenderData->InstanceBuffer_GameThread->SetInstanceEditorData(RenderIndex, HitProxyColor, bSelected);
					}
#endif					
				}
			}

			// save render data real size
			uint64 CurPos = Ar.Tell();
			RenderDataSizeBytes = CurPos - RenderDataPos;
			Ar.Seek(RenderDataSizePos);
			Ar << RenderDataSizeBytes;
			Ar.Seek(CurPos);
		}
	}
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

#if WITH_EDITOR
	if (Ar.IsLoading() && Ar.CustomVer(FMobileObjectVersion::GUID) < FMobileObjectVersion::InstancedStaticMeshLightmapSerialization)
	{
		TArray<FInstancedStaticMeshInstanceData_DEPRECATED> DeprecatedData;
		DeprecatedData.BulkSerialize(Ar);
		PerInstanceSMData.Reset(DeprecatedData.Num());
		Algo::Transform(DeprecatedData, PerInstanceSMData, [](const FInstancedStaticMeshInstanceData_DEPRECATED& OldData){ 
			return FIBMInstanceData(OldData.Transform);
		});
	}
	else
#endif //WITH_EDITOR
	{
		PerInstanceSMData.BulkSerialize(Ar);
	}

	if (bCooked && (Ar.CustomVer(FFortniteMainBranchObjectVersion::GUID) >= FFortniteMainBranchObjectVersion::SerializeInstancedStaticMeshRenderData || Ar.CustomVer(FEditorObjectVersion::GUID) >= FEditorObjectVersion::SerializeInstancedStaticMeshRenderData))
	{
		SerializeRenderData(Ar);
	}
	
#if WITH_EDITOR
	if( Ar.IsTransacting() )
	{
		Ar << SelectedInstances;
	}
#endif
}

void UInstanceBufferMeshComponent::PreAllocateInstancesMemory(int32 AddedInstanceCount)
{
	PerInstanceSMData.Reserve(PerInstanceSMData.Num() + AddedInstanceCount);
}

int32 UInstanceBufferMeshComponent::AddInstanceInternal(int32 InstanceIndex, FIBMInstanceData* InNewInstanceData, const FTransform& InstanceTransform)
{
	FIBMInstanceData* NewInstanceData = InNewInstanceData;

	if (NewInstanceData == nullptr)
	{
		NewInstanceData = new(PerInstanceSMData) FIBMInstanceData();
	}

	SetupNewInstanceData(*NewInstanceData, InstanceIndex, InstanceTransform);

#if WITH_EDITOR
	if (SelectedInstances.Num())
	{
		SelectedInstances.Add(false);
	}
#endif

	PartialNavigationUpdate(InstanceIndex);

	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();

	return InstanceIndex;
}

int32 UInstanceBufferMeshComponent::AddInstance(const FTransform& InstanceTransform)
{
	return AddInstanceInternal(PerInstanceSMData.Num(), nullptr, InstanceTransform);
}


bool UInstanceBufferMeshComponent::RemoveInstanceInternal(int32 InstanceIndex, bool InstanceAlreadyRemoved)
{
	// Request navigation update
	PartialNavigationUpdate(InstanceIndex);

	// remove instance
	if (!InstanceAlreadyRemoved && PerInstanceSMData.IsValidIndex(InstanceIndex))
	{
		PerInstanceSMData.RemoveAt(InstanceIndex);
	}

#if WITH_EDITOR
	// remove selection flag if array is filled in
	if (SelectedInstances.IsValidIndex(InstanceIndex))
	{
		SelectedInstances.RemoveAt(InstanceIndex);
	}
#endif

	// update the physics state
	if (bPhysicsStateCreated && InstanceBodies.IsValidIndex(InstanceIndex))
	{
		if (FBodyInstance*& InstanceBody = InstanceBodies[InstanceIndex])
		{
			InstanceBody->TermBody();
			delete InstanceBody;
			InstanceBody = nullptr;

			InstanceBodies.RemoveAt(InstanceIndex);

			// Re-target instance indices for shifting of array.
			for (int32 i = InstanceIndex; i < InstanceBodies.Num(); ++i)
			{
				InstanceBodies[i]->InstanceBodyIndex = i;
			}
		}
	}

	// Force recreation of the render data
	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();
	return true;
}

bool UInstanceBufferMeshComponent::RemoveInstance(int32 InstanceIndex)
{
	return RemoveInstanceInternal(InstanceIndex, false);
}


void UInstanceBufferMeshComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	// We are handling the physics move below, so don't handle it at higher levels
	Super::OnUpdateTransform(UpdateTransformFlags | EUpdateTransformFlags::SkipPhysicsUpdate, Teleport);

	const bool bTeleport = TeleportEnumToFlag(Teleport);

	// Always send new transform to physics
	if (bPhysicsStateCreated && !(EUpdateTransformFlags::SkipPhysicsUpdate & UpdateTransformFlags))
	{
		for (int32 i = 0; i < PerInstanceSMData.Num(); i++)
		{
			const FTransform InstanceTransform(PerInstanceSMData[i].Transform);
			UpdateInstanceBodyTransform(i, InstanceTransform * GetComponentTransform(), bTeleport);
		}
	}
}

void UInstanceBufferMeshComponent::UpdateInstanceBodyTransform(int32 InstanceIndex, const FTransform& WorldSpaceInstanceTransform, bool bTeleport)
{
	check(bPhysicsStateCreated);

	FBodyInstance*& InstanceBodyInstance = InstanceBodies[InstanceIndex];
#if WITH_PHYSX
	if (WorldSpaceInstanceTransform.GetScale3D().IsNearlyZero())
	{
		if (InstanceBodyInstance)
		{
			// delete BodyInstance
			InstanceBodyInstance->TermBody();
			delete InstanceBodyInstance;
			InstanceBodyInstance = nullptr;
		}
	}
	else
	{
		if (InstanceBodyInstance)
		{
			// Update existing BodyInstance
			InstanceBodyInstance->SetBodyTransform(WorldSpaceInstanceTransform, TeleportFlagToEnum(bTeleport));
			InstanceBodyInstance->UpdateBodyScale(WorldSpaceInstanceTransform.GetScale3D());
		}
		else
		{
			// create new BodyInstance
			InstanceBodyInstance = new FBodyInstance();
			InitInstanceBody(InstanceIndex, InstanceBodyInstance);
		}
	}
#endif //WITH_PHYSX
}




bool UInstanceBufferMeshComponent::ShouldCreatePhysicsState() const
{
	return IsRegistered() && !IsBeingDestroyed() && GetStaticMesh() && (bAlwaysCreatePhysicsState || IsCollisionEnabled());
}

float UInstanceBufferMeshComponent::GetTextureStreamingTransformScale() const
{
	// By default if there are no per instance data, use a scale of 1.
	// This is required because some derived class use the instancing system without filling the per instance data. (like landscape grass)
	// In those cases, we assume the instance are spreaded across the bounds with a scale of 1.
	float TransformScale = 1.f; 

	if (PerInstanceSMData.Num() > 0)
	{
		TransformScale = Super::GetTextureStreamingTransformScale();

		float WeightedAxisScaleSum = 0;
		float WeightSum = 0;

		for (int32 InstanceIndex = 0; InstanceIndex < PerInstanceSMData.Num(); InstanceIndex++)
		{
			const float AxisScale = PerInstanceSMData[InstanceIndex].Transform.GetMaximumAxisScale();
			const float Weight = AxisScale; // The weight is the axis scale since we want to weight by surface coverage.
			WeightedAxisScaleSum += AxisScale * Weight;
			WeightSum += Weight;
		}

		if (WeightSum > SMALL_NUMBER)
		{
			TransformScale *= WeightedAxisScaleSum / WeightSum;
		}
	}
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

void UInstanceBufferMeshComponent::ClearInstances()
{
	// Clear all the per-instance data
	PerInstanceSMData.Empty();
	InstanceReorderTable.Empty();
	InstanceDataBuffers.Reset();

	ProxySize = 0;

	// Release any physics representations
	ClearAllInstanceBodies();

	// Force recreation of the render data
	InstanceUpdateCmdBuffer.Reset();
	InstanceUpdateCmdBuffer.Edit();
	MarkRenderStateDirty();

	FNavigationSystem::UpdateComponentData(*this);
}

int32 UInstanceBufferMeshComponent::GetInstanceCount() const
{
	return PerInstanceSMData.Num();
}

void UInstanceBufferMeshComponent::SetCullDistances(int32 StartCullDistance, int32 EndCullDistance)
{
	InstanceStartCullDistance = StartCullDistance;
	InstanceEndCullDistance = EndCullDistance;
	MarkRenderStateDirty();
}

void UInstanceBufferMeshComponent::SetupNewInstanceData(FIBMInstanceData& InOutNewInstanceData, int32 InInstanceIndex, const FTransform& InInstanceTransform)
{
	InOutNewInstanceData.Transform = InInstanceTransform.ToMatrixWithScale();

	if (bPhysicsStateCreated)
	{
		if (InInstanceTransform.GetScale3D().IsNearlyZero())
		{
			InstanceBodies.Insert(nullptr, InInstanceIndex);
		}
		else
		{
			FBodyInstance* NewBodyInstance = new FBodyInstance();
			int32 BodyIndex = InstanceBodies.Insert(NewBodyInstance, InInstanceIndex);
			check(InInstanceIndex == BodyIndex);
			InitInstanceBody(BodyIndex, NewBodyInstance);
		}
	}
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
	if (PerInstanceSMData.Num() > 0)
	{
		MinScale = FVector(MAX_flt);
		MaxScale = FVector(-MAX_flt);

		for (int32 i = 0; i < PerInstanceSMData.Num(); ++i)
		{
			const FIBMInstanceData& InstanceData = PerInstanceSMData[i];
			FVector ScaleVector = InstanceData.Transform.GetScaleVector();

			MinScale = MinScale.ComponentMin(ScaleVector);
			MaxScale = MaxScale.ComponentMax(ScaleVector);
		}
	}
	else
	{
		MinScale = FVector(1.0f);
		MaxScale = FVector(1.0f);
	}
}

void UInstanceBufferMeshComponent::InitPerInstanceRenderData(bool InitializeFromCurrentData, FIBMStaticMeshInstanceData* InSharedInstanceBufferData, bool InRequireCPUAccess)
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

	bool KeepInstanceBufferCPUAccess = GIsEditor || InRequireCPUAccess || ComponentRequestsCPUAccess(this, FeatureLevel);

	if (InSharedInstanceBufferData != nullptr)
	{
		PerInstanceRenderData = MakeShareable(new FIBMPerInstanceRenderData(*InSharedInstanceBufferData, FeatureLevel, KeepInstanceBufferCPUAccess));
	}
	else
	{
		TArray<TRefCountPtr<HHitProxy>> HitProxies;
		FIBMStaticMeshInstanceData InstanceBufferData = FIBMStaticMeshInstanceData();
		
		if (InitializeFromCurrentData)
		{
			// since we recreate data, all pending edits will be uploaded
			InstanceUpdateCmdBuffer.Reset(); 
			BuildRenderData(InstanceBufferData, HitProxies);
		}
			
		PerInstanceRenderData = MakeShareable(new FIBMPerInstanceRenderData(InstanceBufferData, FeatureLevel, KeepInstanceBufferCPUAccess));
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
		const bool InitializeFromCurrentData = PerInstanceSMData.Num() > 0 && (InstanceReorderTable.Num() == PerInstanceSMData.Num() || InstanceReorderTable.Num() == 0);
		InitPerInstanceRenderData(InitializeFromCurrentData);
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
		InitPerInstanceRenderData(true, InstanceDataBuffers.Release());
	}

	// release InstanceDataBuffers
	InstanceDataBuffers.Reset();

	if (PerInstanceRenderData.IsValid())
	{
		if (AActor* Owner = GetOwner())
		{
			ULevel* OwnerLevel = Owner->GetLevel();
			UWorld* OwnerWorld = OwnerLevel ? OwnerLevel->OwningWorld : nullptr;

			if (OwnerWorld && OwnerWorld->GetActiveLightingScenario() != nullptr && OwnerWorld->GetActiveLightingScenario() != OwnerLevel)
			{
				//update the instance data if the lighting scenario isn't the owner level
				InstanceUpdateCmdBuffer.Edit();
			}
		}
	}
}

void UInstanceBufferMeshComponent::PartialNavigationUpdate(int32 InstanceIdx)
{
	// Just update everything
	FNavigationSystem::UpdateComponentData(*this);
}

bool UInstanceBufferMeshComponent::DoCustomNavigableGeometryExport(FNavigableGeometryExport& GeomExport) const
{
	if (GetStaticMesh() && GetStaticMesh()->NavCollision)
	{
		UNavCollisionBase* NavCollision = GetStaticMesh()->NavCollision;
		if (GetStaticMesh()->NavCollision->IsDynamicObstacle())
		{
			return false;
		}
		
		if (NavCollision->HasConvexGeometry())
		{
			GeomExport.ExportCustomMesh(NavCollision->GetConvexCollision().VertexBuffer.GetData(), NavCollision->GetConvexCollision().VertexBuffer.Num(),
				NavCollision->GetConvexCollision().IndexBuffer.GetData(), NavCollision->GetConvexCollision().IndexBuffer.Num(), FTransform::Identity);

			GeomExport.ExportCustomMesh(NavCollision->GetTriMeshCollision().VertexBuffer.GetData(), NavCollision->GetTriMeshCollision().VertexBuffer.Num(),
				NavCollision->GetTriMeshCollision().IndexBuffer.GetData(), NavCollision->GetTriMeshCollision().IndexBuffer.Num(), FTransform::Identity);
		}
		else
		{
			UBodySetup* BodySetup = GetStaticMesh()->BodySetup;
			if (BodySetup)
			{
				GeomExport.ExportRigidBodySetup(*BodySetup, FTransform::Identity);
			}
		}

		// Hook per instance transform delegate
		GeomExport.SetNavDataPerInstanceTransformDelegate(FNavDataPerInstanceTransformDelegate::CreateUObject(this, &UInstanceBufferMeshComponent::GetNavigationPerInstanceTransforms));
	}

	// we don't want "regular" collision export for this component
	return false;
}

void UInstanceBufferMeshComponent::GetNavigationData(FNavigationRelevantData& Data) const
{
	if (GetStaticMesh() && GetStaticMesh()->NavCollision)
	{
		UNavCollisionBase* NavCollision = GetStaticMesh()->NavCollision;
		if (NavCollision->IsDynamicObstacle())
		{
			Data.Modifiers.MarkAsPerInstanceModifier();
			NavCollision->GetNavigationModifier(Data.Modifiers, FTransform::Identity);

			// Hook per instance transform delegate
			Data.NavDataPerInstanceTransformDelegate = FNavDataPerInstanceTransformDelegate::CreateUObject(this, &UInstanceBufferMeshComponent::GetNavigationPerInstanceTransforms);
		}
	}
}

FBox UInstanceBufferMeshComponent::GetNavigationBounds() const
{
	return CalcBounds(GetComponentTransform()).GetBox();
}

void UInstanceBufferMeshComponent::GetNavigationPerInstanceTransforms(const FBox& AreaBox, TArray<FTransform>& InstanceData) const
{
	for (const auto& InstancedData : PerInstanceSMData)
	{
		//TODO: Is it worth doing per instance bounds check here ?
		const FTransform InstanceToComponent(InstancedData.Transform);
		if (!InstanceToComponent.GetScale3D().IsZero())
		{
			InstanceData.Add(InstanceToComponent*GetComponentTransform());
		}
	}
}

void UInstanceBufferMeshComponent::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);

	if (PerInstanceRenderData.IsValid())
	{
		CumulativeResourceSize.AddDedicatedSystemMemoryBytes(PerInstanceRenderData->ResourceSize); 
	}
	
	// component stuff
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(InstanceBodies.GetAllocatedSize());
	for (int32 i=0; i < InstanceBodies.Num(); ++i)
	{
		if (InstanceBodies[i] != NULL && InstanceBodies[i]->IsValidBodyInstance())
		{
			InstanceBodies[i]->GetBodyInstanceResourceSizeEx(CumulativeResourceSize);
		}
	}
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(PerInstanceSMData.GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(InstanceReorderTable.GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(InstanceUpdateCmdBuffer.Cmds.GetAllocatedSize());
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
		InitPerInstanceRenderData(true);		
	}
}

#if WITH_EDITOR
void UInstanceBufferMeshComponent::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent)
{
	if(PropertyChangedEvent.Property != NULL)
	{
		// Only permit editing archetype or instance if instance was changed by an archetype
		if (PropertyChangedEvent.Property->GetFName() == "PerInstanceSMData" && (HasAnyFlags(RF_ArchetypeObject|RF_ClassDefaultObject) ||  PropertyChangedEvent.HasArchetypeInstanceChanged(this)))
		{
			if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd
				|| PropertyChangedEvent.ChangeType == EPropertyChangeType::Duplicate)
			{
				int32 AddedAtIndex = PropertyChangedEvent.GetArrayIndex(PropertyChangedEvent.Property->GetFName().ToString());
				check(AddedAtIndex != INDEX_NONE);

				AddInstanceInternal(AddedAtIndex, &PerInstanceSMData[AddedAtIndex], PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd ? FTransform::Identity : FTransform(PerInstanceSMData[AddedAtIndex].Transform));

				// added via the property editor, so we will want to interactively work with instances
				bHasPerInstanceHitProxies = true;
			}
			else if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayRemove)
			{
				int32 RemovedAtIndex = PropertyChangedEvent.GetArrayIndex(PropertyChangedEvent.Property->GetFName().ToString());
				check(RemovedAtIndex != INDEX_NONE);

				RemoveInstanceInternal(RemovedAtIndex, true);
			}
			else if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear)
			{
				ClearInstances();
			}
			
			MarkRenderStateDirty();
		}
		else if (PropertyChangedEvent.Property->GetFName() == "Transform")
		{
			PartialNavigationUpdate(-1);
			// Force recreation of the render data
			InstanceUpdateCmdBuffer.Edit();
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
	if (InInstanceCount > 0)
	{
		if (PerInstanceSMData.Num() != SelectedInstances.Num())
		{
			SelectedInstances.Init(false, PerInstanceSMData.Num());
		}

		check(InInstanceIndex >= 0 && InInstanceCount > 0);
		check(InInstanceIndex + InInstanceCount - 1 < SelectedInstances.Num());
		
		for (int32 InstanceIndex = InInstanceIndex; InstanceIndex < InInstanceIndex + InInstanceCount; InstanceIndex++)
		{
			if (SelectedInstances.IsValidIndex(InInstanceIndex))
			{
				SelectedInstances[InstanceIndex] = bInSelected;

				if (PerInstanceRenderData.IsValid())
				{
					// Record if the instance is selected
					FColor HitProxyColor(ForceInit);
					bool bSelected = SelectedInstances[InstanceIndex] != 0;
					if (PerInstanceRenderData->HitProxies.IsValidIndex(InstanceIndex))
					{
						HitProxyColor = PerInstanceRenderData->HitProxies[InstanceIndex]->Id.GetColor();
					}

					int32 RenderIndex = InstanceReorderTable.IsValidIndex(InstanceIndex) ? InstanceReorderTable[InstanceIndex] : InstanceIndex;
					if (RenderIndex != INDEX_NONE)
					{
						InstanceUpdateCmdBuffer.SetEditorData(RenderIndex, HitProxyColor, bSelected);
					}
				}
			}			
		}
		
		MarkRenderStateDirty();
	}
#endif
}

void UInstanceBufferMeshComponent::ClearInstanceSelection()
{
#if WITH_EDITOR
	int32 InstanceCount = SelectedInstances.Num();

	if (PerInstanceRenderData.IsValid())
	{
		for (int32 InstanceIndex = 0; InstanceIndex < InstanceCount; InstanceIndex++)
		{
			bool bSelected = SelectedInstances[InstanceIndex] != 0;
			if (bSelected)
			{
				FColor HitProxyColor(ForceInit);
				if (PerInstanceRenderData->HitProxies.IsValidIndex(InstanceIndex))
				{
					HitProxyColor = PerInstanceRenderData->HitProxies[InstanceIndex]->Id.GetColor();
				}
				
				int32 RenderIndex = InstanceReorderTable.IsValidIndex(InstanceIndex) ? InstanceReorderTable[InstanceIndex] : InstanceIndex;
				if (RenderIndex != INDEX_NONE)
				{
					InstanceUpdateCmdBuffer.SetEditorData(RenderIndex, HitProxyColor, false);
				}
			}
		}
	}
	
	SelectedInstances.Empty();
	MarkRenderStateDirty();
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
	else if (CPUInstanceOrigin.IsBound())
	{
		const float ShortScale = 1.0f / 32767.0f;
		auto* InstancingData = (const FInstancingUserData*)BatchElement.UserData;
		check(InstancingData);

		FVector4 InstanceTransform[3];
		FVector4 InstanceLightmapAndShadowMapUVBias;
		FVector4 InstanceOrigin;
		InstancingData->RenderData->PerInstanceRenderData->InstanceBuffer.GetInstanceShaderValues(BatchElement.UserIndex, InstanceTransform, InstanceLightmapAndShadowMapUVBias, InstanceOrigin);

		ShaderBindings.Add(CPUInstanceOrigin, InstanceOrigin);
		ShaderBindings.Add(CPUInstanceTransform, InstanceTransform);
		ShaderBindings.Add(CPUInstanceLightmapAndShadowMapBias, InstanceLightmapAndShadowMapUVBias);
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

			int32 DebugMin = FMath::Min(CVarMinLOD.GetValueOnRenderThread(), InstancingUserData->MeshRenderData->LODResources.Num() - 1);
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

