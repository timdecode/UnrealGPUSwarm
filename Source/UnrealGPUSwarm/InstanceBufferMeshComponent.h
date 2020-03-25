// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "UObject/ObjectMacros.h"
#include "EngineDefines.h"
#include "HitProxies.h"
#include "Misc/Guid.h"
#include "Engine/TextureStreamingTypes.h"
#include "Components/StaticMeshComponent.h"

#include "InstancedStaticMeshComponent.generated.h"

class FLightingBuildOptions;
class FPrimitiveSceneProxy;
class FStaticLightingTextureMapping_InstancedStaticMesh;
class ULightComponent;
struct FNavigableGeometryExport;
struct FNavigationRelevantData;
struct FPerInstanceRenderData;
struct FStaticLightingPrimitiveInfo;

DECLARE_STATS_GROUP(TEXT("Foliage"), STATGROUP_Foliage, STATCAT_Advanced);

class FStaticLightingTextureMapping_InstancedStaticMesh;
class FInstancedLightMap2D;
class FInstancedShadowMap2D;
class FStaticMeshInstanceData;

struct FInstanceUpdateCmdBuffer
{
	enum EUpdateCommandType
	{
		Add,
		Update,
		Hide,
		EditorData,
		LightmapData,
	};
	
	struct FInstanceUpdateCommand
	{
		int32 InstanceIndex;
		EUpdateCommandType Type;
		FMatrix XForm;
		
		FColor HitProxyColor;
		bool bSelected;

		FVector2D LightmapUVBias;
		FVector2D ShadowmapUVBias;
	};
	
	FInstanceUpdateCmdBuffer();
	
	// Commands that can modify render data in place
	void HideInstance(int32 RenderIndex);
	void AddInstance(const FMatrix& InTransform);
	void UpdateInstance(int32 RenderIndex, const FMatrix& InTransform);
	void SetEditorData(int32 RenderIndex, const FColor& Color, bool bSelected);
	void SetLightMapData(int32 RenderIndex, const FVector2D& LightmapUVBias);
	void SetShadowMapData(int32 RenderIndex, const FVector2D& ShadowmapUVBias);
	void ResetInlineCommands();
	int32 NumInlineCommands() const { return Cmds.Num(); }

	// Command that can't be in-lined and should cause full buffer rebuild
	void Edit();
	void Reset();
	int32 NumTotalCommands() const { return NumEdits; };
	
	TArray<FInstanceUpdateCommand> Cmds;
	int32 NumAdds;
	int32 NumEdits;
};

USTRUCT()
struct FInstancedStaticMeshInstanceData
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(EditAnywhere, Category=Instances)
	FMatrix Transform;

	FInstancedStaticMeshInstanceData()
		: Transform(FMatrix::Identity)
	{
	}

	FInstancedStaticMeshInstanceData(const FMatrix& InTransform)
		: Transform(InTransform)
	{
	}

	friend FArchive& operator<<(FArchive& Ar, FInstancedStaticMeshInstanceData& InstanceData)
	{
		// @warning BulkSerialize: FInstancedStaticMeshInstanceData is serialized as memory dump
		// See TArray::BulkSerialize for detailed description of implied limitations.
		Ar << InstanceData.Transform;
		return Ar;
	}
};

USTRUCT()
struct FInstancedStaticMeshMappingInfo
{
	GENERATED_USTRUCT_BODY()

		FStaticLightingTextureMapping_InstancedStaticMesh* Mapping;

	FInstancedStaticMeshMappingInfo()
		: Mapping(nullptr)
	{
	}
};

/** A component that efficiently renders multiple instances of the same StaticMesh. */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent), Blueprintable)
class ENGINE_API UInstancedStaticMeshComponent : public UStaticMeshComponent
{
	GENERATED_UCLASS_BODY()
	
	/** Needs implementation in InstancedStaticMesh.cpp to compile UniquePtr for forward declared class */
	UInstancedStaticMeshComponent(FVTableHelper& Helper);
	virtual ~UInstancedStaticMeshComponent();
	
	/** Array of instances, bulk serialized. */
	UPROPERTY(EditAnywhere, SkipSerialization, DisplayName="Instances", Category=Instances, meta=(MakeEditWidget=true, EditFixedOrder))
	TArray<FInstancedStaticMeshInstanceData> PerInstanceSMData;

	/** Value used to seed the random number stream that generates random numbers for each of this mesh's instances.
	The random number is stored in a buffer accessible to materials through the PerInstanceRandom expression. If
	this is set to zero (default), it will be populated automatically by the editor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=InstancedStaticMeshComponent)
	int32 InstancingRandomSeed;

	/** Distance from camera at which each instance begins to fade out. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Culling)
	int32 InstanceStartCullDistance;

	/** Distance from camera at which each instance completely fades out. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category=Culling)
	int32 InstanceEndCullDistance;

	/** Mapping from PerInstanceSMData order to instance render buffer order. If empty, the PerInstanceSMData order is used. */
	UPROPERTY()
	TArray<int32> InstanceReorderTable;

	/** Tracks outstanding proxysize, as this is a bit hard to do with the fire-and-forget grass. */
	SIZE_T ProxySize;

	/** Add an instance to this component. Transform is given in local space of this component. */
	UFUNCTION(BlueprintCallable, Category="Components|InstancedStaticMesh")
	virtual int32 AddInstance(const FTransform& InstanceTransform);

	/** Add an instance to this component. Transform is given in world space. */
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	int32 AddInstanceWorldSpace(const FTransform& WorldTransform);

	/** Preallocated memory to include the new added instances count, to prevent reallloc during the add operation. */
	virtual void PreAllocateInstancesMemory(int32 AddedInstanceCount);

	/** Get the transform for the instance specified. Instance is returned in local space of this component unless bWorldSpace is set.  Returns True on success. */
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	bool GetInstanceTransform(int32 InstanceIndex, FTransform& OutInstanceTransform, bool bWorldSpace = false) const;

	virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport) override;

	/** Get the scale comming form the component, when computing StreamingTexture data. Used to support instanced meshes. */
	virtual float GetTextureStreamingTransformScale() const override;
	/** Get material, UV density and bounds for a given material index. */
	virtual bool GetMaterialStreamingData(int32 MaterialIndex, FPrimitiveMaterialInfo& MaterialData) const override;
	/** Build the data to compute accuracte StreaminTexture data. */
	virtual bool BuildTextureStreamingData(ETextureStreamingBuildType BuildType, EMaterialQualityLevel::Type QualityLevel, ERHIFeatureLevel::Type FeatureLevel, TSet<FGuid>& DependentResources) override;
	/** Get the StreaminTexture data. */
	virtual void GetStreamingRenderAssetInfo(FStreamingTextureLevelContext& LevelContext, TArray<FStreamingRenderAssetPrimitiveInfo>& OutStreamingRenderAssets) const override;

	/**
	* Update the transform for the instance specified.
	*
	* @param InstanceIndex			The index of the instance to update
	* @param NewInstanceTransform	The new transform
	* @param bWorldSpace			If true, the new transform is interpreted as a World Space transform, otherwise it is interpreted as Local Space
	* @param bMarkRenderStateDirty	If true, the change should be visible immediately. If you are updating many instances you should only set this to true for the last instance.
	* @param bTeleport				Whether or not the instance's physics should be moved normally, or teleported (moved instantly, ignoring velocity).
	* @return						True on success.
	*/
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	virtual bool UpdateInstanceTransform(int32 InstanceIndex, const FTransform& NewInstanceTransform, bool bWorldSpace=false, bool bMarkRenderStateDirty=false, bool bTeleport=false);

    /**
	* Update the transform for an array of instances.
	*
	* @param StartInstanceIndex		The starting index of the instances to update
	* @param NewInstancesTransforms	The new transforms
	* @param bWorldSpace			If true, the new transforms are interpreted as a World Space transform, otherwise it is interpreted as Local Space
	* @param bMarkRenderStateDirty	If true, the change should be visible immediately. If you are updating many instances you should only set this to true for the last instance.
	* @param bTeleport				Whether or not the instances physics should be moved normally, or teleported (moved instantly, ignoring velocity).
	* @return						True on success.
	*/
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	virtual bool BatchUpdateInstancesTransforms(int32 StartInstanceIndex, const TArray<FTransform>& NewInstancesTransforms, bool bWorldSpace=false, bool bMarkRenderStateDirty=false, bool bTeleport=false);

	/**
	* Update the transform for a number of instances.
	*
	* @param StartInstanceIndex		The starting index of the instances to update
	* @param NumInstances			The number of instances to update
	* @param NewInstancesTransform	The new transform
	* @param bWorldSpace			If true, the new transform is interpreted as a World Space transform, otherwise it is interpreted as Local Space
	* @param bMarkRenderStateDirty	If true, the change should be visible immediately. If you are updating many instances you should only set this to true for the last instance.
	* @param bTeleport				Whether or not the instances physics should be moved normally, or teleported (moved instantly, ignoring velocity).
	* @return						True on success.
	*/
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	virtual bool BatchUpdateInstancesTransform(int32 StartInstanceIndex, int32 NumInstances, const FTransform& NewInstancesTransform, bool bWorldSpace=false, bool bMarkRenderStateDirty=false, bool bTeleport=false);

	/** Remove the instance specified. Returns True on success. Note that this will leave the array in order, but may shrink it. */
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	virtual bool RemoveInstance(int32 InstanceIndex);

	/** Clear all instances being rendered by this component. */
	UFUNCTION(BlueprintCallable, Category="Components|InstancedStaticMesh")
	virtual void ClearInstances();

	/** Get the number of instances in this component. */
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	int32 GetInstanceCount() const;

	/** Sets the fading start and culling end distances for this component. */
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	void SetCullDistances(int32 StartCullDistance, int32 EndCullDistance);

	/** Returns the instances with instance bounds overlapping the specified sphere. The return value is an array of instance indices. */
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	virtual TArray<int32> GetInstancesOverlappingSphere(const FVector& Center, float Radius, bool bSphereInWorldSpace=true) const;

	/** Returns the instances with instance bounds overlapping the specified box. The return value is an array of instance indices. */
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	virtual TArray<int32> GetInstancesOverlappingBox(const FBox& Box, bool bBoxInWorldSpace=true) const;

	virtual bool ShouldCreatePhysicsState() const override;

	virtual void PostLoad() override;
	virtual void OnComponentCreated() override;

public:
	/** Render data will be initialized on PostLoad or on demand. Released on the rendering thread. */
	TSharedPtr<FPerInstanceRenderData, ESPMode::ThreadSafe> PerInstanceRenderData;

	/** Recorded modifications to per-instance data */
	FInstanceUpdateCmdBuffer InstanceUpdateCmdBuffer;

	/** 
	 *  Buffers with per-instance data laid out for rendering. 
	 *  Serialized for cooked content. Used to create PerInstanceRenderData. 
	 *  Alive between Serialize and PostLoad calls 
	 */
	TUniquePtr<FStaticMeshInstanceData> InstanceDataBuffers;

#if WITH_EDITOR
	/** One bit per instance if the instance is selected. */
	TBitArray<> SelectedInstances;
#endif
	/** Physics representation of the instance bodies. */
	TArray<FBodyInstance*> InstanceBodies;

	//~ Begin UActorComponent Interface
	virtual TStructOnScope<FActorComponentInstanceData> GetComponentInstanceData() const override;
	//~ End UActorComponent Interface

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
protected:
	virtual void OnCreatePhysicsState() override;
	virtual void OnDestroyPhysicsState() override;
public:
	virtual bool CanEditSimulatePhysics() override;

	virtual FBoxSphereBounds CalcBounds(const FTransform& BoundTransform) const override;
	virtual bool SupportsStaticLighting() const override { return true; }
#if WITH_EDITOR
	virtual void GetStaticLightingInfo(FStaticLightingPrimitiveInfo& OutPrimitiveInfo,const TArray<ULightComponent*>& InRelevantLights,const FLightingBuildOptions& Options) override;
#endif
	virtual void GetLightAndShadowMapMemoryUsage( int32& LightMapMemoryUsage, int32& ShadowMapMemoryUsage ) const override;

	virtual bool DoCustomNavigableGeometryExport(FNavigableGeometryExport& GeomExport) const override;
	//~ End UPrimitiveComponent Interface

	//~ Begin UNavRelevantInterface Interface
	virtual void GetNavigationData(FNavigationRelevantData& Data) const override;
	virtual FBox GetNavigationBounds() const override;
	//~ End UPrimitiveComponent Interface

	//~ Begin UObject Interface
	virtual void Serialize(FArchive& Ar) override;
	virtual void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) override;
	void BeginDestroy() override;
	virtual void PostDuplicate(bool bDuplicateForPIE) override;
#if WITH_EDITOR
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent) override;
	virtual void PostEditUndo() override;
#endif
	//~ End UObject Interface

	/** Applies the cached component instance data to a newly blueprint constructed component. */
	virtual void ApplyComponentInstanceData(struct FInstancedStaticMeshComponentInstanceData* ComponentInstanceData);

	/** Check to see if an instance is selected. */
	bool IsInstanceSelected(int32 InInstanceIndex) const;

	/** Select/deselect an instance or group of instances. */
	void SelectInstance(bool bInSelected, int32 InInstanceIndex, int32 InInstanceCount = 1);

	/** Deselect all instances. */
	void ClearInstanceSelection();

	/** Initialize the Per Instance Render Data */
	void InitPerInstanceRenderData(bool InitializeFromCurrentData, FStaticMeshInstanceData* InSharedInstanceBufferData = nullptr, bool InRequireCPUAccess = false);

	/** Transfers ownership of instance render data to a render thread. Instance render data will be released in scene proxy destructor or on render thread task. */
	void ReleasePerInstanceRenderData();
	
	// Number of instances in the render-side instance buffer
	virtual int32 GetNumRenderInstances() const { return PerInstanceSMData.Num(); }

	virtual void PropagateLightingScenarioChange() override;

	void GetInstancesMinMaxScale(FVector& MinScale, FVector& MaxScale) const;
private:

	/** Sets up new instance data to sensible defaults, creates physics counterparts if possible. */
	void SetupNewInstanceData(FInstancedStaticMeshInstanceData& InOutNewInstanceData, int32 InInstanceIndex, const FTransform& InInstanceTransform);

	/** Update instance body with a new transform */
	void UpdateInstanceBodyTransform(int32 InstanceIndex, const FTransform& WorldSpaceInstanceTransform, bool bTeleport);

protected:
	/** Creates body instances for all instances owned by this component. */
	void CreateAllInstanceBodies();

	/** Terminate all body instances owned by this component. */
	void ClearAllInstanceBodies();

	/** Request to navigation system to update only part of navmesh occupied by specified instance. */
	virtual void PartialNavigationUpdate(int32 InstanceIdx);

	/** Internal version of AddInstance */
	int32 AddInstanceInternal(int32 InstanceIndex, FInstancedStaticMeshInstanceData* InNewInstanceData, const FTransform& InstanceTransform);

	/** Internal version of RemoveInstance */	
	bool RemoveInstanceInternal(int32 InstanceIndex, bool InstanceAlreadyRemoved);

	/** Handles request from navigation system to gather instance transforms in a specific area box. */
	virtual void GetNavigationPerInstanceTransforms(const FBox& AreaBox, TArray<FTransform>& InstanceData) const;

	/** Initializes the body instance for the specified instance of the static mesh. */
	void InitInstanceBody(int32 InstanceIdx, FBodyInstance* InBodyInstance);

	/** Number of pending lightmaps still to be calculated (Apply()'d). */
	UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
	int32 NumPendingLightmaps;

	/** The mappings for all the instances of this component. */
	UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
	TArray<FInstancedStaticMeshMappingInfo> CachedMappings;

	void ApplyLightMapping(FStaticLightingTextureMapping_InstancedStaticMesh* InMapping, ULevel* LightingScenario);
	
	void CreateHitProxyData(TArray<TRefCountPtr<HHitProxy>>& HitProxies);

    /** Build instance buffer for rendering from current component data. */
	void BuildRenderData(FStaticMeshInstanceData& OutData, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies);
	
    /** Serialize instance buffer that is used for rendering. Only for cooked content */
	void SerializeRenderData(FArchive& Ar);
	
	/** Creates rendering buffer from serialized data, if any */
	virtual void OnPostLoadPerInstanceData();

	friend FStaticLightingTextureMapping_InstancedStaticMesh;
	friend FInstancedLightMap2D;
	friend FInstancedShadowMap2D;
};

/** InstancedStaticMeshInstance hit proxy */
struct HInstancedStaticMeshInstance : public HHitProxy
{
	UInstancedStaticMeshComponent* Component;
	int32 InstanceIndex;

	DECLARE_HIT_PROXY(ENGINE_API);
	HInstancedStaticMeshInstance(UInstancedStaticMeshComponent* InComponent, int32 InInstanceIndex) : HHitProxy(HPP_World), Component(InComponent), InstanceIndex(InInstanceIndex) {}

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::CardinalCross;
	}
};

/** Used to store lightmap data during RerunConstructionScripts */
USTRUCT()
struct FInstancedStaticMeshLightMapInstanceData
{
	GENERATED_BODY()

	/** Transform of component */
	UPROPERTY()
	FTransform Transform;

	/** guid from LODData */
	UPROPERTY()
	TArray<FGuid> MapBuildDataIds;
};

/** Helper class used to preserve lighting/selection state across blueprint reinstancing */
USTRUCT()
struct FInstancedStaticMeshComponentInstanceData : public FSceneComponentInstanceData
{
	GENERATED_BODY()
public:
	FInstancedStaticMeshComponentInstanceData() = default;
	FInstancedStaticMeshComponentInstanceData(const UInstancedStaticMeshComponent* InComponent)
		: FSceneComponentInstanceData(InComponent)
		, StaticMesh(InComponent->GetStaticMesh())
	{}
	virtual ~FInstancedStaticMeshComponentInstanceData() = default;

	virtual bool ContainsData() const override
	{
		return true;
	}

	virtual void ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase) override
	{
		Super::ApplyToComponent(Component, CacheApplyPhase);
		CastChecked<UInstancedStaticMeshComponent>(Component)->ApplyComponentInstanceData(this);
	}

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Super::AddReferencedObjects(Collector);
		Collector.AddReferencedObject(StaticMesh);
	}

public:
	/** Mesh being used by component */
	UPROPERTY()
	UStaticMesh* StaticMesh;

	// Static lighting info
	UPROPERTY()
	FInstancedStaticMeshLightMapInstanceData CachedStaticLighting;
	UPROPERTY()
	TArray<FInstancedStaticMeshInstanceData> PerInstanceSMData;

	/** The cached selected instances */
	TBitArray<> SelectedInstances;

	/* The cached random seed */
	UPROPERTY()
	int32 InstancingRandomSeed;
};
