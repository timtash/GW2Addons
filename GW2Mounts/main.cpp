#include "main.h"
#include "d3d9.h"
#include <tchar.h>
#include <imgui.h>
#include <examples\directx9_example\imgui_impl_dx9.h>
#include <set>
#include <sstream>
#include "UnitQuad.h"
#include <d3dx9.h>
#include "Config.h"
#include "Utility.h"

const float BaseSpriteSize = 0.4f;
const float CircleRadiusScreen = 256.f / 1664.f * BaseSpriteSize * 0.5f;

bool LoadedFromGame = true;
Config Cfg;

// Active state
std::set<uint> DownKeys;
bool DisplayMountOverlay = false;
bool DisplayOptionsWindow = false;

char KeybindDisplayString[256];
bool SettingKeybind = false;
D3DXVECTOR2 OverlayPosition;
mstime OverlayTime, MountHoverTime;

enum CurrentMountHovered_t
{
	CMH_NONE = -1,
	CMH_RAPTOR = 0,
	CMH_SPRINGER = 1,
	CMH_SKIMMER = 2,
	CMH_JACKAL = 3
};
CurrentMountHovered_t CurrentMountHovered = CMH_NONE;

WNDPROC BaseWndProc;
HMODULE OriginalD3D9 = nullptr;
HMODULE DllModule = nullptr;
IDirect3DDevice9* RealDevice = nullptr;

// Rendering
uint ScreenWidth, ScreenHeight;
std::unique_ptr<UnitQuad> Quad;
ID3DXEffect* MainEffect = nullptr;
IDirect3DTexture9* MountsTexture = nullptr;
IDirect3DTexture9* MountTextures[4];

void SetKeybindDisplayString(const std::set<uint>& keys)
{
	std::string keybind = "";
	for (const auto& k : keys)
	{
		keybind += GetKeyName(k) + std::string(" + ");
	}

	strcpy_s(KeybindDisplayString, (keybind.size() > 0 ? keybind.substr(0, keybind.size() - 3) : keybind).c_str());
}

void LoadMountTextures()
{
	D3DXCreateTextureFromResource(RealDevice, DllModule, MAKEINTRESOURCE(IDR_MOUNTS), &MountsTexture);
	for (uint i = 0; i < 4; i++)
		D3DXCreateTextureFromResource(RealDevice, DllModule, MAKEINTRESOURCE(IDR_MOUNT1 + i), &MountTextures[i]);
}

void UnloadMountTextures()
{
	COM_RELEASE(MountsTexture);

	for (uint i = 0; i < 4; i++)
		COM_RELEASE(MountTextures[i]);
}

IDirect3D9 *WINAPI Direct3DCreate9(UINT SDKVersion)
{
	if (!OriginalD3D9)
	{
		TCHAR path[MAX_PATH];
		GetSystemDirectory(path, MAX_PATH);
		_tcscat_s(path, TEXT("\\d3d9.dll"));
		OriginalD3D9 = LoadLibrary(path);
	}
	orig_Direct3DCreate9 = (D3DC9)GetProcAddress(OriginalD3D9, "Direct3DCreate9");

	if(LoadedFromGame)
		return new f_iD3D9(orig_Direct3DCreate9(SDKVersion));
	else
		return orig_Direct3DCreate9(SDKVersion);
}

bool WINAPI DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
	{
		DllModule = hModule;

		Cfg.Load();

		SetKeybindDisplayString(Cfg.MountOverlayKeybind());
	}
	case DLL_PROCESS_DETACH:
	{
		if (OriginalD3D9)
		{
			FreeLibrary(OriginalD3D9);
			OriginalD3D9 = nullptr;
		}
	}
	}
	return true;
}

extern LRESULT ImGui_ImplDX9_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// Transform SYSKEY* messages into KEY* messages instead
	UINT effective_msg = msg;
	if (msg == WM_SYSKEYDOWN)
	{
		effective_msg = WM_KEYDOWN;
		if (((lParam >> 29) & 1) == 1)
			DownKeys.insert(VK_MENU);
		else
			DownKeys.erase(VK_MENU);
	}
	if (msg == WM_SYSKEYUP)
		effective_msg = WM_KEYUP;

	bool input_key_down = false, input_key_up = false;
	switch (effective_msg)
	{
	case WM_KEYDOWN:
		DownKeys.insert((uint)wParam);
		input_key_down = true;
		break;
	case WM_LBUTTONDOWN:
		DownKeys.insert(VK_LBUTTON);
		input_key_down = true;
		break;
	case WM_MBUTTONDOWN:
		DownKeys.insert(VK_MBUTTON);
		input_key_down = true;
		break;
	case WM_RBUTTONDOWN:
		DownKeys.insert(VK_RBUTTON);
		input_key_down = true;
		break;
	case WM_KEYUP:
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		input_key_up = true;
		break;
	}

	if (DisplayMountOverlay && effective_msg == WM_MOUSEMOVE)
	{
		auto io = ImGui::GetIO();

		D3DXVECTOR2 MousePos;
		MousePos.x = io.MousePos.x / (float)ScreenWidth;
		MousePos.y = io.MousePos.y / (float)ScreenHeight;
		MousePos -= OverlayPosition;

		CurrentMountHovered_t LastMountHovered = CurrentMountHovered;

		if (D3DXVec2LengthSq(&MousePos) > CircleRadiusScreen * CircleRadiusScreen)
		{
			if (MousePos.x < 0 && abs(MousePos.x) > abs(MousePos.y)) // Raptor, 0
				CurrentMountHovered = CMH_RAPTOR;
			else if (MousePos.x > 0 && abs(MousePos.x) > abs(MousePos.y)) // Jackal, 3
				CurrentMountHovered = CMH_JACKAL;
			else if (MousePos.y < 0 && abs(MousePos.x) < abs(MousePos.y)) // Springer, 1
				CurrentMountHovered = CMH_SPRINGER;
			else if (MousePos.y > 0 && abs(MousePos.x) < abs(MousePos.y)) // Skimmer, 2
				CurrentMountHovered = CMH_SKIMMER;
		}
		else
			CurrentMountHovered = CMH_NONE;

		if (LastMountHovered != CurrentMountHovered)
			MountHoverTime = timeInMS();
	}

	bool isMenuKeybind = GetAsyncKeyState(VK_SHIFT) && GetAsyncKeyState(VK_MENU) && wParam == 'M';

	if (input_key_down || input_key_up)
	{
		bool oldMountOverlay = DisplayMountOverlay;

		DisplayMountOverlay = !Cfg.MountOverlayKeybind().empty() && DownKeys == Cfg.MountOverlayKeybind();
		if (input_key_up && 
			(
				(effective_msg == WM_KEYUP && Cfg.MountOverlayKeybind().count((uint)wParam)) ||
				(effective_msg == WM_LBUTTONUP && Cfg.MountOverlayKeybind().count(VK_LBUTTON)) ||
				(effective_msg == WM_MBUTTONUP && Cfg.MountOverlayKeybind().count(VK_MBUTTON)) ||
				(effective_msg == WM_RBUTTONUP && Cfg.MountOverlayKeybind().count(VK_RBUTTON))
			))
			DisplayMountOverlay = false;

		if (DisplayMountOverlay && !oldMountOverlay)
		{
			auto io = ImGui::GetIO();
			OverlayPosition.x = io.MousePos.x / (float)ScreenWidth;
			OverlayPosition.y = io.MousePos.y / (float)ScreenHeight;
			OverlayTime = timeInMS();
		}
		else if (!DisplayMountOverlay && oldMountOverlay)
		{
			CurrentMountHovered = CMH_NONE;
		}

		if (isMenuKeybind)
			DisplayOptionsWindow = true;
		else if (SettingKeybind)
		{
			SetKeybindDisplayString(DownKeys);

			switch (wParam)
			{
			case VK_MENU:
			case VK_CONTROL:
			case VK_SHIFT:
				break;
			default:
				SettingKeybind = false;

				Cfg.MountOverlayKeybind(DownKeys);
			}
		}
	}

	if (msg == WM_SYSKEYUP)
		DownKeys.erase(VK_MENU);

	switch (effective_msg)
	{
	case WM_KEYUP:
		DownKeys.erase((uint)wParam);
		break;
	case WM_LBUTTONUP:
		DownKeys.erase(VK_LBUTTON);
		break;
	case WM_MBUTTONUP:
		DownKeys.erase(VK_MBUTTON);
		break;
	case WM_RBUTTONUP:
		DownKeys.erase(VK_RBUTTON);
		break;
	}

	if ((input_key_down || input_key_up) && isMenuKeybind)
		return true;

	ImGui_ImplDX9_WndProcHandler(hWnd, msg, wParam, lParam);
	ImGuiIO& io = ImGui::GetIO();

	switch (effective_msg)
	{
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MOUSEWHEEL:
		if (io.WantCaptureMouse)
			return true;
		break;
	case WM_KEYDOWN:
	case WM_KEYUP:
		if (io.WantCaptureKeyboard)
			return true;
		break;
	case WM_CHAR:
		if (io.WantTextInput)
			return true;
		break;
	}

	return CallWindowProc(BaseWndProc, hWnd, msg, wParam, lParam);
}

/*************************
Augmented Callbacks
*************************/

ULONG GameRefCount = 1;

HRESULT f_iD3D9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType,
	HWND hFocusWindow, DWORD BehaviorFlags,
	D3DPRESENT_PARAMETERS *pPresentationParameters,
	IDirect3DDevice9 **ppReturnedDeviceInterface)
{
	// Hook WndProc
	BaseWndProc = (WNDPROC)GetWindowLongPtr(hFocusWindow, GWLP_WNDPROC);
	SetWindowLongPtr(hFocusWindow, GWLP_WNDPROC, (LONG_PTR)&WndProc);

	pPresentationParameters->BackBufferFormat = D3DFMT_A8R8G8B8;

	// Create and initialize device
	IDirect3DDevice9* temp_device = nullptr;
	HRESULT hr = f_pD3D->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &temp_device);
	RealDevice = temp_device;
	*ppReturnedDeviceInterface = new f_IDirect3DDevice9(temp_device);

	// Init ImGui
	ImGuiIO& imio = ImGui::GetIO();
	imio.IniFilename = Cfg.ImGuiConfigLocation();

	// Setup ImGui binding
	ImGui_ImplDX9_Init(hFocusWindow, temp_device);

	// Initialize graphics
	ScreenWidth = pPresentationParameters->BackBufferWidth;
	ScreenHeight = pPresentationParameters->BackBufferHeight;
	try
	{
		Quad = std::make_unique<UnitQuad>(RealDevice);
	}
	catch (...)
	{
		Quad = nullptr;
	}
	D3DXCreateEffectFromResource(RealDevice, DllModule, MAKEINTRESOURCE(IDR_SHADER), nullptr, nullptr, 0, nullptr, &MainEffect, nullptr);
	LoadMountTextures();

	// Initialize reference count for device object
	GameRefCount = 1;

	return hr;
}

HRESULT f_IDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters)
{
	ImGui_ImplDX9_InvalidateDeviceObjects();
	Quad.reset();
	UnloadMountTextures();
	COM_RELEASE(MainEffect);

	pPresentationParameters->BackBufferFormat = D3DFMT_A8R8G8B8;

	HRESULT hr = f_pD3DDevice->Reset(pPresentationParameters);

	ScreenWidth = pPresentationParameters->BackBufferWidth;
	ScreenHeight = pPresentationParameters->BackBufferHeight;

	ImGui_ImplDX9_CreateDeviceObjects();
	D3DXCreateEffectFromResource(RealDevice, DllModule, MAKEINTRESOURCE(IDR_SHADER), nullptr, nullptr, 0, nullptr, &MainEffect, nullptr);
	LoadMountTextures();
	try
	{
		Quad = std::make_unique<UnitQuad>(RealDevice);
	}
	catch (...)
	{
		Quad = nullptr;
	}

	return hr;
}


HRESULT f_IDirect3DDevice9::Present(CONST RECT *pSourceRect, CONST RECT *pDestRect, HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion)
{
	f_pD3DDevice->BeginScene();

	ImGui_ImplDX9_NewFrame();

	if (DisplayMountOverlay || DisplayOptionsWindow)
	{
		if (DisplayOptionsWindow)
		{
			ImGui::Begin("Mounts Options Menu", &DisplayOptionsWindow);
			ImGui::InputText("Keybind", KeybindDisplayString, 256, ImGuiInputTextFlags_ReadOnly);
			if (ImGui::Button("Set Keybind"))
			{
				SettingKeybind = true;
				KeybindDisplayString[0] = '\0';
			}
			if (Cfg.ShowGriffon() != ImGui::Checkbox("Show 5th mount", &Cfg.ShowGriffon()))
				Cfg.ShowGriffonSave();
			ImGui::End();
		}

		ImGui::Render();

		if (DisplayMountOverlay && MainEffect && Quad)
		{
			auto currentTime = timeInMS();

			uint passes = 0;

			Quad->Bind();

			// Setup viewport
			D3DVIEWPORT9 vp;
			vp.X = vp.Y = 0;
			vp.Width = (DWORD)ScreenWidth;
			vp.Height = (DWORD)ScreenHeight;
			vp.MinZ = 0.0f;
			vp.MaxZ = 1.0f;
			RealDevice->SetViewport(&vp);

			D3DXVECTOR4 screenSize((float)ScreenWidth, (float)ScreenHeight, 1.f / ScreenWidth, 1.f / ScreenHeight);

			D3DXVECTOR4 baseSpriteDimensions;
			baseSpriteDimensions.x = OverlayPosition.x;
			baseSpriteDimensions.y = OverlayPosition.y;
			baseSpriteDimensions.z = BaseSpriteSize * screenSize.y * screenSize.z;
			baseSpriteDimensions.w = BaseSpriteSize;

			// Setup render state: fully shader-based
			MainEffect->SetTechnique("MountImage");
			MainEffect->SetVector("g_vScreenSize", &screenSize);

			MainEffect->SetVector("g_vSpriteDimensions", &baseSpriteDimensions);
			MainEffect->SetTexture("texMountImage", MountsTexture);
			MainEffect->SetFloat("g_fTimer", min(1.f, (currentTime - OverlayTime) / 1000.f * 6));

			MainEffect->Begin(&passes, 0);
			MainEffect->BeginPass(0);
			Quad->Draw();
			MainEffect->EndPass();
			MainEffect->End();


			if (CurrentMountHovered != CMH_NONE)
			{
				D3DXVECTOR4 overlaySpriteDimensions = baseSpriteDimensions;

				if (CurrentMountHovered == CMH_RAPTOR)
				{
					overlaySpriteDimensions.x -= 0.5f * BaseSpriteSize * 0.5f * screenSize.y * screenSize.z;
					overlaySpriteDimensions.z *= 0.5f;
					overlaySpriteDimensions.w = BaseSpriteSize * 1024.f / 1664.f;
				}
				else if (CurrentMountHovered == CMH_JACKAL)
				{
					overlaySpriteDimensions.x += 0.5f * BaseSpriteSize * 0.5f * screenSize.y * screenSize.z;
					overlaySpriteDimensions.z *= 0.5f;
					overlaySpriteDimensions.w = BaseSpriteSize * 1024.f / 1664.f;
				}
				else if (CurrentMountHovered == CMH_SPRINGER)
				{
					overlaySpriteDimensions.y -= 0.5f * BaseSpriteSize * 0.5f;
					overlaySpriteDimensions.w *= 0.5f;
					overlaySpriteDimensions.z = BaseSpriteSize * 1024.f / 1664.f * screenSize.y * screenSize.z;
				}
				else if (CurrentMountHovered == CMH_SKIMMER) // Skimmer, 2
				{
					overlaySpriteDimensions.y += 0.5f * BaseSpriteSize * 0.5f;
					overlaySpriteDimensions.w *= 0.5f;
					overlaySpriteDimensions.z = BaseSpriteSize * 1024.f / 1664.f * screenSize.y * screenSize.z;
				}

				MainEffect->SetFloat("g_fTimer", sqrt(min(1.f, (currentTime - MountHoverTime) / 1000.f * 6)));
				MainEffect->SetTexture("texMountImage", MountTextures[CurrentMountHovered]);
				MainEffect->SetVector("g_vSpriteDimensions", &overlaySpriteDimensions);

				MainEffect->Begin(&passes, 0);
				MainEffect->BeginPass(0);
				Quad->Draw();
				MainEffect->EndPass();
				MainEffect->End();
			}
		}
	}

	f_pD3DDevice->EndScene();

	return f_pD3DDevice->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

void Shutdown()
{
	ImGui_ImplDX9_Shutdown();

	Quad.reset();
	COM_RELEASE(MainEffect);

	UnloadMountTextures();
}

ULONG f_IDirect3DDevice9::AddRef()
{
	GameRefCount++;
	return f_pD3DDevice->AddRef();
}

ULONG f_IDirect3DDevice9::Release()
{
	GameRefCount--;
	if (GameRefCount == 0)
		Shutdown();

	return f_pD3DDevice->Release();
}