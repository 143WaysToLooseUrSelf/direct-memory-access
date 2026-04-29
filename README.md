DMA VERSION

The overlay is rendered with **Direct3D 9 Extended (`IDirect3D9Ex`)** + **Dear ImGui**.

`D3DDEVTYPE_HAL`, `D3DCREATE_HARDWARE_VERTEXPROCESSING`, `D3DFMT_A8R8G8B8` 
`DwmExtendFrameIntoClientArea` 
`imgui_impl_win32` + `imgui_impl_dx9`
`WS_POPUP` with `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST`
`ImGui::GetOverlayDrawList()`

Architecture

┌─────────────────────────────────────────────┐
│  Single Thread (reads → logic → render)    │
│                                             │
│  update_entities()   → FPGA scatter reads   │
│        ↓                                    │
│  classify / filter   → populate g_entities  │
│        ↓                                    │
│  draw_esp()          → ImGui + DX9 present  │
└─────────────────────────────────────────────┘

Dependencies

MemProcFS / VMMDLL — FPGA DMA interface
Dear ImGui — UI and overlay draw lists
DirectX 9 SDK — rendering backend

This project is for educational and research purposes only.  
Usage in online games may violate the game’s Terms of Service and could result in a ban.  
