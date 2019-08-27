/*
All modification made by Intel Corporation: � 2016 Intel Corporation

All contributions by the University of California:
Copyright (c) 2014, 2015, The Regents of the University of California (Regents)
All rights reserved.

All other contributions:
Copyright (c) 2014, 2015, the respective contributors
All rights reserved.
For the list of contributors go to https://github.com/BVLC/caffe/blob/master/CONTRIBUTORS.md


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef MKLDNN_SUPPORTED
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layer.hpp"
#include "caffe/layers/mkldnn_layers.hpp"
#include "caffe/util/cpu_info.hpp"

// TODO: Correct process case if there are no bias
// TODO: Exception handling - mkl-dnn produces exceptions on errors

namespace caffe {

template <typename Dtype>
MKLDNNConvolutionLayer<Dtype>::MKLDNNConvolutionLayer(const LayerParameter& param)
            : MKLDNNLayer<Dtype>(param), ConvolutionLayer<Dtype>(param)
            , fwd_bottom_data(NULL), fwd_top_data(NULL), fwd_weights_data(NULL), fwd_bias_data(NULL)
            , bwdd_weights_data(NULL), bwdw_bottom_data(NULL)
            , bwdd_bottom_diff(NULL), bwdd_top_diff(NULL)
            , bwdw_top_diff(NULL), bwdw_weights_diff(NULL), bwdw_bias_diff(NULL)
            , convFwd_pd(NULL), convBwdData_pd(NULL), convBwdWeights_pd(NULL)
            , fwd_top_data_memory(NULL), bwdd_bottom_diff_memory(NULL)
            , bwdw_weights_diff_memory(NULL), bwdw_bias_diff_memory(NULL)
            , fwd_bottom_data_primitive(NULL), fwd_weights_data_primitive(NULL), fwd_bias_data_primitive(NULL)
            , bwdd_top_diff_primitive(NULL), bwdd_weights_data_primitive(NULL)
            , bwdw_top_diff_primitive(NULL), bwdw_bottom_data_primitive(NULL)
            , width_(0), height_(0), depth_(0), width_out_(0), height_out_(0), depth_out_(0), kernel_w_(0), kernel_h_(0), kernel_d_(0)
            , stride_w_(0), stride_h_(0), stride_d_(0), pad_w_(0), pad_h_(0), pad_d_(0),
            bwdw_weights_diff_iter(NULL),
            bwdw_bias_diff_iter(NULL),
            bwdw_weights_diff_memory_iter(NULL),
            bwdw_bias_diff_memory_iter(NULL)
{
  PERFORMANCE_EVENT_ID_RESET(perf_id_fw_);
  PERFORMANCE_EVENT_ID_RESET(perf_id_bw_);
  PERFORMANCE_EVENT_ID_RESET(perf_id_bw_weights_);
  //Modify by --donghn--
  this->init_ber();
  //End modify
}

template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::compute_output_shape()
{
    ConvolutionLayer<Dtype>::compute_output_shape();
    CHECK_GT(this->output_shape_.size(), 1) << "MKLDNN Convolution layer expects at least 2D spatial output dimension!";
    CHECK_LT(this->output_shape_.size(), 4) << "MKLDNN Convolution layer expects at most 3D spatial output dimension!";
    if (this->output_shape_.size() == 2) {
      this->height_out_ = this->output_shape_[0];
      this->width_out_ = this->output_shape_[1];
    } else {
      this->depth_out_ = this->output_shape_[0];
      this->height_out_ = this->output_shape_[1];
      this->width_out_ = this->output_shape_[2];
    }
}

template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::init_properties(const vector<Blob<Dtype>*>& bottom
                                                , const vector<Blob<Dtype>*>& top)
{
    CHECK_GT(this->num_spatial_axes_, 1) << "MKLDNN Convolution layer expects at least 2D spatial input dimension!";
    CHECK_LT(this->num_spatial_axes_, 4) << "MKLDNN Convolution layer expects at most 3D spatial input dimension!";

    this->num_ = bottom[0]->shape(0);
    this->channels_ = bottom[0]->shape(1);

    if (this->num_spatial_axes_ == 2) {
      this->height_ = bottom[0]->shape(2);
      this->width_ = bottom[0]->shape(3);

      this->stride_h_ = this->stride_.cpu_data()[0];
      this->stride_w_ = this->stride_.cpu_data()[1];

      this->pad_h_ = this->pad_.cpu_data()[0];
      this->pad_w_ = this->pad_.cpu_data()[1];

      this->kernel_h_  = this->kernel_shape_.cpu_data()[0];
      this->kernel_w_ = this->kernel_shape_.cpu_data()[1];
    } else {
      this->depth_ = bottom[0]->shape(2);
      this->height_ = bottom[0]->shape(3);
      this->width_ = bottom[0]->shape(4);

      this->stride_d_ = this->stride_.cpu_data()[0];
      this->stride_h_ = this->stride_.cpu_data()[1];
      this->stride_w_ = this->stride_.cpu_data()[2];

      this->pad_d_ = this->pad_.cpu_data()[0];
      this->pad_h_ = this->pad_.cpu_data()[1];
      this->pad_w_ = this->pad_.cpu_data()[2];

      this->kernel_d_ = this->kernel_shape_.cpu_data()[0];
      this->kernel_h_  = this->kernel_shape_.cpu_data()[1];
      this->kernel_w_ = this->kernel_shape_.cpu_data()[2];
    }

    string _conv_algorithm = this->layer_param_.convolution_param().conv_algorithm();
    if(_conv_algorithm == "direct")
    {
        conv_algorithm = algorithm::convolution_direct;
    }
    else if(_conv_algorithm == "winograd")
    {
        conv_algorithm = algorithm::convolution_winograd;
    }
    else if(_conv_algorithm == "auto")
    {
        conv_algorithm = algorithm::convolution_auto;
    }
    else
    {
        LOG(ERROR) << "Unsupported convolution algorithm.";
        CHECK(false);
    }
}

template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom
                                            , const vector<Blob<Dtype>*>& top)
{
    VLOG(1) << "<< MKLDNNConvolutionLayer<Dtype>::LayerSetUp: " << this->layer_param_.name();
    if (this->layer_param_.has_quantization_param() && this->phase_ == TEST) this->need_quantize_ = true;

    ConvolutionLayer<Dtype>::LayerSetUp(bottom, top);
    init_properties(bottom, top);
    this->bottom_shape_ = &bottom[0]->shape();

    // support for (iter_size > 1) requires additional buffer for weights diff and bias diff
    // Because Net is initialized before Caffe::set_iter_size, so additional buffer should be new and set here
    bwdw_weights_diff_iter_blob.reset(new Blob<Dtype>());
    bwdw_weights_diff_iter_blob->ReshapeLike(*(this->blobs_[0]));
    if (this->bias_term_) {
      bwdw_bias_diff_iter_blob.reset(new Blob<Dtype>());
      bwdw_bias_diff_iter_blob->ReshapeLike(*(this->blobs_[1]));
    }
}

template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom
                                            , const vector<Blob<Dtype>*>& top)
{
    VLOG(1) << " MKLDNNConvolutionLayer<Dtype>::Reshape: " << this->layer_param_.name();
    if (this->num_spatial_axes_ == 2) {
      this->reshape = (this->width_ == bottom[0]->shape(3) &&
                       this->height_ == bottom[0]->shape(2) &&
                       this->channels_ == bottom[0]->shape(1) &&
                       this->num_ == bottom[0]->shape(0)) ? false : true;
    } else {
      this->reshape = (this->depth_ == bottom[0]->shape(2) &&
                       this->width_ == bottom[0]->shape(4) &&
                       this->height_ == bottom[0]->shape(3) &&
                       this->channels_ == bottom[0]->shape(1) &&
                       this->num_ == bottom[0]->shape(0)) ? false : true;
    }
    init_properties(bottom, top);
    BaseConvolutionLayer<Dtype>::ReshapeForMKL(bottom, top);
    // if (bottom.size() > 1) {
    if (this->layer_param_.convolution_param().fusion_type() ==
            ConvolutionParameter::SUM_FUSION &&
        bottom.size() > 1) {
      top[0]->ShareData(*bottom[1]);
    }
}

template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::InitConvolutionFwd(const vector<Blob<Dtype>*>& bottom
                                                , const vector<Blob<Dtype>*>& top)
{
    if (std::is_same<Dtype, double>::value)   NOT_IMPLEMENTED;
    auto propagation = this->phase_ == TEST ? prop_kind::forward_scoring : prop_kind::forward_training;
    bool relu = this->layer_param_.convolution_param().relu();
    Dtype negative_slope = 0;
    if(relu)
    {
        negative_slope = this->layer_param_.convolution_param().negative_slope();
    }

    int32_t g  = std::max(this->group_, 1);
    int32_t n  = this->num_;
    int32_t iw = this->width_;
    int32_t ih = this->height_;
    int32_t ic = this->channels_;
    int32_t id = this->depth_;

    int32_t ow = this->width_out_;
    int32_t oh = this->height_out_;
    int32_t oc = this->num_output_;
    int32_t od = this->depth_out_;

    int32_t kw = this->kernel_w_;
    int32_t kh = this->kernel_h_;
    int32_t kd = this->kernel_d_;

    int32_t sw = this->stride_w_;
    int32_t sh = this->stride_h_;
    int32_t sd = this->stride_d_;

    int32_t pw = this->pad_w_;
    int32_t ph = this->pad_h_;
    int32_t pd = this->pad_d_;

    memory::dims convolutionStrides;
    memory::dims padding;
    memory::dims padding_r;
    memory::dims dilation;
    bool dilated_conv = false;
    const int* dilation_data = this->dilation_.cpu_data();
    for (int i = 0; i < this->num_spatial_axes_; ++i) {
      dilation.push_back(dilation_data[i] - 1);
      if (dilation_data[i] != 1) dilated_conv = true;
    }

    if (this->num_spatial_axes_ == 2) {
      convolutionStrides = {sh, sw};
      padding = {ph, pw};
      padding_r.push_back((oh - 1) * sh - ih + ((kh - 1) * (dilation_data[0]) + 1) - ph);
      padding_r.push_back((ow - 1) * sw - iw + ((kw - 1) * (dilation_data[1]) + 1) - pw);
    } else {
      convolutionStrides = {sd, sh, sw};
      padding = {pd, ph, pw};
      padding_r.push_back((od - 1) * sd - id + ((kd - 1) * (dilation_data[0]) + 1) - pd);
      padding_r.push_back((oh - 1) * sh - ih + ((kh - 1) * (dilation_data[1]) + 1) - ph);
      padding_r.push_back((ow - 1) * sw - iw + ((kw - 1) * (dilation_data[2]) + 1) - pw);
    }

    // ---- Initialize memory descriptors (fromat = any) to create convolution descriptor -------------
    memory::data_type mpcsn = memory::data_type::f32;
    memory::data_type bottom_dt = memory::data_type::f32;
    if (this->need_quantize_) {
      if (this->layer_param_.quantization_param().is_negative_input())
        bottom_dt = memory::data_type::s8;
      else
        bottom_dt = memory::data_type::u8;
    }

    memory::data_type top_dt = memory::data_type::f32;
    if (this->need_quantize_) {
      if (this->bw_layer_out_ == 8) {
        if (relu && negative_slope == 0) {
          top_dt = memory::data_type::u8;
        }
        else {
          top_dt = memory::data_type::s8;
        }
      }
      else {
        top_dt = memory::data_type::f32;
      }
    }
    bool is_sum = false;
    if (this->layer_param_.convolution_param().fusion_type() ==
            ConvolutionParameter::SUM_FUSION &&
        bottom.size() > 1) {
      if(relu)
          is_sum = true;

      memory::data_type bottom_1_dt = memory::data_type::f32;
      if (const_cast<Dtype*>(bottom[1]->prv_data()) != NULL){

        shared_ptr<MKLDNNMemoryDescriptor<Dtype, false> > bottom_1_desc =
            get_mkldnn_prv_descriptor<Dtype, false>(bottom[1]);
        bottom_1_dt = static_cast<memory::data_type>(bottom_1_desc->prv_memory_pd()->desc().data.data_type);
      }

      if (top_dt != bottom_1_dt) {
        top_dt = bottom_1_dt;
        // FIXME: to simplify the calibration tool to handle different data types of conv sum in residual block
        if(top_dt ==  memory::data_type::f32){
          this->need_quantize_ = false;
          bottom_dt = memory::data_type::f32;
        }
      }
    }

    memory::data_type weights_dt = this->need_quantize_ ? memory::data_type::s8 : memory::data_type::f32;
    memory::data_type bias_dt = this->need_quantize_ ? memory::data_type::s32 : memory::data_type::f32;
    memory::format mfmt_any = memory::format::any;

    memory::dims bottom_tz;
    memory::dims bias_tz;
    memory::dims top_tz;
    memory::dims weights_tz;
    if (this->num_spatial_axes_ == 2) {
      bottom_tz = {n, ic, ih, iw};
      bias_tz = {oc};
      top_tz = {n, oc, oh, ow};
      weights_tz = (g!= 1) ? memory::dims{g, oc/g, ic/g, kh, kw} : memory::dims{oc, ic, kh, kw};
    } else {
      bottom_tz = {n, ic, id, ih, iw};
      bias_tz = {oc};
      top_tz = {n, oc, od, oh, ow};
      weights_tz = (g!= 1) ? memory::dims{g, oc/g, ic/g, kd, kh, kw} : memory::dims{oc, ic, kd, kh, kw};
    }

    // ---- Memory descriptors for initializing of convolution primitive descriptor -------------
    memory::desc init_bottom_md({bottom_tz}, bottom_dt, mfmt_any);
    memory::desc init_bias_md({bias_tz}, bias_dt, mfmt_any);
    memory::desc init_top_md({top_tz}, top_dt, mfmt_any);
    memory::desc init_weights_md({weights_tz}, weights_dt, mfmt_any);

    size_t coeff_size = this->layer_param_.convolution_param().coeff_size();
    float coeff0 = 1;
    float coeff1 = 1;
    if (coeff_size == 2)
    {
      coeff0 = this->layer_param_.convolution_param().coeff(0);
      coeff1 = this->layer_param_.convolution_param().coeff(1);
    }

    primitive_attr attr;
    if (this->need_quantize_) {
      int mask = 0;
      int count = 1; //single channel
      if(this->fl_params_.size() > 1 || this->scale_params_.size() > 1){
          int oc_dim_id = 1;
          mask = 1 << oc_dim_id;
          count = oc;  //multi channel
      }
      std::vector<float> scales(count);
      float scale;
      #ifdef _OPENMP
      #pragma omp parallel for if (count > 1)
      #endif
      for(int i=0; i<count; i++){
        if (this->scale_params_[i] == 0.0)
            scale = this->scale_out_[0] * coeff1;
        else
            scale = this->scale_out_[0] / (this->scale_in_[0] * this->scale_params_[i]) * coeff1;
        scales[i] = scale;
      }
      attr.set_output_scales(mask, scales);
      attr.set_int_output_round_mode(round_nearest);
    }

    // ---- Determining engine to use -----------------------
    std::string subengines = this->layer_param_.engine();
    if (subengines.find("MKLDNN") == std::string::npos || subengines == "MKLDNN")
      subengines = "MKLDNN:CPU";
    EngineParser ep(subengines);
    unsigned subEngineIndex = 0;
    mkldnn::algorithm eligibleAlgorithms[2] = {conv_algorithm, algorithm::convolution_direct};
    convFwd_pd = NULL;
    mkldnn::post_ops ops;
    float scale = 1.0f;
    Dtype alpha = negative_slope;  // negative slope for mkldnn_eltwise_relu.
    float beta = 1.0f;             // ignored for mkldnn_eltwise_relu.

    if (this->layer_param_.convolution_param().fusion_type() ==
            ConvolutionParameter::SUM_FUSION &&
        bottom.size() > 1) {
      if (this->need_quantize_) {
        float sum_scale;
        sum_scale =
            this->scale_out_[0] /
            get_mkldnn_prv_descriptor<Dtype, false>(bottom[1])->get_scale(0) * coeff0;
        ops.append_sum(sum_scale);
      } else {
        ops.append_sum(1.0f);
      }
    }

    if (relu) ops.append_eltwise(scale, eltwise_relu, alpha, beta);
    attr.set_post_ops(ops);

    for (auto& convAlgorithm : eligibleAlgorithms) {
      // ---- Initialize convolution primitive descriptor -------------
      shared_ptr<convolution_forward::desc> convFwd_desc;
      if (this->bias_term_) {
          if (dilated_conv)
              convFwd_desc.reset(new convolution_forward::desc(
                  propagation, convAlgorithm, init_bottom_md, init_weights_md,
                  init_bias_md, init_top_md, convolutionStrides, dilation, padding, padding_r,
                  padding_kind::zero));
          else
              convFwd_desc.reset(new convolution_forward::desc(
                  propagation, convAlgorithm, init_bottom_md, init_weights_md,
                  init_bias_md, init_top_md, convolutionStrides, padding, padding,
                  padding_kind::zero));
      } else {
          if (dilated_conv)
              convFwd_desc.reset(new convolution_forward::desc(
                  propagation, convAlgorithm, init_bottom_md, init_weights_md,
                  init_top_md, convolutionStrides, dilation, padding, padding_r,
                  padding_kind::zero));
          else
              convFwd_desc.reset(new convolution_forward::desc(
                  propagation, convAlgorithm, init_bottom_md, init_weights_md,
                  init_top_md, convolutionStrides, padding, padding,
                  padding_kind::zero));
      }

      for (subEngineIndex = 0; subEngineIndex < ep.getNumberOfSubEngines();
           subEngineIndex++) {
        try {
          if (this->need_quantize_ || relu ||
              (this->layer_param_.convolution_param().fusion_type() ==
                   ConvolutionParameter::SUM_FUSION &&
               bottom.size() > 1)) {
            convFwd_pd.reset(new convolution_forward::primitive_desc(
                *convFwd_desc, attr, ep.getMKLDNNSubEngine(subEngineIndex)));
          } else {
            convFwd_pd.reset(new convolution_forward::primitive_desc(
                *convFwd_desc, ep.getMKLDNNSubEngine(subEngineIndex)));
          }

        } catch (...) {
            continue;
        }

        break;
      }
      if (convFwd_pd) break;
    }

    CHECK(convFwd_pd);
    engine cpu_engine = CpuEngine::Instance().get_engine();

    // ---- Create priv memory primitive descriptors stored as class members -------------
    typedef typename memory::primitive_desc MemPD; // short name for memory::primitive_desc

    shared_ptr<MemPD> prv_fwd_bottom_data_memory_pd(new MemPD(convFwd_pd->src_primitive_desc()));
    shared_ptr<MemPD> prv_fwd_top_data_memory_pd(new MemPD(convFwd_pd->dst_primitive_desc()));
    shared_ptr<MemPD> prv_fwd_weights_data_memory_pd(new MemPD(convFwd_pd->weights_primitive_desc()));

    // ---- Log prv memory primitive descriptors -------------
    info_mem_pd<Dtype>(prv_fwd_bottom_data_memory_pd, "conv_src:" + this->layer_param_.name());
    info_mem_pd<Dtype>(prv_fwd_top_data_memory_pd, "conv_dst:" + this->layer_param_.name());

    // ---- Create usr memory primitive descriptors -------------
    memory::format data_mfmt;
    memory::format weights_mfmt;
    if (this->num_spatial_axes_ == 2) {
      data_mfmt = memory::format::nchw;
      weights_mfmt = (g!= 1) ? memory::format::goihw : memory::format::oihw;
    } else {
      data_mfmt = memory::format::ncdhw;
      weights_mfmt = (g!= 1) ? memory::format::goidhw : memory::format::oidhw;
    }

    // TODO: There should not be a problem to use this for Backward as well

    shared_ptr<MemPD> usr_bottom_data_memory_pd(new MemPD({{bottom_tz}, mpcsn, data_mfmt}, cpu_engine));
    shared_ptr<MemPD> usr_bias_data_memory_pd(new MemPD({{bias_tz}, mpcsn, memory::format::x}, cpu_engine));
    shared_ptr<MemPD> usr_top_data_memory_pd(new MemPD({{top_tz}, mpcsn, data_mfmt}, cpu_engine));
    shared_ptr<MemPD> usr_weights_data_memory_pd(new MemPD({{weights_tz}, mpcsn, weights_mfmt}, cpu_engine));

    // ---  init primitive and prv_memory descriptors ----------------------
    if (this->need_quantize_){
      std::vector<float> scale_bottom(1, this->layer_param_.quantization_param().force_u8_input()? 1.0f : this->scale_in_[0] );
      fwd_bottom_data.reset(new MKLDNNData<Dtype>(usr_bottom_data_memory_pd, prv_fwd_bottom_data_memory_pd, bottom[0], this, scale_bottom));
    } else {
      fwd_bottom_data.reset(new MKLDNNData<Dtype>(usr_bottom_data_memory_pd, prv_fwd_bottom_data_memory_pd, bottom[0], this));
    }
    fwd_bottom_data->name = "fwd_bottom_data   @ " + this->layer_param_.name();
    fwd_bottom_data_primitive = fwd_bottom_data->create_input(false);

    if (this->need_quantize_){
      std::vector<float> scale_top(1, this->scale_out_[0]);
      fwd_top_data.reset(new MKLDNNData<Dtype>(usr_top_data_memory_pd, prv_fwd_top_data_memory_pd, top[0], this, scale_top, 0, is_sum));
    } else{
      fwd_top_data.reset(new MKLDNNData<Dtype>(usr_top_data_memory_pd, prv_fwd_top_data_memory_pd, top[0], this));
    }
    fwd_top_data->name = "fwd_top_data      @ " + this->layer_param_.name();
    fwd_top_data_memory = fwd_top_data->create_output_memory();

    bool is_wino = (prv_fwd_weights_data_memory_pd->desc().data.format == memory::format::wino_fmt);
  #ifdef _OPENMP
    int node = caffe::cpu::OpenMpManager::getNumaNode();
  #else
    int node = 0;
  #endif
    if (fwd_weights_data == NULL) {
      std::string name = "numa" + std::to_string(node) + "@fwd_weights_data@" + this->layer_param_.name();
      if (this->need_quantize_){
        int count = 1; //single channel
        int reorder_mask = 0;
        if(this->scale_params_.size() > 1){
            count = oc;  //multi channel
            reorder_mask = (g!= 1) ? (1<<1)+(1<<0) : 1<<0;
        }
        std::vector<float> scale_weight(count);
        #ifdef _OPENMP
        #pragma omp parallel for if (count > 1)
        #endif
        for(int i=0; i<count; i++){
          scale_weight[i] = this->scale_params_[i];
        }
        fwd_weights_data.reset(new MKLDNNData<Dtype>(usr_weights_data_memory_pd, prv_fwd_weights_data_memory_pd, this->blobs_[0].get(), this, scale_weight, reorder_mask, false, false, true, name));
      } else{
        fwd_weights_data.reset(new MKLDNNData<Dtype>(usr_weights_data_memory_pd, prv_fwd_weights_data_memory_pd, this->blobs_[0].get(), this, {1.}, 0,  is_sum, is_wino, false, name));
      }
      fwd_weights_data->name.assign(name);
      fwd_weights_data_primitive = fwd_weights_data->create_input(true);
    }

    if (this->bias_term_) {
        if (fwd_bias_data == NULL) {
          shared_ptr<MemPD> prv_fwd_bias_data_memory_pd(new MemPD(convFwd_pd->bias_primitive_desc()));
          std::string name = "numa" + std::to_string(node) + "@fwd_bias_data@" + this->layer_param_.name();
          if (this->need_quantize_){
          int count = 1;  //single channel
          int reorder_mask = 0;
            if(this->scale_params_.size() > 1){
                count = oc;  //multi channel
                reorder_mask = 1<<0;
            }
            std::vector<float> scale_bias(count);
            #ifdef _OPENMP
            #pragma omp parallel for if (count > 1)
            #endif
            for(int i=0; i<count; i++){
              if (this->scale_params_[i] == 0.0)
                  scale_bias[i] = 1.0;
              else
                  scale_bias[i] = this->scale_in_[0] * this->scale_params_[i];
            }
            fwd_bias_data.reset(new MKLDNNData<Dtype>(usr_bias_data_memory_pd, prv_fwd_bias_data_memory_pd, this->blobs_[1].get(), this, scale_bias, reorder_mask, false, false, false, name));
          } else{
            fwd_bias_data.reset(new MKLDNNData<Dtype>(usr_bias_data_memory_pd, prv_fwd_bias_data_memory_pd, this->blobs_[1].get(), this, {1.}, 0,  false, false, false, name));
          }
          fwd_bias_data->name.assign(name);
          fwd_bias_data_primitive = fwd_bias_data->create_input(true);
        }
        convFwd.reset(new convolution_forward(*convFwd_pd
                        , *fwd_bottom_data_primitive, *fwd_weights_data_primitive
                        , *fwd_bias_data_primitive, *fwd_top_data_memory));
        //fwd_bias_data->set_mkldnn_primitive(convFwd);   //Wrong passed primitive! (For sure!)
        MKLDNNPrimitive<Dtype> fwd_bias_data_primitive_transfer(fwd_bias_data_primitive);
        fwd_bias_data->set_mkldnn_primitive(fwd_bias_data_primitive_transfer);
    } else {
        convFwd.reset(new convolution_forward(*convFwd_pd
                        , *fwd_bottom_data_primitive, *fwd_weights_data_primitive
                        , *fwd_top_data_memory));
    }
    //fwd_bottom_data->set_mkldnn_primitive(convFwd);   //Wrong passed primitive! (For sure!)
    MKLDNNPrimitive<Dtype> fwd_bottom_data_primitive_transfer(fwd_bottom_data_primitive);
    fwd_bottom_data->set_mkldnn_primitive(fwd_bottom_data_primitive_transfer);

    //fwd_top_data->set_mkldnn_primitive(convFwd);      //Wrong passed primitive! (TODO: Checking!)
    MKLDNNPrimitive<Dtype> fwd_top_data_memory_transfer(fwd_top_data_memory);
    fwd_top_data->set_mkldnn_primitive(fwd_top_data_memory_transfer);

    //fwd_weights_data->set_mkldnn_primitive(convFwd);  //Wrong passed primitive! (For sure!)
    MKLDNNPrimitive<Dtype> fwd_weights_data_primitive_transfer(fwd_weights_data_primitive);
    fwd_weights_data->set_mkldnn_primitive(fwd_weights_data_primitive_transfer);
    // Names are for debugging purposes only.

}

template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom
                                                , const vector<Blob<Dtype>*>& top)
{
    VLOG(1) << "MKLDNNConvolutionLayer<Dtype>::Forward_cpu: " << this->layer_param_.name();

    //Modify by --donghn--
    bool analysis_mode=true;
    int w_data_type=0;
    int act_data_type=0;
    //End --donghn

    bool _mkldnn_primitive = false;
    if( convFwd_pd == NULL || this->reshape){
        InitConvolutionFwd(bottom, top);
	if(getenv("CAFFE_INFERENCE_MEM_OPT")){
            fwd_weights_data->sync_before_read();
            if (this->bias_term_)
            fwd_bias_data->sync_before_read();
            _mkldnn_primitive = true;
        }
    }
    fwd_bottom_data->sync_before_read();
    if(!getenv("CAFFE_INFERENCE_MEM_OPT")){
        fwd_weights_data->sync_before_read();
        if (this->bias_term_)
            fwd_bias_data->sync_before_read();
    }
    // update top that head at prv
    fwd_top_data->sync_before_write();

    //Modify by -donghn-
    // WEIGHT ERROR INJECTION
    w_data_type = fwd_weights_data->get_prv_memory()->get_primitive_desc().desc().data.data_type;
    if(!fwd_weights_data->is_error() && w_data_type==5 && analysis_mode){
        shared_ptr<memory> w_prv_mem = fwd_weights_data->get_prv_memory();
        int data_size = w_prv_mem->get_primitive_desc().get_size();
        uint8_t *data_int = static_cast<uint8_t *>(w_prv_mem->get_data_handle());
        //Flipping bit
        if(this->flip_ > 0){
            for(int w=0; w<data_size; w++){
                if((data_int[w]&0x80)!=0) data_int[w] = data_int[w]^0x7f; //flip 7th to 1st except sign ^0111.1111
            }
        }

        //xor array to flipping
        uint8_t only_1s[8] = {1, 2, 4, 8, 16, 32, 64, 128};
        //error parameter and random error rate
        int b0_count[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	int b1_count[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	for(int w=0; w<data_size; w++){
                uint8_t wt = data_int[w];
                for(int b=0; b<8; b++){
                    uint8_t and_value = wt&only_1s[b];
                    if(and_value==0) {
                        b0_count[b]+=1;
                    }
                    else {
                        b1_count[b]+=1;
                    }
                }
	    //LOG(INFO) << "MSB count :" << msb_count<< "P: " << (float)msb_count/data_size << "%";
        }
        //Flipping again after error injection
        if(this->flip_>0){
            for(int w=0; w<data_size; w++){
                if((data_int[w]&0x80)!=0) data_int[w] = data_int[w]^0x7f; //flip 7th to 1st
            }
        }
	
	std::cout<< "\nWeights: " <<this->layer_param_.name() <<", ";
	for(int i=0; i<8; i++){
		std::cout << b0_count[i] << ", ";
	}
	for(int i=0; i<8; i++){
		std::cout << b1_count[i] << ", ";
	}

        fwd_weights_data->set_is_error(true);
    }
    // End modify --donghn--

    PERFORMANCE_EVENT_ID_INIT(perf_id_fw_, PERFORMANCE_MKLDNN_NAME("FW"));
    PERFORMANCE_MEASUREMENT_BEGIN();
    convFwd.submit();

    //Modify by --donghn--
    // ACTIVATION ERROR INJECTION (TOP-Activation, BOTTOM-Input)
    act_data_type = fwd_top_data->get_prv_memory()->get_primitive_desc().desc().data.data_type;
    if(act_data_type==6 && analysis_mode){
        shared_ptr<memory> act_prv_mem = fwd_top_data->get_prv_memory();
        int data_size = act_prv_mem->get_primitive_desc().get_size();
        uint8_t *data_int = static_cast<uint8_t *>(act_prv_mem->get_data_handle());
        //xor array to flipping
        uint8_t only_1s[8] = {1, 2, 4, 8, 16, 32, 64, 128};
        int b0_count[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	int b1_count[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	for(int w=0; w<data_size; w++){
                uint8_t wt = data_int[w];
                for(int b=0; b<8; b++){
                    uint8_t and_value = wt&only_1s[b];
                    if(and_value==0) {
                        b0_count[b]+=1;
                    }
                    else {
                        b1_count[b]+=1;
                    }
                }
	    //LOG(INFO) << "MSB count :" << msb_count<< "P: " << (float)msb_count/data_size << "%";
        }
        //LOG(INFO) <<"Activation Analysis Done !";
	//std::cout<< "\nActivation: " <<this->layer_param_.name() <<",";
	//for(int i=0; i<8; i++){
	//	std::cout << b0_count[i] << ", ";
	//}
	//for(int i=0; i<8; i++){
	//	std::cout << b1_count[i] << ", ";
	//}
    }
    //End modify --donghn--

    if(_mkldnn_primitive) {
      CircleBuf::Instance()->DecRefCnt(bottom[0]->prv_data());
    }
    PERFORMANCE_MEASUREMENT_END_ID(perf_id_fw_);
}


template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::InitConvolutionBwd(const vector<Blob<Dtype>*>& top
                                                    , const vector<bool>& propagate_down
                                                    , const vector<Blob<Dtype>*>& bottom)
{
    if (std::is_same<Dtype, double>::value)   NOT_IMPLEMENTED;

    int32_t g  = std::max(this->group_, 1);
    int32_t n  = this->num_;
    int32_t iw = this->width_;
    int32_t ih = this->height_;
    int32_t ic = this->channels_;
    int32_t id = this->depth_;

    int32_t ow = this->width_out_;
    int32_t oh = this->height_out_;
    int32_t od = this->depth_out_;
    int32_t oc = this->num_output_;

    int32_t kw = this->kernel_w_;
    int32_t kh = this->kernel_h_;
    int32_t kd = this->kernel_d_;

    int32_t sw = this->stride_w_;
    int32_t sh = this->stride_h_;
    int32_t sd = this->stride_d_;

    int32_t pw = this->pad_w_;
    int32_t ph = this->pad_h_;
    int32_t pd = this->pad_d_;

    memory::dims convolutionStrides;
    memory::dims padding;
    memory::dims padding_r;
    memory::dims dilation;
    bool dilated_conv = false;
    const int* dilation_data = this->dilation_.cpu_data();
    for (int i = 0; i < this->num_spatial_axes_; ++i) {
      dilation.push_back(dilation_data[i] - 1);
      if (dilation_data[i] != 1) dilated_conv = true;
    }
    if (this->num_spatial_axes_ == 2) {
      convolutionStrides = {sh, sw};
      padding = {ph, pw};
      padding_r.push_back((oh - 1) * sh - ih + ((kh - 1) * (dilation_data[0]) + 1) - ph);
      padding_r.push_back((ow - 1) * sw - iw + ((kw - 1) * (dilation_data[1]) + 1) - pw);
    } else {
      convolutionStrides = {sd, sh, sw};
      padding = {pd, ph, pw};
      padding_r.push_back((od - 1) * sd - id + ((kd - 1) * (dilation_data[0]) + 1) - pd);
      padding_r.push_back((oh - 1) * sh - ih + ((kh - 1) * (dilation_data[1]) + 1) - ph);
      padding_r.push_back((ow - 1) * sw - iw + ((kw - 1) * (dilation_data[2]) + 1) - pw);
    }

    // ---- Initialize memory descriptors (fromat = any) to create convolution descriptor -------------
    memory::data_type mpcsn = memory::data_type::f32;
    memory::format mfmt_any = memory::format::any;

    memory::dims bottom_tz;
    memory::dims bias_tz;
    memory::dims top_tz;
    memory::dims weights_tz;
    if (this->num_spatial_axes_ == 2) {
      bottom_tz = {n, ic, ih, iw};
      bias_tz = {oc};
      top_tz = {n, oc, oh, ow};
      weights_tz = ( g!= 1) ? memory::dims{g, oc/g, ic/g, kh, kw} : memory::dims{oc, ic, kh, kw};
    } else {
      bottom_tz = {n, ic, id, ih, iw};
      bias_tz = {oc};
      top_tz = {n, oc, od, oh, ow};
      weights_tz = ( g!= 1) ? memory::dims{g, oc/g, ic/g, kd, kh, kw} : memory::dims{oc, ic, kd, kh, kw};
    }

    // ---- Memory descriptors for initializing of convolution primitive descriptor -------------
    memory::desc init_bottom_md({bottom_tz}, mpcsn, mfmt_any);
    memory::desc init_bias_md({bias_tz}, mpcsn, mfmt_any);
    memory::desc init_top_md({top_tz}, mpcsn, mfmt_any);
    memory::desc init_weights_md({weights_tz}, mpcsn, mfmt_any);

    // ---- Determining engine to use -----------------------
    std::string subengines = this->layer_param_.engine();
    if (subengines.find("MKLDNN") == std::string::npos || subengines == "MKLDNN")
      subengines = "MKLDNN:CPU";
    EngineParser ep(subengines);
    unsigned subEngineIndex = 0;

    auto eligibleAlgorithms = {conv_algorithm, algorithm::convolution_direct};
    convBwdData_pd = NULL;
    convBwdWeights_pd = NULL;
    for (auto &convAlgorithm : eligibleAlgorithms) {
        // ---- Initialize convolution primitive descriptor -------------
        shared_ptr<convolution_backward_data::desc> convBwdData_desc;
        shared_ptr<convolution_backward_weights::desc> convBwdWeights_desc;
        if (this->bias_term_) {
            if (dilated_conv)
                convBwdWeights_desc.reset(new convolution_backward_weights::desc(convAlgorithm
                            , init_bottom_md, init_weights_md, init_bias_md, init_top_md
                            , convolutionStrides, dilation, padding, padding_r, padding_kind::zero));
            else
                convBwdWeights_desc.reset(new convolution_backward_weights::desc(convAlgorithm
                            , init_bottom_md, init_weights_md, init_bias_md, init_top_md
                            , convolutionStrides, padding, padding, padding_kind::zero));
        } else {
            if (dilated_conv)
                convBwdWeights_desc.reset(new convolution_backward_weights::desc(convAlgorithm
                            , init_bottom_md, init_weights_md, init_top_md
                            , convolutionStrides, dilation, padding, padding_r, padding_kind::zero));
            else
                convBwdWeights_desc.reset(new convolution_backward_weights::desc(convAlgorithm
                            , init_bottom_md, init_weights_md, init_top_md
                            , convolutionStrides, padding, padding, padding_kind::zero));
        }

        if (dilated_conv)
            convBwdData_desc.reset(new convolution_backward_data::desc(convAlgorithm
                            , init_bottom_md, init_weights_md, init_top_md
                            , convolutionStrides, dilation, padding, padding_r, padding_kind::zero));
        else
            convBwdData_desc.reset(new convolution_backward_data::desc(convAlgorithm
                            , init_bottom_md, init_weights_md, init_top_md
                            , convolutionStrides, padding, padding, padding_kind::zero));

        for(subEngineIndex=0; subEngineIndex < ep.getNumberOfSubEngines(); subEngineIndex++) {
            try {
                convBwdData_pd.reset(new convolution_backward_data::primitive_desc(*convBwdData_desc,
                                          ep.getMKLDNNSubEngine(subEngineIndex), *convFwd_pd));

                convBwdWeights_pd.reset(new convolution_backward_weights::primitive_desc(*convBwdWeights_desc,
                                          ep.getMKLDNNSubEngine(subEngineIndex), *convFwd_pd));
            }
            catch(...) {
                continue;
            }
            break;
        }
        if (convBwdData_pd && convBwdWeights_pd)
            break;
    }

    CHECK(convBwdData_pd);
    CHECK(convBwdWeights_pd);
    engine cpu_engine = CpuEngine::Instance().get_engine();

    // ---- Create priv memory primitive descriptors stored as class members -------------
    typedef typename memory::primitive_desc MemPD; // short name for memory::primitive_desc

    shared_ptr<MemPD> prv_bwdd_bottom_diff_memory_pd(new MemPD(convBwdData_pd->diff_src_primitive_desc()));
    shared_ptr<MemPD> prv_bwdd_top_diff_memory_pd(new MemPD(convBwdData_pd->diff_dst_primitive_desc()));
    shared_ptr<MemPD> prv_bwdd_weights_data_memory_pd(new MemPD(convBwdData_pd->weights_primitive_desc()));

    shared_ptr<MemPD> prv_bwdw_bottom_data_memory_pd(new MemPD(convBwdWeights_pd->src_primitive_desc()));
    shared_ptr<MemPD> prv_bwdw_top_diff_memory_pd(new MemPD(convBwdWeights_pd->diff_dst_primitive_desc()));
    shared_ptr<MemPD> prv_bwdw_weights_diff_memory_pd(new MemPD(convBwdWeights_pd->diff_weights_primitive_desc()));

    // ---- Create usr memory primitive descriptors -------------
    memory::format data_mfmt;
    memory::format weights_mfmt;
    if (this->num_spatial_axes_ == 2) {
      data_mfmt = memory::format::nchw;
      weights_mfmt = ( g!= 1) ? memory::format::goihw : memory::format::oihw;
    } else {
      data_mfmt = memory::format::ncdhw;
      weights_mfmt = ( g!= 1) ? memory::format::goidhw : memory::format::oidhw;
    }
    // ???!!! can we use usr memory primitive descrittors for backward??
    shared_ptr<MemPD> usr_bottom_data_memory_pd(new MemPD({{bottom_tz}, mpcsn, data_mfmt}, cpu_engine));
    shared_ptr<MemPD> usr_bias_data_memory_pd(new MemPD({{bias_tz}, mpcsn, memory::format::x}, cpu_engine));
    shared_ptr<MemPD> usr_top_data_memory_pd(new MemPD({{top_tz}, mpcsn, data_mfmt}, cpu_engine));
    shared_ptr<MemPD> usr_weights_data_memory_pd(new MemPD({{weights_tz}, mpcsn, weights_mfmt}, cpu_engine));


    // ---  init primitive and prv_memory descriptors ----------------------
    bwdd_bottom_diff.reset(new MKLDNNDiff<Dtype>(usr_bottom_data_memory_pd, prv_bwdd_bottom_diff_memory_pd, bottom[0], this));
    bwdd_bottom_diff ->name = "bwdd_bottom_diff   @ " + this->layer_param_.name();
    bwdd_bottom_diff_memory = bwdd_bottom_diff->create_output_memory();
    bwdw_bottom_data.reset(new MKLDNNData<Dtype>(usr_bottom_data_memory_pd, prv_bwdw_bottom_data_memory_pd, bottom[0], this));
    bwdw_bottom_data ->name = "bwdw_bottom_data   @ " + this->layer_param_.name();
    bwdw_bottom_data_primitive = bwdw_bottom_data->create_input(false);

    bwdd_top_diff.reset(new MKLDNNDiff<Dtype>(usr_top_data_memory_pd, prv_bwdd_top_diff_memory_pd, top[0], this));
    bwdd_top_diff    ->name = "bwdd_top_diff      @ " + this->layer_param_.name();
    bwdd_top_diff_primitive = bwdd_top_diff->create_input(false);
    bwdw_top_diff.reset(new MKLDNNDiff<Dtype>(usr_top_data_memory_pd, prv_bwdw_top_diff_memory_pd, top[0], this));
    bwdw_top_diff    ->name = "bwdw_top_diff      @ " + this->layer_param_.name();
    bwdw_top_diff_primitive = bwdw_top_diff->create_input(false);

    bwdd_weights_data.reset(new MKLDNNData<Dtype>(usr_weights_data_memory_pd, prv_bwdd_weights_data_memory_pd, this->blobs_[0].get(), this));
    bwdd_weights_data->name = "bwdd_weights_data  @ " + this->layer_param_.name();
    bwdd_weights_data_primitive = bwdd_weights_data->create_input(false);
    bwdw_weights_diff.reset(new MKLDNNDiff<Dtype>(usr_weights_data_memory_pd, prv_bwdw_weights_diff_memory_pd, this->blobs_[0].get(), this));
    bwdw_weights_diff->name = "bwdw_weights_diff  @ " + this->layer_param_.name();
    bwdw_weights_diff_memory = bwdw_weights_diff->create_output_memory();

    if (Caffe::iter_size() > 1) {
      // support for (iter_size > 1) weights diff requires additional buffer
      shared_ptr<MemPD> prv_bwdw_weights_diff_memory_iter_pd(new MemPD(convBwdWeights_pd->diff_weights_primitive_desc()));
      bwdw_weights_diff_iter.reset(new MKLDNNDiff<Dtype>(usr_weights_data_memory_pd, prv_bwdw_weights_diff_memory_iter_pd, bwdw_weights_diff_iter_blob.get(), this));
      bwdw_weights_diff_memory_iter = bwdw_weights_diff_iter->create_output_memory();
    }

    if (this->bias_term_) {
        shared_ptr<MemPD> prv_bwdw_bias_diff_memory_pd(new MemPD(convBwdWeights_pd->diff_bias_primitive_desc()));
        bwdw_bias_diff.reset(new MKLDNNDiff<Dtype>(usr_bias_data_memory_pd, prv_bwdw_bias_diff_memory_pd, this->blobs_[1].get(), this));
        bwdw_bias_diff->name = "bwdw_bias_diff     @ " + this->layer_param_.name();
        bwdw_bias_diff_memory = bwdw_bias_diff->create_output_memory();

        if (Caffe::iter_size() > 1) {
          // support for (iter_size > 1) bias diff requires additional buffer
          shared_ptr<MemPD> prv_bwdw_bias_diff_memory_iter_pd(new MemPD(convBwdWeights_pd->diff_bias_primitive_desc()));
          bwdw_bias_diff_iter.reset(new MKLDNNDiff<Dtype>(usr_bias_data_memory_pd, prv_bwdw_bias_diff_memory_iter_pd, bwdw_bias_diff_iter_blob.get(), this));
          bwdw_bias_diff_memory_iter = bwdw_bias_diff_iter->create_output_memory();
          convBwdWeights.reset(new convolution_backward_weights(*convBwdWeights_pd
                        , *bwdw_bottom_data_primitive, *bwdw_top_diff_primitive
                        , *bwdw_weights_diff_memory_iter, *bwdw_bias_diff_memory_iter));
        } else {
          convBwdWeights.reset(new convolution_backward_weights(*convBwdWeights_pd
                        , *bwdw_bottom_data_primitive, *bwdw_top_diff_primitive
                        , *bwdw_weights_diff_memory, *bwdw_bias_diff_memory));
        }

        //bwdw_bias_diff->set_mkldnn_primitive(convBwdWeights);   //Wrong passed primitive! (For sure!)
        MKLDNNPrimitive<Dtype> bwdw_bias_diff_memory_transfer(bwdw_bias_diff_memory);
        bwdw_bias_diff->set_mkldnn_primitive(bwdw_bias_diff_memory_transfer);

        if (Caffe::iter_size() > 1) {
          // support for (iter_size > 1) bias diff requires additional buffer
          MKLDNNPrimitive<Dtype> bwdw_bias_diff_memory_iter_transfer(bwdw_bias_diff_memory_iter);
          bwdw_bias_diff_iter->set_mkldnn_primitive(bwdw_bias_diff_memory_iter_transfer);
        }
    } else {
        if (Caffe::iter_size() > 1) {
          // if (iter_size > 1) then weights diff should be accumulated across iterations
          convBwdWeights.reset(new convolution_backward_weights(*convBwdWeights_pd
                        , *bwdw_bottom_data_primitive, *bwdw_top_diff_primitive
                        , *bwdw_weights_diff_memory_iter));
        } else {
          convBwdWeights.reset(new convolution_backward_weights(*convBwdWeights_pd
                        , *bwdw_bottom_data_primitive, *bwdw_top_diff_primitive
                        , *bwdw_weights_diff_memory));
        }
    }

    convBwdData.reset(new convolution_backward_data(*convBwdData_pd
                    , *bwdd_top_diff_primitive, *bwdd_weights_data_primitive
                    , *bwdd_bottom_diff_memory));

    //bwdd_bottom_diff->set_mkldnn_primitive(convBwdData);      //Wrong passed primitive! (TODO: Checking!)
    MKLDNNPrimitive<Dtype> bwdd_bottom_diff_memory_transfer(bwdd_bottom_diff_memory);
    bwdd_bottom_diff->set_mkldnn_primitive(bwdd_bottom_diff_memory_transfer);

    //bwdd_top_diff->set_mkldnn_primitive(convBwdData);         //Wrong passed primitive! (TODO: Checking!)
    MKLDNNPrimitive<Dtype> bwdd_top_diff_primitive_transfer(bwdd_top_diff_primitive);
    bwdd_top_diff->set_mkldnn_primitive(bwdd_top_diff_primitive_transfer);

    //bwdd_weights_data->set_mkldnn_primitive(convBwdData);     //Wrong passed primitive! (For sure!)
    MKLDNNPrimitive<Dtype> bwdd_weights_data_primitive_transfer(bwdd_weights_data_primitive);
    bwdd_weights_data->set_mkldnn_primitive(bwdd_weights_data_primitive_transfer);

    //bwdw_bottom_data->set_mkldnn_primitive(convBwdWeights);   //Wrong passed primitive! (TODO: Checking!)
    MKLDNNPrimitive<Dtype> bwdw_bottom_data_primitive_transfer(bwdw_bottom_data_primitive);
    bwdw_bottom_data->set_mkldnn_primitive(bwdw_bottom_data_primitive_transfer);

    //bwdw_top_diff->set_mkldnn_primitive(convBwdWeights);      //Wrong passed primitive! (For sure!)
    MKLDNNPrimitive<Dtype> bwdw_top_diff_primitive_transfer(bwdw_top_diff_primitive);
    bwdw_top_diff->set_mkldnn_primitive(bwdw_top_diff_primitive_transfer);

    //bwdw_weights_diff->set_mkldnn_primitive(convBwdWeights);  //Wrong passed primitive! (TODO: Checking!)
    MKLDNNPrimitive<Dtype> bwdw_weights_diff_memory_transfer(bwdw_weights_diff_memory);
    bwdw_weights_diff->set_mkldnn_primitive(bwdw_weights_diff_memory_transfer);

    if (Caffe::iter_size() > 1) {
      // support for (iter_size > 1) weights diff requires additional buffer
      MKLDNNPrimitive<Dtype> bwdw_weights_diff_memory_iter_transfer(bwdw_weights_diff_memory_iter);
      bwdw_weights_diff_iter->set_mkldnn_primitive(bwdw_weights_diff_memory_iter_transfer);
    }

    // Names are for debugging purposes only.
}


template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top
                                                , const vector<bool>& propagate_down
                                                , const vector<Blob<Dtype>*>& bottom)
{
    VLOG(1) << "MKLDNNConvolutionLayer<Dtype>::Backward_cpu: " << this->layer_param_.name();
    bool top_diff_is_prv = (const_cast<Dtype*>(top[0]->prv_diff()) != NULL);

    if( convBwdData_pd == NULL || this->reshape)
        InitConvolutionBwd(top, propagate_down, bottom);
    if (propagate_down[0]) {
        // making reorders if needed.
        bwdd_top_diff->sync_before_read();
        bwdd_weights_data->sync_before_read();
        bwdd_bottom_diff->sync_before_write();

        PERFORMANCE_EVENT_ID_INIT(perf_id_bw_, PERFORMANCE_MKLDNN_NAME("BW"));
        PERFORMANCE_MEASUREMENT_BEGIN();
#ifdef DEBUG
        if (bottom[0]->prv_data() != NULL)
        {
            LOG(INFO) << "Debug: Bottom prv data: " << *bottom[0]->prv_data();
        }
        else
        {
            LOG(INFO) << "Debug: Bottom prv data is NULL!";
            //LOG(INFO) << "Debug: Bottom cpu data: " << *bottom[0]->cpu_data();
        }

        if (top[0]->prv_diff() != NULL)
        {
            LOG(INFO) << "Debug: Top prv diff: " << *top[0]->prv_diff();
        }
        else
        {
            LOG(INFO) << "Debug: Top prv diff is NULL!";
            LOG(INFO) << "Debug: Top cpu diff: " << *top[0]->cpu_diff();
        }

        if (this->blobs_[0]->prv_data() != NULL)
        {
            LOG(INFO) << "Debug: Weights prv data from blobs_[0]: " << *this->blobs_[0]->prv_data();
        }
        else
        {
            LOG(INFO) << "Debug: Weights prv data is NULL!";
            LOG(INFO) << "Debug: Weights cpu data: " << *this->blobs_[0]->cpu_data();
        }
        //Before submit, so get_prv_ptr() always has the value
        LOG(INFO) << "Debug: Weights prv data from get_prv_ptr: " << *bwdd_weights_data->get_prv_ptr();
#endif
        convBwdData.submit();
#ifdef DEBUG
        if (bottom[0]->prv_diff() != NULL)
        {
            LOG(INFO) << "Debug: Bottom prv diff: " << *bottom[0]->prv_diff();
        }
        else
        {
            LOG(INFO) << "Debug: Bottom prv diff is NULL!";
            LOG(INFO) << "Debug: Bottom cpu diff: " << *bottom[0]->cpu_diff();
        }
#endif
        PERFORMANCE_MEASUREMENT_END_ID(perf_id_bw_);
    }
    if (this->param_propagate_down(0)) {
        // We have to sync top diff to cpu explicitly. This is used to make
        // bwdw_top_diff->sync_before_read() have chance to get coverted data as
        // bwdd_top_diff->sync_before_read() have updated top diff's prv_data
        // to self. This issue only happens when MKLDNN conv layer is followed
        // by a CAFFE layer and conversion is needed.
        if (!top_diff_is_prv && propagate_down[0])
          top[0]->mutable_cpu_diff();
        // making reorders if needed.
        bwdw_top_diff->sync_before_read();
        bwdw_bottom_data->sync_before_read();
        // update top that head at prv
        bwdw_weights_diff->sync_before_write();
        if (this->param_propagate_down(1)) {
            CHECK(bwdw_bias_diff);
            bwdw_bias_diff->sync_before_write();
        }
        PERFORMANCE_EVENT_ID_INIT(perf_id_bw_weights_,
          PERFORMANCE_MKLDNN_NAME_DETAILED("BW", "_weights"));
        PERFORMANCE_MEASUREMENT_BEGIN();
        convBwdWeights.submit();
        PERFORMANCE_MEASUREMENT_END_ID(perf_id_bw_weights_);

        if (Caffe::iter_size() > 1) {
          // if (iter_size > 1) then weights diff should be accumulated across iterations
          if (this->blobs_[0]->prv_diff() != NULL) {
            caffe_axpy(this->blobs_[0]->prv_diff_count(), Dtype(1),
              (Dtype*)(bwdw_weights_diff_memory_iter->get_data_handle()),
              this->blobs_[0]->mutable_prv_diff());
          } else {
            caffe_axpy(this->blobs_[0]->count(), Dtype(1),
              (Dtype*)(bwdw_weights_diff_memory_iter->get_data_handle()),
              this->blobs_[0]->mutable_cpu_diff());
          }
        }

        if (this->param_propagate_down(1)) {
          if (Caffe::iter_size() > 1) {
            // if (iter_size > 1) then bias diff should be accumulated across iterations
            if (this->blobs_[1]->prv_diff() != NULL) {
              caffe_axpy(this->blobs_[1]->prv_diff_count(), Dtype(1),
                (Dtype*)(bwdw_bias_diff_memory_iter->get_data_handle()),
                this->blobs_[1]->mutable_prv_diff());
            } else {
              caffe_axpy(this->blobs_[1]->count(), Dtype(1),
                (Dtype*)(bwdw_bias_diff_memory_iter->get_data_handle()),
                this->blobs_[1]->mutable_cpu_diff());
            }
          }
        }
    }
}

#ifdef CPU_ONLY
STUB_GPU(MKLDNNConvolutionLayer);
#else

template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom
                                                , const vector<Blob<Dtype>*>& top)
{
    NOT_IMPLEMENTED;
}

template <typename Dtype>
void MKLDNNConvolutionLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top
                                                , const vector<bool>& propagate_down
                                                , const vector<Blob<Dtype>*>& bottom)
{
    NOT_IMPLEMENTED;
}
#endif

INSTANTIATE_CLASS(MKLDNNConvolutionLayer);

}  // namespace caffe
#endif  // #ifdef MKLDNN_SUPPORTED
