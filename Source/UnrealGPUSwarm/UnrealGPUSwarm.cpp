// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "UnrealGPUSwarm.h"
#include "Modules/ModuleManager.h"

class FUnrealGPUSwarmModule : public IModuleInterface
{
	virtual bool IsGameModule() const override
	{
		return true;
	}

	void StartupModule() override {
		FString ShaderDirectory = FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping("/ComputeShaderPlugin", ShaderDirectory);
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FUnrealGPUSwarmModule, UnrealGPUSwarm, "UnrealGPUSwarm");
