#include "PlayerUpdateHook.h"

#include "ActorManager.h"

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <cstdint>
#include <spdlog/spdlog.h>

namespace
{
    // Pathological dt clamp (required by checklist)
    constexpr float kMaxDtSeconds = 0.25f;

    struct PlayerUpdateHook
    {
        // vfunc index provided by you (canonical)
        static constexpr std::uint32_t kVFuncIndex = 0xAD;

        static void thunk(RE::PlayerCharacter* a_this, float a_delta)
        {
            // Call original first (per requirement)
            func(a_this, a_delta);

            // Safety gates
            if (a_delta <= 0.0f) {
                return;
            }

            float dt = a_delta;
            if (dt > kMaxDtSeconds) {
                dt = kMaxDtSeconds;
            }

            FB::ActorManager::Update(dt);
        }

        static inline REL::Relocation<decltype(thunk)> func;
    };
}

namespace FB::Hooks
{
    void InstallPlayerUpdateHook()
    {
        // Trampoline must exist before write_vfunc
        auto& trampoline = SKSE::GetTrampoline();

        // This is the standard CommonLib way to grab the PlayerCharacter vtable.
        // If this symbol does not exist in your setup, tell me the compile error and I’ll adjust.
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };

        PlayerUpdateHook::func = vtbl.write_vfunc(PlayerUpdateHook::kVFuncIndex, PlayerUpdateHook::thunk);

        spdlog::info("[FB] Hooked PlayerCharacter::Update(float) vfunc index 0x{:X}", PlayerUpdateHook::kVFuncIndex);
    }
}
