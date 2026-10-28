# Third-party components

| Component | Where | License |
|---|---|---|
| Dear ImGui 1.91.5 | `vrkit/third_party/imgui/` | MIT (`vrkit/third_party/imgui/LICENSE.txt`) |
| MinHook | not vendored; fetched by `scripts/setup_env.sh` into `/opt/so6deps/minhook` | BSD 2-Clause |
| Khronos OpenXR SDK / loader | not vendored; headers + `openxr_loader.dll` from `scripts/setup_env.sh` | Apache 2.0 |
| RenderDoc in-application API header | `third_party/renderdoc/renderdoc_app.h` | MIT |

Release zips ship `openxr_loader.dll` and the license texts of ImGui, MinHook and the OpenXR loader.
