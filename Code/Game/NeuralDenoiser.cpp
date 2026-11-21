#include "Game/NeuralDenoiser.hpp"
#include "Game/DenoiseCommon.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>
#include "Engine/Job/JobSystem.h"
#include "Engine/Core/Time.hpp"
#include "Engine/Core/ErrorWarningAssert.hpp"

#include "ThirdParty/onnxruntime/include/onnxruntime_cxx_api.h"

struct NeuralDenoiser::Session
{
    Ort::Env     env{ ORT_LOGGING_LEVEL_WARNING, "denoiser" };
    Ort::Session session{ nullptr };
};

class DenoiseJob : public Job
{
public:
    NeuralDenoiser*       m_owner = nullptr;
    Ort::Session*         m_session = nullptr;
    uint32_t              m_w = 0, m_h = 0;
    std::vector<uint16_t> m_light;   // RGBA16F
    std::vector<uint8_t>  m_albedo;  // RGBA8
    std::vector<uint8_t>  m_result;  // RGBA8
    float                 m_inferenceMs = 0.f;
    float                 m_totalMs = 0.f;

    void Execute() override
    {
        double t0 = GetCurrentTimeSeconds();
        const size_t n = (size_t)m_w * m_h;

        std::vector<float> in(n * 6), out(n * 3);
        for (size_t i = 0; i < n; ++i)
        {
            for (int c = 0; c < 3; ++c)
            {
                in[c * n + i]       = HalfToFloat(m_light[i * 4 + c]);
                in[(3 + c) * n + i] = m_albedo[i * 4 + c] / 255.f;
            }
        }

        const int64_t inShape[4]  = { 1, 6, m_h, m_w };
        const int64_t outShape[4] = { 1, 3, m_h, m_w };
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inT  = Ort::Value::CreateTensor<float>(mem, in.data(),  in.size(),  inShape,  4);
        Ort::Value outT = Ort::Value::CreateTensor<float>(mem, out.data(), out.size(), outShape, 4);
        const char* inName  = "input";
        const char* outName = "output";

        double t1 = GetCurrentTimeSeconds();
        m_session->Run(Ort::RunOptions{ nullptr }, &inName, &inT, 1, &outName, &outT, 1);
        double t2 = GetCurrentTimeSeconds();

        m_result.resize(n * 4);
        for (size_t i = 0; i < n; ++i)
            ComposeLDR(out[i], out[n + i], out[2 * n + i], &m_albedo[i * 4], &m_result[i * 4]);

        m_inferenceMs = (float)((t2 - t1) * 1000.0);
        m_totalMs     = (float)((GetCurrentTimeSeconds() - t0) * 1000.0);
    }

    void OnComplete() override { m_owner->OnJobDone(this); }
};

NeuralDenoiser::NeuralDenoiser() = default;
NeuralDenoiser::~NeuralDenoiser() = default;

bool NeuralDenoiser::Load(const std::wstring& modelPath, int numThreads)
{
    try
    {
        auto s = std::make_unique<Session>();
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(numThreads);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        s->session = Ort::Session(s->env, modelPath.c_str(), opts);
        m_session = std::move(s);
        return true;
    }
    catch (const Ort::Exception& e)
    {
        DebuggerPrintf("NeuralDenoiser: failed to load model: %s\n", e.what());
        return false;
    }
}

void NeuralDenoiser::Shutdown()
{
    GUARANTEE_OR_DIE(m_inFlight == nullptr, "NeuralDenoiser::Shutdown with a job in flight");
    m_session.reset();
    m_result.clear();
}

void NeuralDenoiser::OnJobDone(DenoiseJob* job)
{
    m_result.swap(job->m_result);
    m_resultW = job->m_w;
    m_resultH = job->m_h;
    m_lastInferenceMs = job->m_inferenceMs;
    m_lastJobMs       = job->m_totalMs;
    m_inFlight = nullptr;
}

bool NeuralDenoiser::Update(const VulkanRTPath::DenoiseFrame& frame)
{
    if (!m_session) return false;

    if (!m_inFlight && frame.light)
    {
        const size_t n = (size_t)frame.width * frame.height;
        DenoiseJob* job = new DenoiseJob();
        job->m_owner   = this;
        job->m_session = &m_session->session;
        job->m_w = frame.width;
        job->m_h = frame.height;
        job->m_light.assign(frame.light, frame.light + n * 4);
        job->m_albedo.assign(frame.albedo, frame.albedo + n * 4);
        m_inFlight = job;
        g_theJobSystem->AddPendingJob(job);
    }

    if (m_result.empty() || m_resultW != frame.width || m_resultH != frame.height)
        return false;
    memcpy(frame.upload, m_result.data(), m_result.size());
    return true;
}
