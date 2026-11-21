#pragma once
#include "Engine/Renderer/VulkanRTPath.h"

#include <memory>
#include <string>
#include <vector>

class DenoiseJob;

// CPU (ONNX Runtime) denoiser for the RT path. One inference runs at a time
// on the JobSystem; the main thread keeps showing the last finished result,
// so the denoised image lags the camera by a few frames.
//
// Model contract: input  "input"  [1, 6, H, W] = demodulated light rgb, albedo rgb
//                 output "output" [1, 3, H, W] = denoised demodulated light rgb
class NeuralDenoiser
{
public:
    NeuralDenoiser();
    ~NeuralDenoiser();

    bool Load(const std::wstring& modelPath, int numThreads);
    void Shutdown();
    bool IsLoaded() const { return m_session != nullptr; }

    // Main thread, once per RT frame. Returns true if frame.upload was filled.
    bool Update(const VulkanRTPath::DenoiseFrame& frame);
    void OnJobDone(DenoiseJob* job);  // from DenoiseJob::OnComplete, main thread

    // Call after JobSystem::Shutdown, which owns (and deletes) any job still queued.
    void ForgetInFlightJob() { m_inFlight = nullptr; }

    float LastInferenceMs() const { return m_lastInferenceMs; }
    float LastJobMs() const       { return m_lastJobMs; }

private:
    struct Session;
    std::unique_ptr<Session> m_session;

    DenoiseJob*          m_inFlight = nullptr;
    std::vector<uint8_t> m_result;  // RGBA8, tonemapped
    uint32_t             m_resultW = 0;
    uint32_t             m_resultH = 0;
    float                m_lastInferenceMs = 0.f;
    float                m_lastJobMs = 0.f;
};
