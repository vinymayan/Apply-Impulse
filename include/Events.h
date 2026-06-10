#pragma once
#include "PayloadAPI.h"
#include "FFC_API.h"

// Função de impulso que já está funcionando perfeitamente
void ApplyCustomRotation(RE::Actor* a_actor, float a_yawDegrees, float a_time);
void ApplyCustomRotation3D(RE::Actor* a_actor, float a_pitchDegrees, float a_rollDegrees, float a_yawDegrees, float a_time, std::string_view a_mode);
void ApplyCustomVelocityImpulse(RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_time, bool a_inflictDamage);
bool IsCollisionDamageSuppressed(RE::Actor* a_actor);
void RestoreSuppressedCollisionHealth(RE::Actor* a_actor);

// Ouvinte para o payload de Força (X | Y | Z)
class ApplyImpulseFFC : public payloadinterpreter::PayloadHandler
{
public:
	static ApplyImpulseFFC* GetSingleton()
	{
		static ApplyImpulseFFC singleton;
		return &singleton;
	}

	void Process(RE::TESObjectREFR* a_holder, const std::string_view& a_payload, RE::BShkbAnimationGraph* a_animationGraph) override;
};

// Ouvinte para o payload de Rotação (Graus Yaw)
class ApplyRotationFFCHandler : public payloadinterpreter::PayloadHandler
{
public:
	static ApplyRotationFFCHandler* GetSingleton()
	{
		static ApplyRotationFFCHandler singleton;
		return &singleton;
	}

	void Process(RE::TESObjectREFR* a_holder, const std::string_view& a_payload, RE::BShkbAnimationGraph* a_animationGraph) override;
};
