// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "StructuredBufferInstancedMesh.generated.h"

/**
 * 
 */
UCLASS()
class UNREALGPUSWARM_API UStructuredBufferInstancedMesh : public UPrimitiveComponent
{
	GENERATED_BODY()
	
private:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = StaticMesh, ReplicatedUsing = OnRep_StaticMesh, meta = (AllowPrivateAccess = "true"))
	class UStaticMesh * StaticMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = StaticMesh, ReplicatedUsing = OnRep_StaticMesh, meta = (AllowPrivateAccess = "true"))
	class UMaterialInterface * Material;

public:
	//~ Begin UPrimitiveComponent Interface.
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	//~ End UPrimitiveComponent Interface.

	//~ Begin USceneComponent Interface.
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	//~ Begin USceneComponent Interface.

protected:
	UStaticMesh * GetStaticMesh();
};
