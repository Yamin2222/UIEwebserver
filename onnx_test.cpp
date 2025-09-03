#include <iostream>
#include <vector>
#include <opencv2/opencv.hpp>
#include <onnxruntime_cxx_api.h>
#include <random>

class ONNXImageEnhancer {
private:
    Ort::Env env;
    Ort::Session session;
    Ort::AllocatorWithDefaultOptions allocator;
    std::vector<const char*> input_names;
    std::vector<const char*> output_names;
    std::vector<int64_t> input_shape;
    int input_width;
    int input_height;

public:
    // 构造函数：初始化环境和模型
    ONNXImageEnhancer(const std::string& model_path, int width, int height) 
        : env(ORT_LOGGING_LEVEL_WARNING, "ImageEnhancer"),
          session(env, model_path.c_str(), Ort::SessionOptions{nullptr}),
          input_width(width),
          input_height(height) {
        
        // 获取输入输出节点数量
        size_t num_input_nodes = session.GetInputCount();
        size_t num_output_nodes = session.GetOutputCount();
        
        // 新版本API：获取输入名称（使用GetInputNameAllocated）
        for (size_t i = 0; i < num_input_nodes; ++i) {
            Ort::AllocatedStringPtr input_name = session.GetInputNameAllocated(i, allocator);
            input_names.push_back(input_name.get());
            input_name.release(); // 释放内存
        }
        
        // 新版本API：获取输出名称（使用GetOutputNameAllocated）
        for (size_t i = 0; i < num_output_nodes; ++i) {
            Ort::AllocatedStringPtr output_name = session.GetOutputNameAllocated(i, allocator);
            output_names.push_back(output_name.get());
            output_name.release(); // 释放内存
        }
        
        // 设置输入形状 (NCHW格式: 批次大小, 通道数, 高度, 宽度)
        input_shape = {1, 3, input_height, input_width};
        std::cout << "模型加载成功，输入尺寸: " << input_width << "x" << input_height << std::endl;
    }

    // 预处理：图像缩放、归一化、通道转换
    std::vector<float> preprocess(const cv::Mat& image) {
        cv::Mat resized, normalized;
        // 缩放到模型输入尺寸
        cv::resize(image, resized, cv::Size(input_width, input_height));
        // 转换为float并归一化到[0,1]
        resized.convertTo(normalized, CV_32F, 1.0 / 255.0);
        
        // 分离通道 (OpenCV默认BGR，转换为RGB)
        std::vector<cv::Mat> channels(3);
        cv::split(normalized, channels);
        
        // 构建输入数据 (NCHW格式)
        std::vector<float> input_data;
        input_data.reserve(3 * input_height * input_width);
        
        // 按RGB顺序排列通道数据
        for (int c = 2; c >= 0; --c) {  // BGR -> RGB转换
            for (int h = 0; h < input_height; ++h) {
                for (int w = 0; w < input_width; ++w) {
                    input_data.push_back(channels[c].at<float>(h, w));
                }
            }
        }
        
        return input_data;
    }

    // 后处理：将模型输出转换为图像格式
    cv::Mat postprocess(const std::vector<float>& output_data, int original_width, int original_height) {
        // 构建输出图像通道
        std::vector<cv::Mat> channels(3);
        for (int c = 0; c < 3; ++c) {
            channels[c] = cv::Mat(input_height, input_width, CV_32F);
        }
        
        // 解析输出数据到通道 (NCHW -> 图像格式)
        int idx = 0;
        for (int c = 0; c < 3; ++c) {
            for (int h = 0; h < input_height; ++h) {
                for (int w = 0; w < input_width; ++w) {
                    channels[c].at<float>(h, w) = output_data[idx++];
                }
            }
        }
        
        // 合并通道并转换为8位图像
        cv::Mat result;
        cv::merge(channels, result);
        result *= 255.0;  // 反归一化
        result.convertTo(result, CV_8UC3);
        
        // RGB -> BGR转换（适应OpenCV保存格式）
        cv::cvtColor(result, result, cv::COLOR_RGB2BGR);
        
        // 缩放回原始尺寸
        cv::Mat final_result;
        cv::resize(result, final_result, cv::Size(original_width, original_height));
        
        return final_result;
    }

    // 图像增强主函数
    cv::Mat enhance(const cv::Mat& image) {
        if (image.empty()) {
            throw std::runtime_error("输入图像为空");
        }
        
        if (image.channels() != 3) {
            throw std::runtime_error("输入图像必须是三通道（RGB/BGR）");
        }
        
        // 保存原始图像尺寸
        int original_width = image.cols;
        int original_height = image.rows;
        
        // 预处理
        std::vector<float> input_data = preprocess(image);
        
        // 创建输入张量
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info,
            input_data.data(),
            input_data.size(),
            input_shape.data(),
            input_shape.size()
        );
        
        // 模型推理
        std::vector<Ort::Value> output_tensors = session.Run(
            Ort::RunOptions{nullptr},
            input_names.data(),
            &input_tensor,
            1,
            output_names.data(),
            output_names.size()
        );
        
        if (output_tensors.empty()) {
            throw std::runtime_error("模型推理失败，未产生输出");
        }
        
        // 解析输出
        float* output_data = output_tensors[0].GetTensorMutableData<float>();
        size_t output_size = output_tensors[0].GetTensorTypeAndShapeInfo().GetElementCount();
        std::vector<float> output(output_data, output_data + output_size);
        
        // 后处理并返回结果
        return postprocess(output, original_width, original_height);
    }
};

// 生成随机测试图像
cv::Mat generateRandomImage(int width, int height) {
    cv::Mat image(height, width, CV_8UC3);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            cv::Vec3b& pixel = image.at<cv::Vec3b>(i, j);
            pixel[0] = dist(gen);  // B通道
            pixel[1] = dist(gen);  // G通道
            pixel[2] = dist(gen);  // R通道
        }
    }
    return image;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "用法: " << argv[0] << " <模型路径>" << std::endl;
        return 1;
    }
    
    try {
        // 模型输入尺寸（请根据你的LPGPNet模型实际要求修改）
        const int input_width = 256;   // 例如：256、512等
        const int input_height = 256;
        
        // 初始化增强器
        ONNXImageEnhancer enhancer(argv[1], input_width, input_height);
        
        // 生成随机测试图像
        cv::Mat test_image = generateRandomImage(input_width, input_height);
        
        // 执行增强
        cv::Mat enhanced_image = enhancer.enhance(test_image);
        
        // 保存结果
        cv::imwrite("input_test.jpg", test_image);
        cv::imwrite("output_enhanced.jpg", enhanced_image);
        
        std::cout << "处理完成！输入图像已保存为 input_test.jpg" << std::endl;
        std::cout << "增强结果已保存为 output_enhanced.jpg" << std::endl;
    } 
    catch (const std::exception& e) {
        std::cerr << "执行失败: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
    