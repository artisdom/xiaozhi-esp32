#ifndef CAMERA_H
#define CAMERA_H

#include <string>
#include <cstdint>
#include <cstddef>

class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::string Explain(const std::string& question) = 0;
    
    // Preview support - get raw frame data for live preview
    virtual bool GetPreviewFrame(uint8_t** data, size_t* len, uint16_t* width, uint16_t* height) { return false; }
    virtual void ReleasePreviewFrame() {}
    virtual bool IsReady() const { return false; }
    virtual uint32_t GetSensorFormat() const { return 0; }
    
    // Save captured frame to file
    virtual bool SaveJpegToFile(const std::string& path, int quality = 80) { return false; }
};

#endif // CAMERA_H
