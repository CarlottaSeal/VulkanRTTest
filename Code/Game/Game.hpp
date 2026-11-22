#pragma once
#include "Game/Gamecommon.hpp"
#include "Game/App.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Math/AABB2.hpp"
#include "Engine/Core/Vertex_PCU.hpp"
#include "Engine/Audio/AudioSystem.hpp"

#include <vector>

class Clock;
class Entity;

class Game 
{
public:
	Game();
	~Game();

	void Update();
	void Render() const;

	void AdjustForPauseAndTimeDistortion();

public:
	bool m_openDevConsole = false;
	bool m_isInAttractMode;
	Clock* m_gameClock;
	//Camera m_gameCamera;
	Camera m_screenCamera;

	Entity* m_player;

private:
	void AttractModeUpdate();
	void AttractModeRender() const;
	void PrintGameControlToDevConsole();
	void DebugRenderSystemInputUpdate();

private:
	bool m_useDenoiser       = true;  // B: neural denoiser
	bool m_denoiseOnGpu      = true;  // G: Vulkan compute KPCN vs ONNX Runtime on the CPU
public:
	// L: wait for the GPU at the end of every frame. With the GPU saturated back to back,
	// the denoiser passes intermittently stall for ~5 frames (DRAM write storm, 20 FPS);
	// the small idle gap this leaves avoided most of them in testing for ~2% FPS.
	bool m_syncGpuEachFrame  = false;
private:
	mutable uint32_t m_rtFrameId = 0;
	bool m_isSlowMo;
	bool m_isUsingUserTimeScale;

	float m_userTimeScale;

	bool m_hasPlayedAttractSound = false;
	SoundPlaybackID m_attractSoundID = MISSING_SOUND_ID;

	float m_varyTime = 0.f;
};




