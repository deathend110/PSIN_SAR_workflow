#pragma once

#include "pipeline/base/enums.hpp"

#include "modelzoo_utils.hpp"
#include "et_device.hpp"

// icraft includes
#include <icraft-xrt/core/tensor.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/buyi_device.h>
// 3rd parties
#include <opencv2/opencv.hpp>

#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <mutex>

struct SiamFCppConfig
{
	std::string name = "siamfc++";
	float WINDOW_INFLUENCE = 0.21;
	int EXEMPLAR_SIZE = 127;
	int SEARCH_SIZE = 303;
	float CONTEXT_AMOUNT = 0.5;
	int SCORE_SIZE = 17;
	std::string TEMPLATE_JSON_PATH;
	std::string TEMPLATE_RAW_PATH;
	std::string SEARCH_JSON_PATH;
	std::string SEARCH_RAW_PATH;
};

struct TargetCXYWH
{
	float cx;
	float cy;
	float w;
	float h;
};

enum class TrackerState
{
	NONE,
	TEMPLATE,
	SEARCH
};

class SiamFCppTracker
{
public:
	SiamFCppTracker(int width, float height, float context_amount, float window_influence, int z_size, int x_size,
					int frame_w, int frame_h)
		: width_(width), height_(height),
		  context_amount_(context_amount),
		  x_size_(x_size), z_size_(z_size),
		  window_influence_(window_influence),
		  state_(TrackerState::NONE),
		  FRAME_W_(frame_w),
		  FRAME_H_(frame_h)
	{
		// 创建汉宁窗
		this->creatHannWindow(width, height);
		// 初始化跟踪目标
		init_target_ = {0.0f, 0.0f, 0.0f, 0.0f};
		target_ = {0.0f, 0.0f, 0.0f, 0.0f};
		siam_scale_ = 1.0f;
	}
	~SiamFCppTracker() = default;
	// Explicitly delete copy operations because this class is non-copyable.
	SiamFCppTracker(const SiamFCppTracker &) = delete;
	SiamFCppTracker &operator=(const SiamFCppTracker &) = delete;

	// You can keep or define move operations if needed
	SiamFCppTracker(SiamFCppTracker &&) = default;
	SiamFCppTracker &operator=(SiamFCppTracker &&) = default;

	TargetCXYWH get_target() const { return target_; };
	TargetCXYWH get_init_target() const { return init_target_; };
	void set_target(const TargetCXYWH &target) { target_ = target; };
	TrackerState get_state() const { return state_; };
	void reset()
	{
		state_ = TrackerState::NONE;
		// init_target_ = {0.0f, 0.0f, 0.0f, 0.0f};
		// target_ = {0.0f, 0.0f, 0.0f, 0.0f};
		siam_scale_ = 1.0f;
		spdlog::warn("[SiamFC++] Tracker reset to NONE state.");
	}

	void fpga_warpaffine(icraft::xrt::Device &device)
	{
		std::lock_guard<std::mutex> lock(warpaffine_mutex_);
		spdlog::debug("[SiamFC++] fpga_warpaffine: M = [{:.6f}, {:.6f}, {:.6f}; {:.6f}, {:.6f}, {:.6f}]",
					  siam_inv_matrix_[0][0], siam_inv_matrix_[0][1], siam_inv_matrix_[0][2],
					  siam_inv_matrix_[1][0], siam_inv_matrix_[1][1], siam_inv_matrix_[1][2]);
		fpgaWarpaffine(siam_inv_matrix_, device);
	}

	bool clamp_crop(std::vector<float> crop_cxywh)
	{
		float x0 = crop_cxywh[0] - (crop_cxywh[2] - 1) / 2;
		float y0 = crop_cxywh[1] - (crop_cxywh[3] - 1) / 2;
		float x2 = crop_cxywh[0] + (crop_cxywh[2] - 1) / 2;
		float y2 = crop_cxywh[1] + (crop_cxywh[3] - 1) / 2;

		if (x0 < 0 || y0 < 0 || x2 >= FRAME_W_ || y2 >= FRAME_H_)
		{
			spdlog::error("[SiamFC++] clamp_crop: WARNING! crop out of frame boundary, x0:{:.2f}, y0:{:.2f}, x2:{:.2f}, y2:{:.2f}", x0, y0, x2, y2);
			return false;
		}
		return true;
	}

	// siamfc++模型前处理
	void siamfc_template_preprocess(const TargetCXYWH &target)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		init_target_ = target;
		float wc = target.w + context_amount_ * (target.w + target.h);
		float hc = target.h + context_amount_ * (target.w + target.h);
		float s_crop = sqrt(wc * hc);
		spdlog::debug("[SiamFC++] siamfc_template_preprocess: target(cx,cy,w,h): [{:.2f}, {:.2f}, {:.2f}, {:.2f}]", target.cx, target.cy, target.w, target.h);
		std::vector<float> crop_cxywh = {target.cx, target.cy, round(s_crop), round(s_crop)};
		spdlog::debug("[SiamFC++] siamfc_template_preprocess: crop_cxywh: [{:.2f}, {:.2f}, {:.2f}, {:.2f}]", crop_cxywh[0], crop_cxywh[1], crop_cxywh[2], crop_cxywh[3]);
		if (!clamp_crop(crop_cxywh))
		{
			this->reset();
			return;
		}
		float crop_xyxy_0 = crop_cxywh[0] - (crop_cxywh[2] - 1) / 2;
		float crop_xyxy_1 = crop_cxywh[1] - (crop_cxywh[3] - 1) / 2;
		float crop_xyxy_2 = crop_cxywh[0] + (crop_cxywh[2] - 1) / 2;
		float crop_xyxy_3 = crop_cxywh[1] + (crop_cxywh[3] - 1) / 2;

		float M_11 = (crop_xyxy_2 - crop_xyxy_0) / (z_size_ - 1);
		float M_22 = (crop_xyxy_3 - crop_xyxy_1) / (z_size_ - 1);
		// update
		siam_inv_matrix_[0][0] = M_11;
		siam_inv_matrix_[0][2] = crop_xyxy_0;
		siam_inv_matrix_[1][1] = M_22;
		siam_inv_matrix_[1][2] = crop_xyxy_1;
		state_ = TrackerState::TEMPLATE; // 设置为已初始化状态，接下要需要执行template网络
	}

	void siamfc_search_preprocess()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		TargetCXYWH target;
		if (state_ == TrackerState::SEARCH)
		{
			target = target_.load();
		}
		else
		{
			target = init_target_.load();
		}

		float wc = target.w + context_amount_ * (target.w + target.h);
		float hc = target.h + context_amount_ * (target.w + target.h);
		float s_crop = sqrt(wc * hc);
		siam_scale_ = z_size_ / s_crop;
		s_crop = x_size_ / siam_scale_;

		spdlog::debug("[SiamFC++] siamfc_search_preprocess: target(cx,cy,w,h): [{:.2f}, {:.2f}, {:.2f}, {:.2f}]", target.cx, target.cy, target.w, target.h);
		std::vector<float> crop_cxywh = {target.cx, target.cy, round(s_crop), round(s_crop)};
		spdlog::debug("[SiamFC++] siamfc_search_preprocess: crop_cxywh: [{:.2f}, {:.2f}, {:.2f}, {:.2f}]", crop_cxywh[0], crop_cxywh[1], crop_cxywh[2], crop_cxywh[3]);

		float crop_xyxy_0 = crop_cxywh[0] - (crop_cxywh[2] - 1) / 2;
		float crop_xyxy_1 = crop_cxywh[1] - (crop_cxywh[3] - 1) / 2;
		float crop_xyxy_2 = crop_cxywh[0] + (crop_cxywh[2] - 1) / 2;
		float crop_xyxy_3 = crop_cxywh[1] + (crop_cxywh[3] - 1) / 2;

		float M_11 = (crop_xyxy_2 - crop_xyxy_0) / (x_size_ - 1);
		float M_22 = (crop_xyxy_3 - crop_xyxy_1) / (x_size_ - 1);
		// update
		siam_inv_matrix_[0][0] = M_11;
		siam_inv_matrix_[0][2] = crop_xyxy_0;
		siam_inv_matrix_[1][1] = M_22;
		siam_inv_matrix_[1][2] = crop_xyxy_1;
		this->set_target(target);
		state_ = TrackerState::SEARCH; // 设置为搜索状态，接下要需要执行search网络
	}

	// siamfc++_net2模型后处理，去除cast算子
	void net2_postprocess(const std::vector<icraft::xrt::Tensor> &output_tensors, std::vector<float> net2_output_normratio, int im_w, int im_h)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto target = target_.load();

		// 去除cast算子，手动将输出的tensor 从 pl_ddr 搬移到 ps_ddr
		auto host_tensor_0 = output_tensors[0].to(icraft::xrt::HostDevice::MemRegion());
		auto net2_data_ptr_0 = (int8_t *)host_tensor_0.data().cptr();

		auto host_tensor_1 = output_tensors[1].to(icraft::xrt::HostDevice::MemRegion());
		auto net2_data_ptr_1 = (int8_t *)host_tensor_1.data().cptr();

		auto host_tensor_2 = output_tensors[2].to(icraft::xrt::HostDevice::MemRegion());
		auto net2_data_ptr_2 = (int8_t *)host_tensor_2.data().cptr();

		cv::Mat net_result_mat = cv::Mat(289, 4, CV_8S, net2_data_ptr_2);
		net_result_mat.convertTo(net_result_mat, CV_32F);
		net_result_mat = net_result_mat * net2_output_normratio[2]; // 反量化

		net_result_mat = (net_result_mat * 0.78319091 + 1.60675728);
		cv::exp(net_result_mat, net_result_mat);
		net_result_mat = net_result_mat * 8;

		cv::Mat mat_1 = net_result_mat(cv::Rect(0, 0, 2, net_result_mat.rows));
		cv::Mat mat_2 = net_result_mat(cv::Rect(2, 0, 2, net_result_mat.rows));
		cv::Mat mat_3 = xy_ctr_ - mat_1;
		cv::Mat mat_4 = xy_ctr_ + mat_2;

		cv::Mat box;
		cv::hconcat(mat_3, mat_4, box);

		cv::Mat cls_score = cv::Mat(289, 1, CV_8S, net2_data_ptr_0);
		cls_score.convertTo(cls_score, CV_32F);
		cls_score = cls_score * net2_output_normratio[0]; // 反量化

		cv::Mat ctr_score = cv::Mat(289, 1, CV_8S, net2_data_ptr_1);
		ctr_score.convertTo(ctr_score, CV_32F);
		ctr_score = ctr_score * net2_output_normratio[1]; // 反量化

		for (int i = 0; i < cls_score.rows; i++)
		{
			for (int j = 0; j < cls_score.cols; j++)
			{
				float value = cls_score.at<float>(i, j);
				value = 1.0 / (1.0 + exp(-value));
				cls_score.at<float>(i, j) = value;
			}
		}

		for (int i = 0; i < ctr_score.rows; i++)
		{
			for (int j = 0; j < ctr_score.cols; j++)
			{
				float value = ctr_score.at<float>(i, j);
				value = 1.0 / (1.0 + exp(-value));
				ctr_score.at<float>(i, j) = value;
			}
		}
		// score = cls*ctr
		cv::Mat score;
		cv::multiply(cls_score, ctr_score, score);

		// box：xyxy2cxywh
		for (int i = 0; i < box.rows; i++)
		{
			float value_0 = box.at<float>(i, 0);
			float value_1 = box.at<float>(i, 1);
			float value_2 = box.at<float>(i, 2);
			float value_3 = box.at<float>(i, 3);
			box.at<float>(i, 0) = (value_0 + value_2) / 2;
			box.at<float>(i, 1) = (value_1 + value_3) / 2;
			box.at<float>(i, 2) = value_2 - value_0 + 1;
			box.at<float>(i, 3) = value_3 - value_1 + 1;
		}

		// score post-processing
		float penalty_k = 0.08;
		std::vector<float> target_sz_in_crop = {target.w * siam_scale_, target.h * siam_scale_};

		// box_wh[:, 2] box_wh[:, 3]
		cv::Mat box_wh_2 = box(cv::Rect(2, 0, 1, box.rows));
		cv::Mat box_wh_3 = box(cv::Rect(3, 0, 1, box.rows));

		// sz
		cv::Mat pad_1 = (box_wh_2 + box_wh_3) * 0.5;
		cv::Mat sz2_1;
		cv::multiply((box_wh_2 + pad_1), (box_wh_3 + pad_1), sz2_1);
		cv::sqrt(sz2_1, sz2_1);

		// sz_wh
		float pad_2 = (target_sz_in_crop[0] + target_sz_in_crop[1]) * 0.5;
		float sz2_2 = (target_sz_in_crop[0] + pad_2) * ((target_sz_in_crop[1] + pad_2));
		sz2_2 = sqrt(sz2_2);
		sz2_1 = sz2_1 / sz2_2;

		// change
		for (int i = 0; i < sz2_1.rows; i++)
		{
			for (int j = 0; j < sz2_1.cols; j++)
			{
				float value = sz2_1.at<float>(i, j);
				sz2_1.at<float>(i, j) = value > (1 / value) ? value : (1 / value);
			}
		}

		float data_1 = target_sz_in_crop[0] / target_sz_in_crop[1];
		cv::Mat r_c = data_1 / (box_wh_2 / box_wh_3);

		// change
		for (int i = 0; i < r_c.rows; i++)
		{
			for (int j = 0; j < r_c.cols; j++)
			{
				float value = r_c.at<float>(i, j);
				r_c.at<float>(i, j) = value > (1 / value) ? value : (1 / value);
			}
		}

		cv::Mat penalty;
		cv::multiply(r_c, sz2_1, penalty);
		penalty = (penalty - 1) * (-penalty_k);
		cv::exp(penalty, penalty);

		// pscore = penalty * score
		cv::Mat pscore;
		cv::multiply(penalty, score, pscore);

		pscore = pscore * (1 - window_influence_) + window_ * window_influence_;
		cv::Point maxLoc;
		cv::minMaxLoc(pscore, NULL, NULL, NULL, &maxLoc);

		int best_pscore_id = maxLoc.y;

		// box post-processing
		cv::Mat pred_in_crop = box(cv::Rect(0, best_pscore_id, 4, 1)) / siam_scale_;
		float test_lr = 0.58;
		float lr = penalty.at<float>(best_pscore_id, 0) * score.at<float>(best_pscore_id, 0) * test_lr;
		// float test = pred_in_crop.at<float>(0, 0)- (int(x_size) / 2) / scale;
		// std::cout<<"test"<<test<<std::endl;
		float res_x = pred_in_crop.at<float>(0, 0) + target.cx - (int(x_size_) / 2) / siam_scale_;
		float res_y = pred_in_crop.at<float>(0, 1) + target.cy - (int(x_size_) / 2) / siam_scale_;
		float res_w = target.w * (1 - lr) + pred_in_crop.at<float>(0, 2) * lr;
		float res_h = target.h * (1 - lr) + pred_in_crop.at<float>(0, 3) * lr;

		// restrict new_target_pos & new_target_sz
		// int x = i > j ? i : j; // max(i,j)
		// int y = i < j ? i : j; // min(i,j)
		TargetCXYWH new_target;
		float min_1 = im_w < res_x ? im_w : res_x;
		new_target.cx = 0 > min_1 ? 0 : min_1;
		float min_2 = im_h < res_y ? im_h : res_y;
		new_target.cy = 0 > min_2 ? 0 : min_2;
		float min_3 = im_w < res_w ? im_w : res_w;
		new_target.w = 10 > min_3 ? 10 : min_3;
		float min_4 = im_h < res_h ? im_h : res_h;
		new_target.h = 10 > min_4 ? 10 : min_4;

		// update target
		target_.store(new_target);
	}

	// siamfc++_net2模型后处理,使用removeoutputcast接口去除cast&pruneaxis,需手动搬数&反量化
	void net2_postprocess_removeoutputcast(const std::vector<icraft::xrt::Tensor> &output_tensors, const std::vector<float> net2_output_normratio, const int im_w, const int im_h)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto target = target_.load();
		spdlog::debug("[SiamFC++] net2_postprocess_removeoutputcast: target: [{:.2f}, {:.2f}, {:.2f}, {:.2f}]", target.cx, target.cy, target.w, target.h);
		// 去除cast算子，手动将输出的tensor 从 pl_ddr 搬移到 ps_ddr
		auto host_tensor_0 = output_tensors[0].to(icraft::xrt::HostDevice::MemRegion());
		auto net2_data_ptr_0 = (int8_t *)host_tensor_0.data().cptr();

		auto host_tensor_1 = output_tensors[1].to(icraft::xrt::HostDevice::MemRegion());
		auto net2_data_ptr_1 = (int8_t *)host_tensor_1.data().cptr();

		auto host_tensor_2 = output_tensors[2].to(icraft::xrt::HostDevice::MemRegion());
		auto net2_data_ptr_2 = (int8_t *)host_tensor_2.data().cptr();

		cv::Mat net_result_mat = cv::Mat(289, 4, CV_8S, net2_data_ptr_0);
		net_result_mat.convertTo(net_result_mat, CV_32F);
		net_result_mat = net_result_mat * net2_output_normratio[0]; // 反量化

		net_result_mat = (net_result_mat * 0.78319091 + 1.60675728);
		cv::exp(net_result_mat, net_result_mat);
		net_result_mat = net_result_mat * 8;

		cv::Mat mat_1 = net_result_mat(cv::Rect(0, 0, 2, net_result_mat.rows));
		cv::Mat mat_2 = net_result_mat(cv::Rect(2, 0, 2, net_result_mat.rows));
		cv::Mat mat_3 = xy_ctr_ - mat_1;
		cv::Mat mat_4 = xy_ctr_ + mat_2;

		cv::Mat box;
		cv::hconcat(mat_3, mat_4, box);

		cv::Mat cls_score = cv::Mat(289, 1, CV_8S, net2_data_ptr_1);
		cls_score.convertTo(cls_score, CV_32F);
		cls_score = cls_score * net2_output_normratio[1]; // 反量化

		cv::Mat ctr_score = cv::Mat(289, 1, CV_8S, net2_data_ptr_2);
		ctr_score.convertTo(ctr_score, CV_32F);
		ctr_score = ctr_score * net2_output_normratio[2]; // 反量化

		for (int i = 0; i < cls_score.rows; i++)
		{
			for (int j = 0; j < cls_score.cols; j++)
			{
				float value = cls_score.at<float>(i, j);
				value = 1.0 / (1.0 + exp(-value));
				cls_score.at<float>(i, j) = value;
			}
		}

		for (int i = 0; i < ctr_score.rows; i++)
		{
			for (int j = 0; j < ctr_score.cols; j++)
			{
				float value = ctr_score.at<float>(i, j);
				value = 1.0 / (1.0 + exp(-value));
				ctr_score.at<float>(i, j) = value;
			}
		}
		// score = cls*ctr
		cv::Mat score;
		cv::multiply(cls_score, ctr_score, score);

		// box：xyxy2cxywh
		for (int i = 0; i < box.rows; i++)
		{
			float value_0 = box.at<float>(i, 0);
			float value_1 = box.at<float>(i, 1);
			float value_2 = box.at<float>(i, 2);
			float value_3 = box.at<float>(i, 3);
			box.at<float>(i, 0) = (value_0 + value_2) / 2;
			box.at<float>(i, 1) = (value_1 + value_3) / 2;
			box.at<float>(i, 2) = value_2 - value_0 + 1;
			box.at<float>(i, 3) = value_3 - value_1 + 1;
		}

		// score post-processing
		float penalty_k = 0.08;
		std::vector<float> target_sz_in_crop = {target.w * siam_scale_, target.h * siam_scale_};

		// box_wh[:, 2] box_wh[:, 3]
		cv::Mat box_wh_2 = box(cv::Rect(2, 0, 1, box.rows));
		cv::Mat box_wh_3 = box(cv::Rect(3, 0, 1, box.rows));

		// sz
		cv::Mat pad_1 = (box_wh_2 + box_wh_3) * 0.5;
		cv::Mat sz2_1;
		cv::multiply((box_wh_2 + pad_1), (box_wh_3 + pad_1), sz2_1);
		cv::sqrt(sz2_1, sz2_1);

		// sz_wh
		float pad_2 = (target_sz_in_crop[0] + target_sz_in_crop[1]) * 0.5;
		float sz2_2 = (target_sz_in_crop[0] + pad_2) * ((target_sz_in_crop[1] + pad_2));
		sz2_2 = sqrt(sz2_2);
		sz2_1 = sz2_1 / sz2_2;

		// change
		for (int i = 0; i < sz2_1.rows; i++)
		{
			for (int j = 0; j < sz2_1.cols; j++)
			{
				float value = sz2_1.at<float>(i, j);
				sz2_1.at<float>(i, j) = value > (1 / value) ? value : (1 / value);
			}
		}

		float data_1 = target_sz_in_crop[0] / target_sz_in_crop[1];
		cv::Mat r_c = data_1 / (box_wh_2 / box_wh_3);

		// change
		for (int i = 0; i < r_c.rows; i++)
		{
			for (int j = 0; j < r_c.cols; j++)
			{
				float value = r_c.at<float>(i, j);
				r_c.at<float>(i, j) = value > (1 / value) ? value : (1 / value);
			}
		}

		cv::Mat penalty;
		cv::multiply(r_c, sz2_1, penalty);
		penalty = (penalty - 1) * (-penalty_k);
		cv::exp(penalty, penalty);

		// pscore = penalty * score
		cv::Mat pscore;
		cv::multiply(penalty, score, pscore);

		pscore = pscore * (1 - window_influence_) + window_ * window_influence_;
		cv::Point maxLoc;
		cv::minMaxLoc(pscore, NULL, NULL, NULL, &maxLoc);

		int best_pscore_id = maxLoc.y;

		// box post-processing
		cv::Mat pred_in_crop = box(cv::Rect(0, best_pscore_id, 4, 1)) / siam_scale_;
		spdlog::debug("pred_in_crop: [{:.2f}, {:.2f}, {:.2f}, {:.2f}]", pred_in_crop.at<float>(0, 0), pred_in_crop.at<float>(0, 1), pred_in_crop.at<float>(0, 2), pred_in_crop.at<float>(0, 3));
		float test_lr = 0.58;
		float lr = penalty.at<float>(best_pscore_id, 0) * score.at<float>(best_pscore_id, 0) * test_lr;
		// float test = pred_in_crop.at<float>(0, 0)- (int(x_size) / 2) / scale;
		// std::cout<<"test"<<test<<std::endl;
		float res_x = pred_in_crop.at<float>(0, 0) + target.cx - (float(x_size_) / 2.0f) / siam_scale_;
		float res_y = pred_in_crop.at<float>(0, 1) + target.cy - (float(x_size_) / 2.0f) / siam_scale_;
		float res_w = target.w * (1 - lr) + pred_in_crop.at<float>(0, 2) * lr;
		float res_h = target.h * (1 - lr) + pred_in_crop.at<float>(0, 3) * lr;

		// restrict new_target_pos & new_target_sz
		// int x = i > j ? i : j; // max(i,j)
		// int y = i < j ? i : j; // min(i,j)

		float min_1 = im_w < res_x ? im_w : res_x;
		target.cx = 0 > min_1 ? 0 : min_1;
		float min_2 = im_h < res_y ? im_h : res_y;
		target.cy = 0 > min_2 ? 0 : min_2;
		float min_3 = im_w < res_w ? im_w : res_w;
		target.w = 10 > min_3 ? 10 : min_3;
		float min_4 = im_h < res_h ? im_h : res_h;
		target.h = 10 > min_4 ? 10 : min_4;
		spdlog::debug("[SiamFC++] net2_postprocess_removeoutputcast: new target: [{:.2f}, {:.2f}, {:.2f}, {:.2f}]", target.cx, target.cy, target.w, target.h);
		// update target
		target_.store(target);
	}

	void creatHannWindow(int width, int height)
	{
		cv::Mat vertical(height, 1, CV_32FC1);
		cv::Mat horizontal(1, width, CV_32FC1);
		for (int r = 0; r < height; r++)
		{
			vertical.at<float>(r, 0) = 0.5 - 0.5 * cos(2 * PI * r / (height - 1));
		}
		for (int c = 0; c < width; c++)
		{
			horizontal.at<float>(0, c) = 0.5 - 0.5 * cos(2 * PI * c / (width - 1));
		}
		hanning_ = vertical * horizontal;
		window_ = cv::Mat(289, 1, CV_32F, hanning_.data);
		xy_ctr_ = cv::Mat(289, 2, CV_32F);
		for (int i = 0; i < 17; i++)
		{
			for (int j = 0; j < 17; j++)
			{
				xy_ctr_.at<float>(17 * i + j, 0) = 87 + 8 * j;
				xy_ctr_.at<float>(17 * i + j, 1) = 87 + 8 * i;
			}
		}
	}

private:
	cv::Mat hanning_;
	cv::Mat window_;
	cv::Mat xy_ctr_;
	std::atomic<TargetCXYWH> init_target_;
	std::atomic<TargetCXYWH> target_;
	std::mutex mutex_;
	std::mutex warpaffine_mutex_;
	std::atomic<float> siam_scale_;
	std::vector<std::vector<float>> siam_inv_matrix_ = { // 仿射变换逆矩阵
		{1.0f, 0.0f, 0.0f},
		{0.0f, 1.0f, 0.0f}};
	TrackerState state_;
	// constant variables
	const int width_;
	const int height_;
	const float context_amount_;
	const float window_influence_;
	const int x_size_;
	const int z_size_;
	const int FRAME_W_;
	const int FRAME_H_;
};
