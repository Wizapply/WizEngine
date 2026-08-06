# assets/

Files the scene loads at runtime, copied next to the executable by the build.

- `*.hdr` — equirectangular environment map. Name it in `src/scene.cpp`:

  ```cpp
  constexpr const char* kEnvironmentHdr = "studio.hdr";
  constexpr float kEnvironmentIntensity = 30000.0f;
  ```

  It is converted to a cubemap and prefiltered **on the GPU at load time**, so
  changing the file needs no rebuild — only a restart. Free HDRIs:
  <https://polyhaven.com/hdris> (2k is plenty).

  Why bother: glTF materials default to `metallicFactor = 1.0`, and metal has
  no diffuse colour of its own — it is visible only through what it reflects.
  With no environment, those surfaces go black wherever the direct lights do
  not reach.

- `*.glb` / `*.gltf` — models, named by `kBoxModelPath` or `kModelPath`.
