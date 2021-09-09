This project is a good starting point for learning how to write compute shaders in Unreal. It implements a boid simulation the GPU. It achieves 0.5 million boids at 45 fps on a GTX 1080. 

https://user-images.githubusercontent.com/980432/132757577-500416e4-5f27-4add-9c50-641889336d69.mp4

[Tweet of the above](https://twitter.com/timd_ca/status/1243941167005192192)

This project is for Unreal 4.24. It might work in later versions, but you might need to fix a few compiler errors around shader binding. I would be suprised if my ``FIBMInstanceBuffer`` works in 4.26. 

The compute shader magic happens in [ComputeShaderTestComponent.cpp](Source/UnrealGPUSwarm/ComputeShaderTestComponent.cpp). To work with compute shaders in Unreal you need a few things:
- the compute shader/kernel source that runs on the GPU (a .usf file)
- a way to map/bind parameters and buffers to the compute kernel (subclass a `FGlobalShader`)
- something that will dispatch the shader to the GPU (`FComputeShaderUtils::Dispatch`)

This project was my way of learning to write computer shaders in Unreal, so not everything is perfect, and some things are written for clarity (you should create your ``TShaderMapRef``s once and cache them, I recreate them every frame).

This project is also quite heavily optimized from the theoretical side of things. Some important bits:
- I'm using a hashed grid (powered by the bitonic GPU sort algorithm) to massively accelerate boid neighbourhood queries on the GPU
- I sort the boids by their grid cell every frame to greatly improve cache coherence (it really makes a big difference, try disabling it)
- I rewrote UInstanceStaticMeshComponent to work directly with GPU side position buffers

The hashed grid implementation was inspired by the one from [Wicked Engine](https://wickedengine.net/page/2/). 
