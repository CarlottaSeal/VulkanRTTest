#include "Engine/Window/Window.hpp"
#include "Engine/Renderer/Renderer.hpp"
#include "Engine/Renderer/VulkanRenderer.h"
#include "Engine/Renderer/VulkanDeferredPath.h"
#include "Engine/Renderer/VulkanRTPath.h"
#include "Engine/Renderer/Camera.hpp"
#include <array>
#include <map>
#include <string>
#include "Engine/Renderer/SimpleTriangleFont.hpp"
#include "Engine/Input/InputSystem.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Core/ErrorWarningAssert.hpp"
#include "Engine/Core/VertexUtils.hpp"
#include "Engine/Core/Clock.hpp"
#include "Engine/Core/DebugRenderSystem.hpp"
#include "Engine/Input/KeyButtonState.hpp"
#include "Engine/Input/InputSystem.hpp"
#include "Engine/Audio/AudioSystem.hpp"

#include "Game/App.hpp"
#include "Game/Game.hpp"
#include "Game/Gamecommon.hpp"
#include "Game/EngineBuildPreferences.hpp"

#include <math.h>

App* g_theApp = nullptr;
Renderer* g_theRenderer = nullptr;
VulkanDeferredPath* g_theDeferred = nullptr;
VulkanRTPath* g_theRTPath = nullptr;
InputSystem* g_theInput = nullptr;
//AudioSystem* g_theAudio = nullptr;
Window* g_theWindow = nullptr;
Game* g_theGame = nullptr;

App::App()
{
}

App::~App()
{
}

void App::Startup()
{
	//Parse GameConfig
	XmlDocument gameConfigDoc;
	XmlResult loadResult = gameConfigDoc.LoadFile("Data/GameConfig.xml");
	UNUSED(loadResult)
	/*if (loadResult != XmlResult::XML_SUCCESS)
	{
		return;
	}*/
	XmlElement* rootElement = gameConfigDoc.RootElement();
	if (rootElement)
	{
		g_gameConfigBlackboard.PopulateFromXmlElementAttributes(*rootElement);

	}
	
	//Create all engine subsystems
	InputSystemConfig inputConfig;
	g_theInput = new InputSystem(inputConfig);

	WindowConfig windowConfig;
	windowConfig.m_aspectRatio = 2.f;
	windowConfig.m_inputSystem = g_theInput;
	windowConfig.m_windowTitle = g_gameConfigBlackboard.GetValue("windowTitle", "Protogame3D");
	windowConfig.m_isFullscreen = g_gameConfigBlackboard.GetValue("windowFullscreen", false);
	g_theWindow = new Window(windowConfig);

	RendererConfig rendererConfig;
	rendererConfig.m_window = g_theWindow;
	g_theRenderer = new Renderer(rendererConfig);

	EventSystemConfig eventSystemConfig;
	g_theEventSystem = new EventSystem(eventSystemConfig);

	DevConsoleConfig devConsoleConfig;
	devConsoleConfig.m_defaultRenderer = g_theRenderer;
	devConsoleConfig.m_defaultFontName = "SquirrelFixedFont";
	Camera* devCamera = new Camera();
	devCamera->SetOrthographicView(Vec2(0.f, 0.f), Vec2(1600.f, 800.f));
	//devConsoleConfig.m_camera = &g_theGame->m_screenCamera;
	devConsoleConfig.m_camera = devCamera;
	g_theDevConsole = new DevConsole(devConsoleConfig);

	/*AudioSystemConfig audioSystemConfig;
	g_theAudio = new AudioSystem(audioSystemConfig);*/

	DebugRenderConfig debugRenderConfig;
	debugRenderConfig.m_renderer = g_theRenderer;

	g_theWindow->Startup();
	g_theRenderer->Startup();
	//g_theAudio->Startup();
	g_theEventSystem->StartUp();
	g_theDevConsole->Startup();
	g_theInput->Startup();
	DebugRenderSystemStartup(debugRenderConfig);

#ifdef ENGINE_VULKAN_RENDERER
	g_theDeferred = new VulkanDeferredPath();
	g_theDeferred->Init(g_theRenderer->GetSubRenderer());

	{
		VulkanRenderer* vk = g_theRenderer->GetSubRenderer();
		g_theRTPath = new VulkanRTPath();
		g_theRTPath->Init(vk);

		IntVec2 winDim = g_theWindow->GetClientDimensions();
		g_theRTPath->RecreateOutput((uint32_t)winDim.x, (uint32_t)winDim.y);

		std::vector<float>    objVerts;
		std::vector<uint32_t> objIndices;
		std::vector<uint32_t> triMatIds;
		std::vector<float>    matColorsRGB;
		std::map<std::string, uint32_t> matNameToId;
		{
			FILE* fp = nullptr;
			fopen_s(&fp, "Data/Models/Sponza/sponza.obj", "r");
			if (!fp) ERROR_AND_DIE("App::Startup: failed to open Data/Models/Sponza/sponza.obj");

			// Slot 0 is a fallback for triangles emitted before any usemtl line.
			uint32_t currentMatId = 0;
			matColorsRGB.push_back(0.7f); matColorsRGB.push_back(0.7f); matColorsRGB.push_back(0.7f);

			char line[1024];
			while (fgets(line, sizeof(line), fp))
			{
				if (line[0] == 'v' && line[1] == ' ')
				{
					float x, y, z;
					if (sscanf_s(line + 2, "%f %f %f", &x, &y, &z) == 3)
					{
						objVerts.push_back(x);
						objVerts.push_back(y);
						objVerts.push_back(z);
					}
				}
				else if (line[0] == 'f' && line[1] == ' ')
				{
					unsigned int v1, v2, v3;
					if (sscanf_s(line + 2, "%u/%*u/%*u %u/%*u/%*u %u/%*u/%*u", &v1, &v2, &v3) == 3 ||
					    sscanf_s(line + 2, "%u//%*u %u//%*u %u//%*u",          &v1, &v2, &v3) == 3 ||
					    sscanf_s(line + 2, "%u/%*u %u/%*u %u/%*u",             &v1, &v2, &v3) == 3 ||
					    sscanf_s(line + 2, "%u %u %u",                         &v1, &v2, &v3) == 3)
					{
						objIndices.push_back(v1 - 1);
						objIndices.push_back(v2 - 1);
						objIndices.push_back(v3 - 1);
						triMatIds.push_back(currentMatId);
					}
				}
				else if (strncmp(line, "usemtl ", 7) == 0)
				{
					std::string name(line + 7);
					while (!name.empty() && (name.back() == '\r' || name.back() == '\n' ||
					                         name.back() == ' '  || name.back() == '\t'))
						name.pop_back();
					auto it = matNameToId.find(name);
					if (it == matNameToId.end())
					{
						currentMatId = (uint32_t)(matColorsRGB.size() / 3);
						matNameToId[name] = currentMatId;

						// MTL Kd is uniform gray (color lives in textures we don't load yet);
						// hash the material name to a distinct palette slot.
						uint32_t h = 0x811C9DC5u;
						for (char ch : name) { h ^= (uint8_t)ch; h *= 0x01000193u; }
						auto chan = [&](uint32_t bits) {
							float f = (float)((h ^ bits) & 0xFFFFFFu) / (float)0xFFFFFFu;
							return 0.35f + 0.55f * f;
						};
						matColorsRGB.push_back(chan(0xC0FFEE));
						matColorsRGB.push_back(chan(0xBADF00));
						matColorsRGB.push_back(chan(0x1337AA));
					}
					else
					{
						currentMatId = it->second;
					}
				}
			}
			fclose(fp);
		}

		static VulkanBLAS s_sponzaBLAS;
		s_sponzaBLAS = g_theRTPath->BuildBLAS(
			objVerts.data(), (uint32_t)(objVerts.size() / 3),
			objIndices.data(), (uint32_t)objIndices.size());

		g_theRTPath->SetMaterialBuffers(
			matColorsRGB.data(), (uint32_t)(matColorsRGB.size() / 3),
			triMatIds.data(),    (uint32_t)triMatIds.size());

		// Crytek OBJ axes (Y-up, +X-right, +Z-toward-viewer) → engine
		// (X-fwd, Y-left, Z-up), uniform 0.01 scale.
		const float s = 0.01f;
		VkAccelerationStructureInstanceKHR sponzaInst{};
		sponzaInst.transform.matrix[0][0] =  s;
		sponzaInst.transform.matrix[1][2] = -s;
		sponzaInst.transform.matrix[2][1] =  s;
		sponzaInst.instanceCustomIndex                    = 0;
		sponzaInst.mask                                   = 0xFF;
		sponzaInst.instanceShaderBindingTableRecordOffset = 0;
		sponzaInst.flags                                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		sponzaInst.accelerationStructureReference         = s_sponzaBLAS.address;

		static VulkanTLAS s_sceneTLAS;
		s_sceneTLAS = g_theRTPath->BuildTLAS({ sponzaInst });

		g_theRTPath->CreateRTPipeline("Data/Shaders/Vulkan/rt/raygen.rgen.spv",
		                              "Data/Shaders/Vulkan/rt/closesthit.rchit.spv",
		                              "Data/Shaders/Vulkan/rt/miss.rmiss.spv",
		                              "Data/Shaders/Vulkan/rt/shadowmiss.rmiss.spv");
		g_theRTPath->CreateSBT();
		g_theRTPath->UpdateDescriptors(s_sceneTLAS, s_sponzaBLAS);
	}
#endif

	g_theGame = new Game();
	g_theGame->m_gameClock = new Clock(Clock::GetSystemClock());

	g_theEventSystem->SubscribeEventCallBackFunction("quit", OnQuitEvent);

	g_theDevConsole->AddLine(Rgba8::BLUE, "Type help for a list of commands");
}

void App::Shutdown()
{
	delete g_theGame;
	g_theGame = nullptr;

#ifdef ENGINE_VULKAN_RENDERER
	if (g_theRTPath)   { g_theRTPath->Shutdown();   delete g_theRTPath;   g_theRTPath   = nullptr; }
	if (g_theDeferred) { g_theDeferred->Shutdown(); delete g_theDeferred; g_theDeferred = nullptr; }
#endif

	g_theEventSystem->Shutdown();
	//g_theAudio->Shutdown();
	g_theRenderer->ShutDown();
	g_theWindow->Shutdown();
	g_theInput->Shutdown();
	g_theDevConsole->Shutdown();

	DebugRenderSystemShutdown();

	delete g_theDevConsole;
	g_theDevConsole = nullptr;

	delete g_theEventSystem;
	g_theEventSystem = nullptr;

	/*delete g_theAudio;
	g_theAudio = nullptr;*/

	delete g_theRenderer;
	g_theRenderer = nullptr;

	delete g_theWindow;
	g_theWindow = nullptr;

	delete g_theInput;
	g_theInput = nullptr;

}

void App::BeginFrame()
{
	Clock::TickSystemClock();
	g_theWindow->BeginFrame();
	g_theInput->BeginFrame();
	g_theRenderer->BeginFrame();
	//g_theAudio->BeginFrame();
	g_theEventSystem->BeginFrame();
	g_theDevConsole->BeginFrame();

	DebugRenderBeginFrame();
}

bool App::IsKeyDown(unsigned char keyCode) const
{
	return g_theInput->IsKeyDown(keyCode);
	//return m_keystates[keyCode].IsPressed();
	//return m_currentKeyStates[keyCode]; // 当前帧按键状态
}

bool App::WasKeyJustPressed(unsigned char keyCode) const
{
	return g_theInput->WasKeyJustPressed(keyCode);
	//return m_keystates[keyCode].WasJustPressed();
	//return m_currentKeyStates[keyCode] && !m_previousKeyStates[keyCode]; // 当前帧按下，上一帧未按下
}

void App::HandleKeyPressed(unsigned char keyCode)
{
	g_theInput->HandleKeyPressed(keyCode);
	//m_keystates[keyCode].UpdateStatus(true);
	//m_currentKeyStates[keyCode] = true;
}

void App::HandleKeyReleased(unsigned char keyCode)
{
	g_theInput->HandleKeyReleased(keyCode);
	//m_keystates[keyCode].UpdateStatus(false);
	m_currentKeyStates[keyCode] = false;
}

bool App::IsKeyReleased(unsigned char keyCode) const
{
	return 	m_currentKeyStates[keyCode];
}

void App::HandleQuitRequested()
{
	g_isQuitting = true;
}

//-----------------------------------------------------------------------------------------------
// One "frame" of the game.  Generally: Input, Update, Render.  We call this 60+ times per second.
// #SD1ToDo: Move this function to Game/App.cpp and rename it to  TheApp::RunFrame()

void App::RunFrame()
{
	//float timeNow = static_cast<float>(GetCurrentTimeSeconds());
	//float deltaSeconds = timeNow - m_timeLastFrameStart;
	////DebuggerPrintf("TimeNow = %.06f\n, TimeNow");
	//m_timeLastFrameStart = timeNow;

	BeginFrame();
	Update();
	Render();
	EndFrame();
}

void App::Update()
{
	if (WasKeyJustPressed(KEYCODE_F8))
	{
		delete g_theGame;
		g_theGame = new Game();
	}

	g_theGame->Update();

	UpdateCursor();
}

void App::Render() const
{
	g_theGame->Render();
}

void App::EndFrame()
{
	// let renderer deal with buffers
	g_theWindow->EndFrame();
	g_theRenderer->EndFrame();
	g_theInput->EndFrame();
	//g_theAudio->EndFrame();
	g_theEventSystem->EndFrame();
	g_theDevConsole->EndFrame();

	for (int i = 0; i < 256; ++i)
	{
		m_keystates[i].EndFrame();
	}

	DebugRenderEndFrame();
}

void App::UpdateCursor()
{
	if (g_theGame->m_isInAttractMode || g_theGame->m_openDevConsole || !g_theWindow->WindowHasFocus())
	{
		g_theInput->SetCursorMode(CursorMode::POINTER);
	}
	else
	{
		g_theInput->SetCursorMode(CursorMode::FPS);
	}
}

bool OnQuitEvent(EventArgs& args)
{
	UNUSED(args);
	g_theApp->HandleQuitRequested();
	return true;
}