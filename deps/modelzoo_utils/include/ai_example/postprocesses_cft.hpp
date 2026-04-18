#pragma once
#include "modelzoo_utils.hpp"
#include "constants.hpp"
#include "pipeline/base/enums.hpp"
// icraft includes
#include <icraft-xrt/core/tensor.h>
#include <icraft-xrt/dev/host_device.h>
// 3rd parties
#include <opencv2/opencv.hpp>
// #include <onnxruntime_cxx_api.h>
// #include <onnxruntime_c_api.h>

#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <mutex>

std::vector<float> STRIDE = {8, 16, 32};
std::vector<std::vector<std::vector<float>>> ANCHORS = {{{10, 13}, {16, 30}, {33, 23}},
														{{30, 61}, {62, 45}, {59, 119}},
														{{116, 90}, {156, 198}, {373, 326}}};

struct Grid
{
	uint16_t location_x = 0;
	uint16_t location_y = 0;
	uint16_t anchor_index = 0;
};

template <typename T>
Grid get_grid(int bits, T *tensor_data, int base_addr, int anchor_length)
{
	Grid grid;
	uint16_t anchor_index;
	uint16_t location_y;
	uint16_t location_x;
	if (bits == 8)
	{
		anchor_index = (((uint16_t)tensor_data[base_addr + anchor_length - 1]) << 8) + (uint8_t)tensor_data[base_addr + anchor_length - 2];
		location_y = (((uint16_t)tensor_data[base_addr + anchor_length - 3]) << 8) + (uint8_t)tensor_data[base_addr + anchor_length - 4];
		location_x = (((uint16_t)tensor_data[base_addr + anchor_length - 5]) << 8) + (uint8_t)tensor_data[base_addr + anchor_length - 6];
	}
	else if (bits == 16)
	{
		anchor_index = (uint16_t)tensor_data[base_addr + anchor_length - 1];
		location_y = (uint16_t)tensor_data[base_addr + anchor_length - 2];
		location_x = (uint16_t)tensor_data[base_addr + anchor_length - 3];
	}
	grid.location_x = location_x;
	grid.location_y = location_y;
	grid.anchor_index = anchor_index;
	return grid;
}

std::vector<float> get_stride_yolov5(NetInfo &netinfo)
{
	std::vector<float> stride;
	for (auto i : netinfo.o_cubic)
	{
		stride.emplace_back(netinfo.head_hardop_i_shape_cubic[0].h / i.h);
		// std::cout << "stride:" << std::endl;
		// std::cout << netinfo.head_hardop_i_shape_cubic[0].h / i.h << std::endl;
	}
	return stride;
};

template <typename T>
void get_cls_bbox_yolov5(std::vector<int> &id_list, std::vector<float> &score_list, std::vector<cv::Rect2f> &box_list, T *tensor_data, int base_addr,
						 Grid &grid, float &SCALE, int stride,
						 std::vector<float> anchor, int N_CLASS, float THR_F, bool MULTILABEL)
{
	if (!MULTILABEL)
	{
		auto _score_ = sigmoid(tensor_data[base_addr + 4] * SCALE);
		auto class_ptr_start = tensor_data + base_addr + 5;
		auto max_prob_ptr = std::max_element(class_ptr_start, class_ptr_start + N_CLASS);
		int max_index = std::distance(class_ptr_start, max_prob_ptr);
		auto _prob_ = sigmoid(*max_prob_ptr * SCALE);
		auto realscore = _score_ * _prob_;
		if (realscore > THR_F)
		{
			std::vector<float> xywh = sigmoid(tensor_data, SCALE, base_addr, 4);

			xywh[0] = (2 * xywh[0] + grid.location_x - 0.5) * stride;
			xywh[1] = (2 * xywh[1] + grid.location_y - 0.5) * stride;

			xywh[2] = 4 * powf(xywh[2], 2) * anchor[0];
			xywh[3] = 4 * powf(xywh[3], 2) * anchor[1];
			id_list.emplace_back(max_index);
			score_list.emplace_back(realscore);
			box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),
											 (xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
		}
	}
	else
	{
		for (size_t cls_idx = N_CLASS - 1; cls_idx < N_CLASS; cls_idx++)
		{
			// auto realscore = this->getRealScore(tensor_data, obj_ptr_start, norm, i);

			auto _score_ = sigmoid(tensor_data[base_addr + 4] * SCALE);
			auto _prob_ = sigmoid(tensor_data[base_addr + 5 + cls_idx] * SCALE);
			auto realscore = _score_ * _prob_;
			if (realscore > THR_F)
			{
				// auto bbox = this->getBbox(tensor_data, norm, obj_ptr_start, location_x, location_y, stride, anchor);
				std::vector<float> xywh = sigmoid(tensor_data, SCALE, base_addr, 4);

				xywh[0] = (2 * xywh[0] + grid.location_x - 0.5) * stride;
				xywh[1] = (2 * xywh[1] + grid.location_y - 0.5) * stride;

				xywh[2] = 4 * powf(xywh[2], 2) * anchor[0];
				xywh[3] = 4 * powf(xywh[3], 2) * anchor[1];

				id_list.emplace_back(cls_idx);
				score_list.emplace_back(realscore);
				box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),
												 (xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
			}
		}
	}
}

std::tuple<bool, cv::Rect> yolov5_post_detpost_plin(const std::vector<icraft::xrt::Tensor> &output_tensors, NetInfo &netinfo,
													float conf, float iou_thresh, bool MULTILABEL, int N_CLASS, icraft::xrt::Device &device)
{

	std::vector<int> id_list;
	std::vector<float> score_list;
	std::vector<cv::Rect2f> box_list;
	std::vector<float> stride = get_stride_yolov5(netinfo);
	for (size_t i = 0; i < output_tensors.size(); i++)
	{

		auto host_tensor = output_tensors[i].to(icraft::xrt::HostDevice::MemRegion());
		int output_tensors_bits = output_tensors[i].dtype()->element_dtype.getStorageType().bits();
		int obj_num = output_tensors[i].dtype()->shape[2];
		int anchor_length = output_tensors[i].dtype()->shape[3];
		if (output_tensors_bits == 16)
		{
			auto tensor_data = (int16_t *)host_tensor.data().cptr();
			for (size_t obj = 0; obj < obj_num; obj++)
			{
				int base_addr = obj * anchor_length;
				Grid grid = get_grid(output_tensors_bits, tensor_data, base_addr, anchor_length);
				get_cls_bbox_yolov5(id_list, score_list, box_list, tensor_data, base_addr, grid, netinfo.o_scale[i], stride[i], ANCHORS[i][grid.anchor_index], N_CLASS, conf, MULTILABEL);
			}
		}
		else
		{
			auto tensor_data = (int8_t *)host_tensor.data().cptr();
			for (size_t obj = 0; obj < obj_num; obj++)
			{
				int base_addr = obj * anchor_length;
				Grid grid = get_grid(output_tensors_bits, tensor_data, base_addr, anchor_length);
				get_cls_bbox_yolov5(id_list, score_list, box_list, tensor_data, base_addr, grid, netinfo.o_scale[i], stride[i], ANCHORS[i][grid.anchor_index], N_CLASS, conf, MULTILABEL);
			}
		}
	}
	std::vector<std::tuple<int, float, cv::Rect2f>> nms_res = nms_soft(id_list, score_list, box_list, iou_thresh); // 后处理 之 NMS
	std::vector<int> id_list_ret;
	std::vector<float> score_list_ret;
	std::vector<cv::Rect2f> box_list_ret;
	for (auto idx_score_bbox : nms_res)
	{
		// store
		id_list_ret.emplace_back(std::get<0>(idx_score_bbox));
		score_list_ret.emplace_back(std::get<1>(idx_score_bbox));
		box_list_ret.emplace_back(std::get<2>(idx_score_bbox));
	}
	// std::cout<<id_list_ret.size()<<std::endl;

	// 选择score最大的目标作为追踪目标
	int max_score = 0;
	int biggest_index = -1;
	for (int index : id_list_ret)
	{
		float score = score_list[index];
		if (score > max_score)
		{
			max_score = score;
			biggest_index = index;
		}
	}
	// 选择区域最大的目标作为追踪目标
	// float max_area = FLT_MIN;
	//  for (int index : id_list_ret){
	//  	float w = box_list[index].width;
	//  	float h = box_list[index].height;
	//  	if ((w * h) > max_area) {
	//  		max_area = w * h;
	//  		biggest_index = index;
	//  	}
	//  }

	return biggest_index == -1 ? std::tuple<bool, cv::Rect>{false, cv::Rect(0, 0, 0, 0)} : std::tuple<bool, cv::Rect>{true, box_list[biggest_index]};
}

// 对icorepost得到的检测结果进行nms后处理
std::vector<int> nms(std::vector<cv::Rect> &box_list, std::vector<float> &score_list, const float &conf, const float &iou)
{

	std::vector<int> nms_indices;
	std::vector<std::pair<float, int>> score_index_vec;

	for (size_t i = 0; i < score_list.size(); ++i)
	{
		if (score_list[i] > conf)
		{
			score_index_vec.emplace_back(std::make_pair(score_list[i], i));
		}
	}

	std::stable_sort(score_index_vec.begin(), score_index_vec.end(),
					 [](const std::pair<float, int> &pair1, const std::pair<float, int> &pair2)
					 { return pair1.first > pair2.first; });

	for (size_t i = 0; i < score_index_vec.size(); ++i)
	{
		const int idx = score_index_vec[i].second;
		bool keep = true;
		for (int k = 0; k < nms_indices.size() && keep; ++k)
		{
			if (1.f - jaccardDistance(box_list[idx], box_list[nms_indices[k]]) > iou)
			{
				keep = false;
			}
		}
		if (keep == true)
			nms_indices.emplace_back(idx);
	}

	return nms_indices;
}

std::vector<int> nms_inside(std::vector<cv::Rect> &box_list, std::vector<float> &score_list, std::vector<int> &id_list, const float &conf, const float &iou, const int &NOC)
{

	std::vector<int> nms_indices;
	for (int class_id = 0; class_id < NOC; class_id++)
	{
		std::vector<std::pair<float, int>> score_index_vec;

		for (size_t i = 0; i < score_list.size(); ++i)
		{
			if (score_list[i] > conf && id_list[i] == class_id)
			{
				score_index_vec.emplace_back(std::make_pair(score_list[i], i));
			}
		}

		std::stable_sort(score_index_vec.begin(), score_index_vec.end(),
						 [](const std::pair<float, int> &pair1, const std::pair<float, int> &pair2)
						 { return pair1.first > pair2.first; });

		for (size_t i = 0; i < score_index_vec.size(); ++i)
		{
			const int idx = score_index_vec[i].second;
			bool keep = true;
			for (size_t k = 0; k < nms_indices.size() && keep; ++k)
			{
				if (1.f - jaccardDistance(box_list[idx], box_list[nms_indices[k]]) > iou)
				{
					keep = false;
				}
			}
			if (keep == true)
				nms_indices.emplace_back(idx);
		}
	}
	return nms_indices;
}

// edgesam模型前处理
void edgesam_preprocess(const std::vector<float> &crop_cxywh, const int crop_sz, std::vector<std::vector<float>> &M_inversed)
{
	float crop_xyxy_0 = crop_cxywh[0] - crop_cxywh[2] / 2; // tl_x
	float crop_xyxy_1 = crop_cxywh[1] - crop_cxywh[3] / 2; // tl_y
	float crop_xyxy_2 = crop_cxywh[0] + crop_cxywh[2] / 2; // br_x
	float crop_xyxy_3 = crop_cxywh[1] + crop_cxywh[3] / 2; // br_y

	float M_11 = (crop_xyxy_2 - crop_xyxy_0) / crop_sz;
	float M_22 = (crop_xyxy_3 - crop_xyxy_1) / crop_sz;
	// update
	M_inversed[0][0] = M_11;
	M_inversed[0][2] = crop_xyxy_0; // x trans
	M_inversed[1][1] = M_22;
	M_inversed[1][2] = crop_xyxy_1; // y trans
									// std::cout << "M_inversed[0][0]: " << M_inversed[0][0] << std::endl;
									// std::cout << "M_inversed[0][2]: " << M_inversed[0][2] << std::endl;
									// std::cout << "M_inversed[1][1]: " << M_inversed[1][1] << std::endl;
									// std::cout << "M_inversed[1][2]: " << M_inversed[1][2] << std::endl;
}


std::string intToString(int v)
{
	char buf[32] = {0};
	snprintf(buf, sizeof(buf), "%u", v);

	std::string str = buf;
	return str;
}

std::vector<float> getOutputsNormratio(icraft::xir::NetworkView network)
{

	auto iore_post_results = network.outputs();
	std::vector<float> ret;
	ret.reserve(iore_post_results.size());
	for (auto &&value : iore_post_results)
	{
		auto b = value->dtype.getNormratio().value();
		ret.emplace_back(b[0]);
	}
	return ret;
}

// ================================================================ //
// ========================  Bytrack后处理  ======================== //
// ================================================================ //

inline std::vector<float> get_stride(NetInfo &netinfo)
{
	std::vector<float> stride;
	for (auto i : netinfo.o_cubic)
	{
		stride.emplace_back(netinfo.i_cubic[0].h / i.h);
	}
	return stride;
};

// 根据每个head数，将原本1维的norm分组
inline std::vector<std::vector<float>> set_norm_by_head(int NOH, int parts, std::vector<float> &normalratio)
{
	std::vector<std::vector<float>> _norm;
	for (size_t i = 0; i < NOH; i++)
	{
		std::vector<float> _norm_;
		for (size_t j = 0; j < parts; j++)
		{
			_norm_.push_back(normalratio[i * parts + j]);
		}
		_norm.push_back(_norm_);
	}
	return _norm;
}

template <typename T>
void get_cls_bbox_yolox(std::vector<int> real_out_channels, std::vector<int> &id_list, std::vector<float> &score_list, std::vector<cv::Rect2f> &box_list, T *tensor_data, int base_addr,
						Grid &grid, std::vector<float> &normratio_list, int stride,
						std::vector<float> anchor, int N_CLASS, float THR_F, bool MULTILABLE)
{
	// yolox
	if (!MULTILABLE)
	{
		auto _score_ = sigmoid(tensor_data[base_addr] * normratio_list[0]);
		auto class_ptr_start = tensor_data + base_addr + real_out_channels[0] + real_out_channels[1];
		auto max_prob_ptr = std::max_element(class_ptr_start, class_ptr_start + N_CLASS);
		int max_index = std::distance(class_ptr_start, max_prob_ptr);
		auto _prob_ = sigmoid(*max_prob_ptr * normratio_list[2]);
		auto realscore = _score_ * _prob_;

		if (realscore > THR_F)
		{
			std::vector<float> xywh = {};
			for (size_t i = 0; i < 4; i++)
			{
				auto box = tensor_data[base_addr + real_out_channels[0] + i] * normratio_list[1];
				xywh.push_back(box);
			}

			xywh[0] = (xywh[0] + grid.location_x) * stride;
			xywh[1] = (xywh[1] + grid.location_y) * stride;
			xywh[2] = expf(xywh[2]) * stride;
			xywh[3] = expf(xywh[3]) * stride;

			id_list.emplace_back(max_index);
			score_list.emplace_back(realscore);
			box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),
											 (xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
		}
	}
	else
	{
		for (size_t cls_idx = 0; cls_idx < N_CLASS; cls_idx++)
		{
			// auto realscore = this->getRealScore(tensor_data, base_addr, norm, i);

			auto _score_ = sigmoid(tensor_data[base_addr] * normratio_list[0]);
			auto _prob_ = sigmoid(tensor_data[base_addr + real_out_channels[0] + real_out_channels[1] + cls_idx] * normratio_list[2]);
			auto realscore = _score_ * _prob_;
			if (realscore > THR_F)
			{
				std::vector<float> xywh = {};
				for (size_t i = 0; i < 4; i++)
				{
					auto box = tensor_data[base_addr + real_out_channels[0] + i] * normratio_list[1];
					xywh.push_back(box);
				}
				xywh[0] = (xywh[0] + grid.location_x) * stride;
				xywh[1] = (xywh[1] + grid.location_y) * stride;
				xywh[2] = expf(xywh[2]) * stride;
				xywh[3] = expf(xywh[3]) * stride;

				id_list.emplace_back(cls_idx);
				score_list.emplace_back(realscore);
				box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),
												 (xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
			}
		}
	}
}

template <typename T>
void get_cls_bbox(std::vector<int> &id_list, std::vector<float> &score_list, std::vector<cv::Rect2f> &box_list, T *tensor_data, int base_addr,
				  Grid &grid, float &SCALE, int stride,
				  std::vector<float> anchor, int N_CLASS, float THR_F, bool MULTILABEL)
{
	if (!MULTILABEL)
	{
		auto _score_ = sigmoid(tensor_data[base_addr + 4] * SCALE);
		auto class_ptr_start = tensor_data + base_addr + 5;
		auto max_prob_ptr = std::max_element(class_ptr_start, class_ptr_start + N_CLASS);
		int max_index = std::distance(class_ptr_start, max_prob_ptr);
		auto _prob_ = sigmoid(*max_prob_ptr * SCALE);
		auto realscore = _score_ * _prob_;
		if (realscore > THR_F)
		{
			std::vector<float> xywh = sigmoid(tensor_data, SCALE, base_addr, 4);

			xywh[0] = (2 * xywh[0] + grid.location_x - 0.5) * stride;
			xywh[1] = (2 * xywh[1] + grid.location_y - 0.5) * stride;

			xywh[2] = 4 * powf(xywh[2], 2) * anchor[0];
			xywh[3] = 4 * powf(xywh[3], 2) * anchor[1];
			id_list.emplace_back(max_index);
			score_list.emplace_back(realscore);
			box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),
											 (xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
		}
	}
	else
	{
		for (size_t cls_idx = 0; cls_idx < N_CLASS; cls_idx++)
		{
			// auto realscore = this->getRealScore(tensor_data, obj_ptr_start, norm, i);

			auto _score_ = sigmoid(tensor_data[base_addr + 4] * SCALE);
			auto _prob_ = sigmoid(tensor_data[base_addr + 5 + cls_idx] * SCALE);
			auto realscore = _score_ * _prob_;
			if (realscore > THR_F)
			{
				// auto bbox = this->getBbox(tensor_data, norm, obj_ptr_start, location_x, location_y, stride, anchor);
				std::vector<float> xywh = sigmoid(tensor_data, SCALE, base_addr, 4);

				xywh[0] = (2 * xywh[0] + grid.location_x - 0.5) * stride;
				xywh[1] = (2 * xywh[1] + grid.location_y - 0.5) * stride;

				xywh[2] = 4 * powf(xywh[2], 2) * anchor[0];
				xywh[3] = 4 * powf(xywh[3], 2) * anchor[1];

				id_list.emplace_back(cls_idx);
				score_list.emplace_back(realscore);
				box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),
												 (xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
			}
		}
	}
}

using YoloPostResult = std::tuple<std::vector<int>, std::vector<float>, std::vector<cv::Rect2f>>; // id_list,score_list, box_list

YoloPostResult cft_detpost_plin(const std::vector<icraft::xrt::Tensor> &output_tensors, YoloPostResult &last_frame_result, NetInfo &netinfo,
								 float conf, float iou_thresh, bool MULTILABEL, bool fpga_nms, int N_CLASS,
								 std::vector<std::vector<std::vector<float>>> &ANCHORS, icraft::xrt::Device device)
{
	// const int output_tensors_bits = 8;
	std::vector<int> id_list;
	std::vector<float> score_list;
	std::vector<cv::Rect2f> box_list;
	std::vector<float> stride = get_stride(netinfo);
	for (size_t i = 0; i < output_tensors.size(); i++)
	{
		auto host_tensor = output_tensors[i].to(icraft::xrt::HostDevice::MemRegion());
		int output_tensors_bits = output_tensors[i].dtype()->element_dtype.getStorageType().bits();
		// std::cout << "output_tensors_bits: " << output_tensors_bits << std::endl;
		int obj_num = output_tensors[i].dtype()->shape[2];
		// std::cout << "obj_num: " << obj_num << std::endl;
		int anchor_length = output_tensors[i].dtype()->shape[3];
		// std::cout << "anchor_length: " << anchor_length << std::endl;
		// std::cout << "stride[" << i << "]: " << stride[i] << std::endl;
		if (output_tensors_bits == 16)
		{
			auto tensor_data = (int16_t *)host_tensor.data().cptr();
			for (size_t obj = 0; obj < obj_num; obj++)
			{
				int base_addr = obj * anchor_length;
				Grid grid = get_grid(output_tensors_bits, tensor_data, base_addr, anchor_length);
				get_cls_bbox(id_list, score_list, box_list, tensor_data, base_addr, grid, netinfo.o_scale[i], stride[i], ANCHORS[i][grid.anchor_index], N_CLASS, conf, MULTILABEL);
			}
		}
		else
		{
			auto tensor_data = (int8_t *)host_tensor.data().cptr();
			for (size_t obj = 0; obj < obj_num; obj++)
			{
				int base_addr = obj * anchor_length;
				Grid grid = get_grid(output_tensors_bits, tensor_data, base_addr, anchor_length);
				get_cls_bbox(id_list, score_list, box_list, tensor_data, base_addr, grid, netinfo.o_scale[i], stride[i], ANCHORS[i][grid.anchor_index], N_CLASS, conf, MULTILABEL);
			}
		}
	}
	std::vector<std::tuple<int, float, cv::Rect2f>> nms_res;
	if (fpga_nms)
	{
		nms_res = nms_hard(box_list, score_list, id_list, iou_thresh, device);
	}
	else
	{
		nms_res = nms_soft(id_list, score_list, box_list, iou_thresh); // 后处理 之 NMS
	}

	// // 对前后帧的结果求平均
	auto id_list_last_frame = std::get<0>(last_frame_result);
	auto score_list_last_frame = std::get<1>(last_frame_result);
	auto box_list_last_frame = std::get<2>(last_frame_result);

	for (auto idx_score_bbox : nms_res)
	{
		for (size_t i = 0; i < box_list_last_frame.size(); ++i)
		{
			if ((1.f - cv::jaccardDistance(std::get<2>(idx_score_bbox), box_list_last_frame[i])) > SMOOTH_IOU && (std::get<0>(idx_score_bbox) == id_list_last_frame[i]))
			{

				std::get<2>(idx_score_bbox).x = box_list_last_frame[i].x * ALPHA + std::get<2>(idx_score_bbox).x * (1.0f - ALPHA);
				std::get<2>(idx_score_bbox).y = box_list_last_frame[i].y * ALPHA + std::get<2>(idx_score_bbox).y * (1.0f - ALPHA);
				std::get<2>(idx_score_bbox).width = box_list_last_frame[i].width * ALPHA + std::get<2>(idx_score_bbox).width * (1.0f - ALPHA);
				std::get<2>(idx_score_bbox).height = box_list_last_frame[i].height * ALPHA + std::get<2>(idx_score_bbox).height * (1.0f - ALPHA);
				std::get<1>(idx_score_bbox) = score_list_last_frame[i] * ALPHA + std::get<1>(idx_score_bbox) * (1.0f - ALPHA);
				break;
			}
		}
	}
	std::vector<int> id_list_ret;		  // id_list_ret.reserve(nms_indices.size());
	std::vector<float> score_list_ret;	  // score_list_ret.reserve(nms_indices.size());
	std::vector<cv::Rect2f> box_list_ret; // box_list_ret.reserve(nms_indices.size());
	for (auto idx_score_bbox : nms_res)
	{
		// store
		id_list_ret.emplace_back(std::get<0>(idx_score_bbox));
		score_list_ret.emplace_back(std::get<1>(idx_score_bbox));
		box_list_ret.emplace_back(std::get<2>(idx_score_bbox));
	}
	return YoloPostResult{id_list_ret, score_list_ret, box_list_ret};
}


inline YoloPostResult post_detpost_soft(const std::vector<icraft::xrt::Tensor> &output_tensors, YoloPostResult &last_frame_result, std::vector<std::string> &LABELS,
										std::vector<std::vector<std::vector<float>>> &ANCHORS, NetInfo &netinfo, int N_CLASS, float conf, float iou_thresh)
{

	std::vector<int> id_list;
	std::vector<float> socre_list;
	std::vector<cv::Rect2f> box_list;
	std::vector<float> stride = get_stride(netinfo);
	for (int yolo = 0; yolo < output_tensors.size(); yolo++)
	{
		int _H = output_tensors[yolo].dtype()->shape[1];
		int _W = output_tensors[yolo].dtype()->shape[2];
		// std::cout << "Stride[" << yolo << "]:" << stride[yolo] << std::endl;
		auto host_tensor = output_tensors[yolo].to(icraft::xrt::HostDevice::MemRegion());
		auto tensor_data = (float *)host_tensor.data().cptr();
		for (size_t h = 0; h < _H; h++)
		{
			int _h = h;
			for (size_t w = 0; w < _W; w++)
			{
				int _w = w;
				for (size_t anchor_index = 0; anchor_index < ANCHORS.size(); anchor_index++)
				{
					int _anchor_id = anchor_index;
					auto one_head_stride = stride[yolo];
					std::vector<float> one_head_anchor = {};
					if (ANCHORS.size() != 0)
					{
						one_head_anchor = ANCHORS[yolo][anchor_index];
					}

					auto boxPtr = tensor_data + h * _W * (N_CLASS + 5) * ANCHORS.size() + w * (N_CLASS + 5) * ANCHORS.size() + anchor_index * (N_CLASS + 5);
					auto scorePtr = boxPtr + 4;
					auto classPtr = boxPtr + 5;
					auto _score_ = sigmoid(*scorePtr);
					float *max_prob_ptr = std::max_element(classPtr, classPtr + N_CLASS);
					int max_index = std::distance(classPtr, max_prob_ptr);
					auto _prob_ = sigmoid(*max_prob_ptr);
					auto realscore = _score_ * _prob_;

					auto xywh = sigmoid(boxPtr, 4);
					xywh[0] = (2 * xywh[0] + w - 0.5) * one_head_stride;
					xywh[1] = (2 * xywh[1] + h - 0.5) * one_head_stride;
					xywh[2] = 4 * powf(xywh[2], 2) * one_head_anchor[0];
					xywh[3] = 4 * powf(xywh[3], 2) * one_head_anchor[1];
					if (realscore > conf)
					{
						id_list.emplace_back(max_index);
						socre_list.emplace_back(realscore);
						box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),
														 (xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
					}
				}
			}
		}
	}

	std::vector<std::tuple<int, float, cv::Rect2f>> nms_res = nms_soft(id_list, socre_list, box_list, iou_thresh); // 后处理 之 NMS
	// std::cout << "number of results = " << nms_res.size() << std::endl;

	// 对前后帧的结果求平均
	auto id_list_last_frame = std::get<0>(last_frame_result);
	auto score_list_last_frame = std::get<1>(last_frame_result);
	auto box_list_last_frame = std::get<2>(last_frame_result);

	for (auto idx_score_bbox : nms_res)
	{
		for (size_t i = 0; i < box_list_last_frame.size(); ++i)
		{
			if ((1.f - cv::jaccardDistance(std::get<2>(idx_score_bbox), box_list_last_frame[i])) > SMOOTH_IOU && (std::get<0>(idx_score_bbox) == id_list_last_frame[i]))
			{

				std::get<2>(idx_score_bbox).x = box_list_last_frame[i].x * ALPHA + std::get<2>(idx_score_bbox).x * (1.0f - ALPHA);
				std::get<2>(idx_score_bbox).y = box_list_last_frame[i].y * ALPHA + std::get<2>(idx_score_bbox).y * (1.0f - ALPHA);
				std::get<2>(idx_score_bbox).width = box_list_last_frame[i].width * ALPHA + std::get<2>(idx_score_bbox).width * (1.0f - ALPHA);
				std::get<2>(idx_score_bbox).height = box_list_last_frame[i].height * ALPHA + std::get<2>(idx_score_bbox).height * (1.0f - ALPHA);
				std::get<1>(idx_score_bbox) = score_list_last_frame[i] * ALPHA + std::get<1>(idx_score_bbox) * (1.0f - ALPHA);
				break;
			}
		}
	}

	std::vector<int> id_list_ret;
	std::vector<float> score_list_ret;
	std::vector<cv::Rect2f> box_list_ret;
	for (auto idx_score_bbox : nms_res)
	{
		// store
		id_list_ret.emplace_back(std::get<0>(idx_score_bbox));
		score_list_ret.emplace_back(std::get<1>(idx_score_bbox));
		box_list_ret.emplace_back(std::get<2>(idx_score_bbox));
	}
	return YoloPostResult{id_list_ret, score_list_ret, box_list_ret};
}