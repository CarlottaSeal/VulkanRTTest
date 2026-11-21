#include "Game.hpp"

// stb_image_write impl lives in DX12Renderer.cpp for DX12 builds; for the Vulkan-only
// build it has to be defined in exactly one TU here (DenoiseCapture writes PNGs).
#pragma warning(push)
#pragma warning(disable: 4996)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ThirdParty/stb/stb_image_write.h"
#pragma warning(pop)

#include "Engine/Math/RandomNumberGenerator.hpp"
#include "Engine/Math/AABB2.hpp"
#include "Engine/Math/Mat44.hpp"
#include "Engine/Math/MathUtils.hpp"
#include "Engine/Core/ErrorWarningAssert.hpp"
#include "Engine/Core/Rgba8.hpp"
#include "Engine/Core/Clock.hpp"
#include "Engine/Core/DebugRenderSystem.hpp"
#include "Engine/Renderer/BitmapFont.hpp"
#include "Engine/Renderer/Camera.hpp"

#include <cmath>
#include "Engine/Renderer/VulkanRenderer.h"
#include "Game/NeuralDenoiser.hpp"
#include "Game/DenoiseCapture.hpp"
#include "Engine/Job/JobSystem.h"

extern NeuralDenoiser* g_theDenoiser;
extern DenoiseCapture* g_theDenoiseCapture;
#include "Engine/Renderer/VulkanDeferredPath.h"
#include "Engine/Renderer/VulkanRTPath.h"
#include "Engine/Audio/AudioSystem.hpp"

#include "Game/Entity.hpp"
#include "Game/Player.hpp"
#include "Game/Gamecommon.hpp"
#include "Game/EngineBuildPreferences.hpp"

RandomNumberGenerator g_RNG;
//extern AudioSystem* g_theAudio;
extern Clock* s_theSystemClock;

Game::Game()
{
	m_isInAttractMode = true;
	m_screenCamera.SetOrthographicView(Vec2(0.f, 0.f), Vec2(1600.f, 800.f));

	m_player = static_cast<Entity*>(new Player(this));

	((Player*)m_player)->m_worldCamera.SetCameraMode(Camera::CameraMode::eMode_Perspective);
	((Player*)m_player)->m_worldCamera.SetPerspectiveView(2.f, 60.f, 0.1f, 100.f);
	Mat44 mat;
	mat.SetIJK3D(Vec3(0.f, 0.f,1.f), Vec3(-1.f, 0.f, 0.f), Vec3(0.f, 1.f, 0.f));
	((Player*)m_player)->m_worldCamera.SetCameraToRenderTransform(mat);

	PrintGameControlToDevConsole();
}

Game::~Game()
{
	delete m_player;
	m_player = nullptr;
}

void Game::Update()
{
	if (g_theApp->WasKeyJustPressed(KEYCODE_ESC)
		|| g_theInput->GetController(0).WasButtonJustPressed(XboxButtonID::X))
	{
		if (m_isInAttractMode)
		{
			g_theApp->g_isQuitting = true;
		}
		if (!m_isInAttractMode)
		{
			m_isInAttractMode = true;
		}
	}

	AdjustForPauseAndTimeDistortion();

	if (g_theJobSystem)
	{
		for (Job* job : g_theJobSystem->RetrieveCompletedJobs())
		{
			job->OnComplete();
			delete job;
		}
	}

	if (m_isInAttractMode)
	{
		AttractModeUpdate();

		if (!m_hasPlayedAttractSound)
		{
			//SoundID Attract = g_theAudio->CreateOrGetSound("Data/Audio/Attract.MP3");
			//m_attractSoundID = g_theAudio->StartSound(Attract, false, 1.0f, 0.5f, 1.0f, false);
			m_hasPlayedAttractSound = true;
		}
	}

	if (!m_isInAttractMode)
	{
		m_player->Update((float)s_theSystemClock->GetDeltaSeconds());
		DebugRenderSystemInputUpdate();
	}

	if (g_theApp->WasKeyJustPressed('B'))
	{
		m_useDenoiser = !m_useDenoiser;
	}
	if (g_theApp->WasKeyJustPressed('G'))
	{
		m_denoiseOnGpu = !m_denoiseOnGpu;
	}

	if (g_theDevConsole->GetMode() == OPEN_FULL)
	{
		m_openDevConsole = true;
	}
	if (g_theDevConsole->GetMode() == HIDDEN)
	{
		m_openDevConsole = false;
	}

	m_varyTime += (float)m_gameClock->GetDeltaSeconds();
	if (m_varyTime > 360.f)
	{
		m_varyTime = 0.f;
	}
}

void Game::Render() const
{
	if (m_isInAttractMode)
	{
		g_theRenderer->ClearScreen();
		g_theRenderer->BeginCamera(m_screenCamera);
		g_theRenderer->BindTexture(nullptr);
		g_theRenderer->SetBlendMode(BlendMode::ALPHA);
		g_theRenderer->SetRasterizerMode(RasterizerMode::SOLID_CULL_NONE);
		AttractModeRender();
		g_theRenderer->EndCamera(m_screenCamera);
		g_theDevConsole->Render(AABB2(m_screenCamera.GetOrthographicBottomLeft(), m_screenCamera.GetOrthographicTopRight()), g_theRenderer);
		return;
	}

	VulkanRenderer* vk = g_theRenderer->GetSubRenderer();
	VkCommandBuffer cmd = vk->GetCurrentCommandBuffer();
	uint32_t swapIdx = vk->GetCurrentSwapImageIndex();

	VulkanRTPath::DenoiseFrame dn = g_theRTPath->BeginDenoiseFrame();
	const bool capturing = g_theDenoiseCapture && g_theDenoiseCapture->IsActive();
	if (capturing)
		g_theDenoiseCapture->Update(dn, m_player->m_position, m_player->m_orientation);

	Vec3 fwd, left, up;
	m_player->m_orientation.GetAsVectors_IFwd_JLeft_KUp(fwd, left, up);
	Vec3 right = -1.0f * left;
	Vec3 eye   = m_player->m_position;

	const float fovTan = 0.577f;
	const float aspect = 2.0f;
	const float eyeArr[3] = { eye.x,   eye.y,   eye.z   };
	const float fwdArr[3] = { fwd.x,   fwd.y,   fwd.z   };
	const float rgtArr[3] = { right.x, right.y, right.z };
	const float upArr[3]  = { up.x,    up.y,    up.z    };
	extern uint32_t g_rtNumLights;
	g_theRTPath->UpdateCameraVectors(eyeArr, fwdArr, rgtArr, upArr,
	                                  fovTan, aspect,
	                                  m_rtFrameId++, g_rtNumLights);

	IntVec2 winDim = g_theWindow->GetClientDimensions();
	const uint32_t w = (uint32_t)winDim.x;
	const uint32_t h = (uint32_t)winDim.y;
	g_theRTPath->TraceRays(cmd, w, h);
	const bool gpuDenoise = !capturing && m_useDenoiser && m_denoiseOnGpu && g_theRTPath->HasNeuralDenoiser();
	if (gpuDenoise)
	{
		g_theRTPath->RunNeuralDenoise(cmd);
	}
	else
	{
		bool denoised = !capturing && m_useDenoiser && !m_denoiseOnGpu && g_theDenoiser && g_theDenoiser->Update(dn);
		g_theRTPath->EndDenoiseFrame(cmd, denoised);
	}
	g_theRTPath->BlitToSwapImage(cmd, w, h, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	char hudBuf[160];
	snprintf(hudBuf, sizeof(hudBuf),
	         " pos: %.1f %.1f %.1f   yaw: %.1f  pitch: %.1f",
	         m_player->m_position.x, m_player->m_position.y, m_player->m_position.z,
	         m_player->m_orientation.m_yawDegrees, m_player->m_orientation.m_pitchDegrees);
	DebugAddScreenText(hudBuf,
	                   m_screenCamera.GetOrthographicTopRight() - Vec2(580.f, 50.f),
	                   12.f, Vec2::ZERO, 0.f);

	char fpsBuf[96];
	snprintf(fpsBuf, sizeof(fpsBuf),
	         " FPS: %.1f   dt: %.2f ms",
	         (float)m_gameClock->GetFrameRate(),
	         (float)m_gameClock->GetDeltaSeconds() * 1000.f);
	DebugAddScreenText(fpsBuf,
	                   m_screenCamera.GetOrthographicTopRight() - Vec2(580.f, 70.f),
	                   12.f, Vec2::ZERO, 0.f);

	char dnBuf[128];
	if (!m_useDenoiser)
		snprintf(dnBuf, sizeof(dnBuf), " DENOISE [B]: off");
	else if (m_denoiseOnGpu)
		snprintf(dnBuf, sizeof(dnBuf), " DENOISE [B]: GPU %.2f ms [G]",
		         g_theRTPath->GetNeuralDenoiseMs());
	else if (g_theDenoiser)
		snprintf(dnBuf, sizeof(dnBuf), " DENOISE [B]: CPU %.1f ms [G]",
		         g_theDenoiser->LastInferenceMs());
	else
		snprintf(dnBuf, sizeof(dnBuf), " DENOISE [B]: on   [G] CPU: no model");
	DebugAddScreenText(dnBuf,
	                   m_screenCamera.GetOrthographicTopRight() - Vec2(580.f, 90.f),
	                   12.f, Vec2::ZERO, 0.f);
	if (m_useDenoiser && m_denoiseOnGpu)
	{
		const float* t = g_theRTPath->GetNeuralDenoisePassMs();
		char passBuf[128];
		snprintf(passBuf, sizeof(passBuf), " in %.2f mid %.2f %.2f %.2f out %.2f tmp %.2f",
		         t[0], t[1], t[2], t[3], t[4], t[5]);
		DebugAddScreenText(passBuf,
		                   m_screenCamera.GetOrthographicTopRight() - Vec2(580.f, 130.f),
		                   12.f, Vec2::ZERO, 0.f);
	}
	if (g_theDenoiseCapture && !g_theDenoiseCapture->Status().empty())
		DebugAddScreenText(g_theDenoiseCapture->Status(),
		                   m_screenCamera.GetOrthographicTopRight() - Vec2(580.f, 110.f),
		                   12.f, Vec2::ZERO, 0.f);

	// The overlay pass (HUD + dev console on top of the blitted RT image) lives in VulkanDeferredPath.
	g_theDeferred->BeginForwardOverlay(cmd, swapIdx);
	DebugRenderScreen(m_screenCamera);
	g_theDevConsole->Render(AABB2(m_screenCamera.GetOrthographicBottomLeft(),
	                              m_screenCamera.GetOrthographicTopRight()),
	                        g_theRenderer);
	g_theDeferred->EndForwardOverlay(cmd);
}

void Game::AdjustForPauseAndTimeDistortion()
{
	if (g_theApp->WasKeyJustPressed('T'))
	{
		m_isSlowMo = !m_isSlowMo;
		m_isUsingUserTimeScale = false;
	}
	if (m_isUsingUserTimeScale) // 如果用户通过 set_time_scale 设定了时间缩放，保持该值
	{
		m_gameClock->SetTimeScale(m_userTimeScale);
	}
	else
	{
		m_gameClock->SetTimeScale(m_isSlowMo ? 0.1f : 1.0f);
	}

	if (g_theApp->WasKeyJustPressed('P'))
	{
		m_gameClock->TogglePause();
		if (!m_gameClock->IsPaused())
		{
			m_gameClock->Unpause();
			m_gameClock->Reset();
			//SoundID Pause = g_theAudio->CreateOrGetSound("Data/Audio/Pause.mp3");
			//g_theAudio->StartSound(Pause, false, 0.05f, 0.5f, 1.f, false);
		}
		if (m_gameClock->IsPaused())
		{
			m_gameClock->Reset();
			//SoundID Unpause = g_theAudio->CreateOrGetSound("Data/Audio/Unpause.mp3");
			//g_theAudio->StartSound(Unpause, false, 0.05f, 0.5f, 1.f, false);
		}
	}

	if (g_theApp->WasKeyJustPressed('O'))
	{
		m_gameClock->StepSingleFrame();
	}
}

void Game::AttractModeUpdate()
{
	if (g_theApp->IsKeyDown(' ') ||
		g_theApp->IsKeyDown('N') ||
		g_theInput->GetController(0).WasButtonJustPressed(XboxButtonID::START) ||
		g_theInput->GetController(0).WasButtonJustPressed(XboxButtonID::A))
	{
		m_isInAttractMode = false;
	}
}

void Game::AttractModeRender() const
{
	DebugDrawRing(Vec2(800.f, 400.f), 100.f, 10.f + sinf(m_varyTime) * 10.f, Rgba8::YELLOW);
}

void Game::PrintGameControlToDevConsole()
{
	g_theDevConsole->AddLine(Rgba8::BLUE, "Type help for a list of commands");
	g_theDevConsole->AddLine(Rgba8::CYAN, "Mouse x-axis Right stick x-axis Yaw");
	g_theDevConsole->AddLine(Rgba8::CYAN, "Mouse y-axis Right stick y-axis Pitch");
	g_theDevConsole->AddLine(Rgba8::CYAN, "Q / E Left trigger / right trigger Roll");
	g_theDevConsole->AddLine(Rgba8::CYAN, "A / D Left stick x-axis Move left or right");
	g_theDevConsole->AddLine(Rgba8::CYAN, "W / S Left stick y-axis Move forward or back");
	g_theDevConsole->AddLine(Rgba8::CYAN, "Z / C Left shoulder / right shoulder Move down or up");
	g_theDevConsole->AddLine(Rgba8::CYAN, "H / Start button Reset position and orientation to zero");
	g_theDevConsole->AddLine(Rgba8::CYAN, "Shift / A button Increase speed by a factor of 10 while held");
	g_theDevConsole->AddLine(Rgba8::CYAN, "B - Toggle denoiser");
	g_theDevConsole->AddLine(Rgba8::CYAN, "G - Denoise on GPU / CPU");
	g_theDevConsole->AddLine(Rgba8::CYAN, "P - Pause the game");
	g_theDevConsole->AddLine(Rgba8::CYAN, "O - Single step frame");
	g_theDevConsole->AddLine(Rgba8::CYAN, "T - Slow motion mode");
	g_theDevConsole->AddLine(Rgba8::CYAN, "ESC - Exit game");
}

void Game::DebugRenderSystemInputUpdate()
{
	Vec3 pos = m_player->m_position;
	std::string reportHUD = " Player Position: " +
		RoundToOneDecimalString(pos.x) + ", " + RoundToOneDecimalString(pos.y) + ", " + RoundToOneDecimalString(pos.z);
	DebugAddMessage(reportHUD, 0.f, m_screenCamera);

	float timeTotal = (float)m_gameClock->GetTotalSeconds();
	float fps = (float)m_gameClock->GetFrameRate();
	float timeScale = m_gameClock->GetTimeScale();
	std::string timeReportHUD = " Time: " + RoundToTwoDecimalsString(timeTotal)
	+ " FPS: " + RoundToOneDecimalString(fps) + " Scale: " + RoundToTwoDecimalsString(timeScale);
	float textWidth = GetTextWidth(12.f, timeReportHUD, 0.7f);
	DebugAddScreenText(timeReportHUD, m_screenCamera.GetOrthographicTopRight() - Vec2(textWidth + 1.f, 15.f), 12.f, Vec2::ZERO, 0.f);
}
