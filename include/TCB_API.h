#pragma once
#include <cstdint>
#include <Windows.h>

namespace RE { class Actor; }

namespace TCB_API {
    // Borrowed interface; never delete. Call on the game thread, after PostLoad.
    // Angles are radians. Both plugins must use compatible CommonLib/runtime ABIs.
    class IV1 {
    public:
        virtual bool IsActive(RE::Actor* actor) const noexcept = 0;
        virtual bool AddAuthoredYawDelta(RE::Actor* actor, float delta) noexcept = 0;
        // Also succeeds while tracking is locked; false leaves outYaw untouched.
        virtual bool TryGetMovementYaw(RE::Actor* actor, float& outYaw) const noexcept = 0;
        // Starts magnetism, updates an active session without resetting it, or
        // fully ends and suppresses it until enabled again. Game-thread only.
        virtual bool SetMagnetismActive(RE::Actor* actor, bool active, float maxAngleDegrees,
                                        float turnSpeedDegreesPerSecond, float strength,
                                        bool allowRetargeting) noexcept = 0;
    };
    using RequestAPI = IV1* (*)(std::uint32_t);
    inline IV1* api = nullptr;

    inline IV1* Connect() {
        auto module = GetModuleHandleW(L"TriggerBehaviour.dll");
        auto request = module ? reinterpret_cast<RequestAPI>(GetProcAddress(module, "GetTCBAPI")) : nullptr;
        api = request ? request(1) : nullptr;
        return api;
    }

}
