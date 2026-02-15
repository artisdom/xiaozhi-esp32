#pragma once
#include "sdkconfig.h"

#include <lvgl.h>
#include <thread>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "jpg/image_to_jpeg.h"
#include "esp_video_init.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class EspVideo : public Camera {
private:
    struct FrameBuffer {
        uint8_t *data = nullptr;
        size_t len = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        v4l2_pix_fmt_t format = 0;
    } frame_;
    v4l2_pix_fmt_t sensor_format_ = 0;
#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    uint16_t sensor_width_ = 0;
    uint16_t sensor_height_ = 0;
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    int video_fd_ = -1;
    bool streaming_on_ = false;
    struct MmapBuffer { void *start = nullptr; size_t length = 0; };
    std::vector<MmapBuffer> mmap_buffers_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    
    // Preview frame management
    uint8_t* preview_frame_data_ = nullptr;
    size_t preview_frame_len_ = 0;
    bool preview_frame_locked_ = false;

public:
    EspVideo(const esp_video_init_config_t& config);
    ~EspVideo();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);
    
    // Preview support
    virtual bool GetPreviewFrame(uint8_t** data, size_t* len, uint16_t* width, uint16_t* height) override;
    virtual void ReleasePreviewFrame() override;
    virtual bool IsReady() const override { return streaming_on_ && video_fd_ >= 0; }
    virtual uint32_t GetSensorFormat() const override { return sensor_format_; }
    
    // Save JPEG to file
    virtual bool SaveJpegToFile(const std::string& path, int quality = 80) override;
};
