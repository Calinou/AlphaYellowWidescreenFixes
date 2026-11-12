#include "..\..\common\FixBase.hpp"
#include <cmath>

class _3DMark2000WidescreenFix final : public FixBase
{
public:
	explicit _3DMark2000WidescreenFix(HMODULE selfModule) : FixBase(selfModule)
	{
		s_instance_ = this;
	}

	~_3DMark2000WidescreenFix() override
	{
		if (s_instance_ == this)
		{
			s_instance_ = nullptr;
		}
	}

	const char* FixName() const override
	{
		return "3DMark2000WidescreenFix";
	}

	const char* FixVersion() const override
	{
		return "1.0";
	}

	const char* TargetName() const override
	{
		return "3DMark2000";
	}

	InitMode GetInitMode() const override
	{
		return InitMode::WorkerThread;
	}

	bool IsCompatibleExecutable(const std::string& exeName) const override
	{
		return Util::stringcmp_caseless(exeName, "3DMark2000.exe");
	}

	void ParseFixConfig(inipp::Ini<char>& ini) override
	{
		inipp::get_value(ini.sections["Settings"], "Width", m_newResX);
		inipp::get_value(ini.sections["Settings"], "Height", m_newResY);
		inipp::get_value(ini.sections["Settings"], "FOVFactor", m_fovFactor);
		inipp::get_value(ini.sections["Settings"], "UseRadialFog", m_useRadialFog);

		FallbackToDesktopResolution(m_newResX, m_newResY);

		spdlog_confparse(m_newResX);
		spdlog_confparse(m_newResY);
		spdlog_confparse(m_fovFactor);
		spdlog_confparse(m_useRadialFog);
	}

	void ApplyFix() override
	{
		m_newAspectRatio = static_cast<float>(m_newResX) / static_cast<float>(m_newResY);
		m_aspectRatioScale = m_newAspectRatio / m_oldAspectRatio;
		spdlog::info("Render resolution: {}x{} (aspect ratio = {}, scale = {})", m_newResX, m_newResY, m_newAspectRatio, m_aspectRatioScale);

		// Aspect ratios wider than the display can't be shown without stretching, so render at the desktop resolution and
		// letterbox a centered band with the requested aspect ratio instead.
		const auto desktop = Util::GetPhysicalDesktopDimensions();
		m_displayResX = m_newResX;
		m_displayResY = m_newResY;
		if (desktop.first > 0 && desktop.second > 0)
		{
			const float desktopAspectRatio = static_cast<float>(desktop.first) / static_cast<float>(desktop.second);
			if (m_newAspectRatio > desktopAspectRatio + 0.001f)
			{
				m_letterbox = true;
				m_displayResX = desktop.first;
				m_displayResY = desktop.second;
				spdlog::info("Aspect ratio is wider than the display: using {}x{} display mode with letterboxing.", m_displayResX, m_displayResY);
			}
		}

		// The MAX-FX engine (P_Camera) lives in e2mfc.dll, which 3DMark2000.exe imports statically.
		HMODULE engineModule = GetModuleHandleA("e2mfc.dll");
		if (!engineModule)
		{
			spdlog::error("e2mfc.dll is not loaded.");
			return;
		}

		auto* engineBase = reinterpret_cast<std::uint8_t*>(engineModule);

		// Override the resolution chosen in 3DMark's own settings: P_Driver::setFullscreenMode/setWindowMode receive
		// (this, HWND, width, height, ...), so width/height live at [esp+8]/[esp+0Ch] on entry.
		auto* fullscreenFunc = Memory::PatternScan(engineModule, "55 8B EC 83 E4 F8 83 EC 2C 53 56 57");
		auto* windowFunc = Memory::PatternScan(engineModule, "51 53 56 57 6A 00 8B F1 6A 00 8D 4C");

		if (!fullscreenFunc || !windowFunc)
		{
			spdlog::error("Failed to locate P_Driver::setFullscreenMode/setWindowMode memory addresses.");
			return;
		}

		auto resolutionCallback = [](SafetyHookContext& ctx)
		{
			if (auto* self = s_instance_)
			{
				auto* width = reinterpret_cast<std::uint32_t*>(ctx.esp + 0x8);
				auto* height = reinterpret_cast<std::uint32_t*>(ctx.esp + 0xC);
				spdlog::info("Overriding display mode {}x{} -> {}x{}", *width, *height, self->m_displayResX, self->m_displayResY);
				*width = static_cast<std::uint32_t>(self->m_displayResX);
				*height = static_cast<std::uint32_t>(self->m_displayResY);

				// The Direct3D driver DLL is loaded on demand, so it's only available once a display mode is set.
				self->InstallRadialFogHook();
			}
		};

		m_fullscreenModeHook = safetyhook::create_mid(fullscreenFunc, resolutionCallback);
		m_windowModeHook = safetyhook::create_mid(windowFunc, resolutionCallback);
		auto* validateFunc = Memory::PatternScan(engineModule, "8B 81 B8 00 00 00 8B D0 C1 EA 06 F6");

		if (!validateFunc)
		{
			spdlog::error("Failed to locate P_Camera::validate memory address.");
			return;
		}

		// P_Camera::validate:
		//   fld [ecx+118h] (FOV) ; fmul [fovScaleConstant] ; fptan ; fst [ecx+208h] (tanX)
		//   tanY = tanX * (vpBottom - vpTop) / (vpRight - vpLeft) -> [ecx+20Ch]
		if (validateFunc[0x10] != 0xD9 || validateFunc[0x11] != 0x81 || validateFunc[0x19] != 0xD8 || validateFunc[0x1A] != 0x0D)
		{
			spdlog::error("Unexpected P_Camera::validate code layout.");
			return;
		}

		m_fovScaleConstant = *reinterpret_cast<float**>(validateFunc + 0x1B);
		spdlog::info("P_Camera::validate: Address is e2mfc.dll+{:x}, FOV scale constant = {}", validateFunc - engineBase, *m_fovScaleConstant);

		// Hook the final 'ret' of P_Camera::validate (ecx = camera). Every consumer of the tangents (projection matrix in
		// prepare, frustum/guard band planes via getViewPlaneRay, getViewportCoordinate, ...) calls validate first, so
		// patching here keeps all of them consistent, even when validate runs outside of prepare (e.g. from setFOV).
		std::uint8_t* validateRetAddress = validateFunc + 0x53;
		if (validateRetAddress[0] != 0xC3 || validateRetAddress[1] != 0x90 || validateRetAddress[4] != 0x90)
		{
			spdlog::error("Unexpected P_Camera::validate epilogue.");
			return;
		}

		spdlog::info("P_Camera::validate hook: Address is e2mfc.dll+{:x}", validateRetAddress - engineBase);

		m_cameraHook = safetyhook::create_mid(validateRetAddress, [](SafetyHookContext& ctx)
		{
			auto* self = s_instance_;
			if (self && ctx.ecx)
			{
				self->AdjustCamera(ctx.ecx);
			}
		});

		auto* clearRect = Memory::PatternScan(engineModule, "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 0C 53 56 57 89 65 F0 6A 00 8B F1 6A 00 8D 4D EC FF 15 ?? ?? ?? ?? 8B 4D 0C 8B 55 08 8B 06 51 8B 4D 10 52 51 C7 45");
		auto* endScene = Memory::PatternScan(engineModule, "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 56 6A 00 8B F1 6A 00 8D 4C 24 0C FF 15 ?? ?? ?? ?? 8B 06 8B CE C7 44 24 10 00 00 00 00 FF 50 10 8D 4C 24 04 C6 46 74 00 C7 44");

		if (!clearRect || !endScene)
		{
			spdlog::error("Failed to locate P_Driver functions; intro side bars disabled.");
			return;
		}

		// When a demo run ends (or is aborted) the engine restores the desktop display mode; restart the bar timer there so
		// it counts from 0 again when the demo is restarted from the 3DMark2000 interface.
		if (auto* restoreDisplayMode = Memory::PatternScan(engineModule, "51 56 6A 00 8B F1 6A 00 8D 4C 24 0C FF 15 ?? ?? ?? ?? 83 7E 68 02 7E 07 8B 06 8B CE FF 50 24 8D"))
		{
			m_restoreDisplayModeHook = safetyhook::create_mid(restoreDisplayMode, [](SafetyHookContext&)
			{
				if (auto* self = s_instance_)
				{
					// The display mode is also restored during startup before anything is rendered; only a run that has
					// rendered frames ends the demo.
					if (self->m_firstFrameTick != 0)
					{
						self->m_demoMode = false;
					}

					self->m_firstFrameTick = 0;
				}
			});
		}
		else
		{
			spdlog::error("Failed to locate P_Driver::restoreDisplayMode; bar timer will not reset on demo restart.");
		}

		// The demo's initialization routine (which logs "Demo initializing took %2.2f seconds") only runs when the demo is
		// started, whether from the GUI or with /demo, so it identifies demo mode as opposed to benchmark mode.
		auto DemoInitScanResult = Memory::PatternScan(ExeModule(), "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 81 EC 4C 01 00 00 53 56 57 8B D9 89 65 F0 89 5D B0 E8");
		if (DemoInitScanResult)
		{
			spdlog::info("Demo Initialization: Address is {:s}+{:x}", ExeName().c_str(), DemoInitScanResult - reinterpret_cast<std::uint8_t*>(ExeModule()));

			m_demoInitHook = safetyhook::create_mid(DemoInitScanResult, [](SafetyHookContext&)
			{
				if (auto* self = s_instance_)
				{
					spdlog::info("Demo mode started.");
					self->m_demoMode = true;
				}
			});
		}
		else
		{
			spdlog::error("Failed to locate demo initialization memory address; side bars disabled.");
		}

		m_clearScreenRect = reinterpret_cast<ClearScreenRectFn>(clearRect);
		spdlog::info("P_Driver::endScene: Address is e2mfc.dll+{:x}", endScene - engineBase);

		// The demo intro (loading text, DirectX 7 logo and white flash) is laid out for 4:3, so the widened sides are covered
		// with black bars for the first seconds after the demo starts rendering. Benchmark runs don't get these bars.
		m_endSceneHook = safetyhook::create_mid(endScene, [](SafetyHookContext& ctx)
		{
			auto* self = s_instance_;
			if (!self || !ctx.ecx)
			{
				return;
			}

			const DWORD now = GetTickCount();
			if (self->m_firstFrameTick == 0)
			{
				self->m_firstFrameTick = now;
			}

			if (self->m_letterbox)
			{
				self->DrawLetterbox(ctx.ecx);
			}

			if (!self->m_demoMode)
			{
				return;
			}

			const DWORD elapsed = now - self->m_firstFrameTick;
			if (elapsed < m_introBarsDurationMs)
			{
				self->DrawSideBars(ctx.ecx, 0x00);
			}
			else if (elapsed >= m_whiteBarsStartMs && elapsed < m_whiteBarsEndMs)
			{
				self->DrawSideBars(ctx.ecx, 0xFF);
			}
		});
	}

private:
	void AdjustCamera(uintptr_t camera)
	{
		const float fov = *reinterpret_cast<float*>(camera + 0x118);
		const float vpLeft = *reinterpret_cast<float*>(camera + 0x1CC);
		const float vpRight = *reinterpret_cast<float*>(camera + 0x1D0);
		const float vpTop = *reinterpret_cast<float*>(camera + 0x1D4);
		const float vpBottom = *reinterpret_cast<float*>(camera + 0x1D8);

		const float vpWidth = vpRight - vpLeft;
		if (!(fov > 0.0f) || std::fabs(vpWidth) < 0.0001f)
		{
			return;
		}

		// Recomputed from the camera FOV every time so repeated calls stay idempotent.
		const float baseTanX = std::tan(fov * *m_fovScaleConstant) * m_fovFactor;
		const float tanX = baseTanX * m_aspectRatioScale;

		// When letterboxing, keep pixels square on the full display surface; the visible band then shows exactly the
		// requested aspect ratio.
		const float tanY = m_letterbox
			? tanX * static_cast<float>(m_displayResY) / static_cast<float>(m_displayResX)
			: baseTanX * (vpBottom - vpTop) / vpWidth;

		*reinterpret_cast<float*>(camera + 0x208) = tanX;
		*reinterpret_cast<float*>(camera + 0x20C) = tanY;
	}

	void DrawSideBars(uintptr_t driver, std::uint8_t shade)
	{
		// The driver's own surface dimensions (P_Driver +0x8/+0xC) define the coordinate space used by clearScreen, which
		// can differ from the physical window size (e.g. with DPI scaling).
		const int surfaceWidth = *reinterpret_cast<int*>(driver + 0x8);
		const int surfaceHeight = *reinterpret_cast<int*>(driver + 0xC);
		if (surfaceWidth <= 0 || surfaceHeight <= 0)
		{
			return;
		}

		int bandTop = 0;
		int bandHeight = surfaceHeight;
		GetVisibleBand(surfaceWidth, surfaceHeight, bandTop, bandHeight);

		const int viewWidth = static_cast<int>(std::lround(bandHeight * m_oldAspectRatio));
		const int barWidth = (surfaceWidth - viewWidth) / 2;
		if (barWidth <= 0)
		{
			return;
		}

		const std::uint8_t color[4] = { shade, shade, shade, 0xFF };
		const RECT leftBar{ 0, bandTop, barWidth, bandTop + bandHeight };
		const RECT rightBar{ surfaceWidth - barWidth, bandTop, surfaceWidth, bandTop + bandHeight };
		m_clearScreenRect(reinterpret_cast<void*>(driver), &leftBar, 2, color);
		m_clearScreenRect(reinterpret_cast<void*>(driver), &rightBar, 2, color);
	}

	void InstallRadialFogHook()
	{
		if (!m_useRadialFog || m_rangeFogHook)
		{
			return;
		}

		HMODULE driverModule = GetModuleHandleA("e2_d3d_driver_mfc.dll");
		if (!driverModule)
		{
			spdlog::error("e2_d3d_driver_mfc.dll is not loaded; radial fog disabled.");
			return;
		}

		// Device setup: mov eax,[edi] ; mov ecx,[eax] ; push ebx ; push 30h (D3DRENDERSTATE_RANGEFOGENABLE) ; push eax ;
		// call [ecx+50h] (IDirect3DDevice7::SetRenderState). The game uses vertex fog with unchanged fog start/end, so
		// enabling range-based fog turns planar fog into radial fog at the same distance.
		auto* RangeFogScanResult = Memory::PatternScan(driverModule, "8B 07 8B 08 53 6A 30 50 FF 51 50");
		if (!RangeFogScanResult)
		{
			spdlog::error("Failed to locate range fog render state memory address.");
			return;
		}

		spdlog::info("Range Fog Render State: Address is e2_d3d_driver_mfc.dll+{:x}", RangeFogScanResult + 7 - reinterpret_cast<std::uint8_t*>(driverModule));

		// Hook at 'push eax': [esp] = render state, [esp+4] = value.
		m_rangeFogHook = safetyhook::create_mid(RangeFogScanResult + 7, [](SafetyHookContext& ctx)
		{
			*reinterpret_cast<std::uint32_t*>(ctx.esp + 0x4) = TRUE;
		});
	}

	void GetVisibleBand(int surfaceWidth, int surfaceHeight, int& bandTop, int& bandHeight) const
	{
		bandTop = 0;
		bandHeight = surfaceHeight;
		if (m_letterbox)
		{
			bandHeight = std::min(surfaceHeight, static_cast<int>(std::lround(surfaceWidth / m_newAspectRatio)));
			bandTop = (surfaceHeight - bandHeight) / 2;
		}
	}

	void DrawLetterbox(uintptr_t driver)
	{
		const int surfaceWidth = *reinterpret_cast<int*>(driver + 0x8);
		const int surfaceHeight = *reinterpret_cast<int*>(driver + 0xC);
		if (surfaceWidth <= 0 || surfaceHeight <= 0)
		{
			return;
		}

		int bandTop = 0;
		int bandHeight = surfaceHeight;
		GetVisibleBand(surfaceWidth, surfaceHeight, bandTop, bandHeight);
		if (bandTop <= 0)
		{
			return;
		}

		const std::uint8_t black[4] = { 0x00, 0x00, 0x00, 0xFF };
		const RECT topBar{ 0, 0, surfaceWidth, bandTop };
		const RECT bottomBar{ 0, bandTop + bandHeight, surfaceWidth, surfaceHeight };
		m_clearScreenRect(reinterpret_cast<void*>(driver), &topBar, 2, black);
		m_clearScreenRect(reinterpret_cast<void*>(driver), &bottomBar, 2, black);
	}

	using ClearScreenRectFn = void(__thiscall*)(void* driver, const RECT* rect, int clearingMode, const void* color);

	static constexpr float m_oldAspectRatio = 4.0f / 3.0f;
	static constexpr DWORD m_introBarsDurationMs = 16750;
	static constexpr DWORD m_whiteBarsStartMs = 80000;
	static constexpr DWORD m_whiteBarsEndMs = 87750;

	SafetyHookMid m_cameraHook{};
	SafetyHookMid m_fullscreenModeHook{};
	SafetyHookMid m_windowModeHook{};
	SafetyHookMid m_endSceneHook{};
	ClearScreenRectFn m_clearScreenRect = nullptr;
	DWORD m_firstFrameTick = 0;
	SafetyHookMid m_demoInitHook{};
	bool m_demoMode = false;
	SafetyHookMid m_restoreDisplayModeHook{};
	float* m_fovScaleConstant = nullptr;
	float m_fovFactor = 1.0f;
	bool m_useRadialFog = true;
	SafetyHookMid m_rangeFogHook{};
	int m_displayResX = 0;
	int m_displayResY = 0;
	bool m_letterbox = false;

	inline static _3DMark2000WidescreenFix* s_instance_ = nullptr;
};

static std::unique_ptr<_3DMark2000WidescreenFix> g_fix;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		DisableThreadLibraryCalls(hModule);
		g_fix = std::make_unique<_3DMark2000WidescreenFix>(hModule);
		g_fix->Start();
		break;
	}

	case DLL_PROCESS_DETACH:
	{
		g_fix->Shutdown();
		g_fix.reset();
		break;
	}

	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	default:
		break;
	}

	return TRUE;
}
