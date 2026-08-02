# Windows video pipeline

## Ownership boundary

The project vendors the Win32/DX12 backend from the pinned RmlUi commit under
`client/src/dx12/rmlui_backend`. The vcpkg package remains unmodified upstream
RmlUi. The in-tree backend owns the device, queue, command list, descriptor
allocator, swap chain, frame fences, and presentation, and is the maintained
extension point for external textures and planar video sampling.

`PartiesRenderInterface_DX12` wraps upstream handles only to attach producer
ownership and the last back-buffer index which sampled each texture. Replacing
or releasing a video frame puts the upstream texture handle and its owner in
that back buffer's retirement list. `RenderInterface_DX12::EndFrame` waits when
the swap chain returns to the same buffer; only then does the adapter release
the descriptor, D3D12 resource, AMF surface, or CUDA ring lease.

## Portable I420 and NV12

Software decoders still return CPU planes. `Dx12VideoConverter` keeps one set
of persistently mapped upload resources per swap-chain back buffer, so the CPU
never overwrites memory referenced by an in-flight command list. NV12 uploads
go directly into stable R8 luma and R8G8 chroma textures. RmlUi binds both SRVs
and performs YUV-to-RGB in the final pixel shader, with no intermediate RGBA
texture and no conversion dispatch. I420 keeps the compute path until its three
planes are moved to the same final-shader abstraction.

This path retains a CPU upload because a software decoder necessarily produces
host memory, but it performs no CPU colour conversion and no per-frame texture
or descriptor allocation. If compute-pipeline construction fails, the adapter
keeps the scalar RGBA conversion as a compatibility fallback.

## AMD AMF DX11/DX12 interop

AMF hardware decode remains on its supported Windows DX11 path. The decoder
selects the AMD adapter matching RmlUI's D3D12 adapter LUID, then a D3D11 video
processor converts AMF's NV12 surface to RGBA entirely on the GPU. A four-slot
ring copies that result into NT-handle shared RGBA textures opened by D3D12.
An `ID3D11Fence` shared with D3D12 provides the producer timeline; the RmlUI
queue waits for the exact signalled value before sampling the RGBA texture.

Each ring lease retains the AMF surface, component/context lifetime, interop
state, and slot until RmlUI's consumer back-buffer fence completes. Destroying
or replacing a decoder therefore cannot invalidate a frame still referenced by
the renderer. A full ring drops the newest display callback to preserve bounded
latency. Resolution changes wait for outstanding slots to retire before the
ring is recreated.

The previous `AMFContext2::InitDX12` decode path was removed. Hardware testing
showed that consuming its output could remove the D3D12 device with
`DXGI_ERROR_INVALID_CALL`; AMD documents native D3D12 support for encoding and
PreAnalysis while its Windows hardware-decode guidance uses DX11. The supported
DX11 decode path plus explicit shared resource/fence bridge avoids that driver
failure without a host pixel transfer.

## NVIDIA NVDEC CUDA/D3D12 interop

The decoder matches the CUDA device to RmlUI's D3D12 adapter by LUID. On an SDK
13.1-capable driver it allocates a DPB-sized pool plus headroom (capped at 32)
of native CUDA arrays with `CU_AD_FORMAT_NV12` and
`CUDA_ARRAY3D_VIDEO_ENCODE_DECODE`. The arrays are registered through
`cuvidRegisterDecodeSurfaces` and filled by `cuvidDecodePictureAsync`. Keeping
the private block-linear decode surfaces in CUDA avoids asking NVDEC to write
its opaque layout into an imported D3D12 NV12 texture, whose plane layout is a
graphics-API contract rather than an NVDEC decode-surface contract.

Each slot also owns shared D3D12 R8 luma and R8G8 chroma textures. Two
stream-ordered `cuMemcpy2DAsync` plane copies publish a completed decode into
those textures, and RmlUi samples both planes directly in its final pixel
shader. `cuvidMapVideoFrame`, the CUDA color-conversion kernel, host transfer,
and the RGBA intermediate are absent from the normal NVIDIA path. The remaining
device-to-device plane copy is intentional: it is the explicit boundary
between NVDEC's private block-linear surface and D3D12's texture layout.

A shared D3D12 fence is imported into CUDA as an external semaphore. After the
decode and plane-copy submissions, CUDA signals a monotonically increasing
fence value; the RmlUI command queue waits on that exact value before sampling
the texture. A ring lease keeps the CUDA array, D3D12 plane resources, fence,
CUDA context, and slot reservation
alive until the consumer back-buffer fence completes. Before NVDEC reuses an
indexed surface, it verifies that the renderer has retired its lease. Coded-size
padding (for example 1920x1088 for 1080p H.264) is removed by UV crop metadata
in the final quad, without another copy or shader pass.

The SDK 13.1 functions and opaque format bit are loaded as optional runtime
capabilities. Older drivers retain the prior four-slot CUDA NV12-to-RGBA kernel
and external-fence path. This fallback remains asynchronous to D3D12 and avoids
host transfer, while current drivers use opaque direct decode plus a GPU-only
publish into D3D12 presentation planes.

## Fallbacks

- No renderer device or no CUDA adapter-LUID match: NVDEC copies into pinned
  host NV12 and uses the portable compute-upload path.
- Missing SDK 13.1 symbols/format support, or rejected opaque surface allocation/registration:
  the synchronized CUDA RGBA fallback is selected automatically.
- External-memory, external-semaphore, PTX, or shared-resource initialization
  failure in that fallback: pinned-host NV12 is selected before frame delivery.
- 10-bit NVDEC output: the sequence is rejected because the current frame
  contract and CUDA kernel are intentionally 8-bit; P016 is never reinterpreted
  as byte NV12.
- No matching AMD adapter, unavailable RGBA video-processor output, or failed
  DX11/DX12 resource sharing: AMF switches once to the portable host-NV12 path.

## Verification

`parties_dx12_video_converter` checks I420/NV12 conversion and reads back the
direct R8/R8G8 planar upload. `parties_amf_decoder_hardware` validates host
output and the shared DX11/DX12 RGBA path, destroys the decoder while a frame is
retained, waits the producer fence, and reads a non-empty texture without device
removal. `parties_nvdec_d3d12_interop` encodes an AV1 sequence, decodes it with
NVDEC on the renderer adapter, waits the CUDA-signalled D3D12 fence, reads both
D3D12 presentation planes, and rejects empty luma or incomplete chroma output. The
multi-window RmlUI test covers teardown of multiple HWND renderers and validates
that a packed NV12 resource can be registered as two plane SRVs.
