#include "libhevc_decoder.h"

#include <libhevc/ihevc_typedefs.h>
#include <libhevc/iv.h>
#include <libhevc/ivd.h>
#include <libhevc/ihevcd_cxa.h>

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <thread>
#include <parties/log.h>
#include <parties/profiler.h>

namespace parties::encdec::libhevc {

static inline iv_obj_t* obj(void* p) { return static_cast<iv_obj_t*>(p); }

static void* hevc_aligned_alloc(void*, WORD32 alignment, WORD32 size) {
#ifdef _WIN32
    return _aligned_malloc(static_cast<size_t>(size), static_cast<size_t>(alignment));
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<size_t>(alignment), static_cast<size_t>(size)) != 0)
        return nullptr;
    return ptr;
#endif
}

static void hevc_aligned_free(void*, void* ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

LibhevcDecoder::~LibhevcDecoder() {
    if (codec_obj_) {
        ivd_delete_ip_t del_ip = {};
        ivd_delete_op_t del_op = {};
        del_ip.u4_size = sizeof(del_ip);
        del_ip.e_cmd = IVD_CMD_DELETE;
        del_op.u4_size = sizeof(del_op);
        ihevcd_cxa_api_function(obj(codec_obj_), &del_ip, &del_op);
        codec_obj_ = nullptr;
    }
}

bool LibhevcDecoder::init(VideoCodecId codec, uint32_t, uint32_t) {
    if (codec != VideoCodecId::H265)
        return false;

    ihevcd_cxa_create_ip_t create_ip = {};
    ihevcd_cxa_create_op_t create_op = {};

    create_ip.s_ivd_create_ip_t.u4_size = sizeof(create_ip);
    create_ip.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
    create_ip.s_ivd_create_ip_t.e_output_format = IV_YUV_420P;
    create_ip.s_ivd_create_ip_t.u4_share_disp_buf = 0;
    create_ip.s_ivd_create_ip_t.pf_aligned_alloc = hevc_aligned_alloc;
    create_ip.s_ivd_create_ip_t.pf_aligned_free = hevc_aligned_free;
    create_ip.s_ivd_create_ip_t.pv_mem_ctxt = nullptr;
    create_ip.u4_enable_frame_info = 0;
    create_ip.u4_keep_threads_active = 0;
    create_ip.u4_enable_yuv_formats = 0;

    create_op.s_ivd_create_op_t.u4_size = sizeof(create_op);

    IV_API_CALL_STATUS_T status = ihevcd_cxa_api_function(
        nullptr, &create_ip, &create_op);

    if (status != IV_SUCCESS || !create_op.s_ivd_create_op_t.pv_handle) {
        LOG_ERROR("Create failed: {:#010x}",
                  create_op.s_ivd_create_op_t.u4_error_code);
        return false;
    }

    codec_obj_ = create_op.s_ivd_create_op_t.pv_handle;
    obj(codec_obj_)->pv_fxns = reinterpret_cast<void*>(&ihevcd_cxa_api_function);
    obj(codec_obj_)->u4_size = sizeof(iv_obj_t);

    set_num_cores();

    set_decode_mode(IVD_DECODE_FRAME);

    LOG_INFO("H.265 software decoder initialized");
    return true;
}

bool LibhevcDecoder::set_decode_mode(int mode) {
    ihevcd_cxa_ctl_set_config_ip_t ctl_ip = {};
    ihevcd_cxa_ctl_set_config_op_t ctl_op = {};

    ctl_ip.s_ivd_ctl_set_config_ip_t.u4_size = sizeof(ctl_ip);
    ctl_ip.s_ivd_ctl_set_config_ip_t.e_cmd = IVD_CMD_VIDEO_CTL;
    ctl_ip.s_ivd_ctl_set_config_ip_t.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
    ctl_ip.s_ivd_ctl_set_config_ip_t.e_vid_dec_mode =
        static_cast<IVD_VIDEO_DECODE_MODE_T>(mode);
    ctl_ip.s_ivd_ctl_set_config_ip_t.e_frm_skip_mode = IVD_SKIP_NONE;
    ctl_ip.s_ivd_ctl_set_config_ip_t.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
    ctl_ip.s_ivd_ctl_set_config_ip_t.u4_disp_wd = 0;

    ctl_op.s_ivd_ctl_set_config_op_t.u4_size = sizeof(ctl_op);

    return ihevcd_cxa_api_function(obj(codec_obj_), &ctl_ip, &ctl_op) == IV_SUCCESS;
}

bool LibhevcDecoder::set_num_cores() {
    ihevcd_cxa_ctl_set_num_cores_ip_t cores_ip = {};
    ihevcd_cxa_ctl_set_num_cores_op_t cores_op = {};

    cores_ip.u4_size = sizeof(cores_ip);
    cores_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    cores_ip.e_sub_cmd = static_cast<IVD_CONTROL_API_COMMAND_TYPE_T>(
        IHEVCD_CXA_CMD_CTL_SET_NUM_CORES);
    cores_ip.u4_num_cores = std::min(4u, std::thread::hardware_concurrency());

    cores_op.u4_size = sizeof(cores_op);

    return ihevcd_cxa_api_function(obj(codec_obj_), &cores_ip, &cores_op) == IV_SUCCESS;
}

bool LibhevcDecoder::decode(const uint8_t* data, size_t len, int64_t timestamp) {
    ZoneScopedN("LibhevcDecoder::decode");
    if (!codec_obj_) return false;

    if (y_buf_.empty()) {
        buf_width_ = 1920;
        buf_height_ = 1088;
        size_t y_size = static_cast<size_t>(buf_width_) * buf_height_;
        size_t uv_size = y_size / 4;
        y_buf_.resize(y_size);
        u_buf_.resize(uv_size);
        v_buf_.resize(uv_size);
    }

    size_t bytes_remaining = len;
    const uint8_t* ptr = data;

    while (bytes_remaining > 0) {
        ihevcd_cxa_video_decode_ip_t dec_ip = {};
        ihevcd_cxa_video_decode_op_t dec_op = {};

        ivd_video_decode_ip_t* ip = &dec_ip.s_ivd_video_decode_ip_t;
        ivd_video_decode_op_t* op = &dec_op.s_ivd_video_decode_op_t;

        ip->u4_size = sizeof(dec_ip);
        ip->e_cmd = IVD_CMD_VIDEO_DECODE;
        ip->u4_ts = static_cast<UWORD32>(timestamp & 0xFFFFFFFF);
        ip->pv_stream_buffer = const_cast<void*>(static_cast<const void*>(ptr));
        ip->u4_num_Bytes = static_cast<UWORD32>(bytes_remaining);

        ip->s_out_buffer.u4_num_bufs = 3;
        ip->s_out_buffer.pu1_bufs[0] = y_buf_.data();
        ip->s_out_buffer.pu1_bufs[1] = u_buf_.data();
        ip->s_out_buffer.pu1_bufs[2] = v_buf_.data();
        ip->s_out_buffer.u4_min_out_buf_size[0] = static_cast<UWORD32>(y_buf_.size());
        ip->s_out_buffer.u4_min_out_buf_size[1] = static_cast<UWORD32>(u_buf_.size());
        ip->s_out_buffer.u4_min_out_buf_size[2] = static_cast<UWORD32>(v_buf_.size());

        op->u4_size = sizeof(dec_op);

        IV_API_CALL_STATUS_T status = ihevcd_cxa_api_function(
            obj(codec_obj_), &dec_ip, &dec_op);

        if (status != IV_SUCCESS &&
            (op->u4_error_code & 0xFF) == IVD_RES_CHANGED) {
            ihevcd_cxa_ctl_getbufinfo_ip_t buf_ip = {};
            ihevcd_cxa_ctl_getbufinfo_op_t buf_op = {};
            buf_ip.s_ivd_ctl_getbufinfo_ip_t.u4_size = sizeof(buf_ip);
            buf_ip.s_ivd_ctl_getbufinfo_ip_t.e_cmd = IVD_CMD_VIDEO_CTL;
            buf_ip.s_ivd_ctl_getbufinfo_ip_t.e_sub_cmd = IVD_CMD_CTL_GETBUFINFO;
            buf_op.s_ivd_ctl_getbufinfo_op_t.u4_size = sizeof(buf_op);

            ihevcd_cxa_api_function(obj(codec_obj_), &buf_ip, &buf_op);

            auto& info = buf_op.s_ivd_ctl_getbufinfo_op_t;
            if (info.u4_num_disp_bufs >= 1) {
                y_buf_.resize(info.u4_min_out_buf_size[0]);
                u_buf_.resize(info.u4_min_out_buf_size[1]);
                v_buf_.resize(info.u4_min_out_buf_size[2]);
            }

            ihevcd_cxa_ctl_reset_ip_t rst_ip = {};
            ihevcd_cxa_ctl_reset_op_t rst_op = {};
            rst_ip.s_ivd_ctl_reset_ip_t.u4_size = sizeof(rst_ip);
            rst_ip.s_ivd_ctl_reset_ip_t.e_cmd = IVD_CMD_VIDEO_CTL;
            rst_ip.s_ivd_ctl_reset_ip_t.e_sub_cmd = IVD_CMD_CTL_RESET;
            rst_op.s_ivd_ctl_reset_op_t.u4_size = sizeof(rst_op);
            ihevcd_cxa_api_function(obj(codec_obj_), &rst_ip, &rst_op);

            set_num_cores();
            set_decode_mode(IVD_DECODE_FRAME);
            continue;
        }

        UWORD32 consumed = op->u4_num_bytes_consumed;
        if (consumed == 0 && status != IV_SUCCESS)
            break;

        if (op->u4_output_present && on_decoded) {
            iv_yuv_buf_t& disp = op->s_disp_frm_buf;
            DecodedFrame f{};
            f.y_plane  = static_cast<const uint8_t*>(disp.pv_y_buf);
            f.u_plane  = static_cast<const uint8_t*>(disp.pv_u_buf);
            f.v_plane  = static_cast<const uint8_t*>(disp.pv_v_buf);
            f.y_stride = disp.u4_y_strd;
            f.uv_stride = disp.u4_u_strd;
            f.width    = disp.u4_y_wd;
            f.height   = disp.u4_y_ht;
            f.timestamp = timestamp;
            on_decoded(f);
        }

        ptr += consumed;
        bytes_remaining -= consumed;

        if (consumed == 0)
            break;
    }

    return true;
}

void LibhevcDecoder::drain_flush() {
    if (!codec_obj_) return;

    while (true) {
        ihevcd_cxa_video_decode_ip_t dec_ip = {};
        ihevcd_cxa_video_decode_op_t dec_op = {};

        ivd_video_decode_ip_t* ip = &dec_ip.s_ivd_video_decode_ip_t;
        ivd_video_decode_op_t* op = &dec_op.s_ivd_video_decode_op_t;

        ip->u4_size = sizeof(dec_ip);
        ip->e_cmd = IVD_CMD_VIDEO_DECODE;
        ip->u4_ts = 0;
        ip->pv_stream_buffer = nullptr;
        ip->u4_num_Bytes = 0;

        ip->s_out_buffer.u4_num_bufs = 3;
        ip->s_out_buffer.pu1_bufs[0] = y_buf_.data();
        ip->s_out_buffer.pu1_bufs[1] = u_buf_.data();
        ip->s_out_buffer.pu1_bufs[2] = v_buf_.data();
        ip->s_out_buffer.u4_min_out_buf_size[0] = static_cast<UWORD32>(y_buf_.size());
        ip->s_out_buffer.u4_min_out_buf_size[1] = static_cast<UWORD32>(u_buf_.size());
        ip->s_out_buffer.u4_min_out_buf_size[2] = static_cast<UWORD32>(v_buf_.size());

        op->u4_size = sizeof(dec_op);

        ihevcd_cxa_api_function(obj(codec_obj_), &dec_ip, &dec_op);

        if (op->u4_output_present && on_decoded) {
            iv_yuv_buf_t& disp = op->s_disp_frm_buf;
            DecodedFrame f{};
            f.y_plane  = static_cast<const uint8_t*>(disp.pv_y_buf);
            f.u_plane  = static_cast<const uint8_t*>(disp.pv_u_buf);
            f.v_plane  = static_cast<const uint8_t*>(disp.pv_v_buf);
            f.y_stride = disp.u4_y_strd;
            f.uv_stride = disp.u4_u_strd;
            f.width    = disp.u4_y_wd;
            f.height   = disp.u4_y_ht;
            f.timestamp = 0;
            on_decoded(f);
        } else {
            break;
        }
    }
}

void LibhevcDecoder::flush() {
    ZoneScopedN("LibhevcDecoder::flush");
    if (!codec_obj_) return;

    ivd_ctl_flush_ip_t flush_ip = {};
    ivd_ctl_flush_op_t flush_op = {};
    flush_ip.u4_size = sizeof(flush_ip);
    flush_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    flush_ip.e_sub_cmd = IVD_CMD_CTL_FLUSH;
    flush_op.u4_size = sizeof(flush_op);

    if (ihevcd_cxa_api_function(obj(codec_obj_), &flush_ip, &flush_op) != IV_SUCCESS)
        return;

    drain_flush();
}

DecoderInfo LibhevcDecoder::info() const {
    return {Backend::Libhevc, VideoCodecId::H265};
}

}
