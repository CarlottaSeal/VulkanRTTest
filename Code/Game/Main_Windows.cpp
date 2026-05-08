#define WIN32_LEAN_AND_MEAN		// Always #define this before #including <windows.h>
#include <windows.h>			// #include this (massive, platform-specific) header in VERY few places (and .CPPs only)
#include <math.h>
#include <cassert>
#include <crtdbg.h>
#include <direct.h>				// _chdir
#include <string>
#include "Game/App.hpp"
#include "Game/EngineBuildPreferences.hpp"
#include "Game/Gamecommon.hpp"

#define UNUSED(x) (void)(x);

constexpr float CLIENT_ASPECT = 2.0f; // We are requesting a 2:1 aspect (square) window area



int WINAPI WinMain(HINSTANCE applicationInstanceHandle, HINSTANCE, LPSTR commandLineString, int)
{
	UNUSED(commandLineString);
	UNUSED(applicationInstanceHandle);

	// Asset paths in this engine are relative ("Data/..."). VS may launch the
	// exe from build/Temporary, not from Run/. Walk up from the exe's directory
	// looking for a sibling Run/Data/ (or just Data/), then _chdir into the
	// directory that contains it. Robust to any launcher.
	{
		char exePath[MAX_PATH] = {0};
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		std::string dir = exePath;
		size_t slash = dir.find_last_of("\\/");
		if (slash != std::string::npos) dir.resize(slash);

		auto exists = [](const std::string& p) -> bool {
			DWORD a = GetFileAttributesA(p.c_str());
			return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
		};

		bool found = false;
		std::string current = dir;
		for (int up = 0; up < 6 && !found; ++up)
		{
			if (exists(current + "\\Data"))
			{
				_chdir(current.c_str());
				found = true;
			}
			else if (exists(current + "\\Run\\Data"))
			{
				_chdir((current + "\\Run").c_str());
				found = true;
			}
			else
			{
				size_t s = current.find_last_of("\\/");
				if (s == std::string::npos) break;
				current.resize(s);
			}
		}

		char cwd[MAX_PATH] = {0};
		_getcwd(cwd, MAX_PATH);
		OutputDebugStringA(("[chdir] exe='" + std::string(exePath) +
		                    "' cwd='" + cwd +
		                    "' found=" + (found ? "YES" : "NO") + "\n").c_str());
	}

	g_theApp = new App();// #SD1ToDo: g_theApp = new App();
	g_theApp->Startup();
	
	while (!g_theApp->IsQuitting())			// #SD1ToDo: ...becomes:  !g_theApp->IsQuitting()
	{
		g_theApp->RunFrame(); // #SD1ToDo: g_theApp->RunFrame();
	}
	g_theApp->Shutdown();
	delete g_theApp;
	g_theApp = nullptr;

	return 0;
}
