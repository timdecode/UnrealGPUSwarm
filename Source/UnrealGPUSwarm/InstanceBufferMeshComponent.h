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

#include "InstanceBufferMeshComponent.generated.h"

class FLightingBuildOptions;
class FPrimitiveSceneProxy;
class FStaticLightingTextureMapping_InstanceBufferMesh;
class ULightComponent;
struct FNavigableGeometryExport;
struct FNavigationRelevantData;
struct FIBMPerInstanceRenderData;
struct FStaticLightingPrimitiveInfo;

// Tim: disabled
// DECLARE_STATS_GROUP(TEXT("Foliage"), STATGROUP_Foliage, STATCAT_Advanced);

class FStaticLightingTextureMapping_InstanceBufferMesh;
class FInstancedLightMap2D;
class FInstancedShadowMap2D;
class FIBMStaticMeshInstanceData;

struct FIBMInstanceUpdateCmdBuffer
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
	
	FIBMInstanceUpdateCmdBuffer();
	
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
struct FInstanceBufferMeshMappingInfo
{
	GENERATED_USTRUCT_BODY()

		FStaticLightingTextureMapping_InstanceBufferMesh* Mapping;

	FInstanceBufferMeshMappingInfo()
		: Mapping(nullptr)
	{
	}
};

/** A component that efficiently renders multiple instances of the same StaticMesh. */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent), Blueprintable)
class UNREALGPUSWARM_API UInstanceBufferMeshComponent : public UStaticMeshComponent
{
	GENERATED_UCLASS_BODY()
	
	/** Needs implementation in InstancedStaticMesh.cpp to compile UniquePtr for forward declared class */
	UInstanceBufferMeshComponent(FVTableHelper& Helper);
	virtual ~UInstanceBufferMeshComponent();

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


	virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport) override;

	/** Get the scale coming form the component, when computing StreamingTexture data. Used to support instanced meshes. */
	virtual float GetTextureStreamingTransformScale() const override;
	/** Get material, UV density and bounds for a given material index. */
	virtual bool GetMaterialStreamingData(int32 MaterialIndex, FPrimitiveMaterialInfo& MaterialData) const override;
	/** Build the data to compute accurate StreaminTexture data. */
	virtual bool BuildTextureStreamingData(ETextureStreamingBuildType BuildType, EMaterialQualityLevel::Type QualityLevel, ERHIFeatureLevel::Type FeatureLevel, TSet<FGuid>& DependentResources) override;
	/** Get the StreaminTexture data. */
	virtual void GetStreamingRenderAssetInfo(FStreamingTextureLevelContext& LevelContext, TArray<FStreamingRenderAssetPrimitiveInfo>& OutStreamingRenderAssets) const override;


	/** Get the number of instances in this component. */
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	int32 GetInstanceCount() const;

	/** Sets the fading start and culling end distances for this component. */
	UFUNCTION(BlueprintCallable, Category = "Components|InstancedStaticMesh")
	void SetCullDistances(int32 StartCullDistance, int32 EndCullDistance);

	void SetNumInstances(int numInstances);
	int32 GetNumInstancesCurrentlyAllocated() const;

	virtual bool ShouldCreatePhysicsState() const override;

	virtual void PostLoad() override;
	virtual void OnComponentCreated() override;

public:
	/** Render data will be initialized on PostLoad or on demand. Released on the rendering thread. */
	TSharedPtr<FIBMPerInstanceRenderData, ESPMode::ThreadSafe> PerInstanceRenderData;


#if WITH_EDITOR
	/** One bit per instance if the instance is selected. */
	TBitArray<> SelectedInstances;
#endif

	uint32_t _numInstances = 0;


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
	virtual void ApplyComponentInstanceData(struct FIBMComponentInstanceData* ComponentInstanceData);

	/** Check to see if an instance is selected. */
	bool IsInstanceSelected(int32 InInstanceIndex) const;

	/** Select/deselect an instance or group of instances. */
	void SelectInstance(bool bInSelected, int32 InInstanceIndex, int32 InInstanceCount = 1);

	/** Deselect all instances. */
	void ClearInstanceSelection();

	/** Initialize the Per Instance Render Data */
	void InitPerInstanceRenderData();

	/** Transfers ownership of instance render data to a render thread. Instance render data will be released in scene proxy destructor or on render thread task. */
	void ReleasePerInstanceRenderData();
	
	// Number of instances in the render-side instance buffer
	virtual int32 GetNumRenderInstances() const { return GetNumInstancesCurrentlyAllocated(); }

	virtual void PropagateLightingScenarioChange() override;

	void GetInstancesMinMaxScale(FVector& MinScale, FVector& MaxScale) const;
private:



protected:


	/** Request to navigation system to update only part of navmesh occupied by specified instance. */
	virtual void PartialNavigationUpdate(int32 InstanceIdx);

	/** Handles request from navigation system to gather instance transforms in a specific area box. */
	virtual void GetNavigationPerInstanceTransforms(const FBox& AreaBox, TArray<FTransform>& InstanceData) const;


	/** Number of pending lightmaps still to be calculated (Apply()'d). */
	UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
	int32 NumPendingLightmaps;

	/** The mappings for all the instances of this component. */
	UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
	TArray<FInstanceBufferMeshMappingInfo> CachedMappings;

	void ApplyLightMapping(FStaticLightingTextureMapping_InstanceBufferMesh* InMapping, ULevel* LightingScenario);
	
	void CreateHitProxyData(TArray<TRefCountPtr<HHitProxy>>& HitProxies);

    /** Build instance buffer for rendering from current component data. */
	void BuildRenderData(TArray<TRefCountPtr<HHitProxy>>& OutHitProxies);
	
    /** Serialize instance buffer that is used for rendering. Only for cooked content */
	void SerializeRenderData(FArchive& Ar);
	
	/** Creates rendering buffer from serialized data, if any */
	virtual void OnPostLoadPerInstanceData();

	friend FStaticLightingTextureMapping_InstanceBufferMesh;
	friend FInstancedLightMap2D;
	friend FInstancedShadowMap2D;
};

/** InstancedStaticMeshInstance hit proxy */
struct HInstanceBufferMeshInstance : public HHitProxy
{
	UInstanceBufferMeshComponent* Component;
	int32 InstanceIndex;

	DECLARE_HIT_PROXY(UNREALGPUSWARM_API);
	HInstanceBufferMeshInstance(UInstanceBufferMeshComponent* InComponent, int32 InInstanceIndex) : HHitProxy(HPP_World), Component(InComponent), InstanceIndex(InInstanceIndex) {}

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::CardinalCross;
	}
};

/** Used to store lightmap data during RerunConstructionScripts */
USTRUCT()
struct FInstanceBufferMeshLightMapInstanceData
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
struct FIBMComponentInstanceData : public FSceneComponentInstanceData
{
	GENERATED_BODY()
public:
	FIBMComponentInstanceData() = default;
	FIBMComponentInstanceData(const UInstanceBufferMeshComponent* InComponent)
		: FSceneComponentInstanceData(InComponent)
		, StaticMesh(InComponent->GetStaticMesh())
	{}
	virtual ~FIBMComponentInstanceData() = default;

	virtual bool ContainsData() const override
	{
		return true;
	}

	virtual void ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase) override
	{
		Super::ApplyToComponent(Component, CacheApplyPhase);
		CastChecked<UInstanceBufferMeshComponent>(Component)->ApplyComponentInstanceData(this);
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
	FInstanceBufferMeshLightMapInstanceData CachedStaticLighting;


	/** The cached selected instances */
	TBitArray<> SelectedInstances;

	/* The cached random seed */
	UPROPERTY()
	int32 InstancingRandomSeed;
};
