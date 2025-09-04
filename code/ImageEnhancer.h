#ifndef IMAGE_ENHANCER_H
#define IMAGE_ENHANCER_H

#include <opencv2/opencv.hpp>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <mutex>

// 单例模式的图像增强器类：全局唯一实例，封装ONNX模型加载与推理
class ImageEnhancer {
public:
    // 禁用拷贝构造和赋值运算符（单例核心：禁止复制）
    ImageEnhancer(const ImageEnhancer&) = delete;
    ImageEnhancer& operator=(const ImageEnhancer&) = delete;

    // 获取单例实例（线程安全的懒加载）
    // 需先调用Init()初始化模型路径，否则会抛出异常
    static ImageEnhancer& GetInstance(const std::string& model_path = "./models/LPGPNet.onnx") {
        // 修复：用 [=] 值捕获 model_path，避免悬空引用
        std::call_once(init_flag_, [=]() {
            instance_.reset(new ImageEnhancer(model_path));
        });
        if (!instance_) {
            throw std::runtime_error("ImageEnhancer not initialized.");
        }
        return *instance_;
    }

    // 初始化单例（必须先调用，传入模型路径）
    static void Init(const std::string& model_path = "./models/LPGPNet.onnx") {
        if (instance_) {
            throw std::runtime_error("ImageEnhancer already initialized.");
        }
        instance_.reset(new ImageEnhancer(model_path));
    }

    // 核心接口：输入原始图像，返回增强后的图像
    // 输入：cv::Mat（彩色图像，BGR通道，uint8类型）
    // 输出：cv::Mat（增强后彩色图像，BGR通道，uint8类型）
    cv::Mat EnhanceImage(const cv::Mat& input_image);

    // 获取模型输入尺寸
    std::pair<int, int> GetModelInputSize() const {
        return {input_height_, input_width_};
    }

private:
    // 私有构造函数：仅允许内部创建实例
    explicit ImageEnhancer(const std::string& model_path);

    // 初始化模型信息（输入输出节点、尺寸等）
    void InitModelInfo();

    // 图像预处理
    std::vector<float> PreprocessImage(const cv::Mat& input_image);

    // 模型推理
    std::vector<float> Inference(const std::vector<float>& input_tensor);

    // 图像后处理
    cv::Mat PostprocessOutput(const std::vector<float>& output_tensor, 
                              const cv::Size& original_size);

private:
    // 单例实例（智能指针自动管理生命周期）
    static std::unique_ptr<ImageEnhancer> instance_;
    // 线程安全初始化标记
    static std::once_flag init_flag_;

    // ONNX Runtime组件
    Ort::Env env_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> session_;
    Ort::AllocatorWithDefaultOptions allocator_;

    // 模型参数
    std::vector<std::string> input_node_names_; // 改为存储字符串
    std::vector<std::string> output_node_names_; // 改为存储字符串
    int input_channels_ = 3;  // 默认3通道（RGB/BGR）
    int input_height_ = 0;    // 模型输入高度（从模型获取）
    int input_width_ = 0;     // 模型输入宽度（从模型获取）
    float norm_scale_ = 1.0f / 255.0f;  // 归一化因子
    bool need_bgr2rgb_ = true;  // 是否需要BGR转RGB（根据模型训练数据决定）
};

#endif // IMAGE_ENHANCER_H
    