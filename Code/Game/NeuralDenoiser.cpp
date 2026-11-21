#include "Game/NeuralDenoiser.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>
#include "Engine/Job/JobSystem.h"
#include "Engine/Core/Time.hpp"
#include "Engine/Core/ErrorWarningAssert.hpp"

#include "ThirdParty/onnxruntime/include/onnxruntime_cxx_api.h"

#include <algorithm>
#include <cstring>

struct NeuralDenoiser::Session
{
    Ort::Env     env{ ORT_LOGGING_LEVEL_WARNING, "denoiser" };
    Ort::Session session{ nullptr };
};

static float HalfToFloat(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp  = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t bits;
    if (exp == 0)
    {
        if (mant == 0) bits = sign;
        else
        {
            // subnormal: renormalize
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) { mant <<= 1; --exp; }
            bits = sign | (exp << 23) | ((mant & 0x3ff) << 13);
        }
    }
    else if (exp == 31) bits = sign | 0x7f800000 | (mant << 13);
    else                bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

class DenoiseJob : public Job
{
public:
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

        // Same compose as raygen: remodulate (except sky), luminance Reinhard.
        m_result.resize(n * 4);
        for (size_t i = 0; i < n; ++i)
        {
            const bool sky = m_albedo[i * 4 + 3] < 128;
            float rgb[3];
            for (int c = 0; c < 3; ++c)
            {
                float l = (std::max)(out[c * n + i], 0.f);
                rgb[c] = sky ? l : l * in[(3 + c) * n + i];
            }
            float Y  = (std::max)(0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2], 1e-4f);
            float sc = (Y / (Y + 1.f)) / Y;
            for (int c = 0; c < 3; ++c)
                m_result[i * 4 + c] = (uint8_t)((std::min)(rgb[c] * sc, 1.f) * 255.f + 0.5f);
            m_result[i * 4 + 3] = 255;
        }

        m_inferenceMs = (float)((t2 - t1) * 1000.0);
        m_totalMs     = (float)((GetCurrentTimeSeconds() - t0) * 1000.0);
    }

    void OnComplete() override {}
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

bool NeuralDenoiser::Update(const VulkanRTPath::DenoiseFrame& frame)
{
    if (!m_session) return false;

    // The denoiser is the only JobSystem client in this app, so every
    // completed job is ours.
    for (Job* job : g_theJobSystem->RetrieveCompletedJobs())
    {
        if (job == m_inFlight)
        {
            m_result.swap(m_inFlight->m_result);
            m_resultW = m_inFlight->m_w;
            m_resultH = m_inFlight->m_h;
            m_lastInferenceMs = m_inFlight->m_inferenceMs;
            m_lastJobMs       = m_inFlight->m_totalMs;
            m_inFlight = nullptr;
        }
        delete job;
    }

    if (!m_inFlight && frame.light)
    {
        const size_t n = (size_t)frame.width * frame.height;
        DenoiseJob* job = new DenoiseJob();
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
