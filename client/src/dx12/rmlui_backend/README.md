# Parties RmlUi DX12 backend

This directory vendors the Win32/DX12 backend from
[`mikke89/RmlUi`](https://github.com/mikke89/RmlUi) commit
`0ae381e00d7426762bb5ed897973366358b16642`.

It is compiled as `parties_rmlui_dx12_backend`; the vcpkg RmlUi package remains
an unmodified build of the pinned upstream library. Keeping the backend here
makes Parties-specific GPU integration reviewable and versioned with the code
that consumes it.

Parties extensions over that upstream snapshot:

- safe lifetime for a caller-owned shared `ID3D12Device`;
- registration of external D3D12 textures and producer fences;
- direct two-plane NV12 SRV binding and YUV-to-RGB conversion in the final
  RmlUi pixel shader.

When updating RmlUi, diff this directory against the matching upstream
`Backends` bundle first, then reapply and test the extensions above.
