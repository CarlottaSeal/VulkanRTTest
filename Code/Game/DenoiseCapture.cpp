#include "Game/DenoiseCapture.hpp"
#include "Game/DenoiseCommon.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>
#include "Engine/Job/JobSystem.h"
#include "Engine/Core/StringUtils.hpp"
#include "ThirdParty/stb/stb_image_write.h"

#include <cstdio>
#include <filesystem>

static void WriteRaw(std::string const& path, const void* data, size_t bytes)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return;
    fwrite(data, 1, bytes, f);
    fclose(f);
}

class CaptureWriteJob : public Job
{
public:
    CaptureWriteJob() : Job(JOB_TYPE_IO) {}

    DenoiseCapture*       m_owner = nullptr;
    std::string           m_prefix;
    uint32_t              m_w = 0, m_h = 0;
    std::vector<uint16_t> m_noisy;
    std::vector<uint8_t>  m_albedo;
    std::vector<float>    m_clean;

    void Execute() override
    {
        const size_t n = (size_t)m_w * m_h;
        WriteRaw(m_prefix + "_noisy.f16", m_noisy.data(),  m_noisy.size()  * 2);
        WriteRaw(m_prefix + "_clean.f32", m_clean.data(),  m_clean.size()  * 4);
        WriteRaw(m_prefix + "_albedo.u8", m_albedo.data(), m_albedo.size());

        std::vector<uint8_t> png(n * 4);
        for (size_t i = 0; i < n; ++i)
            ComposeLDR(HalfToFloat(m_noisy[i * 4]), HalfToFloat(m_noisy[i * 4 + 1]), HalfToFloat(m_noisy[i * 4 + 2]),
                       &m_albedo[i * 4], &png[i * 4]);
        stbi_write_png((m_prefix + "_noisy.png").c_str(), (int)m_w, (int)m_h, 4, png.data(), (int)m_w * 4);
        for (size_t i = 0; i < n; ++i)
            ComposeLDR(m_clean[i * 3], m_clean[i * 3 + 1], m_clean[i * 3 + 2], &m_albedo[i * 4], &png[i * 4]);
        stbi_write_png((m_prefix + "_clean.png").c_str(), (int)m_w, (int)m_h, 4, png.data(), (int)m_w * 4);
    }

    void OnComplete() override { m_owner->OnSampleWritten(); }
};

void DenoiseCapture::Start(std::string const& outDir, int numPoses, int cleanFrames)
{
    m_dir         = outDir;
    m_numPoses    = numPoses;
    m_cleanFrames = cleanFrames;
    m_pose        = 0;
    m_written     = 0;
    m_w = m_h     = 0;
    std::filesystem::create_directories(m_dir);
    std::filesystem::remove(m_dir + "/poses.txt");
    NextPose();
}

void DenoiseCapture::NextPose()
{
    // Inside the nave, where the lights are (App.cpp places them in
    // x [-14,14], y [-5,5]); kept a bit off the colonnades.
    m_camPos = Vec3(m_rng.RollRandomFloatInRange(-12.f, 12.f),
                    m_rng.RollRandomFloatInRange(-3.5f, 3.5f),
                    m_rng.RollRandomFloatInRange(0.4f, 4.f));
    m_camOri = EulerAngles(m_rng.RollRandomFloatInRange(0.f, 360.f),
                           m_rng.RollRandomFloatInRange(-25.f, 25.f), 0.f);
    m_frame = 0;

    FILE* f = nullptr;
    if (fopen_s(&f, (m_dir + "/poses.txt").c_str(), "a") == 0 && f)
    {
        fprintf(f, "%d %.4f %.4f %.4f %.3f %.3f\n", m_pose, m_camPos.x, m_camPos.y, m_camPos.z,
                m_camOri.m_yawDegrees, m_camOri.m_pitchDegrees);
        fclose(f);
    }
}

void DenoiseCapture::Update(const VulkanRTPath::DenoiseFrame& frame, Vec3& camPos, EulerAngles& camOri)
{
    if (!IsActive()) return;
    camPos = m_camPos;
    camOri = m_camOri;

    if (m_w != frame.width || m_h != frame.height)
    {
        // First frame, or the window was resized mid-capture: restart this pose.
        m_w = frame.width;
        m_h = frame.height;
        m_frame = 0;
        FILE* f = nullptr;
        if (fopen_s(&f, (m_dir + "/info.txt").c_str(), "w") == 0 && f)
        {
            fprintf(f, "%u %u %d\n", m_w, m_h, m_cleanFrames);
            fclose(f);
        }
    }

    const int f = m_frame++;
    if (f < kWarmupFrames || !frame.light) return;

    const size_t n = (size_t)m_w * m_h;
    if (f == kWarmupFrames)
    {
        m_noisy.assign(frame.light, frame.light + n * 4);
        m_albedo.assign(frame.albedo, frame.albedo + n * 4);
        m_sum.assign(n * 3, 0.f);
        return;
    }

    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c)
            m_sum[i * 3 + c] += HalfToFloat(frame.light[i * 4 + c]);

    if (f - kWarmupFrames < m_cleanFrames) return;

    const float inv = 1.f / (float)m_cleanFrames;
    for (float& v : m_sum) v *= inv;

    CaptureWriteJob* job = new CaptureWriteJob();
    job->m_owner  = this;
    job->m_prefix = Stringf("%s/%04d", m_dir.c_str(), m_pose);
    job->m_w = m_w;
    job->m_h = m_h;
    job->m_noisy.swap(m_noisy);
    job->m_albedo.swap(m_albedo);
    job->m_clean.swap(m_sum);
    g_theJobSystem->AddPendingJob(job);

    if (++m_pose < m_numPoses) NextPose();
}

std::string DenoiseCapture::Status() const
{
    if (IsActive())
        return Stringf(" CAPTURE: pose %d/%d  frame %d/%d  written %d",
                       m_pose + 1, m_numPoses, m_frame, kWarmupFrames + m_cleanFrames, m_written);
    if (m_numPoses > 0)
        return Stringf(" CAPTURE: done, %d/%d written to %s", m_written, m_numPoses, m_dir.c_str());
    return "";
}
