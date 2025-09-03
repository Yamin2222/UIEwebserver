#include "image_enhancer.h"
#include <iostream>
#include <opencv2/imgproc.hpp>

// 静态成员初始化（单例实例）
ImageEnhancer* ImageEnhancer::instance_ = nullptr;

// 单例获取接口
ImageEnhancer* ImageEnhancer::GetInstance(const std::string& model_path) {
    if (instance_ == nullptr) {
        instance_ = new ImageEnhancer(model_path);
    }
    return instance_;
}

// 私有构造：加载ONNX模型并初始化
ImageEnhancer::ImageEnhancer(const std::string& model_path) 
    : env_(ORT_LOGGING_LEVEL_WARNING) { // 只输出警告级别的日志
    try {
        // 1. 创建ONNX会话选项（设置线程数，优化推理速度）
        Ort::SessionOptions session_opt;
        session_opt.SetIntraOpNumThreads(4); // 线程数根据CPU核心数调整
        session_opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC); // 基础优化

        // 2. 加载模型（模型路径为仓库根目录的LPGPNet.onnx）
        session_ = std::make_unique<Ort::Session>(env_, model_path.c_str(), session_opt);
        std::cout << "[ImageEnhancer] 模型加载成功：" << model_path << std::endl;

        // 3. 获取输入/输出节点名
        size_t input_count = session_->GetInputCount();
        size_t output_count = session_->GetOutputCount();
        if (input_count != 1 || output_count != 1) {
            throw std::runtime_error("模型必须有1个输入和1个输出");
        }

        // 输入节点名
        Ort::AllocatedStringPtr input_name_ptr = session_->GetInputNameAllocated(0, alloc_);
        input_names_.push_back(input_name_ptr.get());
        input_name_ptr.release(); // 释放内存（避免内存泄漏）

        // 输出节点名
        Ort::AllocatedStringPtr output_name_ptr = session_->GetOutputNameAllocated(0, alloc_);
        output_names_.push_back(output_name_ptr.get());
        output_name_ptr.release();

        // 4. 获取输入形状（假设输入形状为 [1, 3, H, W]，即batch=1，3通道）
        auto input_type_info = session_->GetInputTypeInfo(0);
        auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
        input_shape_ = input_tensor_info.GetShape();

        // 校验输入形状（必须是4维：[batch, channel, height, width]）
        if (input_shape_.size() != 4) {
            throw std::runtime_error("模型输入必须是4维张量 [batch, channel, height, width]");
        }

        // 提取输入尺寸和通道数
        input_channel_ = static_cast<int>(input_shape_[1]); // 通道数（通常为3）
        input_size_ = cv::Size(
            static_cast<int>(input_shape_[3]), // width（第4维）
            static_cast<int>(input_shape_[2])  // height（第3维）
        );
        std::cout << "[ImageEnhancer] 模型输入尺寸：" << input_size_.width << "x" << input_size_.height 
                  << "，通道数：" << input_channel_ << std::endl;

    } catch (const Ort::Exception& e) {
        std::cerr << "[ImageEnhancer] ONNX模型加载失败：" << e.what() << std::endl;
        throw; // 抛出异常，让调用者感知错误
    } catch (const std::exception& e) {
        std::cerr << "[ImageEnhancer] 初始化失败：" << e.what() << std::endl;
        throw;
    }
}

// 图像增强主逻辑
bool ImageEnhancer::Enhance(const cv::Mat& input_img, cv::Mat& output_img) {
    try {
        // 1. 校验输入图像（必须是3通道彩色图）
        if (input_img.empty() || input_img.channels() != 3) {
            std::cerr << "[ImageEnhancer] 输入图像无效（必须是3通道彩色图）" << std::endl;
            return false;
        }

        // 2. 预处理：生成模型输入张量
        std::vector<float> input_tensor_data;
        if (!Preprocess(input_img, input_tensor_data)) {
            std::cerr << "[ImageEnhancer] 预处理失败" << std::endl;
            return false;
        }

        // 3. ONNX推理（执行模型）
        auto memory_info = Ort::MemoryInfo::CreateCpu(
            OrtAllocatorType::OrtArenaAllocator, 
            OrtMemType::OrtMemTypeDefault
        );

        // 创建输入张量
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info,
            input_tensor_data.data(),
            input_tensor_data.size(),
            input_shape_.data(),
            input_shape_.size()
        );
        if (!input_tensor.IsTensor()) {
            std::cerr << "[ImageEnhancer] 创建输入张量失败" << std::endl;
            return false;
        }

        // 执行推理
        std::vector<Ort::Value> output_tensors = session_->Run(
            Ort::RunOptions{nullptr}, // 无特殊运行选项
            input_names_.data(),      // 输入节点名
            &input_tensor,            // 输入张量
            1,                        // 输入数量
            output_names_.data(),     // 输出节点名
            1                         // 输出数量
        );
        if (output_tensors.empty()) {
            std::cerr << "[ImageEnhancer] 推理无输出结果" << std::endl;
            return false;
        }

        // 4. 提取输出张量数据
        auto& output_tensor = output_tensors[0];
        if (!output_tensor.IsTensor()) {
            std::cerr << "[ImageEnhancer] 输出不是张量类型" << std::endl;
            return false;
        }
        float* output_tensor_ptr = output_tensor.GetTensorMutableData<float>();
        if (output_tensor_ptr == nullptr) {
            std::cerr << "[ImageEnhancer] 获取输出张量数据失败" << std::endl;
            return false;
        }

        // 5. 后处理：生成增强后图像
        std::vector<float> output_tensor_data(
            output_tensor_ptr, 
            output_tensor_ptr + input_tensor_data.size() // 输出尺寸与输入一致
        );
        if (!Postprocess(output_tensor_data, output_img)) {
            std::cerr << "[ImageEnhancer] 后处理失败" << std::endl;
            return false;
        }

        // 6. 调整输出图像尺寸与输入一致
        cv::resize(output_img, output_img, input_img.size(), 0, 0, cv::INTER_LANCZOS4);
        return true;

    } catch (const Ort::Exception& e) {
        std::cerr << "[ImageEnhancer] 推理失败：" << e.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[ImageEnhancer] 增强失败：" << e.what() << std::endl;
        return false;
    }
}

// 预处理：图像缩放、归一化、通道转换（BGR→RGB→CHW）
bool ImageEnhancer::Preprocess(const cv::Mat& input_img, std::vector<float>& input_tensor_data) {
    try {
        cv::Mat img_rgb, img_resized, img_float;

        // 1. BGR→RGB（OpenCV默认BGR，模型通常需要RGB）
        cv::cvtColor(input_img, img_rgb, cv::COLOR_BGR2RGB);

        // 2. 缩放至模型输入尺寸（使用高质量插值）
        cv::resize(img_rgb, img_resized, input_size_, 0, 0, cv::INTER_LANCZOS4);

        // 3. 转换为浮点型并归一化（模型通常要求输入在[0,1]或[-1,1]，这里按[0,1]处理）
        img_resized.convertTo(img_float, CV_32F, 1.0 / 255.0); // 除以255归一化

        // 4. 通道转换：HWC（Height-Width-Channel）→ CHW（Channel-Height-Width）
        input_tensor_data.clear();
        std::vector<cv::Mat> channels(input_channel_);
        cv::split(img_float, channels); // 拆分RGB通道

        // 按CHW顺序填充数据（R→G→B）
        for (int c = 0; c < input_channel_; ++c) {
            // 获取当前通道的浮点数据
            float* channel_data = reinterpret_cast<float*>(channels[c].data);
            // 追加到输入张量（通道优先）
            input_tensor_data.insert(
                input_tensor_data.end(),
                channel_data,
                channel_data + channels[c].total() // total() = width * height
            );
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "[ImageEnhancer] 预处理异常：" << e.what() << std::endl;
        return false;
    }
}

// 后处理：张量转图像、反归一化、通道转换（CHW→HWC→BGR）
bool ImageEnhancer::Postprocess(const std::vector<float>& output_tensor_data, cv::Mat& output_img) {
    try {
        // 1. 计算单通道数据量（width * height）
        size_t channel_size = input_size_.width * input_size_.height;
        if (output_tensor_data.size() != input_channel_ * channel_size) {
            std::cerr << "[ImageEnhancer] 输出张量尺寸不匹配" << std::endl;
            return false;
        }

        // 2. CHW→HWC：拆分通道并合并
        std::vector<cv::Mat> channels(input_channel_);
        for (int c = 0; c < input_channel_; ++c) {
            // 提取当前通道的数据（从输出张量中截取）
            const float* channel_data = output_tensor_data.data() + c * channel_size;
            // 创建单通道浮点图像
            channels[c] = cv::Mat(input_size_.height, input_size_.width, CV_32F, const_cast<float*>(channel_data));
        }

        // 合并通道（得到RGB格式的HWC图像）
        cv::Mat img_rgb_float;
        cv::merge(channels, img_rgb_float);

        // 3. 反归一化并转换为8位图像（[0,1]→[0,255]）
        cv::Mat img_rgb;
        img_rgb_float.convertTo(img_rgb, CV_8UC3, 255.0); // 乘以255反归一化

        // 4. RGB→BGR（转换回OpenCV默认格式）
        cv::cvtColor(img_rgb, output_img, cv::COLOR_RGB2BGR);

        return true;

    } catch (const std::exception& e) {
        std::cerr << "[ImageEnhancer] 后处理异常：" << e.what() << std::endl;
        return false;
    }
}