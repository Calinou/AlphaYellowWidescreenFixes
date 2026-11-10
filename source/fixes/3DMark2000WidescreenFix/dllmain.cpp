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

		FallbackToDesktopResolution(m_newResX, m_newResY);

		spdlog_confparse(m_newResX);
		spdlog_confparse(m_newResY);
		spdlog_confparse(m_fovFactor);
	}

	void ApplyFix() override
	{
		m_newAspectRatio = static_cast<float>(m_newResX) / static_cast<float>(m_newResY);
		m_aspectRatioScale = m_newAspectRatio / m_oldAspectRatio;
		spdlog::info("Render resolution: {}x{} (aspect ratio = {}, scale = {})", m_newResX, m_newResY, m_newAspectRatio, m_aspectRatioScale);

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
		auto* fullscreenFunc = GetProcAddress(engineModule, "?setFullscreenMode@P_Driver@@QAEXPAUHWND__@@IIIW4Buffering@1@IPAVP_ZBufferFormat@@_N@Z");
		auto* windowFunc = GetProcAddress(engineModule, "?setWindowMode@P_Driver@@QAEXPAUHWND__@@IIPAVP_ZBufferFormat@@_N@Z");

		if (!fullscreenFunc || !windowFunc)
		{
			spdlog::error("Failed to locate P_Driver::setFullscreenMode/setWindowMode exports.");
			return;
		}

		auto resolutionCallback = [](SafetyHookContext& ctx)
		{
			if (auto* self = s_instance_)
			{
				auto* width = reinterpret_cast<std::uint32_t*>(ctx.esp + 0x8);
				auto* height = reinterpret_cast<std::uint32_t*>(ctx.esp + 0xC);
				spdlog::info("Overriding display mode {}x{} -> {}x{}", *width, *height, self->m_newResX, self->m_newResY);
				*width = static_cast<std::uint32_t>(self->m_newResX);
				*height = static_cast<std::uint32_t>(self->m_newResY);
			}
		};

		m_fullscreenModeHook = safetyhook::create_mid(fullscreenFunc, resolutionCallback);
		m_windowModeHook = safetyhook::create_mid(windowFunc, resolutionCallback);
		auto* validateFunc = reinterpret_cast<std::uint8_t*>(GetProcAddress(engineModule, "?validate@P_Camera@@QAEXXZ"));

		if (!validateFunc)
		{
			spdlog::error("Failed to locate P_Camera::validate export.");
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

		auto* clearRect = reinterpret_cast<std::uint8_t*>(GetProcAddress(engineModule, "?clearScreen@P_Driver@@QAEXABUtagRECT@@W4ClearingMode@1@ABVG_Color@@@Z"));
		auto* endScene = reinterpret_cast<std::uint8_t*>(GetProcAddress(engineModule, "?endScene@P_Driver@@QAEXXZ"));

		if (!clearRect || !endScene)
		{
			spdlog::error("Failed to locate P_Driver functions; intro side bars disabled.");
			return;
		}

		// When a demo run ends (or is aborted) the engine restores the desktop display mode; restart the bar timer there so
		// it counts from 0 again when the demo is restarted from the 3DMark2000 interface.
		if (auto* restoreDisplayMode = GetProcAddress(engineModule, "?restoreDisplayMode@P_Driver@@QAEXXZ"))
		{
			m_restoreDisplayModeHook = safetyhook::create_mid(restoreDisplayMode, [](SafetyHookContext&)
			{
				if (auto* self = s_instance_)
				{
					self->m_firstFrameTick = 0;
				}
			});
		}
		else
		{
			spdlog::error("Failed to locate P_Driver::restoreDisplayMode; bar timer will not reset on demo restart.");
		}
		m_clearScreenRect = reinterpret_cast<ClearScreenRectFn>(clearRect);
		spdlog::info("P_Driver::endScene: Address is e2mfc.dll+{:x}", endScene - engineBase);

		// The intro (loading text, DirectX 7 logo and white flash) is laid out for 4:3, so the widened sides are covered with
		// black bars for the first seconds after the demo starts rendering.
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
		const float tanY = baseTanX * (vpBottom - vpTop) / vpWidth;
		const float tanX = baseTanX * m_aspectRatioScale;

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

		const int viewWidth = static_cast<int>(std::lround(surfaceHeight * m_oldAspectRatio));
		const int barWidth = (surfaceWidth - viewWidth) / 2;
		if (barWidth <= 0)
		{
			return;
		}

		const std::uint8_t color[4] = { shade, shade, shade, 0xFF };
		const RECT leftBar{ 0, 0, barWidth, surfaceHeight };
		const RECT rightBar{ surfaceWidth - barWidth, 0, surfaceWidth, surfaceHeight };
		m_clearScreenRect(reinterpret_cast<void*>(driver), &leftBar, 2, color);
		m_clearScreenRect(reinterpret_cast<void*>(driver), &rightBar, 2, color);
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
	SafetyHookMid m_restoreDisplayModeHook{};
	float* m_fovScaleConstant = nullptr;
	float m_fovFactor = 1.0f;

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
