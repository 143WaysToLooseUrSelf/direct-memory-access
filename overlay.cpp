#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <thread>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <Windows.h>

#include "overlay.h"
#include "font.h"
#include "settings.h"
#include "xorstr.h"

#include "Imgui/byte.h"
#include "Imgui/elements.h"

// Defined in main.cpp
extern void Log(const char* msg);
extern void Log(const std::string& msg);

enum heads {
	HEAD_1,
	HEAD_2,
	HEAD_3,
	HEAD_4,
	HEAD_5
};

enum subheads {
	SUBHEAD_1,
	SUBHEAD_2,
	SUBHEAD_3,
	SUBHEAD_4,
	SUBHEAD_5,
	SUBHEAD_6,
	SUBHEAD_7,
	SUBHEAD_8,
	SUBHEAD_9,
	SUBHEAD_10,
	SUBHEAD_11,
	SUBHEAD_12,
	SUBHEAD_13,
	SUBHEAD_14
};

// System-wide hotkey so the menu still toggles when the game consumes raw keyboard input.
static constexpr int kMenuHotkeyId = 1;

static bool s_startupBlackPending = false;
static HWND s_taskbarWnd = nullptr;
static HWND s_secondaryTrayWnd = nullptr;
static bool s_hidTaskbar = false;

static void AtExitRestoreTaskbar()
{
	// Always resolve HWND again — stale handles after explorer/DWM changes are common.
	HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
	if (tray && IsWindow(tray)) {
		ShowWindow(tray, SW_SHOW);
		ShowWindow(tray, SW_RESTORE);
	}
	HWND tray2 = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
	if (tray2 && IsWindow(tray2)) {
		ShowWindow(tray2, SW_SHOW);
		ShowWindow(tray2, SW_RESTORE);
	}
	s_hidTaskbar = false;
	s_taskbarWnd = nullptr;
	s_secondaryTrayWnd = nullptr;
}

static void EnsureAtExitRestoreTaskbar()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;
	std::atexit(AtExitRestoreTaskbar);
}

static void ApplyTaskbarVisibility(bool hide)
{
	HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
	HWND tray2 = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
	if (hide) {
		s_taskbarWnd = tray;
		s_secondaryTrayWnd = tray2;
		bool hidAny = false;
		if (tray && IsWindow(tray)) {
			ShowWindow(tray, SW_HIDE);
			s_hidTaskbar = true;
			hidAny = true;
		}
		if (tray2 && IsWindow(tray2)) {
			ShowWindow(tray2, SW_HIDE);
			hidAny = true;
		}
		if (hidAny)
			EnsureAtExitRestoreTaskbar();
	} else {
		AtExitRestoreTaskbar();
	}
}

static void RegisterMenuToggleHotkey();

static bool TitleContainsInsensitive(HWND h, const char* needle)
{
	if (!needle || !*needle)
		return false;
	char title[512]{};
	GetWindowTextA(h, title, (int)sizeof(title) - 1);
	const size_t nlen = strlen(needle);
	for (size_t i = 0; title[i]; i++) {
		size_t j = 0;
		for (; j < nlen && title[i + j]; j++) {
			if (std::tolower(static_cast<unsigned char>(title[i + j])) !=
			    std::tolower(static_cast<unsigned char>(needle[j])))
				break;
		}
		if (j == nlen)
			return true;
	}
	return false;
}

static bool IsProcessPresentLocal(DWORD pid)
{
	if (!pid)
		return false;
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h)
		return false;
	CloseHandle(h);
	return true;
}

struct HwndPickCtx {
	DWORD pid;
	HWND hwnd;
	int bestArea;
	bool requireVisible;
};

static void ConsiderHwndForPick(HWND h, HwndPickCtx* c)
{
	if (c->requireVisible && !IsWindowVisible(h))
		return;
	RECT rc{};
	if (!GetWindowRect(h, &rc))
		return;
	const int w = rc.right - rc.left;
	const int hgt = rc.bottom - rc.top;
	if (w <= 0 || hgt <= 0)
		return;
	const int area = w * hgt;
	if (area < 32 * 32)
		return;
	if (area > c->bestArea) {
		c->bestArea = area;
		c->hwnd = h;
	}
}

static BOOL CALLBACK EnumPickChildProc(HWND h, LPARAM lp)
{
	auto* c = reinterpret_cast<HwndPickCtx*>(lp);
	DWORD wpid = 0;
	GetWindowThreadProcessId(h, &wpid);
	if (wpid != c->pid)
		return TRUE;
	ConsiderHwndForPick(h, c);
	EnumChildWindows(h, EnumPickChildProc, lp);
	return TRUE;
}

static BOOL CALLBACK EnumPickTopProc(HWND h, LPARAM lp)
{
	auto* c = reinterpret_cast<HwndPickCtx*>(lp);
	DWORD wpid = 0;
	GetWindowThreadProcessId(h, &wpid);
	if (wpid != c->pid)
		return TRUE;
	ConsiderHwndForPick(h, c);
	EnumChildWindows(h, EnumPickChildProc, lp);
	return TRUE;
}

/** Largest HWND (top-level or child) belonging to pid. */
static HWND FindLargestHwndForProcess(DWORD pid, bool requireVisible)
{
	if (!pid)
		return nullptr;
	HwndPickCtx ctx = { pid, nullptr, 0, requireVisible };
	EnumWindows(EnumPickTopProc, reinterpret_cast<LPARAM>(&ctx));
	return ctx.hwnd;
}

/** Same PID, title contains needle (e.g. "dayz"), largest area wins. */
static HWND FindForProcessTitleContains(DWORD pid, const char* needle)
{
	if (!pid || !needle)
		return nullptr;

	struct Ctx {
		DWORD pid;
		const char* needle;
		HWND hwnd;
		int bestArea;
	} ctx = { pid, needle, nullptr, 0 };

	EnumWindows([](HWND h, LPARAM lp) -> BOOL {
		auto* c = reinterpret_cast<Ctx*>(lp);
		DWORD wpid = 0;
		GetWindowThreadProcessId(h, &wpid);
		if (wpid != c->pid)
			return TRUE;
		if (!TitleContainsInsensitive(h, c->needle))
			return TRUE;
		RECT rc{};
		if (!GetWindowRect(h, &rc))
			return TRUE;
		int area = (rc.right - rc.left) * (rc.bottom - rc.top);
		if (area < 32 * 32)
			return TRUE;
		if (area > c->bestArea) {
			c->bestArea = area;
			c->hwnd = h;
		}
		return TRUE;
	}, reinterpret_cast<LPARAM>(&ctx));

	return ctx.hwnd;
}

static HWND ResolveGameWindow()
{
	/* Do not use "DayZ Client" here — same text as our overlay window title would match ourselves after create. */
	static const char* kExactTitles[] = {
		"DayZ",
		"DayZ x64",
		"DayZ_x64",
	};

	for (const char* t : kExactTitles) {
		HWND h = FindWindowA(nullptr, t);
		if (h && IsWindow(h))
			return h;
	}

	if (!globals.process_id)
		return nullptr;

	HWND h = FindLargestHwndForProcess(globals.process_id, true);
	if (!h)
		h = FindLargestHwndForProcess(globals.process_id, false);
	if (!h)
		h = FindForProcessTitleContains(globals.process_id, "dayz");
	return h;
}

static void ApplyStreamproofToOverlay()
{
	if (!globals.OverlayWnd || !IsWindow(globals.OverlayWnd))
		return;
	if (GameVars.streamproof)
		SetWindowDisplayAffinity(globals.OverlayWnd, WDA_EXCLUDEFROMCAPTURE);
	else
		SetWindowDisplayAffinity(globals.OverlayWnd, WDA_NONE);
}

namespace fonts {
	ImFont* medium;
	ImFont* semibold;

	ImFont* logo;
}

IDirect3D9Ex* p_Object = NULL;
IDirect3DDevice9Ex* p_Device = NULL;
D3DPRESENT_PARAMETERS p_Params = { NULL };

// Vector3 is defined in vector.h (included via globals.h)

void ov::create_window(bool console)
{
	if (!console)
		ShowWindow(GetConsoleWindow(), SW_HIDE);

	HINSTANCE hInst = GetModuleHandle(nullptr);
	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = 0;
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = hInst;
	wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.lpszClassName = globals.lWindowName;
	wcex.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

	ATOM reg = RegisterClassEx(&wcex);
	if (!reg) {
		const DWORD err = GetLastError();
		if (err != ERROR_CLASS_ALREADY_EXISTS) {
			Log("[ ! ] RegisterClassEx failed — overlay window class not registered.");
			return;
		}
	}

	globals.overlayMonitorFuserMode = false;
	globals.TargetWnd = ResolveGameWindow();

	RECT placeRect{};

	if (!globals.TargetWnd) {
		if (!IsProcessPresentLocal(globals.process_id)) {
			globals.overlayMonitorFuserMode = true;
			globals.TargetWnd = nullptr;
			HMONITOR mon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
			MONITORINFO mi = {};
			mi.cbSize = sizeof(mi);
			if (!GetMonitorInfo(mon, &mi)) {
				Log("[ ! ] GetMonitorInfo failed — cannot place fuser overlay.");
				return;
			}
			placeRect = mi.rcMonitor;
			Log("[ + ] Video fuser / dual-PC mode: game runs on another machine; overlay fills PRIMARY monitor here.");
			Log("[ * ] Route this display into the fuser's cheat-side HDMI input; match game resolution when possible.");
			Log("[ * ] Uses the Windows PRIMARY monitor — make that output the one wired to the fuser, or change display settings.");
		} else {
			char ebuf[220];
			snprintf(ebuf, sizeof(ebuf),
				"[ ! ] Game window not found for PID %lu (no HWND on this PC after title + child scan).",
				(unsigned long)globals.process_id);
			Log(ebuf);
			Log("[ * ] Process exists locally: try windowed/borderless, not exclusive fullscreen.");
			return;
		}
	} else {
		char gameTitle[256] = {};
		GetWindowTextA(globals.TargetWnd, gameTitle, (int)sizeof(gameTitle));
		char buf[384];
		snprintf(buf, sizeof(buf), "[ + ] Game window: HWND=0x%p title=\"%s\" PID=%lu",
			(void*)globals.TargetWnd, gameTitle[0] ? gameTitle : "(no title)",
			(unsigned long)globals.process_id);
		Log(buf);
		GetWindowRect(globals.TargetWnd, &placeRect);
	}

	globals.Width = placeRect.right - placeRect.left;
	globals.Height = placeRect.bottom - placeRect.top;

	if (globals.Width <= 0 || globals.Height <= 0) {
		Log("[ ! ] Invalid overlay placement rect — not created.");
		return;
	}

	globals.OverlayWnd = CreateWindowEx(
		WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
		globals.lWindowName,
		globals.lWindowName,
		WS_POPUP | WS_VISIBLE,
		placeRect.left,
		placeRect.top,
		globals.Width,
		globals.Height,
		NULL,
		NULL,
		hInst,
		NULL
	);

	if (!globals.OverlayWnd) {
		Log("[ ! ] CreateWindowEx failed — no overlay HWND.");
		return;
	}

	const MARGINS Margin = { -1, -1, -1, -1 };
	DwmExtendFrameIntoClientArea(globals.OverlayWnd, &Margin);
	// D3D9 + layered: без этого на части систем буфер не композится поверх игры.
	SetLayeredWindowAttributes(globals.OverlayWnd, 0, 255, LWA_ALPHA);

	ShowWindow(globals.OverlayWnd, SW_SHOW);
	SetWindowPos(globals.OverlayWnd, HWND_TOPMOST, placeRect.left, placeRect.top, globals.Width, globals.Height,
		SWP_SHOWWINDOW | SWP_NOACTIVATE);

	s_startupBlackPending = GameVars.blackStartupOverlay;
	if (s_startupBlackPending)
		GameVars.menuShow = false;

	if (GameVars.hideTaskbarOnStartup)
		ApplyTaskbarVisibility(true);

	RegisterMenuToggleHotkey();

	ApplyStreamproofToOverlay();
	if (GameVars.streamproof)
		Log("[ * ] Streamproof: окно скрыто из захвата экрана (WDA_EXCLUDEFROMCAPTURE). На мониторе обычно видно; в OBS может быть пусто.");

	if (!setup_directx(globals.OverlayWnd)) {
		Log("[ ! ] Direct3D / ImGui init failed — see messages above.");
		if (globals.OverlayWnd && IsWindow(globals.OverlayWnd)) {
			DestroyWindow(globals.OverlayWnd);
			globals.OverlayWnd = nullptr;
		}
		return;
	}

	{
		char buf[220];
		const char* mn = GetKeyName(GameVars.menuToggleHotkey ? GameVars.menuToggleHotkey : VK_INSERT);
		if (globals.overlayMonitorFuserMode)
			snprintf(buf, sizeof(buf), "[ + ] Overlay ready: %dx%d on primary monitor (fuser path). %s = menu.",
				globals.Width, globals.Height, mn);
		else
			snprintf(buf, sizeof(buf), "[ + ] Overlay ready: %dx%d aligned to game window. %s = menu.",
				globals.Width, globals.Height, mn);
		Log(buf);
	}
}

bool ov::setup_directx(HWND hWnd)
{
	if (!hWnd || !IsWindow(hWnd)) {
		Log("[ ! ] setup_directx: неверный HWND.");
		return false;
	}
	if (globals.Width <= 0 || globals.Height <= 0) {
		Log("[ ! ] setup_directx: ширина/высота должны быть > 0.");
		return false;
	}

	if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &p_Object))) {
		Log("[ ! ] Direct3DCreate9Ex failed.");
		return false;
	}

	ZeroMemory(&p_Params, sizeof(p_Params));
	p_Params.Windowed = TRUE;
	p_Params.SwapEffect = D3DSWAPEFFECT_DISCARD;
	p_Params.hDeviceWindow = hWnd;
	p_Params.MultiSampleQuality = D3DMULTISAMPLE_NONE;
	p_Params.BackBufferFormat = D3DFMT_A8R8G8B8;
	p_Params.BackBufferWidth = globals.Width;
	p_Params.BackBufferHeight = globals.Height;
	p_Params.EnableAutoDepthStencil = TRUE;
	p_Params.AutoDepthStencilFormat = D3DFMT_D16;
	p_Params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

	if (FAILED(p_Object->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &p_Params, 0, &p_Device)))
	{
		Log("[ ! ] CreateDeviceEx failed (HAL). Попробуйте оконный/borderless режим игры, обновите драйвер GPU.");
		p_Object->Release();
		p_Object = nullptr;
		p_Device = nullptr;
		return false;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	ImGuiIO& io = ImGui::GetIO();

	GameVars.smallFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Consolas\\consola.ttf", 15.0f);

	ImGui_ImplWin32_Init(hWnd);
	ImGui_ImplDX9_Init(p_Device);

	// Load Fonts
	ImFontConfig font_config;
	font_config.PixelSnapH = false;
	font_config.OversampleH = 5;
	font_config.OversampleV = 5;
	font_config.RasterizerMultiply = 1.2f;

	static const ImWchar ranges[] =
	{
		0x0020, 0x00FF, // Basic Latin + Latin Supplement
		0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
		0x2DE0, 0x2DFF, // Cyrillic Extended-A
		0xA640, 0xA69F, // Cyrillic Extended-B
		0xE000, 0xE226, // icons
		0,
	};

	font_config.GlyphRanges = ranges;

	fonts::medium = io.Fonts->AddFontFromMemoryTTF(InterMedium, sizeof(InterMedium), 15.0f, &font_config, ranges);
	fonts::semibold = io.Fonts->AddFontFromMemoryTTF(InterSemiBold, sizeof(InterSemiBold), 17.0f, &font_config, ranges);

	fonts::logo = io.Fonts->AddFontFromMemoryTTF(catrine_logo, sizeof(catrine_logo), 17.0f, &font_config, ranges);

	ImVec4 clear_color = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);

	p_Object->Release();
	p_Object = nullptr;
	return true;
}

void ov::update_overlay()
{
	if (!globals.OverlayWnd || !IsWindow(globals.OverlayWnd))
		return;

	if (globals.overlayMonitorFuserMode) {
		HMONITOR mon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFO mi = {};
		mi.cbSize = sizeof(mi);
		if (!GetMonitorInfo(mon, &mi))
			return;
		RECT r = mi.rcMonitor;
		int w = r.right - r.left;
		int h = r.bottom - r.top;
		if (w <= 0 || h <= 0)
			return;
		SetWindowPos(globals.OverlayWnd, HWND_TOPMOST, r.left, r.top, w, h,
			SWP_SHOWWINDOW | SWP_NOACTIVATE);

		if (p_Device && (w != globals.Width || h != globals.Height)) {
			globals.Width = w;
			globals.Height = h;
			p_Params.BackBufferWidth = w;
			p_Params.BackBufferHeight = h;
			ImGui_ImplDX9_InvalidateDeviceObjects();
			if (FAILED(p_Device->Reset(&p_Params)))
				Log("[ ! ] D3D Device Reset failed after monitor resize.");
			else
				ImGui_ImplDX9_CreateDeviceObjects();
		}
		return;
	}

	HWND game = globals.TargetWnd;
	if (!game || !IsWindow(game))
		game = ResolveGameWindow();

	if (!game)
		return;

	globals.TargetWnd = game;

	RECT tSize{};
	GetWindowRect(game, &tSize);
	int w = tSize.right - tSize.left;
	int h = tSize.bottom - tSize.top;
	if (w <= 0 || h <= 0)
		return;

	SetWindowPos(globals.OverlayWnd, HWND_TOPMOST, tSize.left, tSize.top, w, h,
		SWP_SHOWWINDOW | SWP_NOACTIVATE);

	if (p_Device && (w != globals.Width || h != globals.Height))
	{
		globals.Width = w;
		globals.Height = h;
		p_Params.BackBufferWidth = w;
		p_Params.BackBufferHeight = h;
		ImGui_ImplDX9_InvalidateDeviceObjects();
		if (FAILED(p_Device->Reset(&p_Params)))
			Log("[ ! ] D3D Device Reset failed after game window resize.");
		else
			ImGui_ImplDX9_CreateDeviceObjects();
	}
}

static bool showImGuiWindow = false;

const char* GetKeyName(int vkCode) {
	static char name[128];
	if (vkCode == 0) {
		return xorstr_("None");
	}

	// Verifica se o vkCode corresponde a bot�es do mouse
	if (vkCode == VK_LBUTTON) {
		return xorstr_("Mouse 1");
	}
	else if (vkCode == VK_RBUTTON) {
		return xorstr_("Mouse 2");
	}
	else if (vkCode == VK_MBUTTON) {
		return xorstr_("Mouse 3");
	}
	else if (vkCode == VK_XBUTTON1) {
		return xorstr_("Mouse 4");
	}
	else if (vkCode == VK_XBUTTON2) {
		return xorstr_("Mouse 5");
	}
	else if (vkCode == VK_INSERT) {
		return xorstr_("Insert");
	}

	// Para outras teclas, usa GetKeyNameText normalmente
	GetKeyNameText(MapVirtualKey(vkCode, MAPVK_VK_TO_VSC) << 16, name, 128);
	return name;
}

static void RegisterMenuToggleHotkey()
{
	if (!globals.OverlayWnd || !IsWindow(globals.OverlayWnd))
		return;
	UnregisterHotKey(globals.OverlayWnd, kMenuHotkeyId);
	UINT vk = (UINT)(GameVars.menuToggleHotkey & 0xFF);
	if (vk == 0)
		vk = VK_INSERT;
	g_menuToggleUsesRegisterHotkey = RegisterHotKey(globals.OverlayWnd, kMenuHotkeyId, 0, vk) != FALSE;
	char buf[160];
	if (g_menuToggleUsesRegisterHotkey)
		snprintf(buf, sizeof(buf), "[ + ] Menu toggle: %s (RegisterHotKey OK)", GetKeyName((int)vk));
	else
		snprintf(buf, sizeof(buf), "[ ! ] RegisterHotKey(%s) failed — polling fallback", GetKeyName((int)vk));
	Log(buf);
}

int CaptureKeyOrMouse()
{
	// Track which keys were already held when capture started
	// so we don't immediately register the mouse click that opened the popup
	static bool initialized = false;
	static bool keysHeldOnOpen[256] = {};

	if (!initialized) {
		for (int key = 0; key < 256; key++) {
			keysHeldOnOpen[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
		}
		initialized = true;
		return 0;
	}

	for (int key = 0; key < 256; key++) {
		if (GetAsyncKeyState(key) & 0x8000) {
			// Skip keys that were already held when popup opened
			if (keysHeldOnOpen[key]) continue;
			initialized = false; // Reset for next capture
			return key;
		} else {
			// Key was released — no longer considered "held on open"
			keysHeldOnOpen[key] = false;
		}
	}

	return 0;
}

std::vector<std::string> configFiles;
std::string newConfigName;

void EnsureDirectoryExists(const std::string& path) {
	std::filesystem::path dirPath(path);
	if (!std::filesystem::exists(dirPath)) {
		try {
			std::filesystem::create_directories(dirPath);
			std::cout << "Directory created: " << path << std::endl;
		}
		catch (const std::exception& e) {
			std::cerr << "Error creating directory: " << e.what() << std::endl;
		}
	}
}

void LoadSettings(const char* filename) {
	std::ifstream file(filename, std::ios::binary);
	if (!file.is_open()) {
		std::cerr << "Error opening file for reading: " << filename << std::endl;
		return;
	}

	file.read(reinterpret_cast<char*>(&GameVars.streamproof), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.inventory), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.ToggleInvFov), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.inhands), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.speedhh), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.deadESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.playerESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.playerlistm), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.healESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.skeletonESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.boxESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.lineESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.nameESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.distanceESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.espDistance), sizeof(int));
	file.read(reinterpret_cast<char*>(&GameVars.lineZESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.infectedESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.skeletonzESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.namezESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.distancezESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.espzDistance), sizeof(int));
	file.read(reinterpret_cast<char*>(&GameVars.citiesESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Berezino), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Solnechniy), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Kamyshovo), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Electrozavodsk), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Prigorodki), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Chernogorsk), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Svetloyarsk), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Krasnostav), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Novodmitrovsk), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Airfield), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Novaya), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Severograd), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Gorka), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Tulga), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Balota), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Zelenogorsk), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.StarySobor), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Polana), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Mishkino), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Grishino), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Pogorevka), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Rogovo), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Dubrovka), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Kabanino), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Petrovka), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Vybor), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Tisy), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.itemsESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.allitemsESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.sPristine), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.sWorn), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.sDamaged), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.sBadly), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.sRuined), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.carsESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.animalsESP), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.tperson), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.sethour), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.seteye), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.SilentAim), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.SilentAimZ), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.ToggleFov), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.aimfov), sizeof(float));
	file.read(reinterpret_cast<char*>(&GameVars.ToggleTpFov), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.LootTeleport), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.qality), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.itemlistm), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.weapon), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.ProxyMagazines), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Ammunation), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Miras), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.rodas), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.clothing), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.inventoryItem), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.comida), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.drinks), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.medicine), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Containers), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.berrel), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.BackPack), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.inventoryItem12), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.qualidade), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Filtrarlloot), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.Filtrarlloot1), sizeof(bool));
	file.read(reinterpret_cast<char*>(&GameVars.showBots), sizeof(bool));
	// Hotkeys — added at end; if file is shorter (old config), these just keep defaults
	file.read(reinterpret_cast<char*>(&GameVars.silentAimHotkey), sizeof(int));
	file.read(reinterpret_cast<char*>(&GameVars.lootTPHotkey), sizeof(int));

	std::streampos tailPos = file.tellg();
	file.seekg(0, std::ios::end);
	std::streampos endPos = file.tellg();
	file.seekg(tailPos);
	const std::streamoff extraBytes = endPos - tailPos;
	if (extraBytes >= (std::streamoff)(sizeof(int) + sizeof(int) + sizeof(bool) + sizeof(bool))) {
		file.read(reinterpret_cast<char*>(&GameVars.menuToggleHotkey), sizeof(int));
		file.read(reinterpret_cast<char*>(&GameVars.dmaRefreshHotkey), sizeof(int));
		file.read(reinterpret_cast<char*>(&GameVars.blackStartupOverlay), sizeof(bool));
		file.read(reinterpret_cast<char*>(&GameVars.hideTaskbarOnStartup), sizeof(bool));
	}

	file.close();
}

void SaveSettings(const char* filename) {
	std::ofstream file(filename, std::ios::binary);
	if (!file.is_open()) {
		std::cerr << "Error opening file for writing: " << filename << std::endl;
		return;
	}

	file.write(reinterpret_cast<char*>(&GameVars.streamproof), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.inventory), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.ToggleInvFov), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.inhands), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.speedhh), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.deadESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.playerESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.playerlistm), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.healESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.skeletonESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.boxESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.lineESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.nameESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.distanceESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.espDistance), sizeof(int));
	file.write(reinterpret_cast<char*>(&GameVars.lineZESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.infectedESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.skeletonzESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.namezESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.distancezESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.espzDistance), sizeof(int));
	file.write(reinterpret_cast<char*>(&GameVars.citiesESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Berezino), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Solnechniy), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Kamyshovo), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Electrozavodsk), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Prigorodki), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Chernogorsk), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Svetloyarsk), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Krasnostav), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Novodmitrovsk), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Airfield), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Novaya), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Severograd), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Gorka), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Tulga), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Balota), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Zelenogorsk), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.StarySobor), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Polana), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Mishkino), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Grishino), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Pogorevka), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Rogovo), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Dubrovka), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Kabanino), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Petrovka), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Vybor), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Tisy), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.itemsESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.allitemsESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.sPristine), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.sWorn), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.sDamaged), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.sBadly), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.sRuined), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.carsESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.animalsESP), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.tperson), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.sethour), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.seteye), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.SilentAim), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.SilentAimZ), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.ToggleFov), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.aimfov), sizeof(float));
	file.write(reinterpret_cast<char*>(&GameVars.ToggleTpFov), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.LootTeleport), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.qality), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.itemlistm), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.weapon), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.ProxyMagazines), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Ammunation), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Miras), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.rodas), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.clothing), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.inventoryItem), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.comida), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.drinks), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.medicine), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Containers), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.berrel), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.BackPack), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.inventoryItem12), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.qualidade), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Filtrarlloot), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.Filtrarlloot1), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.showBots), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.silentAimHotkey), sizeof(int));
	file.write(reinterpret_cast<char*>(&GameVars.lootTPHotkey), sizeof(int));
	file.write(reinterpret_cast<char*>(&GameVars.menuToggleHotkey), sizeof(int));
	file.write(reinterpret_cast<char*>(&GameVars.dmaRefreshHotkey), sizeof(int));
	file.write(reinterpret_cast<char*>(&GameVars.blackStartupOverlay), sizeof(bool));
	file.write(reinterpret_cast<char*>(&GameVars.hideTaskbarOnStartup), sizeof(bool));

	file.close();
}

void ov::preload_config()
{
	const std::string configDir = "C:\\SecretSociety\\";
	const std::string configPath = configDir + "config.dat";
	EnsureDirectoryExists(configDir);
	LoadSettings(configPath.c_str());
	GameVars.loadConfigRequested = false;
}

void SaveConfig() {
	const std::string configDir = "C:\\SecretSociety\\";
	const std::string configPath = configDir + "config.dat";

	EnsureDirectoryExists(configDir);
	SaveSettings(configPath.c_str());
}

void LoadConfig() {
	const std::string configPath = "C:\\SaturnDayZ\\config.dat";

	LoadSettings(configPath.c_str());
	RegisterMenuToggleHotkey();
}

void UpdateConfigList(const std::string& configDir) {
	configFiles.clear();
	for (const auto& entry : std::filesystem::directory_iterator(configDir)) {
		if (entry.is_regular_file() && entry.path().extension() == ".dat") {
			configFiles.push_back(entry.path().filename().string());
		}
	}
}

void SaveNewConfig(const std::string& configDir, const std::string& configName) {
	if (configName.empty()) {
		std::cerr << "Error: Config name cannot be empty." << std::endl;
		return;
	}

	const std::string filePath = configDir + configName + ".dat";
	SaveSettings(filePath.c_str());
	UpdateConfigList(configDir);
}

void ov::render()
{
	if (!p_Device)
		return;

	update_overlay();

	static bool s_lastMenuShow = !GameVars.menuShow;
	static bool s_lastStreamproof = GameVars.streamproof;
	if (globals.OverlayWnd && IsWindow(globals.OverlayWnd))
	{
		if (s_lastMenuShow != GameVars.menuShow || s_lastStreamproof != GameVars.streamproof)
		{
			s_lastMenuShow = GameVars.menuShow;
			s_lastStreamproof = GameVars.streamproof;
			LONG ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
			if (!GameVars.menuShow)
				ex |= WS_EX_TRANSPARENT;
			SetWindowLong(globals.OverlayWnd, GWL_EXSTYLE, ex);
			ApplyStreamproofToOverlay();
		}
	}

	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowMinSize = ImVec2(550, 370);
	style.Colors[ImGuiCol_SliderGrab] = ImColor(157, 173, 241, 255);
	style.Colors[ImGuiCol_SliderGrabActive] = ImColor(255, 255, 255, 255);
	style.Colors[ImGuiCol_FrameBg] = ImColor(32, 32, 32, 255);

	style.Colors[ImGuiCol_Button] = ImColor(40, 40, 40, 255);
	style.Colors[ImGuiCol_ButtonActive] = ImColor(45, 45, 45, 255);
	style.Colors[ImGuiCol_ButtonHovered] = ImColor(45, 45, 45, 255);
	style.FrameRounding = 5;
	style.ChildRounding = 5;
	style.GrabRounding = 3;

	// Frame timing - log every 60 frames unconditionally
	static LARGE_INTEGER rFreq = {};
	if (!rFreq.QuadPart) QueryPerformanceFrequency(&rFreq);
	auto hpc = []() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; };
	double rMs = 1000.0 / rFreq.QuadPart;
	auto rStart = hpc();
	static int frameCount = 0;

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

	ImGui::NewFrame();

	p_Device->SetRenderState(D3DRS_ZENABLE, false);
	p_Device->SetRenderState(D3DRS_ALPHABLENDENABLE, false);
	p_Device->SetRenderState(D3DRS_SCISSORTESTENABLE, false);

	const bool blackStartupFill = s_startupBlackPending && !GameVars.menuShow;
	if (GameVars.menuShow)
		s_startupBlackPending = false;

	p_Device->Clear(0, NULL, D3DCLEAR_TARGET,
		blackStartupFill ? D3DCOLOR_ARGB(255, 0, 0, 0) : D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

	auto rAfterClear = hpc();

	static heads head_selected = HEAD_1;
	static subheads subhead_selected = SUBHEAD_1;

	auto draw = ImGui::GetWindowDrawList();

	auto pos = ImGui::GetWindowPos();
	auto size = ImGui::GetWindowSize();

	if (!blackStartupFill)
		draw_esp();
	auto rAfterESP = hpc();

	if (GameVars.menuShow)
		{
			ImGui::SetNextWindowSize({ 550, 370 });
			ImGui::Begin(xorstr_("KLG"), nullptr, ImGuiWindowFlags_NoDecoration);

			auto draw = ImGui::GetWindowDrawList();

			auto pos = ImGui::GetWindowPos();
			auto size = ImGui::GetWindowSize();

			draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + 50), ImColor(24, 24, 24), 9.0f);

			draw->AddText(fonts::semibold, 19.0f, ImVec2(pos.x + 25, pos.y + 13), ImColor(192, 203, 229), "KLG");
			draw->AddText(fonts::semibold, 11.0f, ImVec2(pos.x + 25, pos.y + 34), ImColor(140, 150, 170), "Canadian Built");

			ImGui::SetCursorPos({ 125, 19 });
			ImGui::BeginGroup(); {
				if (elements::tab(xorstr_("Visuals"), head_selected == HEAD_1)) head_selected = HEAD_1;
				ImGui::SameLine();
				if (elements::tab(xorstr_("Aim Options"), head_selected == HEAD_2)) head_selected = HEAD_2;
				ImGui::SameLine();
				if (elements::tab(xorstr_("Lists"), head_selected == HEAD_3)) head_selected = HEAD_3;
				ImGui::SameLine();
				if (elements::tab(xorstr_("Exploits"), head_selected == HEAD_4)) head_selected = HEAD_4;
				ImGui::SameLine();
				if (elements::tab(xorstr_("Settings"), head_selected == HEAD_5)) head_selected = HEAD_5;
			}
			ImGui::EndGroup();

			switch (head_selected) {
			case HEAD_1:
				ImGui::SetCursorPos({ 125, 50 });
				ImGui::BeginGroup(); {
					if (elements::tab(xorstr_("Players"), subhead_selected == SUBHEAD_1)) subhead_selected = SUBHEAD_1;
					ImGui::SameLine();
					if (elements::tab(xorstr_("Zombies"), subhead_selected == SUBHEAD_2)) subhead_selected = SUBHEAD_2;
					ImGui::SameLine();
					if (elements::tab(xorstr_("Cities"), subhead_selected == SUBHEAD_3)) subhead_selected = SUBHEAD_3;
					ImGui::SameLine();
					if (elements::tab(xorstr_("Itens"), subhead_selected == SUBHEAD_4)) subhead_selected = SUBHEAD_4;
					ImGui::SameLine();
					if (elements::tab(xorstr_("Others"), subhead_selected == SUBHEAD_5)) subhead_selected = SUBHEAD_5;
					ImGui::SameLine();
				}
				switch (subhead_selected) {
				case SUBHEAD_1:

					ImGui::SetCursorPos({ 25, 75 });
					ImGui::BeginChild(xorstr_("##container"), ImVec2(190, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options"));
						ImGui::Spacing();

						ImGui::Checkbox(xorstr_("ESP Players"), &GameVars.playerESP);
						if (GameVars.playerESP) {
							ImGui::Checkbox(xorstr_("ESP In Dead"), &GameVars.deadESP);
							ImGui::Checkbox(xorstr_("ESP Skeleton"), &GameVars.skeletonESP);
							ImGui::Checkbox(xorstr_("ESP Box"), &GameVars.boxESP);
							ImGui::Checkbox(xorstr_("ESP Health"), &GameVars.healESP);
							ImGui::Checkbox(xorstr_("ESP Lines"), &GameVars.lineESP);
							ImGui::Checkbox(xorstr_("ESP In Hands"), &GameVars.inhands);
							ImGui::Checkbox(xorstr_("ESP Name"), &GameVars.nameESP);
							ImGui::Checkbox(xorstr_("ESP Distance"), &GameVars.distanceESP);
						}
					}
					ImGui::EndChild();


					ImGui::SetCursorPos({ 285, 75 });
					ImGui::BeginChild(xorstr_("##container1"), ImVec2(190, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Misc"));
						ImGui::Spacing();

						ImGui::ColorEdit4(xorstr_("##NameColor"), GameVars.EspNameColorArray, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("Names Color"));

						ImGui::ColorEdit4(xorstr_("##SkeletonColor"), GameVars.skeletonColorArray, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("Skeleton Color"));

						ImGui::ColorEdit4(xorstr_("##boxColor"), GameVars.boxColorArray, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("Box Color"));

						ImGui::ColorEdit4(xorstr_("##LinesColor"), GameVars.linesColorArray, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("Lines Color"));

						ImGui::Checkbox(xorstr_("Local Player"), &GameVars.inlocal);

						if (GameVars.boxESP) {
							ImGui::Combo(xorstr_("Box Type"), &GameVars.selectedBoxType, GameVars.boxTypes, IM_ARRAYSIZE(GameVars.boxTypes));
						}

						if (GameVars.lineESP) {
							ImGui::Combo(xorstr_("Line Pos"), &GameVars.selectedLinePos, GameVars.linePos, IM_ARRAYSIZE(GameVars.linePos));
						}

						ImGui::SliderInt(xorstr_("Distance"), &GameVars.espDistance, 100, 1200);
					}
					ImGui::EndChild();
					break;
				}

				switch (subhead_selected) {
				case SUBHEAD_2:

					ImGui::SetCursorPos({ 25, 75 });
					ImGui::BeginChild(xorstr_("##container"), ImVec2(190, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options"));
						ImGui::Spacing();
						ImGui::Checkbox(xorstr_("ESP Zombies"), &GameVars.infectedESP);
						if (GameVars.infectedESP) {
							ImGui::Checkbox(xorstr_("ESP Skeleton"), &GameVars.skeletonzESP);
							ImGui::Checkbox(xorstr_("ESP Lines"), &GameVars.lineZESP);
							ImGui::Checkbox(xorstr_("ESP Name"), &GameVars.namezESP);
							ImGui::Checkbox(xorstr_("ESP Distance"), &GameVars.distancezESP);
						}
					}
					ImGui::EndChild();


					ImGui::SetCursorPos({ 285, 75 });
					ImGui::BeginChild(xorstr_("##container1"), ImVec2(190, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Misc"));
						ImGui::Spacing();

						ImGui::ColorEdit4(xorstr_("##NameZColor"), GameVars.EspNamezColorArray, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("Names Color"));

						ImGui::ColorEdit4(xorstr_("##SkeletonzColor"), GameVars.skeletonzColorArray, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("Skeleton Color"));


						ImGui::ColorEdit4(xorstr_("##LinesZColor"), GameVars.linesColorZArray, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("Lines Color"));

						if (GameVars.lineZESP) {
							ImGui::Combo(xorstr_("Line Pos"), &GameVars.selectedLinePos, GameVars.linePos, IM_ARRAYSIZE(GameVars.linePos));
						}

						ImGui::SliderInt(xorstr_("Distance"), &GameVars.espzDistance, 100, 1200);

					}
					ImGui::EndChild();
					break;
				}

				switch (subhead_selected) {
				case SUBHEAD_3:

					ImGui::SetCursorPos({ 25, 75 });
					ImGui::BeginChild(xorstr_("##container"), ImVec2(190, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options 1"));
						ImGui::Spacing();

						ImGui::Checkbox(xorstr_("Berezino"), &GameVars.Berezino);
						ImGui::Checkbox(xorstr_("Solnechniy"), &GameVars.Solnechniy);
						ImGui::Checkbox(xorstr_("Kamyshovo"), &GameVars.Kamyshovo);
						ImGui::Checkbox(xorstr_("Electrozavodsk"), &GameVars.Electrozavodsk);
						ImGui::Checkbox(xorstr_("Prigorodki"), &GameVars.Prigorodki);
						ImGui::Checkbox(xorstr_("Chernogorsk"), &GameVars.Chernogorsk);
						ImGui::Checkbox(xorstr_("Svetloyarsk"), &GameVars.Svetloyarsk);

					}
					ImGui::EndChild();


					ImGui::SetCursorPos({ 285, 75 });
					ImGui::BeginChild(xorstr_("##container1"), ImVec2(190, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options 2"));
						ImGui::Spacing();

						ImGui::Checkbox(xorstr_("Krasnostav"), &GameVars.Krasnostav);
						ImGui::Checkbox(xorstr_("Novodmitrovsk"), &GameVars.Novodmitrovsk);
						ImGui::Checkbox(xorstr_("Airfield"), &GameVars.Airfield);
						ImGui::Checkbox(xorstr_("Novaya"), &GameVars.Novaya);
						ImGui::Checkbox(xorstr_("Severograd"), &GameVars.Severograd);
						ImGui::Checkbox(xorstr_("Gorka"), &GameVars.Gorka);
						ImGui::Checkbox(xorstr_("Tulga"), &GameVars.Tulga);

					}
					ImGui::EndChild();
					break;
				}

				switch (subhead_selected) {
				case SUBHEAD_4:

					ImGui::SetCursorPos({ 25, 75 });
					ImGui::BeginChild(xorstr_("##container"), ImVec2(190, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options"));
						ImGui::Spacing();

						ImGui::Checkbox(xorstr_("Filter for Name"), &GameVars.Filtrarlloot);

						if (GameVars.Filtrarlloot)
						{
							for (size_t i = 0; i < GameVars.lootFilters.size(); ++i) {
								char buffer[256]; // Buffer tempor�rio para edi��o
								std::snprintf(buffer, sizeof(buffer), "%s", GameVars.lootFilters[i].c_str());
								if (ImGui::InputText(std::string(std::to_string(i + 1)).c_str(), buffer, sizeof(buffer))) {
									GameVars.lootFilters[i] = buffer; // Atualiza o filtro no vetor
								}
							}
							ImGui::SameLine();
							if (ImGui::Button("+")) {
								if (GameVars.lootFilters.size() < 5) {
									GameVars.lootFilters.push_back(""); // Adiciona um novo filtro vazio
								}
							}
							ImGui::SameLine();
							if (ImGui::Button("-")) {
								if (GameVars.lootFilters.size() > 1) {
									GameVars.lootFilters.pop_back(); // Remove o �ltimo filtro
								}
							}

						}

						ImGui::Checkbox(xorstr_("ESP Loot"), &GameVars.itemsESP);
						ImGui::Separator();


						if (GameVars.itemsESP) {

							ImGui::Checkbox(xorstr_("ESP Weapon"), &GameVars.weapon);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##WeapColor"), GameVars.colorweapon2, ImGuiColorEditFlags_NoInputs);

							ImGui::Checkbox(xorstr_("ESP Magazines"), &GameVars.ProxyMagazines);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##MagColor"), GameVars.coloriMagazines2, ImGuiColorEditFlags_NoInputs);

							ImGui::Checkbox(xorstr_("ESP Ammunation"), &GameVars.Ammunation);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##AmmuColor"), GameVars.coloriAmmu2, ImGuiColorEditFlags_NoInputs);

							ImGui::Checkbox(xorstr_("ESP Scoop"), &GameVars.Miras);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##ScoColor"), GameVars.coloriScoop2, ImGuiColorEditFlags_NoInputs);

							ImGui::Separator();

							ImGui::Checkbox(xorstr_("ESP Well"), &GameVars.rodas);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##WelColor"), GameVars.corrodas2, ImGuiColorEditFlags_NoInputs);

							ImGui::Checkbox(xorstr_("ESP Clothing"), &GameVars.clothing);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##CloColor"), GameVars.colorcloting2, ImGuiColorEditFlags_NoInputs);

							ImGui::Checkbox(xorstr_("ESP Others"), &GameVars.inventoryItem);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##OthColor"), GameVars.colorothers2, ImGuiColorEditFlags_NoInputs);

							ImGui::Checkbox(xorstr_("ESP Containers"), &GameVars.Containers);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##ConColor"), GameVars.colorcont2, ImGuiColorEditFlags_NoInputs);

							ImGui::Checkbox(xorstr_("ESP BackPack"), &GameVars.BackPack);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##BackColor"), GameVars.colorbackpack2, ImGuiColorEditFlags_NoInputs);

							ImGui::Separator();

							ImGui::Checkbox(xorstr_("ESP Food"), &GameVars.comida);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##FoodColor"), GameVars.colorfood2, ImGuiColorEditFlags_NoInputs);

							ImGui::Checkbox(xorstr_("ESP Drinks"), &GameVars.drinks);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##DrinksColor"), GameVars.colordrnks2, ImGuiColorEditFlags_NoInputs);

							ImGui::Checkbox(xorstr_("ESP Medicine"), &GameVars.medicine);
							ImGui::SameLine();
							ImGui::ColorEdit4(xorstr_("##MedColor"), GameVars.colormedcine2, ImGuiColorEditFlags_NoInputs);

							ImGui::Separator();

							ImGui::Checkbox(xorstr_("EPS All"), &GameVars.inventoryItem12);

						}

					}
					ImGui::EndChild();


					ImGui::SetCursorPos({ 285, 75 });
					ImGui::BeginChild(xorstr_("##container1"), ImVec2(190, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Misc"));
						ImGui::Spacing();

						ImGui::Checkbox(xorstr_("Itens Quality"), &GameVars.qality);
						if (GameVars.qality) {
							ImGui::Indent(20.0f); // Adiciona um deslocamento de 20 pixels � esquerda

							ImGui::Checkbox(xorstr_("Pristine"), &GameVars.sPristine);
							ImGui::Checkbox(xorstr_("Worn"), &GameVars.sWorn);
							ImGui::Checkbox(xorstr_("Damaged"), &GameVars.sDamaged);
							ImGui::Checkbox(xorstr_("Badly"), &GameVars.sBadly);
							ImGui::Checkbox(xorstr_("Ruined"), &GameVars.sRuined);

							ImGui::Unindent(20.0f); // Remove o deslocamento
						}
						ImGui::SliderInt(xorstr_("Distance"), &GameVars.espDistanceitems, 100, 1200);

					}
					ImGui::EndChild();
					break;
				}

				switch (subhead_selected) {
				case SUBHEAD_5:

					ImGui::SetCursorPos({ 25, 75 });
					ImGui::BeginChild(xorstr_("##container"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options 1"));
						ImGui::Spacing();

						ImGui::Checkbox(xorstr_("ESP Cars"), &GameVars.carsESP);
						if (GameVars.carsESP) {
							ImGui::SliderInt(xorstr_("Cars Distance"), &GameVars.espDistancecars, 100, 1200);
						}

						ImGui::Checkbox(xorstr_("ESP Animals"), &GameVars.animalsESP);
						if (GameVars.animalsESP) {
							ImGui::SliderInt(xorstr_("Animals Distance"), &GameVars.espDistanceanimals, 100, 1200);
						}

						ImGui::Checkbox(xorstr_("ESP Inventory"), &GameVars.inventory);
						if (GameVars.inventory) {
							ImGui::Checkbox(xorstr_("Inventory Fov"), &GameVars.ToggleInvFov);
							ImGui::SliderFloat(xorstr_("FOV Inv"), &GameVars.invfov, 1, 999);
						}

						ImGui::Checkbox(xorstr_("Remove Grass"), &GameVars.grass);

						ImGui::Checkbox(xorstr_("Hour"), &GameVars.sethour);
						if (GameVars.sethour) {
							ImGui::SliderInt(xorstr_("Time"), &GameVars.dayf, 0, 10);
						}

						ImGui::Checkbox(xorstr_("Eye Accom"), &GameVars.seteye);
						if (GameVars.seteye) {
							ImGui::SliderInt(xorstr_("Eye"), &GameVars.eye, 0, 10);
						}

					}
					ImGui::EndChild();


					ImGui::SetCursorPos({ 285, 75 });
					ImGui::BeginChild(xorstr_("##container1"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options 2"));
						ImGui::Spacing();

						ImGui::ColorEdit4(xorstr_("##CarColor"), GameVars.EspCarColorArray, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("Cars Color"));

						ImGui::ColorEdit4(xorstr_("##AnimaColor"), GameVars.EspAnimalColorArray, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("Animals Color"));

						ImGui::ColorEdit4(xorstr_("##FovinvColor"), GameVars.colorfovinv, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("FOV Color"));

					}
					ImGui::EndChild();

					break;
				}
			}

			switch (head_selected) {
			case HEAD_2:

				ImGui::SetCursorPos({ 25, 75 });
				ImGui::BeginChild(xorstr_("##container"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options 1"));
					ImGui::Spacing();

					ImGui::Checkbox(xorstr_("Silent Aim"), &GameVars.SilentAim);
					if (GameVars.SilentAim) {
						ImGui::Checkbox(xorstr_("In Zombies"), &GameVars.SilentAimZ);
						ImGui::Text(xorstr_("Silent Key:"));
						ImGui::SameLine();

						const char* buttonText = GameVars.silentAimHotkey == 0 ? "Set Hotkey" : GetKeyName(GameVars.silentAimHotkey);
						ImVec2 textSize = ImGui::CalcTextSize(buttonText);
						ImVec2 buttonSize(textSize.x + 10, textSize.y + 5);
						if (ImGui::Button(buttonText, buttonSize)) {
							ImGui::OpenPopup(xorstr_("SetAimHotkey"));
						}

						if (ImGui::BeginPopup(xorstr_("SetAimHotkey")))
						{
							ImGui::Text(xorstr_("Press any key..."));

							int key = CaptureKeyOrMouse();
							if (key != 0) {
								GameVars.silentAimHotkey = key;
								ImGui::CloseCurrentPopup();
							}

							ImGui::EndPopup();
						}

					}

					ImGui::Checkbox(xorstr_("Aim Fov"), &GameVars.ToggleFov);

				}
				ImGui::EndChild();


				ImGui::SetCursorPos({ 285, 75 });
				ImGui::BeginChild(xorstr_("##container1"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options 2"));
					ImGui::Spacing();

					ImGui::Text(xorstr_("FOV Color"));
					ImGui::SameLine();
					ImGui::ColorEdit4(xorstr_("##FovaimColor"), GameVars.colorfovaim, ImGuiColorEditFlags_NoInputs);
					ImGui::SliderFloat("FOV Size", &GameVars.aimfov, 1, 999);
					const char* items[] = { "Head", "Neck", "Chest", "Pelvis", "Left Leg", "Right Leg" };
					ImGui::Combo(xorstr_("HitBox"), &GameVars.bonechoose, items, IM_ARRAYSIZE(items));

				}
				ImGui::EndChild();

				break;
			}

			static int selectedPlayerIndex = -1;
			int playerDisplayIndex = 0;

			static int selectedItemIndex = -1;
			int itemDisplayIndex = 0;

			switch (head_selected) {
			case HEAD_3:
				ImGui::SetCursorPos({ 125, 50 });
				ImGui::BeginGroup(); {
					if (elements::tab(xorstr_("Players List"), subhead_selected == SUBHEAD_1)) subhead_selected = SUBHEAD_1;
					ImGui::SameLine();
					if (elements::tab(xorstr_("Itens List"), subhead_selected == SUBHEAD_2)) subhead_selected = SUBHEAD_2;
					ImGui::SameLine();
				}

				switch (subhead_selected) {
				case SUBHEAD_1:

					ImGui::SetCursorPos({ 25, 75 });
					ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(13.0f / 255.0f, 13.0f / 255.0f, 13.0f / 255.0f, 240.0f / 255.0f));
					ImGui::BeginChild(xorstr_("##container"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Players:"));
						ImGui::Spacing();

						for (size_t i = 0; i < GameVars.playerList.size(); i++) {
							const std::string& playerName = GameVars.playerList[i];

							std::ostringstream displayText;
							displayText << playerName;

							if (ImGui::Selectable(displayText.str().c_str(), selectedPlayerIndex == playerDisplayIndex)) {
								selectedPlayerIndex = playerDisplayIndex;
							}

							playerDisplayIndex++;
						}

					}
					ImGui::EndChild();
					ImGui::PopStyleColor();


					ImGui::SetCursorPos({ 285, 75 });
					ImGui::BeginChild(xorstr_("##container1"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {

						ImGui::Checkbox(xorstr_("Show Players"), &GameVars.playerlistm);
						ImGui::Checkbox(xorstr_("Show Bots"), &GameVars.showBots);

						if (selectedPlayerIndex != -1 && selectedPlayerIndex < playerDisplayIndex) {
							ImGui::Text("Player: %s", GameVars.playerList[selectedPlayerIndex].c_str());
							ImGui::Text("Distance: %.1fm", GameVars.playerDistances[selectedPlayerIndex]);

							bool isFriend = GameVars.friendsSet.find(GameVars.playerList[selectedPlayerIndex]) != GameVars.friendsSet.end();

							if (ImGui::Button(isFriend ? xorstr_("Remove Friend") : xorstr_("Add Friend"))) {
								if (isFriend) {
									GameVars.friendsSet.erase(GameVars.playerList[selectedPlayerIndex]);
								}
								else {
									GameVars.friendsSet.insert(GameVars.playerList[selectedPlayerIndex]);
								}
							}

							ImGui::ColorEdit4(xorstr_("##SkelfrinColor"), GameVars.skeletonFrinColorArray, ImGuiColorEditFlags_NoInputs);
							ImGui::SameLine();
							ImGui::Text(xorstr_("Friend Skeleton"));
							ImGui::ColorEdit4(xorstr_("##BoxfrinColor"), GameVars.boxFrinColorArray, ImGuiColorEditFlags_NoInputs);
							ImGui::SameLine();
							ImGui::Text(xorstr_("Friend Box"));
							ImGui::ColorEdit4(xorstr_("##LinefrinColor"), GameVars.linesFrinColorArray, ImGuiColorEditFlags_NoInputs);
							ImGui::SameLine();
							ImGui::Text(xorstr_("Friend line"));
						}

					}
					ImGui::EndChild();

					break;
				}

				switch (subhead_selected) {
				case SUBHEAD_2:

					ImGui::SetCursorPos({ 25, 75 });
					ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(13.0f / 255.0f, 13.0f / 255.0f, 13.0f / 255.0f, 240.0f / 255.0f));
					ImGui::BeginChild(xorstr_("##containerItems"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Itens:"));
						ImGui::Spacing();

						for (size_t i = 0; i < GameVars.itemList.size(); i++) {
							const std::string& itemName = GameVars.itemList[i];


							std::ostringstream displayText;
							displayText << itemName;

							if (ImGui::Selectable(displayText.str().c_str(), selectedItemIndex == itemDisplayIndex)) {
								selectedItemIndex = itemDisplayIndex;
							}

							itemDisplayIndex++;
						}

					}
					ImGui::EndChild();
					ImGui::PopStyleColor();

					ImGui::SetCursorPos({ 285, 75 });
					ImGui::BeginChild(xorstr_("##containerItemDetails"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
						ImGui::Checkbox(xorstr_("Show Items"), &GameVars.itemlistm);

						if (selectedItemIndex != -1 && selectedItemIndex < itemDisplayIndex) {
							ImGui::Text("Item: %s", GameVars.itemList[selectedItemIndex].c_str());
							ImGui::Text("Distance: %.1fm", GameVars.itemDistances[selectedItemIndex]);

							ImGui::Button(xorstr_("Pull Item"));
							ImGui::Button(xorstr_("Tp Item"));

						}
					}
					ImGui::EndChild();

					GameVars.itemList.clear();
					GameVars.itemDistances.clear();

					break;
				}


				break;
			}

			GameVars.playerList.clear();
			GameVars.playerDistances.clear();

			switch (head_selected) {
			case HEAD_4:

				ImGui::SetCursorPos({ 25, 75 });
				ImGui::BeginChild(xorstr_("##container"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options 1"));
					ImGui::Spacing();

					ImGui::Checkbox(xorstr_("Speed Hack"), &GameVars.speedhh);

					ImGui::Checkbox(xorstr_("Loot Teleport"), &GameVars.LootTeleport);

					ImGui::Checkbox(xorstr_("Corpse Teleport"), &GameVars.CorpseTeleport);

					ImGui::Checkbox(xorstr_("Force Third Person"), &GameVars.tperson);

					if (GameVars.LootTeleport || GameVars.CorpseTeleport) {

						ImGui::Text(xorstr_("TP Key:"));
						ImGui::SameLine();

						const char* buttonText2 = GameVars.lootTPHotkey == 0 ? "Set Hotkey" : GetKeyName(GameVars.lootTPHotkey);
						ImVec2 textSize2 = ImGui::CalcTextSize(buttonText2);
						ImVec2 buttonSize2(textSize2.x + 10, textSize2.y + 5);
						if (ImGui::Button(buttonText2, buttonSize2)) {
							ImGui::OpenPopup(xorstr_("SetTPHotkey"));
						}

						if (ImGui::BeginPopup(xorstr_("SetTPHotkey")))
						{
							ImGui::Text(xorstr_("Press any key..."));

							int key = CaptureKeyOrMouse();
							if (key != 0) {
								GameVars.lootTPHotkey = key;
								ImGui::CloseCurrentPopup();
							}

							ImGui::EndPopup();
						}
					}

				}
				ImGui::EndChild();


				ImGui::SetCursorPos({ 285, 75 });
				ImGui::BeginChild(xorstr_("##container1"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Options 2"));
					ImGui::Spacing();

					if (GameVars.speedhh) {
						ImGui::SliderInt(xorstr_("Speed"), &GameVars.sliderSpeed, 1, 10);
						switch (GameVars.sliderSpeed) {
						case 1: GameVars.tickness = 10000000; break;
						case 2: GameVars.tickness = 9000000; break;
						case 3: GameVars.tickness = 8000000; break;
						case 4: GameVars.tickness = 7000000; break;
						case 5: GameVars.tickness = 6000000; break;
						case 6: GameVars.tickness = 5000000; break;
						case 7: GameVars.tickness = 4000000; break;
						case 8: GameVars.tickness = 3000000; break;
						case 9: GameVars.tickness = 2000000; break;
						case 10: GameVars.tickness = 1000000; break;
						default: GameVars.tickness = 10000000;
						}
					}

					if (GameVars.LootTeleport || GameVars.CorpseTeleport) {
						ImGui::Checkbox(xorstr_("Teleport Fov"), &GameVars.ToggleTpFov);
						ImGui::SliderFloat(xorstr_("FOV Tp"), &GameVars.tpfov, 1, 999);
						ImGui::ColorEdit4(xorstr_("##FovtpColor"), GameVars.colorfovtp, ImGuiColorEditFlags_NoInputs);
						ImGui::SameLine();
						ImGui::Text(xorstr_("FOV Color"));
					}

				}
				ImGui::EndChild();

				break;
			}

			switch (head_selected) {
			case HEAD_5:

				const std::string configDir = "C:\\SaturnDayZ\\";
				EnsureDirectoryExists(configDir); // Garante que o diret�rio existe
				static bool saveConfigOpen = false;

				// Atualiza a lista de configura��es quando a interface � aberta
				if (configFiles.empty()) {
					UpdateConfigList(configDir);
				}

				ImGui::SetCursorPos({ 25, 75 });
				ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(13.0f / 255.0f, 13.0f / 255.0f, 13.0f / 255.0f, 240.0f / 255.0f));
				ImGui::BeginChild(xorstr_("##container"), ImVec2(230, 275), false, ImGuiWindowFlags_NoScrollbar); {
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Configs"));
					ImGui::Separator();
					ImGui::Spacing();

					for (const auto& file : configFiles) {
						if (ImGui::Selectable(file.c_str())) {
							LoadSettings((configDir + file).c_str());
							RegisterMenuToggleHotkey();
						}
					}

				}
				ImGui::EndChild();
				ImGui::PopStyleColor();

				ImGui::SetCursorPos({ 285, 75 });
				ImGui::BeginChild(xorstr_("##container1"), ImVec2(230, 275), false, ImGuiWindowFlags_AlwaysVerticalScrollbar); {
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Others"));
					ImGui::Spacing();

					if (ImGui::Button("Load Config")) {
						GameVars.loadConfigRequested = true;
					}
					ImGui::SameLine();
					if (ImGui::Button("Save Config")) {
						SaveConfig();
					}

					ImGui::Checkbox(xorstr_("Streamproof"), &GameVars.streamproof);

					ImGui::Separator();
					ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), xorstr_("Bindings"));
					ImGui::Text(xorstr_("Menu toggle"));
					ImGui::SameLine();
					{
						const char* menuBindText = GameVars.menuToggleHotkey == 0 ? xorstr_("Set") : GetKeyName(GameVars.menuToggleHotkey);
						ImVec2 textSize = ImGui::CalcTextSize(menuBindText);
						ImVec2 buttonSize(textSize.x + 10, textSize.y + 5);
						if (ImGui::Button(menuBindText, buttonSize)) {
							ImGui::OpenPopup(xorstr_("SetMenuToggleHotkey"));
						}
						if (ImGui::BeginPopup(xorstr_("SetMenuToggleHotkey"))) {
							ImGui::Text(xorstr_("Press any key..."));
							int key = CaptureKeyOrMouse();
							if (key != 0) {
								GameVars.menuToggleHotkey = key;
								RegisterMenuToggleHotkey();
								ImGui::CloseCurrentPopup();
							}
							ImGui::EndPopup();
						}
					}

					ImGui::Text(xorstr_("DMA full refresh"));
					ImGui::SameLine();
					{
						const char* dmaBindText = GameVars.dmaRefreshHotkey == 0 ? xorstr_("Set") : GetKeyName(GameVars.dmaRefreshHotkey);
						ImVec2 textSize2 = ImGui::CalcTextSize(dmaBindText);
						ImVec2 buttonSize2(textSize2.x + 10, textSize2.y + 5);
						if (ImGui::Button(dmaBindText, buttonSize2)) {
							ImGui::OpenPopup(xorstr_("SetDmaRefreshHotkey"));
						}
						if (ImGui::BeginPopup(xorstr_("SetDmaRefreshHotkey"))) {
							ImGui::Text(xorstr_("Press any key..."));
							int key2 = CaptureKeyOrMouse();
							if (key2 != 0) {
								GameVars.dmaRefreshHotkey = key2;
								ImGui::CloseCurrentPopup();
							}
							ImGui::EndPopup();
						}
					}

					ImGui::Checkbox(xorstr_("Black screen on start"), &GameVars.blackStartupOverlay);
					{
						bool htPrev = GameVars.hideTaskbarOnStartup;
						ImGui::Checkbox(xorstr_("Hide taskbar"), &GameVars.hideTaskbarOnStartup);
						if (htPrev != GameVars.hideTaskbarOnStartup)
							ApplyTaskbarVisibility(GameVars.hideTaskbarOnStartup);
					}

					if (ImGui::Button(xorstr_("Unload Cheat"))) {
						AtExitRestoreTaskbar();
						exit(0);
					}

					ImGui::Checkbox(xorstr_("Show id bones (SURVIVOR)"), &GameVars.idbones);
					ImGui::Checkbox(xorstr_("Show id bones (INFECTED)"), &GameVars.idbonesZ);

				}
				ImGui::EndChild();

				break;
			}

			ImGui::End();
		}

	if (GameVars.loadConfigRequested) {
		LoadConfig();
		GameVars.loadConfigRequested = false;
	}

	// Perf: time the DirectX render/present
	static LARGE_INTEGER pFreq = {};
	if (!pFreq.QuadPart) QueryPerformanceFrequency(&pFreq);
	auto hpc_now = []() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; };
	double msScale = 1000.0 / pFreq.QuadPart;

	auto t0 = hpc_now();

	if (p_Device->BeginScene() >= 0)
	{
		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

		p_Device->EndScene();
	}

	auto t1 = hpc_now();

	HRESULT result = p_Device->Present(NULL, NULL, NULL, NULL);

	auto t2 = hpc_now();

	// Log frame breakdown every 10 frames to file + console
	frameCount++;
	if (frameCount % 10 == 0) {
		double clearMs = (rAfterClear - rStart) * rMs;
		double espMs = (rAfterESP - rAfterClear) * rMs;
		double menuMs = (t0 - rAfterESP) * rMs;
		double renderMs = (t1 - t0) * msScale;
		double presentMs = (t2 - t1) * msScale;
		double totalMs = (t2 - rStart) * rMs;
		char buf[256];
		snprintf(buf, sizeof(buf), "[ FRAME %d ] Clear=%.0f ESP=%.0f Menu=%.0f Render=%.0f Present=%.0f TOTAL=%.0fms",
			frameCount, clearMs, espMs, menuMs, renderMs, presentMs, totalMs);
		Log(buf);
	}

	if (result == D3DERR_DEVICELOST && p_Device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
	{
		ImGui_ImplDX9_InvalidateDeviceObjects();
		p_Device->Reset(&p_Params);
		ImGui_ImplDX9_CreateDeviceObjects();
	}

}

WPARAM ov::loop()
{
	MSG msg;

	// Stutter detection: measure time between render() calls
	static LARGE_INTEGER loopFreq = {};
	if (!loopFreq.QuadPart) QueryPerformanceFrequency(&loopFreq);
	auto loop_now = []() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; };
	double loopToMs = 1000.0 / loopFreq.QuadPart;
	long long lastRenderEnd = loop_now();

	while (true)
	{
		ZeroMemory(&msg, sizeof(MSG));
		while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_HOTKEY && msg.wParam == kMenuHotkeyId)
			{
				GameVars.menuShow = !GameVars.menuShow;
				continue;
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		// Detect gap between frames (message pump stalls, DWM, etc)
		long long beforeRender = loop_now();
		double gapMs = (beforeRender - lastRenderEnd) * loopToMs;
		if (gapMs > 50.0) {
			char gapBuf[128];
			snprintf(gapBuf, sizeof(gapBuf), "[ !!! RENDER GAP ] %.0fms between frames (msg pump/DWM stall)", gapMs);
			Log(gapBuf);
		}

		ov::render();
		lastRenderEnd = loop_now();
	}
	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	clean_directx();
	DestroyWindow(globals.OverlayWnd);

	return msg.wParam;
}

void ov::clean_directx()
{
	ApplyTaskbarVisibility(false);

	if (globals.OverlayWnd && IsWindow(globals.OverlayWnd))
		UnregisterHotKey(globals.OverlayWnd, kMenuHotkeyId);

	if (p_Device != NULL)
	{
		p_Device->EndScene();
		p_Device->Release();
	}
	if (p_Object != NULL)
	{
		p_Object->Release();
	}
}