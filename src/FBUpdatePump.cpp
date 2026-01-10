#include "FBUpdatePump.h"

#include "ActorManager.h"
#include "AnimationEvents.h"  // RegisterAnimationEventSink

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <spdlog/spdlog.h>

namespace
{
	// Enable/disable the pump without uninstalling the vfunc hook.
	std::atomic_bool g_running{ false };

	// Ensure we only install the hook once.
	std::atomic_bool g_installed{ false };

	// One-time successful registration flag (retry until true).
	bool g_animSinkRegistered = false;

	// Pathological dt clamp (keeps timelines sane through hitches/loading).
	constexpr float kMaxDtSeconds = 0.25f;

	struct PlayerUpdateHook
	{
		// Canonical vfunc index (your established value).
		static constexpr std::uint32_t kVFuncIndex = 0xAD;

		static void thunk(RE::PlayerCharacter* a_this, float a_delta)
		{
			// Call original first (per requirement).
			func(a_this, a_delta);

			// If not running, do nothing (but keep hook installed).
			if (!g_running.load(std::memory_order_acquire)) {
				return;
			}

			// Retry animation sink registration until graphs exist.
			if (!g_animSinkRegistered) {
				if (RegisterAnimationEventSink(a_this)) {
					g_animSinkRegistered = true;
					spdlog::info("[FB] Animation event sink registered via PlayerCharacter::Update");
				}
			}

			// Safety gates.
			if (a_delta <= 0.0f) {
				return;
			}

			// Clamp dt defensively.
			const float dt = std::min(a_delta, kMaxDtSeconds);

			FB::ActorManager::Update(dt);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace FB::UpdatePump
{
	void Install()
	{
		bool expected = false;
		if (!g_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
			// Already installed.
			return;
		}

		// Trampoline must exist before write_vfunc.
		(void)SKSE::GetTrampoline();

		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdateHook::func = vtbl.write_vfunc(PlayerUpdateHook::kVFuncIndex, PlayerUpdateHook::thunk);

		spdlog::info("[FB] Hooked PlayerCharacter::Update(float) vfunc index 0x{:X}", PlayerUpdateHook::kVFuncIndex);
	}

	void Start()
	{
		g_running.store(true, std::memory_order_release);
		spdlog::info("[FB] UpdatePump started");
	}

	void Stop()
	{
		g_running.store(false, std::memory_order_release);
		spdlog::info("[FB] UpdatePump stopped");
	}
}
