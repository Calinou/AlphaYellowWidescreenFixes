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
		inipp::get_value(ini.sections["Settings"], "FOVFactor", m_fovFactor);
		spdlog_confparse(m_fovFactor);
	}

	void ApplyFix() override
	{
		// The MAX-FX engine (P_Camera) lives in e2mfc.dll, which 3DMark2000.exe imports statically.
		HMODULE engineModule = GetModuleHandleA("e2mfc.dll");
		if (!engineModule)
		{
			spdlog::error("e2mfc.dll is not loaded.");
			return;
		}

		auto* engineBase = reinterpret_cast<std::uint8_t*>(engineModule);
		auto* validateFunc = reinterpret_cast<std::uint8_t*>(GetProcAddress(engineModule, "?validate@P_Camera@@QAEXXZ"));
		auto* prepareFunc = reinterpret_cast<std::uint8_t*>(GetProcAddress(engineModule, "?prepare@P_Camera@@QAEXXZ"));

		if (!validateFunc || !prepareFunc)
		{
			spdlog::error("Failed to locate P_Camera::validate / P_Camera::prepare exports.");
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

		// P_Camera::prepare: push ebp; mov ebp,esp; sub esp,20h; push ebx; push esi; push edi; mov esi,ecx; call validate; <hook>
		std::uint8_t* prepareHookAddress = prepareFunc + 0x10;
		if (prepareFunc[0x0B] != 0xE8 || prepareHookAddress + *reinterpret_cast<std::int32_t*>(prepareFunc + 0x0C) != validateFunc)
		{
			spdlog::error("Unexpected P_Camera::prepare code layout.");
			return;
		}

		spdlog::info("P_Camera::prepare hook: Address is e2mfc.dll+{:x}", prepareHookAddress - engineBase);

		m_cameraHook = safetyhook::create_mid(prepareHookAddress, [](SafetyHookContext& ctx)
		{
			auto* self = s_instance_;
			if (self && ctx.esi)
			{
				self->AdjustCamera(ctx.esi);
			}
		});
	}

private:
	void UpdateAspectRatio()
	{
		int width = 0;
		int height = 0;

		RECT rect{};
		HWND window = GetActiveWindow();
		if (window && GetClientRect(window, &rect))
		{
			width = rect.right - rect.left;
			height = rect.bottom - rect.top;
		}

		if (width <= 0 || height <= 0)
		{
			auto desktopDimensions = Util::GetPhysicalDesktopDimensions();
			width = desktopDimensions.first;
			height = desktopDimensions.second;
		}

		if (width != m_newResX || height != m_newResY)
		{
			m_newResX = width;
			m_newResY = height;
			m_newAspectRatio = static_cast<float>(width) / static_cast<float>(height);
			m_aspectRatioScale = m_newAspectRatio / m_oldAspectRatio;
			spdlog::info("Render resolution: {}x{} (aspect ratio = {}, scale = {})", width, height, m_newAspectRatio, m_aspectRatioScale);
		}
	}

	void AdjustCamera(uintptr_t camera)
	{
		UpdateAspectRatio();

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

	static constexpr float m_oldAspectRatio = 4.0f / 3.0f;

	SafetyHookMid m_cameraHook{};
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
