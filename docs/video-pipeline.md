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
| Sharer | `VideoSendController` admission (`common/include/parties/video_send_controller.h`) | Counts bytes handed to QUIC minus bytes acknowledged in `SEND_COMPLETE`. On the WGC capture thread a frame is encoded only when the outstanding bytes imply no more than ~120 ms of queueing beyond the expected in-flight time, `RTT x (1 + min(30 x loss, 1.5))` (each lost packet costs about one more round trip before it is acknowledged); otherwise the capture frame is skipped. The RTT is the controller's own windowed minimum of the smoothed RTT (two 5 s blocks), not MsQuic's `MinRtt`, which can be stale from the handshake or a previous path. Skipping at capture keeps the reference chain intact because `frame_seq` is stamped after encode. Atomics only, never blocks |
| Sharer | `VideoSendController` AIMD bitrate | Evaluated on the main thread in 500 ms windows. A window is congested when its *minimum* queueing delay is >= 100 ms (a standing queue: the link never drained) or more than 20 % of the frames were skipped by admission. After two consecutive congested windows the bitrate drops to `min(0.85 x current, 0.9 x goodput)`, goodput being the bytes acknowledged per second in that window (not below the 800 kbps floor). x1.10 after 1 s clean, up to the user target. Packet loss is deliberately not a congestion signal: the transport retransmits, random Wi-Fi/mobile loss says nothing about capacity, and treating it as congestion pinned streams at the floor. Encoder reconfigurations are gated to one per 500 ms and a >= 10 % delta, except that the final step onto the user's target (or onto the floor) is always published. The encoder picks the new bitrate up on the encode thread via `take_bitrate_update()` and a keyframe request via `take_keyframe_request()` |
| Both hops | Per-frame unidirectional QUIC streams (protocol 1.2) | One stream per encoded frame, `[0x12][header 14 B][encoded]` sharer to server and `[0x12][sender_id][header][encoded]` server to viewer, one `StreamSend` with FIN. A lost packet delays only its frame. Stream 1 stays for pre-1.2 peers and PLI |
| Server | Per-viewer backlog gate (`common/include/parties/video_backlog.h`) | Outstanding bytes per viewer connection, from `StreamSend` to `SEND_COMPLETE`; a frame is dropped as a whole for a viewer whose outstanding bytes exceed `viewer_rate x (0.4 s + (2.5 + min(loss_permille / 30, 2)) x max(MinRtt, SRTT / 2)) + keyframe_headroom` (clamped to 64 KB .. 2 MB), where `viewer_rate` is the sum of the rates of every sharer the viewer is subscribed to, the RTTs and the packet loss toward the viewer are sampled from `QUIC_PARAM_CONN_STATISTICS_V2` inside the connection's QUIC callbacks about once per second, and `keyframe_headroom` is the largest recent keyframe. Loss widens the budget because a viewer on a lossy link keeps more data unacknowledged without being too slow for the stream. When the gate starts dropping, every frame stream already queued for that viewer is aborted (`StreamShutdown(ABORT_SEND)`), and an admitted keyframe aborts the older ones, so stale frames stop using the link. The viewer resumes at a keyframe; its keyframe requests back off from 1 s to 4 s while it stays over budget and reset after 60 frames admitted without a drop |
| Server | Per-frame ingress guard (`Session::video_ingress_allowed`, `Session::video_in_buffered_bytes`) | Per-frame streams are accepted only from authenticated sessions currently registered as sharers; any other session's unidirectional stream is aborted at start. At most 16 MB may be buffered across one session's unfinished per-frame streams, and the largest `[header][encoded]` a sharer may send is `VIDEO_FRAME_MAX_PAYLOAD_BYTES` (`VIDEO_FRAME_MAX_BYTES` - 5: 4 MB minus the 5-byte forwarding prefix) |
| Server and viewer | `VideoFrameReorderBuffer` (`common/include/parties/video_frame_reorder.h`) | Per-frame streams complete out of order. The buffer delivers the next expected `frame_seq` immediately and holds newer complete frames while an older one is missing, for `clamp(3 x RTT + 100 ms, 150 ms, 1.5 s)` (a retransmission needs at least one more round trip), at most 240 frames / 8 MB (the newest is evicted on overflow). A held keyframe is delivered at once. A frame stream that is aborted after its header was read fails its slot immediately instead of waiting out the hold. The early header of a keyframe stream (`on_frame_started`) marks a keyframe as in flight: no keyframe is requested for a gap while one is arriving. Before its first keyframe it holds bounded delta frames without committing a baseline. Arbitrarily late frames never rewind the active sequence; a reset is explicit (watch open, share restart). A gap closed by a keyframe is not reported as a loss; a gap resumed at a delta frame is reported once and leads to a keyframe request |
| Viewer | `VideoReceiveRouter` (`common/include/parties/video_receive_router.h`) | Owns the viewer's reorder buffers and keyframe requests. A request is sent at once for a new gap and then backs off 500 ms, 1 s, 2 s, 4 s while the stream stays broken; a delivered keyframe resets the backoff. Opening a watch waits one base interval before the first request (the server already requests a keyframe on subscribe). The self-preview stream never sends requests |
| Viewer | `VideoPlayoutClock` (`common/include/parties/video_playout.h`) | Jitter buffer. Reliable per-frame streams deliver in clumps (the frames behind a retransmission arrive together), which the renderer shows as a stall and a jump. With `VIDEO_FLAG_CAPTURE_TIME` the sender stamps its capture time in ms into the header `timestamp`; the router's playout thread hands each frame to the decoder at `capture + base + delay`, where `base` is the smallest transit in the last 3 s and `delay` the 95th percentile of the excess (up at once, down at 100 ms/s, at most 300 ms). Frames without the flag play on arrival |

Completed ingress frames pass through a separate queue capped at 16 MB per
session. The server loop restores frame order before backlog gating or legacy
stream-1 forwarding; ordinary in-order traffic incurs at most one polling
interval (nominally 1 ms). Reorder storage is separately capped at 8 MB per
sharer. Queued frames from a stopped share are invalidated by its generation.
RTT sampling never calls the blocking off-worker `GetParam` API while holding
the transport mutex needed by the worker's callbacks.
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
flushes that were not followed by a keyframe, the viewer asks for that stream
at most every 5 s (it logs "decoder too slow") instead of asking after every
flush for keyframes it cannot decode in time; the cap lifts after 4 s of clean
decoding. The self-preview stream (the sharer watching its own output) sends no
PLIs: it asks the local encoder directly, once per resync.

Sharers send no periodic keyframes. Under the two-frame CBR VBV a keyframe is
coded much coarser than the picture the P frames have refined on static
content, so a periodic keyframe showed as the picture falling apart for
0.1-0.2 s every interval. Keyframes come from PLIs (a subscribe, a lost or
aborted frame, a decoder resync); the encoders' GOP of
`VIDEO_KEYFRAME_INTERVAL_MS` (120 s) is only a safety net.

NVDEC packets are submitted with `CUVID_PKT_ENDOFPICTURE`, so the decoder emits
a picture as soon as its data is in rather than waiting for the first packet of
the next frame. dav1d runs with `clamp(hardware_concurrency() / 2, 4, 16)`
threads.

### Encoder rate control

The UI bitrate is an average. The old policy (peak 2x average, VBV = 2x average
bits, i.e. a two-second reservoir) let one keyframe be several hundred kilobytes,
which on a link close to the average is a stall followed by catch-up. The
low-latency policy in `encdec/include/encdec/rate_control.h`
(`make_stream_cbr_rate_control(bitrate, fps)`) is CBR with a reservoir of two
frame intervals, so a frame, keyframes included, never needs more than about two
frame intervals of link time. The first keyframe after a join is coarse and the
following P frames refine it, which is far less visible than a freeze. Filler
data stays disabled: CBR is used for its strict VBV bound, not to pad the stream.

| Field | Value |
|-------|-------|
| `peak_bitrate` | = average (CBR) |
| `vbv_buffer_size` | max(2 frame intervals at average, 50 kbit) |
| `vbv_initial_delay` | = `vbv_buffer_size` |
| `max_frame_bits` | = `vbv_buffer_size` |

Which fields each backend consumes:

| Backend | Fields |
|---------|--------|
| NVENC | `average` (CBR, ultra-low-latency tuning), `vbv_buffer_size`, `vbv_initial_delay` |
| AMF | `average`, `peak`, `vbv_buffer_size`, `max_frame_bits` (`MAX_AU_SIZE` / `HEVC_MAX_AU_SIZE` / `AV1_MAX_COMPRESSED_FRAME_SIZE`) |
| MFT | `average` (CBR), `vbv_buffer_size` (`CODECAPI_AVEncCommonBufferSize`, in bytes, set before the output type) |
| OpenH264 | `average` (= max bitrate); no VBV, frame skipping bounds it |
| VideoToolbox | `average` plus a `DataRateLimits` window derived from `peak` |

Bitrate changes reach the encoder only from the encode thread
(`AppCore::take_video_bitrate_update()`); the UI slider merely updates the
controller's target ceiling. An encoder that can only reach a much higher rate
by being created again reports `needs_reinit()` (AMF HEVC beyond 1.5x its
creation rate); the encode thread re-creates it, at most every 2 s. Keyframe
requests from the network reach the encoder through an atomic flag consumed on
the encode thread; on a static screen the newest captured frame is re-encoded.

## Transport tuning

- **Congestion control: BBR** (`apply_media_transport_settings`, both client and
  server). CUBIC treats every loss as congestion: in the network test a viewer
  with 10 % loss on its downlink got 0.5 fps. MsQuic's BBR still entered
  recovery (cwnd cut to the in-flight data) on every loss and never left it
  under constant random loss; the overlay patch
  `vendor/ports/msquic/0003-bbr-ignore-random-loss.patch` only does that on
  persistent congestion. BBR's ProbeRTT drained the window to four packets for
  200 ms plus a round trip whenever MinRtt was 10 s old, i.e. every 10 s on a
  stable path: a ~250 ms "network" freeze, about every 5 s with the sharer's
  and the viewer's connections probing independently.
  `0004-bbr-gentle-probe-rtt.patch` keeps about one BDP in flight there
  instead (half the regular window; BBRv2/v3 keep half a BDP), which the
  network test's `steady-rtt-50ms` scenario checks.
  `PARTIES_QUIC_CC=cubic` switches back for A/B tests.
- **Stream priorities**: control streams 0xC000 (above video), file transfers
  0x1000 (below it).
- **Network compatibility mode** (Settings > Screen Share): turns off the
  Windows UDP send segmentation (USO) and receive coalescing (URO) offloads,
  which misbehave with some NIC drivers, VPNs and routers, and pins the path
  MTU to 1280 bytes. The offloads belong to MsQuic's datapath, which is created
  by the first connection and lives until exit (overlay patch
  `0002-datapath-offload-toggle.patch`, global param
  `QUIC_PARAM_GLOBAL_DATAPATH_DISABLE_OFFLOADS`); a change after that needs a
  restart.

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
