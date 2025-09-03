#include "ImageEnhancer.h"
#include <iostream>
#include <opencv2/imgproc.hpp>

// 初始化单例静态成员（cpp中必须定义，否则链接错误）
std::unique_ptr<ImageEnhancer> ImageEnhancer::instance_ = nullptr;
std::once_flag ImageEnhancer::init_flag_;

// 私有构造函数：初始化ONNX环境与模型会话
ImageEnhancer::ImageEnhancer(const std::string& model_path) {
    try {
        // 1. 初始化ONNX环境（日志级别：仅警告及以上）
        env_ = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "LPGPNet_Enhancer");

        // 2. 配置会话参数（平衡性能与资源占用）
        session_options_.SetIntraOpNumThreads(2);  // 线程数：根据CPU核心数调整（如2/4）
        session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);  // 基础图优化（加速推理）
        
        // 3. 加载ONNX模型，创建会话
        session_ = std::make_unique<Ort::Session>(env_, model_path.c_str(), session_options_);

        // 4. 初始化模型关键信息（输入输出节点、尺寸等）
        InitModelInfo();
        std::cout << "[ImageEnhancer] Model loaded: " << model_path << std::endl;
        std::cout << "[ImageEnhancer] Input size: " << input_width_ << "x" << input_height_ 
                  << ", Channels: " << input_channels_ << std::endl;
    } catch (const Ort::Exception& e) {
        throw std::runtime_error("[ImageEnhancer] Model init failed: " + std::string(e.what()));
    } catch (const std::exception& e) {
        throw std::runtime_error("[ImageEnhancer] Constructor error: " + std::string(e.what()));
    }
}

// 初始化模型信息：解析输入输出节点、输入尺寸
void ImageEnhancer::InitModelInfo() {
    try {
        // 1. 处理输入节点（默认取第一个输入节点，LPGPNet通常仅1个输入）
        size_t input_node_count = session_->GetInputCount();
        if (input_node_count == 0) {
            throw std::runtime_error("Model has no input nodes");
        }
        // 获取输入节点名（用ONNX分配器，避免内存泄漏）
        const char* input_node_name = session_->GetInputName(0, allocator_);
        input_node_names_.emplace_back(input_node_name);

        // 2. 解析输入张量形状（NCHW格式：batch, channel, height, width）
        Ort::TypeInfo input_type_info = session_->GetInputTypeInfo(0);
        auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
        std::vector<int64_t> input_shape = input_tensor_info.GetShape();

        if (input_shape.size() != 4) {
            throw std::runtime_error("Input must be 4-dimensional (NCHW), got " + std::to_string(input_shape.size()));
        }
        // 解析通道数、高度、宽度（兼容动态维度，如-1时设默认值256）
        input_channels_ = static_cast<int>(input_shape[1]);
        input_height_ = (input_shape[2] > 0) ? static_cast<int>(input_shape[2]) : 256;
        input_width_ = (input_shape[3] > 0) ? static_cast<int>(input_shape[3]) : 256;

        // 3. 处理输出节点（默认取第一个输出节点，LPGPNet通常仅1个输出）
        size_t output_node_count = session_->GetOutputCount();
        if (output_node_count == 0) {
            throw std::runtime_error("Model has no output nodes");
        }
        const char* output_node_name = session_->GetOutputName(0, allocator_);
        output_node_names_.emplace_back(output_node_name);

    } catch (const Ort::Exception& e) {
        throw std::runtime_error("[ImageEnhancer] Init model info failed: " + std::string(e.what()));
    }
}

// 图像预处理：OpenCV图像 → 模型输入张量
std::vector<float> ImageEnhancer::PreprocessImage(const cv::Mat& input_image) {
    // 1. 校验输入图像有效性
    if (input_image.empty()) {
        throw std::runtime_error("Input image is empty");
    }
    if (input_image.channels() != 3) {
        throw std::runtime_error("Input must be 3-channel (BGR), got " + std::to_string(input_image.channels()));
    }
    if (input_image.type() != CV_8UC3) {
        throw std::runtime_error("Input must be CV_8UC3 type");
    }

    // 2. 缩放图像到模型输入尺寸（INTER_LINEAR：避免失真）
    cv::Mat resized_img;
    cv::resize(input_image, resized_img, cv::Size(input_width_, input_height_), 0, 0, cv::INTER_LINEAR);

    // 3. BGR→RGB转换（模型训练用RGB时启用，否则跳过）
    cv::Mat rgb_img;
    if (need_bgr2rgb_) {
        cv::cvtColor(resized_img, rgb_img, cv::COLOR_BGR2RGB);
    } else {
        rgb_img = resized_img.clone();
    }

    // 4. 归一化：uint8[0,255] → float32[0,1]
    cv::Mat float_img;
    rgb_img.convertTo(float_img, CV_32FC3, norm_scale_);

    // 5. 维度重排：HWC → CHW（NCHW格式要求）
    std::vector<float> input_tensor(input_channels_ * input_height_ * input_width_, 0.0f);
    int channel_stride = input_height_ * input_width_;  // 每个通道的元素数

    for (int c = 0; c < input_channels_; ++c) {
        for (int h = 0; h < input_height_; ++h) {
            for (int w = 0; w < input_width_; ++w) {
                // 从HWC图像取像素，赋值到CHW张量
                float pixel_val = float_img.at<cv::Vec3f>(h, w)[c];
                input_tensor[c * channel_stride + h * input_width_ + w] = pixel_val;
            }
        }
    }

    return input_tensor;
}

// 模型推理：输入张量 → 输出张量
std::vector<float> ImageEnhancer::Inference(const std::vector<float>& input_tensor) {
    try {
        // 1. 校验输入张量尺寸
        size_t expected_size = input_channels_ * input_height_ * input_width_;
        if (input_tensor.size() != expected_size) {
            throw std::runtime_error("Input tensor size mismatch: expected " + std::to_string(expected_size) + 
                                    ", got " + std::to_string(input_tensor.size()));
        }

        // 2. 创建ONNX内存信息（CPU内存，默认分配器）
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault
        );

        // 3. 封装输入张量（不拷贝数据，仅引用）
        std::vector<int64_t> input_shape = {1, input_channels_, input_height_, input_width_};
        Ort::Value input_ort_tensor = Ort::Value::CreateTensor<float>(
            memory_info,
            const_cast<float*>(input_tensor.data()),  // 兼容接口的const_cast
            input_tensor.size(),
            input_shape.data(),
            input_shape.size()
        );
        if (!input_ort_tensor.IsTensor()) {
            throw std::runtime_error("Create input tensor failed");
        }

        // 4. 执行推理
        Ort::RunOptions run_options;  // 默认配置（无特殊参数）
        std::vector<Ort::Value> output_ort_tensors = session_->Run(
            run_options,
            input_node_names_.data(),
            &input_ort_tensor,
            input_node_names_.size(),
            output_node_names_.data(),
            output_node_names_.size()
        );

        // 5. 校验输出有效性
        if (output_ort_tensors.empty()) {
            throw std::runtime_error("Inference got no output");
        }
        Ort::Value& output_ort_tensor = output_ort_tensors[0];
        if (!output_ort_tensor.IsTensor()) {
            throw std::runtime_error("Output is not a tensor");
        }

        // 6. 提取输出数据
        float* output_data = output_ort_tensor.GetTensorMutableData<float>();
        if (output_data == nullptr) {
            throw std::runtime_error("Get output data failed");
        }
        // 计算输出张量总元素数
        auto output_shape = output_ort_tensor.GetTensorTypeAndShapeInfo().GetShape();
        size_t output_size = 1;
        for (int64_t dim : output_shape) {
            output_size *= static_cast<size_t>(dim);
        }
        // 拷贝数据到向量（避免野指针）
        std::vector<float> output_tensor(output_data, output_data + output_size);

        return output_tensor;

    } catch (const Ort::Exception& e) {
        throw std::runtime_error("[ImageEnhancer] Inference failed: " + std::string(e.what()));
    }
}

// 图像后处理：模型输出张量 → OpenCV图像
cv::Mat ImageEnhancer::PostprocessOutput(const std::vector<float>& output_tensor, const cv::Size& original_size) {
    try {
        // 1. 校验输出张量尺寸
        size_t expected_size = input_channels_ * input_height_ * input_width_;
        if (output_tensor.size() != expected_size) {
            throw std::runtime_error("Output tensor size mismatch: expected " + std::to_string(expected_size) + 
                                    ", got " + std::to_string(output_tensor.size()));
        }

        // 2. 维度重排：CHW → HWC
        cv::Mat chw_mat(input_height_, input_width_, CV_32FC3);
        int channel_stride = input_height_ * input_width_;

        for (int c = 0; c < input_channels_; ++c) {
            for (int h = 0; h < input_height_; ++h) {
                for (int w = 0; w < input_width_; ++w) {
                    // 取张量值，限制在[0,1]（避免溢出）
                    float pixel_val = std::max(0.0f, std::min(1.0f, output_tensor[c * channel_stride + h * input_width_ + w]));
                    chw_mat.at<cv::Vec3f>(h, w)[c] = pixel_val;
                }
            }
        }

        // 3. 反归一化：float32[0,1] → uint8[0,255]
        cv::Mat uint8_mat;
        chw_mat.convertTo(uint8_mat, CV_8UC3, 255.0);

        // 4. RGB→BGR转换（还原OpenCV默认格式）
        cv::Mat bgr_mat;
        if (need_bgr2rgb_) {
            cv::cvtColor(uint8_mat, bgr_mat, cv::COLOR_RGB2BGR);
        } else {
            bgr_mat = uint8_mat.clone();
        }

        // 5. 恢复到原始图像尺寸（INTER_CUBIC：适合放大，保留细节）
        cv::Mat enhanced_img;
        cv::resize(bgr_mat, enhanced_img, original_size, 0, 0, cv::INTER_CUBIC);

        return enhanced_img;

    } catch (const std::exception& e) {
        throw std::runtime_error("[ImageEnhancer] Postprocess failed: " + std::string(e.what()));
    }
}

// 核心接口：整合预处理→推理→后处理，返回增强图像
cv::Mat ImageEnhancer::EnhanceImage(const cv::Mat& input_image) {
    try {
        // 记录原始尺寸（后处理恢复用）
        cv::Size original_size = input_image.size();

        // 1. 预处理
        std::vector<float> input_tensor = PreprocessImage(input_image);

        // 2. 模型推理
        std::vector<float> output_tensor = Inference(input_tensor);

        // 3. 后处理
        cv::Mat enhanced_img = PostprocessOutput(output_tensor, original_size);

        return enhanced_img;

    } catch (const std::exception& e) {
        throw std::runtime_error("[ImageEnhancer] Enhance failed: " + std::string(e.what()));
    }
}