#include "logger.h"
#include "PayloadAPI.h"
#include "Events.h"
#include "Hooks.h"

namespace PapyrusAPI
{
	void ApplyCustomVelocityImpulse(RE::StaticFunctionTag*, RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_time, bool a_inflictDamage)
	{
		::ApplyCustomVelocityImpulse(a_actor, a_x, a_y, a_z, a_time, a_inflictDamage);
	}

	void ApplyCustomRotation(RE::StaticFunctionTag*, RE::Actor* a_actor, float a_yawDegrees, float a_time)
	{
		::ApplyCustomRotation(a_actor, a_yawDegrees, a_time);
	}

	bool IsCollisionDamageSuppressed(RE::StaticFunctionTag*, RE::Actor* a_actor)
	{
		return ::IsCollisionDamageSuppressed(a_actor);
	}

	void RestoreSuppressedCollisionHealth(RE::StaticFunctionTag*, RE::Actor* a_actor)
	{
		::RestoreSuppressedCollisionHealth(a_actor);
	}

	bool Bind(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->RegisterFunction("ApplyCustomVelocityImpulse", "ApplyImpulse", ApplyCustomVelocityImpulse);
		a_vm->RegisterFunction("ApplyCustomRotation", "ApplyImpulse", ApplyCustomRotation);
		a_vm->RegisterFunction("IsCollisionDamageSuppressed", "ApplyImpulse", IsCollisionDamageSuppressed);
		return true;
	}
}

// Crie uma classe concreta que herda da interface e chama suas funções locais
class FFCInterfaceImpl : public FFC_API::IFFCInterface {
public:
    void ApplyCustomVelocityImpulse(RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_time, bool a_inflictDamage) override {
        // Chama a sua função original definida no Events.cpp com a nova flag
        ::ApplyCustomVelocityImpulse(a_actor, a_x, a_y, a_z, a_time, a_inflictDamage);
    }

    void ApplyCustomRotation(RE::Actor* a_actor, float a_yawDegrees, float a_time) override {
        // Chama a sua função original definida no Events.cpp
        ::ApplyCustomRotation(a_actor, a_yawDegrees, a_time);
    }

    bool IsCollisionDamageSuppressed(RE::Actor* a_actor) override {
        return ::IsCollisionDamageSuppressed(a_actor);
    }
};

// Instância global única da sua API
static FFCInterfaceImpl g_FFCInterface;

extern "C" __declspec(dllexport) void* GetFFCAPI()
{
    return &g_FFCInterface;
}

void PayloadInterpreterMessageListener(SKSE::MessagingInterface::Message* a_msg)
{
    if (!a_msg || std::string_view(a_msg->sender) != "PayloadInterpreter") {
        return;
    }

    auto payloadInterpreterMsg = static_cast<payloadinterpreter::API::Message*>(a_msg->data);

    if (payloadInterpreterMsg) {
        payloadInterpreterMsg->payloadHandlerCollector->RegisterPayloadHandler("ApplyImpulseFFC", ApplyImpulseFFC::GetSingleton());
        payloadInterpreterMsg->payloadHandlerCollector->RegisterPayloadHandler("ApplyRotationFFC", ApplyRotationFFCHandler::GetSingleton());

        SKSE::log::info("Sucesso: Tags 'ApplyImpulseFFC' e 'ApplyRotationFFC' registradas.");
    }
    else {
        SKSE::log::error("Erro: Mensagem do Payload Interpreter veio sem o coletor de handlers.");
    }
}
void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kPostLoad) {
        if (SKSE::GetMessagingInterface()->RegisterListener("PayloadInterpreter", PayloadInterpreterMessageListener)) {
            SKSE::log::info("Listener para PayloadInterpreter registrado com sucesso.");
        }
        Hooks::Install();
    }
    if (message->type == FFC_API::kMessage_RequestAPI) {
        // Responde enviando o ponteiro da sua API de volta
        SKSE::GetMessagingInterface()->Dispatch(FFC_API::kMessage_ProvideAPI, &g_FFCInterface, sizeof(&g_FFCInterface), message->sender);
        SKSE::log::info("FFC_API enviada para o mod: {}", message->sender);
    }
    if (message->type == SKSE::MessagingInterface::kNewGame || message->type == SKSE::MessagingInterface::kPostLoadGame) {
        // Post-load
    }
}
SKSEPluginLoad(const SKSE::LoadInterface *skse) {

    SetupLog();
    logger::info("Plugin loaded");
    SKSE::Init(skse);
    Hooks::Install();
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    SKSE::GetPapyrusInterface()->Register(PapyrusAPI::Bind);
    return true;
}
