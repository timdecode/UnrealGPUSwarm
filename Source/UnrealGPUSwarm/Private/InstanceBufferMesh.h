// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	InstancedStaticMesh.h: Instanced static mesh header
=============================================================================*/

#pragma once

#include "CoreMinimal.h"
#include "Containers/IndirectArray.h"
#include "Stats/Stats.h"
#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"
#include "RenderResource.h"
#include "PrimitiveViewRelevance.h"
#include "ShaderParameters.h"
#include "SceneView.h"
#include "VertexFactory.h"
#include "LocalVertexFactory.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "StaticMeshResources.h"
#include "../InstanceBufferMeshComponent.h"
#include "Engine/StaticMesh.h"

#include "StaticMeshLight.h"

#if WITH_EDITOR
#include "LightMap.h"
#include "ShadowMap.h"
#endif

class ULightComponent;
class FIBMStaticMeshInstanceData;

extern TAutoConsoleVariable<float> CVarFoliageMinimumScreenSize;
extern TAutoConsoleVariable<float> CVarFoliageLODDistanceScale;
extern TAutoConsoleVariable<float> CVarRandomLODRange;
extern TAutoConsoleVariable<int32> CVarMinLOD;


// This must match the maximum a user could specify in the material (see 
// FHLSLMaterialTranslator::TextureCoordinate), otherwise the material will attempt 
// to look up a texture coordinate we didn't provide an element for.
extern const int32 InstancedStaticMeshMaxTexCoord;














/*-----------------------------------------------------------------------------
	FIBMStaticMeshInstanceData
-----------------------------------------------------------------------------*/

/** The implementation of the static mesh instance data storage type. */
class FIBMStaticMeshInstanceData
{
	template<typename F>
	struct FInstanceTransformMatrix
	{
		F InstanceTransform1[4];
		F InstanceTransform2[4];
		F InstanceTransform3[4];

		friend FArchive& operator<<(FArchive& Ar, FInstanceTransformMatrix& V)
		{
			return Ar
				<< V.InstanceTransform1[0]
				<< V.InstanceTransform1[1]
				<< V.InstanceTransform1[2]
				<< V.InstanceTransform1[3]

				<< V.InstanceTransform2[0]
				<< V.InstanceTransform2[1]
				<< V.InstanceTransform2[2]
				<< V.InstanceTransform2[3]

				<< V.InstanceTransform3[0]
				<< V.InstanceTransform3[1]
				<< V.InstanceTransform3[2]
				<< V.InstanceTransform3[3];
		}

	};

	struct FInstanceLightMapVector
	{
		int16 InstanceLightmapAndShadowMapUVBias[4];

		friend FArchive& operator<<(FArchive& Ar, FInstanceLightMapVector& V)
		{
			return Ar
				<< V.InstanceLightmapAndShadowMapUVBias[0]
				<< V.InstanceLightmapAndShadowMapUVBias[1]
				<< V.InstanceLightmapAndShadowMapUVBias[2]
				<< V.InstanceLightmapAndShadowMapUVBias[3];
		}
	};

public:

	/**
	 * Constructor - we support 32-bit floats only
	 */
	FIBMStaticMeshInstanceData()
	{
		AllocateBuffers(0);
	}

	~FIBMStaticMeshInstanceData()
	{
		delete InstanceOriginData;
		delete InstanceLightmapData;
		delete InstanceTransformData;
	}

	void Serialize(FArchive& Ar);

	void AllocateInstances(int32 InNumInstances, EResizeBufferFlags BufferFlags, bool DestroyExistingInstances)
	{
		NumInstances = InNumInstances;

		if (DestroyExistingInstances)
		{
			InstanceOriginData->Empty(NumInstances);
			InstanceLightmapData->Empty(NumInstances);
			InstanceTransformData->Empty(NumInstances);
		}

		// We cannot write directly to the data on all platforms,
		// so we make a TArray of the right type, then assign it
		InstanceOriginData->ResizeBuffer(NumInstances, BufferFlags);
		InstanceOriginDataPtr = InstanceOriginData->GetDataPointer();

		InstanceLightmapData->ResizeBuffer(NumInstances, BufferFlags);
		InstanceLightmapDataPtr = InstanceLightmapData->GetDataPointer();

		InstanceTransformData->ResizeBuffer(NumInstances, BufferFlags);
		InstanceTransformDataPtr = InstanceTransformData->GetDataPointer();
	}

	FORCEINLINE_DEBUGGABLE int32 IsValidIndex(int32 Index) const
	{
		return InstanceOriginData->IsValidIndex(Index);
	}

	FORCEINLINE_DEBUGGABLE void GetInstanceTransform(int32 InstanceIndex, FMatrix& Transform) const
	{
		FVector4 TransformVec[3];

		GetInstanceTransformInternal<float>(InstanceIndex, TransformVec);

		Transform.M[0][0] = TransformVec[0][0];
		Transform.M[0][1] = TransformVec[0][1];
		Transform.M[0][2] = TransformVec[0][2];
		Transform.M[0][3] = 0.f;

		Transform.M[1][0] = TransformVec[1][0];
		Transform.M[1][1] = TransformVec[1][1];
		Transform.M[1][2] = TransformVec[1][2];
		Transform.M[1][3] = 0.f;

		Transform.M[2][0] = TransformVec[2][0];
		Transform.M[2][1] = TransformVec[2][1];
		Transform.M[2][2] = TransformVec[2][2];
		Transform.M[2][3] = 0.f;

		FVector4 Origin;
		GetInstanceOriginInternal(InstanceIndex, Origin);

		Transform.M[3][0] = Origin.X;
		Transform.M[3][1] = Origin.Y;
		Transform.M[3][2] = Origin.Z;
		Transform.M[3][3] = 0.f;
	}

	FORCEINLINE_DEBUGGABLE void GetInstanceShaderValues(int32 InstanceIndex, FVector4(&InstanceTransform)[3], FVector4& InstanceLightmapAndShadowMapUVBias, FVector4& InstanceOrigin) const
	{
		GetInstanceTransformInternal<float>(InstanceIndex, InstanceTransform);
		GetInstanceLightMapDataInternal(InstanceIndex, InstanceLightmapAndShadowMapUVBias);
		GetInstanceOriginInternal(InstanceIndex, InstanceOrigin);
	}

	FORCEINLINE_DEBUGGABLE void SetInstance(int32 InstanceIndex, const FMatrix& Transform, float RandomInstanceID)
	{
		FVector4 Origin(Transform.M[3][0], Transform.M[3][1], Transform.M[3][2], RandomInstanceID);
		SetInstanceOriginInternal(InstanceIndex, Origin);

		FVector4 InstanceTransform[3];
		InstanceTransform[0] = FVector4(Transform.M[0][0], Transform.M[0][1], Transform.M[0][2], 0.0f);
		InstanceTransform[1] = FVector4(Transform.M[1][0], Transform.M[1][1], Transform.M[1][2], 0.0f);
		InstanceTransform[2] = FVector4(Transform.M[2][0], Transform.M[2][1], Transform.M[2][2], 0.0f);

		SetInstanceTransformInternal<float>(InstanceIndex, InstanceTransform);
		SetInstanceLightMapDataInternal(InstanceIndex, FVector4(0, 0, 0, 0));
	}

	FORCEINLINE_DEBUGGABLE void SetInstance(int32 InstanceIndex, const FMatrix& Transform, float RandomInstanceID, const FVector2D& LightmapUVBias, const FVector2D& ShadowmapUVBias)
	{
		FVector4 Origin(Transform.M[3][0], Transform.M[3][1], Transform.M[3][2], RandomInstanceID);
		SetInstanceOriginInternal(InstanceIndex, Origin);

		FVector4 InstanceTransform[3];
		InstanceTransform[0] = FVector4(Transform.M[0][0], Transform.M[0][1], Transform.M[0][2], 0.0f);
		InstanceTransform[1] = FVector4(Transform.M[1][0], Transform.M[1][1], Transform.M[1][2], 0.0f);
		InstanceTransform[2] = FVector4(Transform.M[2][0], Transform.M[2][1], Transform.M[2][2], 0.0f);

		SetInstanceTransformInternal<float>(InstanceIndex, InstanceTransform);
		SetInstanceLightMapDataInternal(InstanceIndex, FVector4(LightmapUVBias.X, LightmapUVBias.Y, ShadowmapUVBias.X, ShadowmapUVBias.Y));
	}

	FORCEINLINE void SetInstance(int32 InstanceIndex, const FMatrix& Transform, const FVector2D& LightmapUVBias, const FVector2D& ShadowmapUVBias)
	{
		FVector4 OldOrigin;
		GetInstanceOriginInternal(InstanceIndex, OldOrigin);

		FVector4 NewOrigin(Transform.M[3][0], Transform.M[3][1], Transform.M[3][2], OldOrigin.Component(3));
		SetInstanceOriginInternal(InstanceIndex, NewOrigin);

		FVector4 InstanceTransform[3];
		InstanceTransform[0] = FVector4(Transform.M[0][0], Transform.M[0][1], Transform.M[0][2], 0.0f);
		InstanceTransform[1] = FVector4(Transform.M[1][0], Transform.M[1][1], Transform.M[1][2], 0.0f);
		InstanceTransform[2] = FVector4(Transform.M[2][0], Transform.M[2][1], Transform.M[2][2], 0.0f);

		SetInstanceTransformInternal<float>(InstanceIndex, InstanceTransform);
		SetInstanceLightMapDataInternal(InstanceIndex, FVector4(LightmapUVBias.X, LightmapUVBias.Y, ShadowmapUVBias.X, ShadowmapUVBias.Y));
	}

	FORCEINLINE void SetInstanceLightMapData(int32 InstanceIndex, const FVector2D& LightmapUVBias, const FVector2D& ShadowmapUVBias)
	{
		SetInstanceLightMapDataInternal(InstanceIndex, FVector4(LightmapUVBias.X, LightmapUVBias.Y, ShadowmapUVBias.X, ShadowmapUVBias.Y));
	}

	FORCEINLINE_DEBUGGABLE void NullifyInstance(int32 InstanceIndex)
	{
		SetInstanceOriginInternal(InstanceIndex, FVector4(0, 0, 0, 0));

		FVector4 InstanceTransform[3];
		InstanceTransform[0] = FVector4(0, 0, 0, 0);
		InstanceTransform[1] = FVector4(0, 0, 0, 0);
		InstanceTransform[2] = FVector4(0, 0, 0, 0);

		SetInstanceTransformInternal<float>(InstanceIndex, InstanceTransform);
		SetInstanceLightMapDataInternal(InstanceIndex, FVector4(0, 0, 0, 0));
	}

	FORCEINLINE_DEBUGGABLE void SetInstanceEditorData(int32 InstanceIndex, FColor HitProxyColor, bool bSelected)
	{
		FVector4 InstanceTransform[3];

		GetInstanceTransformInternal<float>(InstanceIndex, InstanceTransform);
		InstanceTransform[0][3] = ((float)HitProxyColor.R) + (bSelected ? 256.f : 0.0f);
		InstanceTransform[1][3] = (float)HitProxyColor.G;
		InstanceTransform[2][3] = (float)HitProxyColor.B;
		SetInstanceTransformInternal<float>(InstanceIndex, InstanceTransform);
	}

	FORCEINLINE_DEBUGGABLE void ClearInstanceEditorData(int32 InstanceIndex)
	{
		FVector4 InstanceTransform[3];
		GetInstanceTransformInternal<float>(InstanceIndex, InstanceTransform);
		InstanceTransform[0][3] = 0.0f;
		InstanceTransform[1][3] = 0.0f;
		InstanceTransform[2][3] = 0.0f;
		SetInstanceTransformInternal<float>(InstanceIndex, InstanceTransform);
	}

	FORCEINLINE_DEBUGGABLE void SwapInstance(int32 Index1, int32 Index2)
	{
		{
			FInstanceTransformMatrix<float>* ElementData = reinterpret_cast<FInstanceTransformMatrix<float>*>(InstanceTransformDataPtr);
			uint32 CurrentSize = InstanceTransformData->Num() * InstanceTransformData->GetStride();
			check((void*)((&ElementData[Index1]) + 1) <= (void*)(InstanceTransformDataPtr + CurrentSize));
			check((void*)((&ElementData[Index1]) + 0) >= (void*)(InstanceTransformDataPtr));
			check((void*)((&ElementData[Index2]) + 1) <= (void*)(InstanceTransformDataPtr + CurrentSize));
			check((void*)((&ElementData[Index2]) + 0) >= (void*)(InstanceTransformDataPtr));

			FInstanceTransformMatrix<float> TempStore = ElementData[Index1];
			ElementData[Index1] = ElementData[Index2];
			ElementData[Index2] = TempStore;
		}
		{

			FVector4* ElementData = reinterpret_cast<FVector4*>(InstanceOriginDataPtr);
			uint32 CurrentSize = InstanceOriginData->Num() * InstanceOriginData->GetStride();
			check((void*)((&ElementData[Index1]) + 1) <= (void*)(InstanceOriginDataPtr + CurrentSize));
			check((void*)((&ElementData[Index1]) + 0) >= (void*)(InstanceOriginDataPtr));
			check((void*)((&ElementData[Index2]) + 1) <= (void*)(InstanceOriginDataPtr + CurrentSize));
			check((void*)((&ElementData[Index2]) + 0) >= (void*)(InstanceOriginDataPtr));

			FVector4 TempStore = ElementData[Index1];
			ElementData[Index1] = ElementData[Index2];
			ElementData[Index2] = TempStore;
		}
		{
			FInstanceLightMapVector* ElementData = reinterpret_cast<FInstanceLightMapVector*>(InstanceLightmapDataPtr);
			uint32 CurrentSize = InstanceLightmapData->Num() * InstanceLightmapData->GetStride();
			check((void*)((&ElementData[Index1]) + 1) <= (void*)(InstanceLightmapDataPtr + CurrentSize));
			check((void*)((&ElementData[Index1]) + 0) >= (void*)(InstanceLightmapDataPtr));
			check((void*)((&ElementData[Index2]) + 1) <= (void*)(InstanceLightmapDataPtr + CurrentSize));
			check((void*)((&ElementData[Index2]) + 0) >= (void*)(InstanceLightmapDataPtr));

			FInstanceLightMapVector TempStore = ElementData[Index1];
			ElementData[Index1] = ElementData[Index2];
			ElementData[Index2] = TempStore;
		}
	}

	FORCEINLINE_DEBUGGABLE int32 GetNumInstances() const
	{
		return NumInstances;
	}

	FORCEINLINE_DEBUGGABLE void SetAllowCPUAccess(bool InNeedsCPUAccess)
	{
		if (InstanceOriginData)
		{
			InstanceOriginData->GetResourceArray()->SetAllowCPUAccess(InNeedsCPUAccess);
		}
		if (InstanceLightmapData)
		{
			InstanceLightmapData->GetResourceArray()->SetAllowCPUAccess(InNeedsCPUAccess);
		}
		if (InstanceTransformData)
		{
			InstanceTransformData->GetResourceArray()->SetAllowCPUAccess(InNeedsCPUAccess);
		}
	}

	FORCEINLINE_DEBUGGABLE bool GetTranslationUsesHalfs() const
	{
		return false;
	}

	FORCEINLINE_DEBUGGABLE FResourceArrayInterface* GetOriginResourceArray()
	{
		return InstanceOriginData->GetResourceArray();
	}

	FORCEINLINE_DEBUGGABLE FResourceArrayInterface* GetTransformResourceArray()
	{
		return InstanceTransformData->GetResourceArray();
	}

	FORCEINLINE_DEBUGGABLE FResourceArrayInterface* GetLightMapResourceArray()
	{
		return InstanceLightmapData->GetResourceArray();
	}

	FORCEINLINE_DEBUGGABLE uint32 GetOriginStride()
	{
		return InstanceOriginData->GetStride();
	}

	FORCEINLINE_DEBUGGABLE uint32 GetTransformStride()
	{
		return InstanceTransformData->GetStride();
	}

	FORCEINLINE_DEBUGGABLE uint32 GetLightMapStride()
	{
		return InstanceLightmapData->GetStride();
	}

	FORCEINLINE_DEBUGGABLE SIZE_T GetResourceSize() const
	{
		return	InstanceOriginData->GetResourceSize() +
			InstanceTransformData->GetResourceSize() +
			InstanceLightmapData->GetResourceSize();
	}

private:
	template<typename T>
	FORCEINLINE_DEBUGGABLE void GetInstanceTransformInternal(int32 InstanceIndex, FVector4(&Transform)[3]) const
	{
		FInstanceTransformMatrix<T>* ElementData = reinterpret_cast<FInstanceTransformMatrix<T>*>(InstanceTransformDataPtr);
		uint32 CurrentSize = InstanceTransformData->Num() * InstanceTransformData->GetStride();
		check((void*)((&ElementData[InstanceIndex]) + 1) <= (void*)(InstanceTransformDataPtr + CurrentSize));
		check((void*)((&ElementData[InstanceIndex]) + 0) >= (void*)(InstanceTransformDataPtr));

		Transform[0][0] = ElementData[InstanceIndex].InstanceTransform1[0];
		Transform[0][1] = ElementData[InstanceIndex].InstanceTransform1[1];
		Transform[0][2] = ElementData[InstanceIndex].InstanceTransform1[2];
		Transform[0][3] = ElementData[InstanceIndex].InstanceTransform1[3];

		Transform[1][0] = ElementData[InstanceIndex].InstanceTransform2[0];
		Transform[1][1] = ElementData[InstanceIndex].InstanceTransform2[1];
		Transform[1][2] = ElementData[InstanceIndex].InstanceTransform2[2];
		Transform[1][3] = ElementData[InstanceIndex].InstanceTransform2[3];

		Transform[2][0] = ElementData[InstanceIndex].InstanceTransform3[0];
		Transform[2][1] = ElementData[InstanceIndex].InstanceTransform3[1];
		Transform[2][2] = ElementData[InstanceIndex].InstanceTransform3[2];
		Transform[2][3] = ElementData[InstanceIndex].InstanceTransform3[3];
	}

	FORCEINLINE_DEBUGGABLE void GetInstanceOriginInternal(int32 InstanceIndex, FVector4 &Origin) const
	{
		FVector4* ElementData = reinterpret_cast<FVector4*>(InstanceOriginDataPtr);
		uint32 CurrentSize = InstanceOriginData->Num() * InstanceOriginData->GetStride();
		check((void*)((&ElementData[InstanceIndex]) + 1) <= (void*)(InstanceOriginDataPtr + CurrentSize));
		check((void*)((&ElementData[InstanceIndex]) + 0) >= (void*)(InstanceOriginDataPtr));

		Origin = ElementData[InstanceIndex];
	}

	FORCEINLINE_DEBUGGABLE void GetInstanceLightMapDataInternal(int32 InstanceIndex, FVector4 &LightmapData) const
	{
		FInstanceLightMapVector* ElementData = reinterpret_cast<FInstanceLightMapVector*>(InstanceLightmapDataPtr);
		uint32 CurrentSize = InstanceLightmapData->Num() * InstanceLightmapData->GetStride();
		check((void*)((&ElementData[InstanceIndex]) + 1) <= (void*)(InstanceLightmapDataPtr + CurrentSize));
		check((void*)((&ElementData[InstanceIndex]) + 0) >= (void*)(InstanceLightmapDataPtr));

		LightmapData = FVector4
		(
			float(ElementData[InstanceIndex].InstanceLightmapAndShadowMapUVBias[0]) / 32767.0f,
			float(ElementData[InstanceIndex].InstanceLightmapAndShadowMapUVBias[1]) / 32767.0f,
			float(ElementData[InstanceIndex].InstanceLightmapAndShadowMapUVBias[2]) / 32767.0f,
			float(ElementData[InstanceIndex].InstanceLightmapAndShadowMapUVBias[3]) / 32767.0f
		);
	}

	template<typename T>
	FORCEINLINE_DEBUGGABLE void SetInstanceTransformInternal(int32 InstanceIndex, FVector4(Transform)[3]) const
	{
		FInstanceTransformMatrix<T>* ElementData = reinterpret_cast<FInstanceTransformMatrix<T>*>(InstanceTransformDataPtr);
		uint32 CurrentSize = InstanceTransformData->Num() * InstanceTransformData->GetStride();
		check((void*)((&ElementData[InstanceIndex]) + 1) <= (void*)(InstanceTransformDataPtr + CurrentSize));
		check((void*)((&ElementData[InstanceIndex]) + 0) >= (void*)(InstanceTransformDataPtr));

		ElementData[InstanceIndex].InstanceTransform1[0] = Transform[0][0];
		ElementData[InstanceIndex].InstanceTransform1[1] = Transform[0][1];
		ElementData[InstanceIndex].InstanceTransform1[2] = Transform[0][2];
		ElementData[InstanceIndex].InstanceTransform1[3] = Transform[0][3];

		ElementData[InstanceIndex].InstanceTransform2[0] = Transform[1][0];
		ElementData[InstanceIndex].InstanceTransform2[1] = Transform[1][1];
		ElementData[InstanceIndex].InstanceTransform2[2] = Transform[1][2];
		ElementData[InstanceIndex].InstanceTransform2[3] = Transform[1][3];

		ElementData[InstanceIndex].InstanceTransform3[0] = Transform[2][0];
		ElementData[InstanceIndex].InstanceTransform3[1] = Transform[2][1];
		ElementData[InstanceIndex].InstanceTransform3[2] = Transform[2][2];
		ElementData[InstanceIndex].InstanceTransform3[3] = Transform[2][3];
	}

	FORCEINLINE_DEBUGGABLE void SetInstanceOriginInternal(int32 InstanceIndex, const FVector4& Origin) const
	{
		FVector4* ElementData = reinterpret_cast<FVector4*>(InstanceOriginDataPtr);
		uint32 CurrentSize = InstanceOriginData->Num() * InstanceOriginData->GetStride();
		check((void*)((&ElementData[InstanceIndex]) + 1) <= (void*)(InstanceOriginDataPtr + CurrentSize));
		check((void*)((&ElementData[InstanceIndex]) + 0) >= (void*)(InstanceOriginDataPtr));

		ElementData[InstanceIndex] = Origin;
	}

	FORCEINLINE_DEBUGGABLE void SetInstanceLightMapDataInternal(int32 InstanceIndex, const FVector4& LightmapData) const
	{
		FInstanceLightMapVector* ElementData = reinterpret_cast<FInstanceLightMapVector*>(InstanceLightmapDataPtr);
		uint32 CurrentSize = InstanceLightmapData->Num() * InstanceLightmapData->GetStride();
		check((void*)((&ElementData[InstanceIndex]) + 1) <= (void*)(InstanceLightmapDataPtr + CurrentSize));
		check((void*)((&ElementData[InstanceIndex]) + 0) >= (void*)(InstanceLightmapDataPtr));

		ElementData[InstanceIndex].InstanceLightmapAndShadowMapUVBias[0] = FMath::Clamp<int32>(FMath::TruncToInt(LightmapData.X * 32767.0f), MIN_int16, MAX_int16);
		ElementData[InstanceIndex].InstanceLightmapAndShadowMapUVBias[1] = FMath::Clamp<int32>(FMath::TruncToInt(LightmapData.Y * 32767.0f), MIN_int16, MAX_int16);
		ElementData[InstanceIndex].InstanceLightmapAndShadowMapUVBias[2] = FMath::Clamp<int32>(FMath::TruncToInt(LightmapData.Z * 32767.0f), MIN_int16, MAX_int16);
		ElementData[InstanceIndex].InstanceLightmapAndShadowMapUVBias[3] = FMath::Clamp<int32>(FMath::TruncToInt(LightmapData.W * 32767.0f), MIN_int16, MAX_int16);
	}

	void AllocateBuffers(int32 InNumInstances, EResizeBufferFlags BufferFlags = EResizeBufferFlags::None)
	{
		delete InstanceOriginData;
		InstanceOriginDataPtr = nullptr;

		delete InstanceTransformData;
		InstanceTransformDataPtr = nullptr;

		delete InstanceLightmapData;
		InstanceLightmapDataPtr = nullptr;

		InstanceOriginData = new TStaticMeshVertexData<FVector4>();
		InstanceOriginData->ResizeBuffer(InNumInstances, BufferFlags);
		InstanceLightmapData = new TStaticMeshVertexData<FInstanceLightMapVector>();
		InstanceLightmapData->ResizeBuffer(InNumInstances, BufferFlags);

		InstanceTransformData = new TStaticMeshVertexData<FInstanceTransformMatrix<float>>();
		InstanceTransformData->ResizeBuffer(InNumInstances, BufferFlags);
	}

	FStaticMeshVertexDataInterface* InstanceOriginData = nullptr;
	uint8* InstanceOriginDataPtr = nullptr;

	FStaticMeshVertexDataInterface* InstanceTransformData = nullptr;
	uint8* InstanceTransformDataPtr = nullptr;

	FStaticMeshVertexDataInterface* InstanceLightmapData = nullptr;
	uint8* InstanceLightmapDataPtr = nullptr;

	int32 NumInstances = 0;
};














/*-----------------------------------------------------------------------------
	FIBMInstanceBuffer
-----------------------------------------------------------------------------*/


/** A vertex buffer of positions. */
class FIBMInstanceBuffer : public FRenderResource
{
public:

	/** Default constructor. */
	FIBMInstanceBuffer(ERHIFeatureLevel::Type InFeatureLevel, bool InRequireCPUAccess);

	/** Destructor. */
	~FIBMInstanceBuffer();

	/**
	 * Initializes the buffer with the component's data.
	 * @param Other - instance data, this call assumes the memory, so this will be empty after the call
	 */
	UNREALGPUSWARM_API void InitFromPreallocatedData(FIBMStaticMeshInstanceData& Other);
	UNREALGPUSWARM_API void UpdateFromCommandBuffer_Concurrent(FIBMInstanceUpdateCmdBuffer& CmdBuffer);

	/**
	 * Specialized assignment operator, only used when importing LOD's. 
	 */
	void operator=(const FIBMInstanceBuffer &Other);

	// Other accessors.
	FORCEINLINE uint32 GetNumInstances() const
	{
		return InstanceData->GetNumInstances();
	}

	FORCEINLINE  void GetInstanceTransform(int32 InstanceIndex, FMatrix& Transform) const
	{
		InstanceData->GetInstanceTransform(InstanceIndex, Transform);
	}

	FORCEINLINE  void GetInstanceShaderValues(int32 InstanceIndex, FVector4 (&InstanceTransform)[3], FVector4& InstanceLightmapAndShadowMapUVBias, FVector4& InstanceOrigin) const
	{
		InstanceData->GetInstanceShaderValues(InstanceIndex, InstanceTransform, InstanceLightmapAndShadowMapUVBias, InstanceOrigin);
	}
	
	FORCEINLINE FIBMStaticMeshInstanceData* GetInstanceData() const
	{
		return InstanceData.Get();
	}

	// FRenderResource interface.
	virtual void InitRHI() override;
	virtual void ReleaseRHI() override;
	virtual void InitResource() override;
	virtual void ReleaseResource() override;
	virtual FString GetFriendlyName() const override { return TEXT("Static-mesh instances"); }
	SIZE_T GetResourceSize() const;

	void BindInstanceVertexBuffer(const class FVertexFactory* VertexFactory, struct FInstanceBufferMeshDataType& InstancedStaticMeshData) const;

public:
	/** The vertex data storage type */
	TSharedPtr<FIBMStaticMeshInstanceData, ESPMode::ThreadSafe> InstanceData;

	/** Keep CPU copy of instance data*/
	bool RequireCPUAccess;

public:
	class FInstanceOriginBuffer : public FVertexBuffer
	{
		virtual FString GetFriendlyName() const override { return TEXT("FInstanceOriginBuffer"); }
	} InstanceOriginBuffer;
	FShaderResourceViewRHIRef InstanceOriginSRV;

	class FInstanceTransformBuffer : public FVertexBuffer
	{
		virtual FString GetFriendlyName() const override { return TEXT("FInstanceTransformBuffer"); }
	} InstanceTransformBuffer;
	FShaderResourceViewRHIRef InstanceTransformSRV;

	class FInstanceLightmapBuffer : public FVertexBuffer
	{
		virtual FString GetFriendlyName() const override { return TEXT("FInstanceLightmapBuffer"); }
	} InstanceLightmapBuffer;
	FShaderResourceViewRHIRef InstanceLightmapSRV;

	/** Delete existing resources */
	void CleanUp();

	void CreateVertexBuffer(FResourceArrayInterface* InResourceArray, uint32 InUsage, uint32 InStride, uint8 InFormat, FVertexBufferRHIRef& OutVertexBufferRHI, FShaderResourceViewRHIRef& OutInstanceSRV);
	
	/**  */
	void UpdateFromCommandBuffer_RenderThread(FIBMInstanceUpdateCmdBuffer& CmdBuffer);
};

/*-----------------------------------------------------------------------------
	FInstanceBufferMeshVertexFactory
-----------------------------------------------------------------------------*/

struct FInstancingUserData
{
	class FInstanceBufferMeshRenderData* RenderData;
	class FStaticMeshRenderData* MeshRenderData;

	int32 StartCullDistance;
	int32 EndCullDistance;

	int32 MinLOD;

	bool bRenderSelected;
	bool bRenderUnselected;
	FVector AverageInstancesScale;
};

struct FInstanceBufferMeshDataType
{
	/** The stream to read the mesh transform from. */
	FVertexStreamComponent InstanceOriginComponent;

	/** The stream to read the mesh transform from. */
	FVertexStreamComponent InstanceTransformComponent[3];

	/** The stream to read the Lightmap Bias and Random instance ID from. */
	FVertexStreamComponent InstanceLightmapAndShadowMapUVBiasComponent;

	FRHIShaderResourceView* InstanceOriginSRV = nullptr;
	FRHIShaderResourceView* InstanceTransformSRV = nullptr;
	FRHIShaderResourceView* InstanceLightmapSRV = nullptr;

	/** Used to track state for debugging. */
	uint32 NumInstances = 0;
	bool bInitialized = false;
};

/**
 * A vertex factory for instanced static meshes
 */
struct FInstanceBufferMeshVertexFactory : public FLocalVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FInstanceBufferMeshVertexFactory);
public:
	FInstanceBufferMeshVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FLocalVertexFactory(InFeatureLevel, "FInstanceBufferMeshVertexFactory")
	{
	}

	struct FDataType : public FInstanceBufferMeshDataType, public FLocalVertexFactory::FDataType
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

		OutEnvironment.SetDefine(TEXT("USE_INSTANCING"),TEXT("1"));
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
	void Copy(const FInstanceBufferMeshVertexFactory& Other);

	// FRenderResource interface.
	virtual void InitRHI() override;

	static FVertexFactoryShaderParameters* ConstructShaderParameters(EShaderFrequency ShaderFrequency);

	/** Make sure we account for changes in the signature of GetStaticBatchElementVisibility() */
	static CONSTEXPR uint32 NumBitsForVisibilityMask()
	{		
		return 8 * sizeof(decltype(((FInstanceBufferMeshVertexFactory*)nullptr)->GetStaticBatchElementVisibility(FSceneView(FSceneViewInitOptions()), nullptr)));
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


struct FEmulatedInstanceBufferMeshVertexFactory : public FInstanceBufferMeshVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FEmulatedInstanceBufferMeshVertexFactory);
public:
	FEmulatedInstanceBufferMeshVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FInstanceBufferMeshVertexFactory(InFeatureLevel)
	{
	}

	/**
	 * Should we cache the material's shadertype on this platform with this vertex factory? 
	 */
	static bool ShouldCompilePermutation(EShaderPlatform Platform, const class FMaterial* Material, const class FShaderType* ShaderType)
	{
		// Android may not support on old devices
		return	(Platform == SP_OPENGL_ES2_ANDROID)
				&& (Material->IsUsedWithInstancedStaticMeshes() || Material->IsSpecialEngineMaterial())
				&& FLocalVertexFactory::ShouldCompilePermutation(Platform, Material, ShaderType);
	}

	/**
	 * Modify compile environment to enable instancing
	 * @param OutEnvironment - shader compile environment to modify
	 */
	static void ModifyCompilationEnvironment(const FVertexFactoryType* Type, EShaderPlatform Platform, const FMaterial* Material, FShaderCompilerEnvironment& OutEnvironment)
	{
		FInstanceBufferMeshVertexFactory::ModifyCompilationEnvironment(Type, Platform, Material, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("USE_INSTANCING_EMULATED"), TEXT("1"));
	}
};

class FInstanceBufferMeshVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
	virtual void Bind(const FShaderParameterMap& ParameterMap) override
	{
		InstancingFadeOutParamsParameter.Bind(ParameterMap, TEXT("InstancingFadeOutParams"));
		InstancingViewZCompareZeroParameter.Bind(ParameterMap, TEXT("InstancingViewZCompareZero"));
		InstancingViewZCompareOneParameter.Bind(ParameterMap, TEXT("InstancingViewZCompareOne"));
		InstancingViewZConstantParameter.Bind(ParameterMap, TEXT("InstancingViewZConstant"));
		InstancingWorldViewOriginZeroParameter.Bind(ParameterMap, TEXT("InstancingWorldViewOriginZero"));
		InstancingWorldViewOriginOneParameter.Bind(ParameterMap, TEXT("InstancingWorldViewOriginOne"));
		CPUInstanceOrigin.Bind(ParameterMap, TEXT("CPUInstanceOrigin"));
		CPUInstanceTransform.Bind(ParameterMap, TEXT("CPUInstanceTransform"));
		CPUInstanceLightmapAndShadowMapBias.Bind(ParameterMap, TEXT("CPUInstanceLightmapAndShadowMapBias"));
		VertexFetch_InstanceOriginBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceOriginBuffer"));
		VertexFetch_InstanceTransformBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceTransformBuffer"));
		VertexFetch_InstanceLightmapBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceLightmapBuffer"));
		InstanceOffset.Bind(ParameterMap, TEXT("InstanceOffset"));
	}

	void GetElementShaderBindingsBase(
		const FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		FRHIUniformBuffer* VertexFactoryUniformBuffer,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const;

	virtual void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
		) const override;

	void Serialize(FArchive& Ar) override
	{
		Ar << InstancingFadeOutParamsParameter;
		Ar << InstancingViewZCompareZeroParameter;
		Ar << InstancingViewZCompareOneParameter;
		Ar << InstancingViewZConstantParameter;
		Ar << InstancingWorldViewOriginZeroParameter;
		Ar << InstancingWorldViewOriginOneParameter;
		Ar << CPUInstanceOrigin;
		Ar << CPUInstanceTransform;
		Ar << CPUInstanceLightmapAndShadowMapBias;
		Ar << VertexFetch_InstanceOriginBufferParameter;
		Ar << VertexFetch_InstanceTransformBufferParameter;
		Ar << VertexFetch_InstanceLightmapBufferParameter;
		Ar << InstanceOffset;
	}

	virtual uint32 GetSize() const override { return sizeof(*this); }

private:
	FShaderParameter InstancingFadeOutParamsParameter;
	FShaderParameter InstancingViewZCompareZeroParameter;
	FShaderParameter InstancingViewZCompareOneParameter;
	FShaderParameter InstancingViewZConstantParameter;
	FShaderParameter InstancingWorldViewOriginZeroParameter;
	FShaderParameter InstancingWorldViewOriginOneParameter;

	FShaderParameter CPUInstanceOrigin;
	FShaderParameter CPUInstanceTransform;
	FShaderParameter CPUInstanceLightmapAndShadowMapBias;

	FShaderResourceParameter VertexFetch_InstanceOriginBufferParameter;
	FShaderResourceParameter VertexFetch_InstanceTransformBufferParameter;
	FShaderResourceParameter VertexFetch_InstanceLightmapBufferParameter;
	FShaderParameter InstanceOffset;
};

struct FIBMInstanceUpdateCmdBuffer;
/*-----------------------------------------------------------------------------
	FIBMPerInstanceRenderData
	Holds render data that can persist between scene proxy reconstruction
-----------------------------------------------------------------------------*/
struct FIBMPerInstanceRenderData
{
	// Should be always constructed on main thread
	FIBMPerInstanceRenderData(FIBMStaticMeshInstanceData& Other, ERHIFeatureLevel::Type InFeaureLevel, bool InRequireCPUAccess);
	~FIBMPerInstanceRenderData();

	/**
	 * Call to update the Instance buffer with pre allocated data without recreating the FIBMPerInstanceRenderData
	 * @param InComponent - The owning component
	 * @param InOther - The Instance data to copy into our instance buffer
	 */
	UNREALGPUSWARM_API void UpdateFromPreallocatedData(FIBMStaticMeshInstanceData& InOther);
		
	/**
	*/
	UNREALGPUSWARM_API void UpdateFromCommandBuffer(FIBMInstanceUpdateCmdBuffer& CmdBuffer);

	/** Hit proxies for the instances */
	TArray<TRefCountPtr<HHitProxy>>		HitProxies;

	/** cached per-instance resource size*/
	SIZE_T								ResourceSize;

	/** Instance buffer */
	FIBMInstanceBuffer			InstanceBuffer;
	TSharedPtr<FIBMStaticMeshInstanceData, ESPMode::ThreadSafe> InstanceBuffer_GameThread;
};


/*-----------------------------------------------------------------------------
	FInstanceBufferMeshRenderData
-----------------------------------------------------------------------------*/

class FInstanceBufferMeshRenderData
{
public:

	FInstanceBufferMeshRenderData(UInstanceBufferMeshComponent* InComponent, ERHIFeatureLevel::Type InFeatureLevel)
	  : Component(InComponent)
	  , PerInstanceRenderData(InComponent->PerInstanceRenderData)
	  , LODModels(Component->GetStaticMesh()->RenderData->LODResources)
	  , FeatureLevel(InFeatureLevel)
	{
		check(PerInstanceRenderData.IsValid());
		// Allocate the vertex factories for each LOD
		InitVertexFactories();
		RegisterSpeedTreeWind();
	}

	void ReleaseResources(FSceneInterface* Scene, const UStaticMesh* StaticMesh)
	{
		// unregister SpeedTree wind with the scene
		if (Scene && StaticMesh && StaticMesh->SpeedTreeWind.IsValid())
		{
			for (int32 LODIndex = 0; LODIndex < VertexFactories.Num(); LODIndex++)
			{
				Scene->RemoveSpeedTreeWind_RenderThread(&VertexFactories[LODIndex], StaticMesh);
			}
		}

		for (int32 LODIndex = 0; LODIndex < VertexFactories.Num(); LODIndex++)
		{
			VertexFactories[LODIndex].ReleaseResource();
		}
	}

	/** Source component */
	UInstanceBufferMeshComponent* Component;

	/** Per instance render data, could be shared with component */
	TSharedPtr<FIBMPerInstanceRenderData, ESPMode::ThreadSafe> PerInstanceRenderData;

	/** Vertex factory */
	TIndirectArray<FInstanceBufferMeshVertexFactory> VertexFactories;

	/** LOD render data from the static mesh. */
	TIndirectArray<FStaticMeshLODResources>& LODModels;

	/** Feature level used when creating instance data */
	ERHIFeatureLevel::Type FeatureLevel;

private:
	void InitVertexFactories();

	void RegisterSpeedTreeWind()
	{
		// register SpeedTree wind with the scene
		if (Component->GetStaticMesh()->SpeedTreeWind.IsValid())
		{
			for (int32 LODIndex = 0; LODIndex < LODModels.Num(); LODIndex++)
			{
				Component->GetScene()->AddSpeedTreeWind(&VertexFactories[LODIndex], Component->GetStaticMesh());
			}
		}
	}
};


/*-----------------------------------------------------------------------------
	FInstanceBufferMeshSceneProxy
-----------------------------------------------------------------------------*/

class FInstanceBufferMeshSceneProxy : public FStaticMeshSceneProxy
{
public:
	SIZE_T GetTypeHash() const override;

	FInstanceBufferMeshSceneProxy(UInstanceBufferMeshComponent* InComponent, ERHIFeatureLevel::Type InFeatureLevel)
	:	FStaticMeshSceneProxy(InComponent, true)
	,	StaticMesh(InComponent->GetStaticMesh())
	,	InstancedRenderData(InComponent, InFeatureLevel)
#if WITH_EDITOR
	,	bHasSelectedInstances(InComponent->SelectedInstances.Num() > 0)
#endif
	{
		bVFRequiresPrimitiveUniformBuffer = true;
		SetupProxy(InComponent);

#if RHI_RAYTRACING
		SetupRayTracingCullClusters();
#endif
	}

	~FInstanceBufferMeshSceneProxy()
	{
		InstancedRenderData.ReleaseResources(&GetScene( ), StaticMesh);

#if RHI_RAYTRACING
		for (int32 i = 0; i < RayTracingCullClusterInstances.Num(); ++i)
		{
			delete RayTracingCullClusterInstances[i];
		}
#endif
	}

	// FPrimitiveSceneProxy interface.
	
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		if(View->Family->EngineShowFlags.InstancedStaticMeshes)
		{
			Result = FStaticMeshSceneProxy::GetViewRelevance(View);
#if WITH_EDITOR
			// use dynamic path to render selected indices
			if( bHasSelectedInstances )
			{
				Result.bDynamicRelevance = true;
			}
#endif
		}
		return Result;
	}

#if RHI_RAYTRACING
	virtual bool IsRayTracingStaticRelevant() const override
	{
		return false;
	}

	virtual void GetDynamicRayTracingInstances(struct FRayTracingMaterialGatheringContext& Context, TArray<FRayTracingInstance>& OutRayTracingInstances) final override;
	void SetupRayTracingCullClusters();

#endif

	virtual void GetLightRelevance(const FLightSceneProxy* LightSceneProxy, bool& bDynamic, bool& bRelevant, bool& bLightMapped, bool& bShadowMapped) const override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

	virtual int32 GetNumMeshBatches() const override;

	/** Sets up a shadow FMeshBatch for a specific LOD. */
	virtual bool GetShadowMeshElement(int32 LODIndex, int32 BatchIndex, uint8 InDepthPriorityGroup, FMeshBatch& OutMeshBatch, bool bDitheredLODTransition) const override;

	/** Sets up a FMeshBatch for a specific LOD and element. */
	virtual bool GetMeshElement(int32 LODIndex, int32 BatchIndex, int32 ElementIndex, uint8 InDepthPriorityGroup, bool bUseSelectionOutline, bool bAllowPreCulledIndices, FMeshBatch& OutMeshBatch) const override;

	/** Sets up a wireframe FMeshBatch for a specific LOD. */
	virtual bool GetWireframeMeshElement(int32 LODIndex, int32 BatchIndex, const FMaterialRenderProxy* WireframeRenderProxy, uint8 InDepthPriorityGroup, bool bAllowPreCulledIndices, FMeshBatch& OutMeshBatch) const override;

	virtual void GetDistancefieldAtlasData(FBox& LocalVolumeBounds, FVector2D& OutDistanceMinMax, FIntVector& OutBlockMin, FIntVector& OutBlockSize, bool& bOutBuiltAsIfTwoSided, bool& bMeshWasPlane, float& SelfShadowBias, TArray<FMatrix>& ObjectLocalToWorldTransforms, bool& bOutThrottled) const override;

	virtual void GetDistanceFieldInstanceInfo(int32& NumInstances, float& BoundsSurfaceArea) const override;

	virtual int32 CollectOccluderElements(FOccluderElementsCollector& Collector) const override;

	/**
	 * Creates the hit proxies are used when DrawDynamicElements is called.
	 * Called in the game thread.
	 * @param OutHitProxies - Hit proxes which are created should be added to this array.
	 * @return The hit proxy to use by default for elements drawn by DrawDynamicElements.
	 */
	virtual HHitProxy* CreateHitProxies(UPrimitiveComponent* Component,TArray<TRefCountPtr<HHitProxy> >& OutHitProxies) override;

	virtual bool IsDetailMesh() const override
	{
		return true;
	}

protected:
	/** Cache of the StaticMesh asset, needed to release SpeedTree resources*/
	UStaticMesh* StaticMesh;

	/** Per component render data */
	FInstanceBufferMeshRenderData InstancedRenderData;

#if WITH_EDITOR
	/* If we we have any selected instances */
	bool bHasSelectedInstances;
#else
	static const bool bHasSelectedInstances = false;
#endif

	/** LOD transition info. */
	FInstancingUserData UserData_AllInstances;
	FInstancingUserData UserData_SelectedInstances;
	FInstancingUserData UserData_DeselectedInstances;

#if RHI_RAYTRACING
	TArray< FVector >						RayTracingCullClusterBoundsMin;
	TArray< FVector >						RayTracingCullClusterBoundsMax;
	TArray< TDoubleLinkedList< uint32 >* >	RayTracingCullClusterInstances;
#endif

	/** Common path for the Get*MeshElement functions */
	void SetupInstancedMeshBatch(int32 LODIndex, int32 BatchIndex, FMeshBatch& OutMeshBatch) const;

private:

	void SetupProxy(UInstanceBufferMeshComponent* InComponent);
};

#if WITH_EDITOR
/*-----------------------------------------------------------------------------
	FInstancedStaticMeshStaticLightingMesh
-----------------------------------------------------------------------------*/

/**
 * A static lighting mesh class that transforms the points by the per-instance transform of an 
 * InstancedStaticMeshComponent
 */
class FStaticLightingMesh_InstancedStaticMesh : public FStaticMeshStaticLightingMesh
{
public:

	/** Initialization constructor. */
	FStaticLightingMesh_InstancedStaticMesh(const UInstanceBufferMeshComponent* InPrimitive, int32 LODIndex, int32 InstanceIndex, const TArray<ULightComponent*>& InRelevantLights)
		: FStaticMeshStaticLightingMesh(InPrimitive, LODIndex, InRelevantLights)
	{
		// override the local to world to combine the per instance transform with the component's standard transform
		SetLocalToWorld(InPrimitive->PerInstanceSMData[InstanceIndex].Transform * InPrimitive->GetComponentTransform().ToMatrixWithScale());
	}
};

/*-----------------------------------------------------------------------------
	FInstancedStaticMeshStaticLightingTextureMapping
-----------------------------------------------------------------------------*/


/** Represents a static mesh primitive with texture mapped static lighting. */
class FStaticLightingTextureMapping_InstanceBufferMesh : public FStaticMeshStaticLightingTextureMapping
{
public:
	/** Initialization constructor. */
	FStaticLightingTextureMapping_InstanceBufferMesh(UInstanceBufferMeshComponent* InPrimitive, int32 LODIndex, int32 InInstanceIndex, FStaticLightingMesh* InMesh, int32 InSizeX, int32 InSizeY, int32 InTextureCoordinateIndex, bool bPerformFullQualityRebuild)
		: FStaticMeshStaticLightingTextureMapping(InPrimitive, LODIndex, InMesh, InSizeX, InSizeY, InTextureCoordinateIndex, bPerformFullQualityRebuild)
		, InstanceIndex(InInstanceIndex)
		, QuantizedData(nullptr)
		, ShadowMapData()
		, bComplete(false)
	{
	}

	// FStaticLightingTextureMapping interface
	virtual void Apply(FQuantizedLightmapData* InQuantizedData, const TMap<ULightComponent*, FShadowMapData2D*>& InShadowMapData, ULevel* LightingScenario) override
	{
		check(bComplete == false);

		UInstanceBufferMeshComponent* InstancedComponent = Cast<UInstanceBufferMeshComponent>(Primitive.Get());

		if (InstancedComponent)
		{
			// Save the static lighting until all of the component's static lighting has been built.
			QuantizedData = TUniquePtr<FQuantizedLightmapData>(InQuantizedData);
			ShadowMapData.Empty(InShadowMapData.Num());
			for (auto& ShadowDataPair : InShadowMapData)
			{
				ShadowMapData.Add(ShadowDataPair.Key, TUniquePtr<FShadowMapData2D>(ShadowDataPair.Value));
			}

			InstancedComponent->ApplyLightMapping(this, LightingScenario);
		}

		bComplete = true;
	}

	virtual bool DebugThisMapping() const override
	{
		return false;
	}

	virtual FString GetDescription() const override
	{
		return FString(TEXT("InstancedSMLightingMapping"));
	}

private:
	friend class UInstanceBufferMeshComponent;

	/** The instance of the primitive this mapping represents. */
	const int32 InstanceIndex;

	// Light/shadow map data stored until all instances for this component are processed
	// so we can apply them all into one light/shadowmap
	TUniquePtr<FQuantizedLightmapData> QuantizedData;
	TMap<ULightComponent*, TUniquePtr<FShadowMapData2D>> ShadowMapData;

	/** Has this mapping already been completed? */
	bool bComplete;
};

#endif

// Tim: I don't think these structures are even used.

/**
 * Structure that maps a component to it's lighting/instancing specific data which must be the same
 * between all instances that are bound to that component.
 */
// struct FComponentInstanceSharingData
// {
// 	/** The component that is associated (owns) this data */
// 	UInstanceBufferMeshComponent* Component;

// 	/** Light map texture */
// 	UTexture* LightMapTexture;

// 	/** Shadow map texture (or NULL if no shadow map) */
// 	UTexture* ShadowMapTexture;


// 	FComponentInstanceSharingData()
// 		: Component( NULL ),
// 		  LightMapTexture( NULL ),
// 		  ShadowMapTexture( NULL )
// 	{
// 	}
// };


// /**
//  * Helper struct to hold information about what components use what lightmap textures
//  */
// struct FComponentInstancedLightmapData
// {
// 	/** List of all original components and their original instances containing */
// 	TMap<UInstanceBufferMeshComponent*, TArray<FIBMInstanceData> > ComponentInstances;

// 	/** List of new components */
// 	TArray< FComponentInstanceSharingData > SharingData;
// };
















