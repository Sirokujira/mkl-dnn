/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <assert.h>
#include <math.h>

#include "c_types_map.hpp"
#include "math_utils.hpp"
#include "mkldnn_thread.hpp"
#include "nstl.hpp"
#include "type_helpers.hpp"

#include "ref_pooling.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <data_type_t data_type, data_type_t acc_type>
void ref_pooling_fwd_t<data_type, acc_type>::execute_forward() {
    using namespace alg_kind;
    using namespace prop_kind;

    auto alg = conf_.desc()->alg_kind;

    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto dst = reinterpret_cast<data_t *>(this->memory(0));
    auto ws = alg == pooling_max && conf_.desc()->prop_kind == forward_training
        ? reinterpret_cast<unsigned char *>(this->memory(1)) : nullptr;

    const memory_desc_wrapper src_d(conf_.src_pd());
    const memory_desc_wrapper dst_d(conf_.dst_pd());
    const memory_desc_wrapper ws_d(conf_.workspace_pd());
    const data_type_t ws_dt = ws ? ws_d.data_type() : data_type::undef;

    const int ID = conf_.ID();
    const int IH = conf_.IH();
    const int IW = conf_.IW();
    const int KD = conf_.KD();
    const int KH = conf_.KH();
    const int KW = conf_.KW();
    const int SD = conf_.KSD();
    const int SH = conf_.KSH();
    const int SW = conf_.KSW();
    const int padF = conf_.padFront();
    const int padT = conf_.padT();
    const int padL = conf_.padL();

    const bool is_3d = conf_.desc()->src_desc.ndims == 5;

    auto apply_offset = [=](int index, int offset) {
        return (index > offset) ? index - offset : 0;
    };

    auto set_ws = [=](int mb, int oc, int od, int oh, int ow, int value) {
        if (ws) {
            assert(ws_dt == data_type::u8 || ws_dt == data_type::s32);
            size_t offset = is_3d
                ? ws_d.off(mb, oc, od, oh, ow) : ws_d.off(mb, oc, oh, ow);;
            if (ws_dt == data_type::u8) {
                assert(0 <= value && value <= 255);
                ws[offset] = value;
            } else
                reinterpret_cast<int *>(ws)[offset] = value;
        }
    };

    auto ker_max = [=](data_t *d, int mb, int oc, int oh, int ow) {
        for (int kh = 0; kh < KH; ++kh) {
            for (int kw = 0; kw < KW; ++kw) {
                const int ih = oh * SH - padT + kh;
                const int iw = ow * SW - padL + kw;

                if (ih < 0 || ih >= IH) continue;
                if (iw < 0 || iw >= IW) continue;

                auto s = src[src_d.off(mb, oc, ih, iw)];
                if (s > d[0]) {
                    d[0] = s;
                    set_ws(mb, oc, 1, oh, ow, kh*KW + kw);
                }
            }
        }
    };

    auto ker_avg = [=](data_t *d, int mb, int oc, int oh, int ow) {
        auto ih_start = apply_offset(oh*SH, padT);
        auto iw_start = apply_offset(ow*SW, padL);
        auto ih_end = nstl::min(oh*SH - padT + KH, IH);
        auto iw_end = nstl::min(ow*SW - padL + KW, IW);

        auto num_summands = (alg == pooling_avg_include_padding) ? KW*KH
            : (ih_end - ih_start)*(iw_end - iw_start);

        acc_data_t dst = 0;
        for (int ih = ih_start; ih < ih_end; ++ih) {
            for (int iw = iw_start; iw < iw_end; ++iw) {
                dst += src[src_d.off(mb, oc, ih, iw)];
            }
        }

        d[0] = math::out_round<data_t>((float)dst / num_summands);
    };

    auto ker_max_3d = [=](data_t *d, int mb, int oc, int od, int oh, int ow) {
        for (int kd = 0; kd < KD; ++kd) {
            for (int kh = 0; kh < KH; ++kh) {
                for (int kw = 0; kw < KW; ++kw) {
                    const int id = od * SD - padF + kd;
                    const int ih = oh * SH - padT + kh;
                    const int iw = ow * SW - padL + kw;

                    if (id < 0 || id >= ID) continue;
                    if (ih < 0 || ih >= IH) continue;
                    if (iw < 0 || iw >= IW) continue;

                    auto s = src[src_d.off(mb, oc, id, ih, iw)];
                    if (s > d[0]) {
                        d[0] = s;
                        set_ws(mb, oc, od, oh, ow, kd * KH * KW + kh*KW + kw);
                    }
                }
            }
        }
    };

    auto ker_avg_3d = [=](data_t *d, int mb, int oc, int od, int oh, int ow) {
        auto id_start = apply_offset(od*SD, padF);
        auto ih_start = apply_offset(oh*SH, padT);
        auto iw_start = apply_offset(ow*SW, padL);
        auto id_end = nstl::min(od*SD - padF + KD, ID);
        auto ih_end = nstl::min(oh*SH - padT + KH, IH);
        auto iw_end = nstl::min(ow*SW - padL + KW, IW);

        auto num_summands = (alg == pooling_avg_include_padding) ? KW*KH*KD
            : (ih_end - ih_start)*(iw_end - iw_start)*(id_end - id_start);

        acc_data_t dst = 0;
        for (int id = id_start; id < id_end; ++id) {
            for (int ih = ih_start; ih < ih_end; ++ih) {
                for (int iw = iw_start; iw < iw_end; ++iw) {
                    dst += src[src_d.off(mb, oc, id, ih, iw)];
                }
            }
        }

        d[0] = math::out_round<data_t>((float)dst / num_summands);
    };

    const int MB = conf_.MB();
    const int OC = conf_.C();
    const int OD = conf_.OD();
    const int OH = conf_.OH();
    const int OW = conf_.OW();

    if (alg == pooling_max) {
#       pragma omp parallel for collapse(5) schedule(static)
        for (int mb = 0; mb < MB; ++mb) {
            for (int oc = 0; oc < OC; ++oc) {
                for (int od = 0; od < OD; ++od)
                for (int oh = 0; oh < OH; ++oh)
                for (int ow = 0; ow < OW; ++ow) {
                    data_t *d = is_3d
                        ? &dst[dst_d.off(mb, oc, od, oh, ow)]
                        : &dst[dst_d.off(mb, oc, oh, ow)];
                        d[0] = nstl::numeric_limits<data_t>::lowest();
                        set_ws(mb, oc, od, oh, ow, 0);
                        if (is_3d) ker_max_3d(d, mb, oc, od, oh, ow);
                        else ker_max(d, mb, oc, oh, ow);
                }
            }
        }
    } else {
#       pragma omp parallel for collapse(5) schedule(static)
        for (int mb = 0; mb < MB; ++mb) {
            for (int oc = 0; oc < OC; ++oc) {
                for (int od = 0; od < OD; ++od)
                for (int oh = 0; oh < OH; ++oh)
                for (int ow = 0; ow < OW; ++ow) {
                        data_t *d = is_3d
                            ? &dst[dst_d.off(mb, oc, od, oh, ow)]
                            : &dst[dst_d.off(mb, oc, oh, ow)];
                        d[0] = 0;
                        if (is_3d) ker_avg_3d(d, mb, oc, od, oh, ow);
                        else ker_avg(d, mb, oc, oh, ow);
                }
            }
        }
    }
}

template <data_type_t data_type, data_type_t acc_type>
void ref_pooling_bwd_t<data_type, acc_type>::execute_backward() {
    using namespace alg_kind;

    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto ws = conf_.desc()->alg_kind != alg_kind::pooling_max ? nullptr
        : reinterpret_cast<const unsigned char *>(this->input_memory(1));
    auto diff_src = reinterpret_cast<data_t *>(this->memory(0));

    const memory_desc_wrapper diff_dst_d(conf_.diff_dst_pd());
    const memory_desc_wrapper ws_d(conf_.workspace_pd());
    const memory_desc_wrapper diff_src_d(conf_.diff_src_pd());

    const int ID = conf_.ID();
    const int IH = conf_.IH();
    const int IW = conf_.IW();
    const int KD = conf_.KD();
    const int KH = conf_.KH();
    const int KW = conf_.KW();
    const int SD = conf_.KSD();
    const int SH = conf_.KSH();
    const int SW = conf_.KSW();
    const int padF = conf_.padFront();
    const int padT = conf_.padT();
    const int padL = conf_.padL();

    const bool is_3d = conf_.desc()->diff_src_desc.ndims == 5;

    auto alg = conf_.desc()->alg_kind;

    auto apply_offset = [=](int index, int offset) {
        return (index > offset) ? index - offset : 0;
    };

    auto ker_zero = [=](int _mb, int _oc) {
        for (int ih = 0; ih < IH; ++ih) {
            for (int iw = 0; iw < IW; ++iw) {
                diff_src[diff_src_d.off(_mb, _oc, ih, iw)] = data_type_t(0);
            }
        }
    };

    auto ker_max = [=](const data_t *d, int mb, int oc, int oh, int ow) {
        const size_t ws_off = ws_d.off(mb, oc, oh, ow);
        const int index = ws_d.data_type() == data_type::u8
            ? (int)ws[ws_off] : ((int *)ws)[ws_off];
        const int kw = index % KW;
        const int kh = index / KW;
        const int ih = oh * SH - padT + kh;
        const int iw = ow * SW - padL + kw;

        // If padding area could fit the kernel,
        // then input displacement would be out of bounds.
        // No need to back propagate there as padding is
        // virtual in pooling_max case.
        if (ih < 0 || ih >= IH)
            return;
        if (iw < 0 || iw >= IW)
            return;

        diff_src[diff_src_d.off(mb, oc, ih, iw)] += d[0];
    };

    auto ker_avg = [=](const data_t *d, int mb, int oc, int oh, int ow) {
        auto ih_start = apply_offset(oh*SH, padT);
        auto iw_start = apply_offset(ow*SW, padL);
        auto ih_end = nstl::min(oh*SH - padT + KH, IH);
        auto iw_end = nstl::min(ow*SW - padL + KW, IW);

        auto num_summands = (alg == pooling_avg_include_padding) ? KW*KH
            : (ih_end - ih_start)*(iw_end - iw_start);

        for (int ih = ih_start; ih < ih_end; ++ih) {
            for (int iw = iw_start; iw < iw_end; ++iw) {
                diff_src[diff_src_d.off(mb, oc, ih, iw)] += d[0] / num_summands;
            }
        }
    };

    auto ker_zero_3d = [=](int _mb, int _oc) {
        for (int id = 0; id < ID; ++id) {
            for (int ih = 0; ih < IH; ++ih) {
                for (int iw = 0; iw < IW; ++iw) {
                    diff_src[diff_src_d.off(_mb, _oc, id, ih, iw)] =
                        data_type_t(0);
                }
            }
        }
    };

    auto ker_max_3d = [=](const data_t *d, int mb, int oc, int od, int oh,
            int ow) {
        const size_t ws_off = ws_d.off(mb, oc, od, oh, ow);
        const int index = ws_d.data_type() == data_type::u8
            ? (int)ws[ws_off] : ((int *)ws)[ws_off];
        const int kw = index % KW;
        const int kh = (index / KW) % KH;
        const int kd = (index / KW) / KH;
        const int id = od * SD - padF + kd;
        const int ih = oh * SH - padT + kh;
        const int iw = ow * SW - padL + kw;

        // If padding area could fit the kernel,
        // then input displacement would be out of bounds.
        // No need to back propagate there as padding is
        // virtual in pooling_max case.
        if (id < 0 || id >= ID)
            return;
        if (ih < 0 || ih >= IH)
            return;
        if (iw < 0 || iw >= IW)
            return;

        diff_src[diff_src_d.off(mb, oc, id, ih, iw)] += d[0];
    };

    auto ker_avg_3d = [=](const data_t *d, int mb, int oc, int od, int oh,
            int ow) {
        auto id_start = apply_offset(od*SD, padF);
        auto ih_start = apply_offset(oh*SH, padT);
        auto iw_start = apply_offset(ow*SW, padL);
        auto id_end = nstl::min(od*SD - padF + KD, ID);
        auto ih_end = nstl::min(oh*SH - padT + KH, IH);
        auto iw_end = nstl::min(ow*SW - padL + KW, IW);

        auto num_summands = (alg == pooling_avg_include_padding) ? KW*KH*KD
            : (ih_end - ih_start)*(iw_end - iw_start)*(id_end - id_start);

        for (int id = id_start; id < id_end; ++id)
        for (int ih = ih_start; ih < ih_end; ++ih)
        for (int iw = iw_start; iw < iw_end; ++iw) {
            diff_src[diff_src_d.off(mb, oc, id, ih, iw)] += d[0] / num_summands;
        }
    };

    const int MB = conf_.MB();
    const int OC = conf_.C();
    const int OD = conf_.OD();
    const int OH = conf_.OH();
    const int OW = conf_.OW();

    if (conf_.desc()->alg_kind == alg_kind::pooling_max) {
#       pragma omp parallel for collapse(2) schedule(static)
        for (int mb = 0; mb < MB; ++mb) {
            for (int oc = 0; oc < OC; ++oc) {
                if (is_3d) ker_zero_3d(mb, oc);
                else ker_zero(mb, oc);
                for (int od = 0; od < OD; ++od) {
                    for (int oh = 0; oh < OH; ++oh) {
                        for (int ow = 0; ow < OW; ++ow) {
                            const data_t *d = is_3d
                                ? &diff_dst[diff_dst_d.off(mb, oc, od, oh, ow)]
                                : &diff_dst[diff_dst_d.off(mb, oc, oh, ow)];
                            if (is_3d) ker_max_3d(d, mb, oc, od, oh, ow);
                            else ker_max(d, mb, oc, oh, ow);
                        }
                    }
                }
            }
        }
    } else {
#       pragma omp parallel for collapse(2) schedule(static)
        for (int mb = 0; mb < MB; ++mb) {
            for (int oc = 0; oc < OC; ++oc) {
                if (is_3d) ker_zero_3d(mb, oc);
                else ker_zero(mb, oc);
                for (int od = 0; od < OD; ++od) {
                    for (int oh = 0; oh < OH; ++oh) {
                        for (int ow = 0; ow < OW; ++ow) {
                            const data_t *d = is_3d
                                ? &diff_dst[diff_dst_d.off(mb, oc, od, oh, ow)]
                                : &diff_dst[diff_dst_d.off(mb, oc, oh, ow)];
                            if (is_3d) ker_avg_3d(d, mb, oc, od, oh, ow);
                            else ker_avg(d, mb, oc, oh, ow);
                        }
                    }
                }
            }
        }
    }
}

template struct ref_pooling_fwd_t<data_type::f32>;
template struct ref_pooling_fwd_t<data_type::s32>;
template struct ref_pooling_fwd_t<data_type::s16, data_type::s32>;
template struct ref_pooling_fwd_t<data_type::s8, data_type::s32>;
template struct ref_pooling_fwd_t<data_type::u8, data_type::s32>;

template struct ref_pooling_bwd_t<data_type::f32>;
template struct ref_pooling_bwd_t<data_type::s32>;
template struct ref_pooling_bwd_t<data_type::s16, data_type::s32>;

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
