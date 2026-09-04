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

## Transport and latency

### Why the reliable stream produced slow motion

Before protocol 1.2 every frame was written to one reliable, ordered QUIC
stream (stream 1) with no accounting. `StreamSend` queues without bound when the
congestion window is full and nothing consumed `SEND_COMPLETE`, so whenever the
sharer's uplink or the server-to-viewer downlink carried less than the encoder
produced, the queue grew forever and every frame arrived later than the one
before it: smooth motion at a growing delay, which viewers perceive as slow
motion. Two secondary problems made it worse: a two-second VBV let a single
keyframe be a multi-hundred-kilobyte burst, and on a single ordered stream one
lost packet blocked every frame behind it (head-of-line blocking). On the viewer
the decode backlog tolerated 240 queued frames before resynchronising, and NVDEC
waited for the next packet before releasing a picture.

### Components

| Where | Component | Role |
|-------|-----------|------|
| Sharer | `VideoSendController` admission (`common/include/parties/video_send_controller.h`) | Counts bytes handed to QUIC minus bytes acknowledged in `SEND_COMPLETE`. On the WGC capture thread a frame is encoded only when the outstanding bytes imply no more than ~120 ms of queueing beyond the RTT; otherwise the capture frame is skipped. Skipping at capture keeps the reference chain intact because `frame_seq` is stamped after encode. Atomics only, never blocks |
| Sharer | `VideoSendController` AIMD bitrate | Evaluated on the main thread in 500 ms windows. A window is congested when its peak queueing delay is >= 80 ms, actual (spurious-corrected) packet loss exceeds 3 %, or any admission, encoder, or send drop occurred in it; after two consecutive congested windows the bitrate is multiplied by 0.8 (not below the 800 kbps floor), and a clean window resets the count. x1.08 after 2 s clean, up to the user target. Encoder reconfigurations are gated to one per 500 ms and a >= 10 % delta, except that the final step onto the user's target (or onto the floor) is always published even when it is smaller than the delta gate. The encoder picks the new bitrate up on the encode thread via `take_bitrate_update()` and a keyframe request via `take_keyframe_request()` |
| Both hops | Per-frame unidirectional QUIC streams (protocol 1.2) | One stream per encoded frame, `[0x12][header 14 B][encoded]` sharer to server and `[0x12][sender_id][header][encoded]` server to viewer, one `StreamSend` with FIN. A lost packet delays only its frame. Stream 1 stays for pre-1.2 peers and PLI |
| Server | Per-viewer backlog gate (`common/include/parties/video_backlog.h`) | Outstanding bytes per viewer connection; a frame is dropped as a whole for a viewer whose outstanding bytes exceed `clamp(viewer_rate x (MinRtt + 250 ms) + keyframe_headroom, 64 KB, 2 MB)`, where `viewer_rate` is the sum of the rates of every sharer the viewer is subscribed to (they share one counter), `MinRtt` is sampled from `QUIC_PARAM_CONN_STATISTICS_V2` about once per second (the smoothed RTT already contains the queueing being detected; with send buffering disabled a send completes only on acknowledgement, so the RTT term keeps a long-RTT viewer from being throttled on a lossless link), and `keyframe_headroom` is the largest recent keyframe. After a drop the viewer resumes only at a keyframe, requested from the sharer once per 200 ms per sharer |
| Server | Per-frame ingress guard (`Session::video_ingress_allowed`, `Session::video_in_buffered_bytes`) | Per-frame streams are accepted only from authenticated sessions currently registered as sharers; any other session's unidirectional stream is aborted at start. At most 16 MB may be buffered across one session's unfinished per-frame streams, and the largest `[header][encoded]` a sharer may send is `VIDEO_FRAME_MAX_PAYLOAD_BYTES` (`VIDEO_FRAME_MAX_BYTES` - 5: 4 MB minus the 5-byte forwarding prefix) |
| Viewer | `VideoFrameReorderBuffer` (`common/include/parties/video_frame_reorder.h`) | Per-frame streams complete out of order. The buffer delivers the next expected `frame_seq` immediately, holds newer complete frames for up to 150 ms / 8 frames / 8 MB while an older one is missing, delivers a held keyframe at once, and re-bases on a sharer counter reset. A gap that is closed by a keyframe is not reported as a loss and sends no PLI: that is how the server resumes a throttled viewer, and the decoder re-anchors on the keyframe. Only a gap resumed at a delta frame (150 ms hold timeout, aborted stream, or buffer overflow) is reported, once, and then the decode gate requests a keyframe through the PLI funnel |
| Viewer | PLI funnel | Single client entry point with a 500 ms cooldown per sharer, retried every 500 ms while a watched stream still awaits a keyframe, subject to the escalation cap described under [Decode backlog policy](#decode-backlog-policy). The self-preview stream never sends PLIs |

### Decode backlog policy

The decode thread queues complete frames per sharer. Two rules trigger a
resynchronisation (flush to the newest keyframe if one is queued, otherwise
flush everything and send a PLI through the funnel):

- hard limit: more than 60 queued frames with no keyframe in the queue;
- age: the oldest queued frame is older than 500 ms, the decoder is warm, and no
  keyframe is queued.

Decoding itself never stops during a backlog: every queued frame is decoded so
the reference chain stays valid, but the decoded planes are copied out and
published only for the last frame of a batch. Catching up therefore costs decode
time, not upload and presentation time.

The PLI side of this policy is capped. After two consecutive stale-backlog
flushes that were not followed by a keyframe, the viewer stops sending PLIs for
that stream (it logs "decoder too slow") and waits for the sharer's periodic
keyframe (at most 5 s apart) instead of asking for keyframes it cannot decode in
time. The self-preview stream (the sharer watching its own output) never sends
PLIs.

NVDEC packets are submitted with `CUVID_PKT_ENDOFPICTURE`, so the decoder emits
a picture as soon as its data is in rather than waiting for the first packet of
the next frame. dav1d runs with `clamp(hardware_concurrency() / 2, 4, 16)`
threads.

### Encoder rate control

The UI bitrate is an average. The old policy (peak 2x average, VBV = 2x average
bits, i.e. a two-second reservoir) let one keyframe be several hundred kilobytes,
which on a link close to the average is a stall followed by catch-up. The
low-latency policy in `encdec/include/encdec/rate_control.h`
(`make_stream_vbr_rate_control(average, fps)`) expresses the reservoir in frame
intervals:

| Field | Value |
|-------|-------|
| `peak_bitrate` | 1.5 x average |
| `vbv_buffer_size` | max(4 frame intervals at average, 250 kbit) |
| `vbv_initial_delay` | = `vbv_buffer_size` |
| `max_frame_bits` | 6 frame intervals at average (~200 ms of link time at 30 fps), never below `vbv_buffer_size` |

Which fields each backend consumes:

| Backend | Fields |
|---------|--------|
| NVENC | `average`, `peak`, `vbv_buffer_size`, `vbv_initial_delay` |
| AMF | `average`, `peak`, `vbv_buffer_size`, `max_frame_bits` (`MAX_AU_SIZE` / `HEVC_MAX_AU_SIZE` / `AV1_MAX_COMPRESSED_FRAME_SIZE`) |
| MFT | `average`, `peak`, `vbv_buffer_size` (`CODECAPI_AVEncCommonBufferSize`) |
| VideoToolbox | `average` plus a `DataRateLimits` window derived from `peak` |

Bitrate changes reach the encoder only from the encode thread
(`AppCore::take_video_bitrate_update()`); the UI slider merely updates the
controller's target ceiling.

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
