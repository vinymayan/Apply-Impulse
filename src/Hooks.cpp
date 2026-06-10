#include "Hooks.h"
#include "Events.h"
#include <string_view>

namespace
{
	template <int ID>
	struct ShouldRespondToActorCollisionHook
	{
		static bool thunk(RE::Actor* a_actor, const RE::MovementMessageActorCollision& a_msg, const RE::ActorHandlePtr& a_target)
		{
			auto* target = a_target.get();
			const bool actorSuppressed = IsCollisionDamageSuppressed(a_actor);
			const bool targetSuppressed = IsCollisionDamageSuppressed(target);
			const auto actorID = a_actor ? a_actor->GetFormID() : 0;
			const auto targetID = target ? target->GetFormID() : 0;

			SKSE::log::debug(
				"FFC ShouldRespondToActorCollision hook [{}] chamado: actor={:08X}, target={:08X}, actorSuppressed={}, targetSuppressed={}",
				ID,
				actorID,
				targetID,
				actorSuppressed,
				targetSuppressed);

			if (actorSuppressed || targetSuppressed) {
				if (a_actor) {
					if (auto* charController = a_actor->GetCharController()) {
						charController->fallTime = 0.0f;
					}
					RestoreSuppressedCollisionHealth(a_actor);
				}
				if (target) {
					if (auto* charController = target->GetCharController()) {
						charController->fallTime = 0.0f;
					}
					RestoreSuppressedCollisionHealth(target);
				}

				SKSE::log::debug("FFC ShouldRespondToActorCollision hook [{}]: colisao bloqueada por a_inflictDamage=false.", ID);
				return false;
			}

			const bool result = func(a_actor, a_msg, a_target);
			SKSE::log::debug("FFC ShouldRespondToActorCollision hook [{}]: original retornou {}.", ID, result);
			return result;
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	template <int ID>
	struct HandleHealthDamageHook
	{
		static void thunk(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage)
		{
			const bool actorSuppressed = IsCollisionDamageSuppressed(a_actor);
			const auto actorID = a_actor ? a_actor->GetFormID() : 0;
			const auto attackerID = a_attacker ? a_attacker->GetFormID() : 0;

			SKSE::log::debug(
				"FFC HandleHealthDamage hook [{}] chamado: actor={:08X}, attacker={:08X}, damage={}, suppressed={}",
				ID,
				actorID,
				attackerID,
				a_damage,
				actorSuppressed);

			if (a_damage != 0.0f && !a_attacker && actorSuppressed) {
				if (a_actor) {
					if (auto* charController = a_actor->GetCharController()) {
						charController->fallTime = 0.0f;
					}
					RestoreSuppressedCollisionHealth(a_actor);
				}

				SKSE::log::debug("FFC HandleHealthDamage hook [{}]: dano sem atacante bloqueado por a_inflictDamage=false.", ID);
				return;
			}

			SKSE::log::debug("FFC HandleHealthDamage hook [{}]: chamando original.", ID);
			func(a_actor, a_attacker, a_damage);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	template <int ID>
	void InstallActorHooks(REL::VariantID a_vtable, std::string_view a_name)
	{
		REL::Relocation<std::uintptr_t> vtable{ a_vtable };
		ShouldRespondToActorCollisionHook<ID>::func = vtable.write_vfunc(REL::Relocate(0x126, 0x126, 0x128), ShouldRespondToActorCollisionHook<ID>::thunk);
		HandleHealthDamageHook<ID>::func = vtable.write_vfunc(REL::Relocate(0x104, 0x104, 0x106), HandleHealthDamageHook<ID>::thunk);

		SKSE::log::info(
			"FFC hooks instalados em {} [{}]: ShouldRespondToActorCollision original={:X}, HandleHealthDamage original={:X}",
			a_name,
			ID,
			ShouldRespondToActorCollisionHook<ID>::func.address(),
			HandleHealthDamageHook<ID>::func.address());
	}
}

namespace Hooks
{
	void Install()
	{
		static bool installed = false;
		if (installed) {
			SKSE::log::info("FFC Hooks::Install ignorado: hooks ja estavam instalados.");
			return;
		}
		installed = true;

		InstallActorHooks<0>(RE::VTABLE_Actor[0], "Actor");
		InstallActorHooks<1>(RE::VTABLE_Character[0], "Character");
		InstallActorHooks<2>(RE::VTABLE_PlayerCharacter[0], "PlayerCharacter");

		SKSE::log::info("Hooks de colisao/dano instalados.");
	}
}
