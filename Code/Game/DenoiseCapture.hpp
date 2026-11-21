#pragma once
#include "Engine/Renderer/VulkanRTPath.h"
#include "Engine/Math/Vec3.hpp"
#include "Engine/Math/EulerAngles.hpp"
#include "Engine/Math/RandomNumberGenerator.hpp"

#include <string>
#include <vector>

// Writes (noisy, clean, albedo) training triples for the RT denoiser.
// Per random camera pose: hold the camera for kWarmupFrames so ReSTIR's
// temporal history settles and the 2-frame readback lag clears, keep that
// frame's noisy light and albedo, then average the next `cleanFrames`
// frames of light as the target. Files go out on the JobSystem IO thread:
//   NNNN_noisy.f16   RGBA16F demodulated light, as read back
//   NNNN_clean.f32   RGB float32 demodulated light, mean of cleanFrames
//   NNNN_albedo.u8   RGBA8, a=0 for sky
//   NNNN_noisy.png / NNNN_clean.png  tonemapped previews
// plus info.txt (width height cleanFrames) and poses.txt.
class DenoiseCapture
{
public:
    static constexpr int kWarmupFrames = 20;

    void Start(std::string const& outDir, int numPoses, int cleanFrames);
    bool IsActive() const { return m_pose < m_numPoses; }

    // Main thread, once per RT frame, before the camera is uploaded.
    void Update(const VulkanRTPath::DenoiseFrame& frame, Vec3& camPos, EulerAngles& camOri);
    void OnSampleWritten() { ++m_written; }

    std::string Status() const;

private:
    std::string m_dir;
    int  m_numPoses    = 0;
    int  m_cleanFrames = 0;
    int  m_pose        = 0;
    int  m_frame       = 0;
    int  m_written     = 0;
    uint32_t m_w = 0, m_h = 0;

    Vec3        m_camPos;
    EulerAngles m_camOri;
    RandomNumberGenerator m_rng;

    std::vector<uint16_t> m_noisy;
    std::vector<uint8_t>  m_albedo;
    std::vector<float>    m_sum;

    void NextPose();
};
