#include "Events.h"
#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <set>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <thread>
#include <chrono>
#include "DelayedDispatcher.h"
#include "TrueDirectionalMovementAPI.h"


// Função auxiliar interna para realizar o Split por caractere pipe '|'
static std::vector<std::string> SplitString(const std::string_view& s, char delimiter) {
	std::vector<std::string> tokens;
	std::string token;
	std::istringstream tokenStream((std::string(s)));
	while (std::getline(tokenStream, token, delimiter)) {
		if (!token.empty()) {
			tokens.push_back(token);
		}
	}
	return tokens;
}

// --- EQUAÇÕES DE SUAVIZAÇÃO (EASING FUNCTIONS) ---
static float GetEasingProgress(float a_progress) {
	if (a_progress > 1.0f) a_progress = 1.0f;
	if (a_progress < 0.0f) a_progress = 0.0f;

	// Cubic Ease-In-Out: Início lento, aceleração forte no meio, estabiliza suave no final.
	// Perfeito para o comportamento gradativo que você descreveu.
	return a_progress < 0.5f ? 4.0f * a_progress * a_progress * a_progress : 1.0f - std::pow(-2.0f * a_progress + 2.0f, 3.0f) / 2.0f;
}

// --- ESTRUTURAS E REGISTROS DE FORÇA (IMPULSO) ---
struct ActiveImpulse {
	RE::hkVector4 totalVelocity; // Força total final alvo
	bool hasHorizontal = false;
	bool hasVertical = false;
	int elapsedFrames = 0;       // Quantos frames já se passaram
	int totalFrames = 15;        // Total de frames que o impulso deve durar
	bool isSmooth = false;
	bool inflictDamage = true;
};

struct NoDamageLandingProtection {
	int graceFrames = 0;
	float protectedHealth = 0.0f;
};

static std::map<RE::FormID, std::vector<ActiveImpulse>> g_ActiveImpulses;
static std::set<RE::FormID> g_ActiveLoops;
static std::map<RE::FormID, NoDamageLandingProtection> g_NoDamageLandingGrace;
static std::mutex g_ImpulseMutex;
static constexpr int kNoDamageLandingGraceFrames = 12;

// --- ESTRUTURAS E REGISTROS DE ROTAÇÃO SUAVE ---
struct ActiveRotation {
	float totalYaw;             // Rotação total desejada em radianos
	int elapsedFrames = 0;      // Contador de frames reais passados
	int totalFrames = 15;       // Total de frames de duração
	float lastEasedProgress = 0.0f; // Guarda o progresso da curva do frame anterior
};

static std::map<RE::FormID, std::vector<ActiveRotation>> g_ActiveRotations;
static std::set<RE::FormID> g_ActiveRotationLoops;
static std::mutex g_RotationMutex;

static bool IsActorGrounded(RE::bhkCharacterController* a_charController)
{
	return a_charController->flags.all(RE::CHARACTER_FLAGS::kSupport) ||
	       a_charController->context.currentState == RE::hkpCharacterStateType::kOnGround;
}

static void DisableCollisionDamage(RE::bhkCharacterController* a_charController)
{
	a_charController->fallTime = 0.0f;
	a_charController->flags.reset(RE::CHARACTER_FLAGS::kHitDamage);
	a_charController->flags.reset(RE::CHARACTER_FLAGS::kRecordHits);
}

static void RestoreCollisionDamage(RE::bhkCharacterController* a_charController)
{
	if (!a_charController) {
		return;
	}

	a_charController->flags.set(RE::CHARACTER_FLAGS::kHitDamage);
	a_charController->flags.set(RE::CHARACTER_FLAGS::kRecordHits);
}

static void RestoreDefaultFallPhysics(RE::bhkCharacterController* a_charController)
{
	if (!a_charController) {
		return;
	}

	a_charController->gravity = 1.0f;
	a_charController->fallTime = 0.0f;
}

static float GetActorHealth(RE::Actor* a_actor)
{
	auto* avOwner = a_actor ? a_actor->AsActorValueOwner() : nullptr;
	return avOwner ? avOwner->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
}

static void RestoreSuppressedCollisionHealthLocked(RE::Actor* a_actor)
{
	if (!a_actor) {
		return;
	}

	auto it = g_NoDamageLandingGrace.find(a_actor->GetFormID());
	if (it == g_NoDamageLandingGrace.end()) {
		return;
	}

	auto* avOwner = a_actor->AsActorValueOwner();
	if (!avOwner) {
		return;
	}

	const float currentHealth = avOwner->GetActorValue(RE::ActorValue::kHealth);
	const float protectedHealth = it->second.protectedHealth;
	if (currentHealth < protectedHealth) {
		avOwner->RestoreActorValue(RE::ActorValue::kHealth, protectedHealth - currentHealth);
		SKSE::log::debug(
			"FFC: Health restaurada durante protecao de colisao actor={:08X}, current={}, protected={}",
			a_actor->GetFormID(),
			currentHealth,
			protectedHealth);
	}
}

static void StartNoDamageLandingProtection(RE::FormID a_actorID, RE::Actor* a_actor)
{
	const float currentHealth = GetActorHealth(a_actor);
	auto& protection = g_NoDamageLandingGrace[a_actorID];
	const bool wasInactive = protection.graceFrames <= 0;
	const float previousProtectedHealth = protection.protectedHealth;
	protection.graceFrames = kNoDamageLandingGraceFrames;
	protection.protectedHealth = std::max(protection.protectedHealth, currentHealth);
	if (wasInactive || protection.protectedHealth != previousProtectedHealth) {
		SKSE::log::debug(
			"FFC: protecao de colisao iniciada/renovada actor={:08X}, currentHealth={}, protectedHealth={}",
			a_actorID,
			currentHealth,
			protection.protectedHealth);
	}
}

static bool UpdateNoDamageLandingProtection(RE::FormID a_actorID, RE::bhkCharacterController* a_charController)
{
	if (!a_charController) {
		std::lock_guard<std::mutex> lock(g_ImpulseMutex);
		g_NoDamageLandingGrace.erase(a_actorID);
		return false;
	}

	DisableCollisionDamage(a_charController);

	std::lock_guard<std::mutex> lock(g_ImpulseMutex);
	auto it = g_NoDamageLandingGrace.find(a_actorID);
	if (it == g_NoDamageLandingGrace.end()) {
		return false;
	}

	if (IsActorGrounded(a_charController)) {
		it->second.graceFrames--;
		if (it->second.graceFrames <= 0) {
			g_NoDamageLandingGrace.erase(it);
			RestoreCollisionDamage(a_charController);
			return false;
		}
	}
	else {
		it->second.graceFrames = kNoDamageLandingGraceFrames;
	}

	return true;
}

bool IsCollisionDamageSuppressed(RE::Actor* a_actor)
{
	if (!a_actor) {
		return false;
	}

	std::lock_guard<std::mutex> lock(g_ImpulseMutex);
	return g_NoDamageLandingGrace.contains(a_actor->GetFormID());
}

void RestoreSuppressedCollisionHealth(RE::Actor* a_actor)
{
	if (!a_actor) {
		return;
	}

	std::lock_guard<std::mutex> lock(g_ImpulseMutex);
	RestoreSuppressedCollisionHealthLocked(a_actor);
}

// =========================================================================
// SISTEMA DE IMPULSO (FORCE) - DISTRIBUIÇÃO CORRIGIDA FRAME-A-FRAME
// =========================================================================

void ProcessActorImpulses(RE::ActorHandle a_actorHandle, RE::FormID a_actorID)
{
	auto actorPtr = a_actorHandle.get();
	if (!actorPtr) {
		std::lock_guard<std::mutex> lock(g_ImpulseMutex);
		g_ActiveImpulses.erase(a_actorID);
		g_ActiveLoops.erase(a_actorID);
		g_NoDamageLandingGrace.erase(a_actorID);
		return;
	}

	auto* charController = actorPtr->GetCharController();
	std::vector<ActiveImpulse> remainingImpulses;

	RE::hkVector4 totalTargetVel{ 0.0f, 0.0f, 0.0f, 0.0f };
	bool anyHorizontalActive = false;
	bool anyVerticalActive = false;
	bool anyVerticalRemaining = false;
	bool anyNoDamageActive = false; // <--- Rastreia se algum impulso ativo bloqueia o dano
	bool hasRemainingImpulses = false;

	{
		std::lock_guard<std::mutex> lock(g_ImpulseMutex);
		auto it = g_ActiveImpulses.find(a_actorID);

		if (it == g_ActiveImpulses.end() || it->second.empty()) {
			if (!g_NoDamageLandingGrace.contains(a_actorID)) {
				g_ActiveLoops.erase(a_actorID);
				RestoreDefaultFallPhysics(charController);
				// Garante a restauração das flags originais ao limpar o loop
				RestoreCollisionDamage(charController);
				return;
			}
		}
		else {
			for (auto& impulse : it->second) {
				if (impulse.elapsedFrames < impulse.totalFrames) {
					impulse.elapsedFrames++;

					if (impulse.isSmooth) {
						float progress = static_cast<float>(impulse.elapsedFrames) / static_cast<float>(impulse.totalFrames);
						float easedMultiplier = GetEasingProgress(progress);

						totalTargetVel.quad.m128_f32[0] += impulse.totalVelocity.quad.m128_f32[0] * easedMultiplier;
						totalTargetVel.quad.m128_f32[1] += impulse.totalVelocity.quad.m128_f32[1] * easedMultiplier;
						totalTargetVel.quad.m128_f32[2] += impulse.totalVelocity.quad.m128_f32[2] * easedMultiplier;
					}
					else {
						totalTargetVel.quad.m128_f32[0] += impulse.totalVelocity.quad.m128_f32[0];
						totalTargetVel.quad.m128_f32[1] += impulse.totalVelocity.quad.m128_f32[1];
						totalTargetVel.quad.m128_f32[2] += impulse.totalVelocity.quad.m128_f32[2];
					}

					if (impulse.hasHorizontal) { anyHorizontalActive = true; }
					if (impulse.hasVertical) { anyVerticalActive = true; }
					if (!impulse.inflictDamage) {
						anyNoDamageActive = true; // <--- Ativa o bloqueio de dano
						StartNoDamageLandingProtection(a_actorID, actorPtr.get());
					}

					if (impulse.elapsedFrames < impulse.totalFrames) {
						remainingImpulses.push_back(impulse);
						if (impulse.hasVertical) {
							anyVerticalRemaining = true;
						}
					}
				}
			}

			hasRemainingImpulses = !remainingImpulses.empty();
			if (!hasRemainingImpulses) {
				g_ActiveImpulses.erase(a_actorID);
				if (!g_NoDamageLandingGrace.contains(a_actorID)) {
					g_ActiveLoops.erase(a_actorID);
				}
			}
			else {
				it->second = remainingImpulses;
			}
		}
	}

	if (IsCollisionDamageSuppressed(actorPtr.get())) {
		RestoreSuppressedCollisionHealth(actorPtr.get());
	}

	if (!anyHorizontalActive && !anyVerticalActive) {
		RestoreDefaultFallPhysics(charController);

		if (UpdateNoDamageLandingProtection(a_actorID, charController)) {
			Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(5), [a_actorHandle, a_actorID]() {
				SKSE::GetTaskInterface()->AddTask([a_actorHandle, a_actorID]() {
					ProcessActorImpulses(a_actorHandle, a_actorID);
					});
				});
			return;
		}

		std::lock_guard<std::mutex> lock(g_ImpulseMutex);
		g_ActiveLoops.erase(a_actorID);
		return;
	}

	if (charController) {
		charController->flags.set(RE::CHARACTER_FLAGS::kJumping);
		charController->flags.reset(RE::CHARACTER_FLAGS::kSupport);

		charController->wantState = RE::hkpCharacterStateType::kInAir;
		charController->context.currentState = RE::hkpCharacterStateType::kInAir;

		charController->gravity = anyVerticalActive && anyVerticalRemaining ? 0.0f : 1.0f;
		charController->fallTime = 0.0f;

		// --- CONTROLE DINÂMICO DE DANO ---
		if (anyNoDamageActive) {
			DisableCollisionDamage(charController);
		}
		else {
			RestoreCollisionDamage(charController);
		}
		// ---------------------------------

		RE::hkVector4 currentVel;
		charController->GetLinearVelocityImpl(currentVel);

		if (anyHorizontalActive) {
			currentVel.quad.m128_f32[0] = totalTargetVel.quad.m128_f32[0];
			currentVel.quad.m128_f32[1] = totalTargetVel.quad.m128_f32[1];
		}

		if (anyVerticalActive) {
			currentVel.quad.m128_f32[2] = totalTargetVel.quad.m128_f32[2];
		}
		else {
			if (currentVel.quad.m128_f32[2] > 0.0f) {
				currentVel.quad.m128_f32[2] = 0.0f;
			}
		}

		charController->SetLinearVelocityImpl(currentVel);
		charController->outVelocity = currentVel;
		charController->velocityMod = currentVel;
	}

	if (!hasRemainingImpulses && !IsCollisionDamageSuppressed(actorPtr.get())) {
		if (charController) {
			RestoreCollisionDamage(charController);
		}
		return;
	}

	Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(5), [a_actorHandle, a_actorID]() {
		SKSE::GetTaskInterface()->AddTask([a_actorHandle, a_actorID]() {
			ProcessActorImpulses(a_actorHandle, a_actorID);
			});
	});
}

void ApplyCustomVelocityImpulse(RE::Actor* a_actor, float a_x, float a_y, float a_z, float a_time, bool a_inflictDamage)
{
	if (!a_actor) return;

	auto* charController = a_actor->GetCharController();
	if (!charController) return;

	RE::hkVector4 forward = charController->forwardVec;
	RE::hkVector4 upVec{ 0.0f, 0.0f, 1.0f, 0.0f };
	RE::hkVector4 rightVec = upVec.Cross(forward);

	RE::hkVector4 impulseVel;
	impulseVel.quad.m128_f32[0] = (rightVec.quad.m128_f32[0] * a_x) - (forward.quad.m128_f32[0] * a_y);
	impulseVel.quad.m128_f32[1] = (rightVec.quad.m128_f32[1] * a_x) - (forward.quad.m128_f32[1] * a_y);
	impulseVel.quad.m128_f32[2] = a_z;
	impulseVel.quad.m128_f32[3] = 0.0f;

	bool hasHoriz = (a_x != 0.0f || a_y != 0.0f);
	bool hasVert = (a_z != 0.0f);

	RE::FormID actorID = a_actor->GetFormID();
	RE::ActorHandle actorHandle = a_actor->GetHandle();

	bool isSmooth = (a_time > 0.0f);
	int durationFrames = isSmooth ? static_cast<int>(a_time * 60.0f) : 10;
	if (durationFrames < 1) durationFrames = 1;

	// =========================================================================
	// APLICAÇÃO IMEDIATA (FRAME 0 / ELAPSED = 1)
	// =========================================================================
	charController->flags.set(RE::CHARACTER_FLAGS::kJumping);
	charController->flags.reset(RE::CHARACTER_FLAGS::kSupport);

	if (hasVert) {
		charController->context.currentState = RE::hkpCharacterStateType::kJumping;
	}
	else {
		charController->context.currentState = RE::hkpCharacterStateType::kInAir;
	}
	charController->wantState = RE::hkpCharacterStateType::kInAir;
	charController->gravity = hasVert ? 0.0f : 1.0f;
	charController->fallTime = 0.0f;

	// --- DESATIVAÇÃO DE DANO NO FRAME ZERO ---
	if (!a_inflictDamage) {
		DisableCollisionDamage(charController);
	}
	// -----------------------------------------

	RE::hkVector4 currentVel;
	charController->GetLinearVelocityImpl(currentVel);

	float initialProgress = 1.0f / static_cast<float>(durationFrames);
	float easedMultiplier = isSmooth ? GetEasingProgress(initialProgress) : 1.0f;

	if (hasHoriz) {
		currentVel.quad.m128_f32[0] = impulseVel.quad.m128_f32[0] * easedMultiplier;
		currentVel.quad.m128_f32[1] = impulseVel.quad.m128_f32[1] * easedMultiplier;
	}

	if (hasVert) {
		currentVel.quad.m128_f32[2] = impulseVel.quad.m128_f32[2] * easedMultiplier;
	}
	else {
		if (currentVel.quad.m128_f32[2] > 0.0f) {
			currentVel.quad.m128_f32[2] = 0.0f;
		}
	}

	charController->SetLinearVelocityImpl(currentVel);
	charController->outVelocity = currentVel;
	charController->velocityMod = currentVel;
	// =========================================================================

	{
		std::lock_guard<std::mutex> lock(g_ImpulseMutex);
		if (!a_inflictDamage) {
			StartNoDamageLandingProtection(actorID, a_actor);
		}

		// Adiciona a_inflictDamage à tupla armazenada
		g_ActiveImpulses[actorID].push_back({ impulseVel, hasHoriz, hasVert, 1, durationFrames, isSmooth, a_inflictDamage });

		if (g_ActiveLoops.contains(actorID)) {
			return;
		}
		g_ActiveLoops.insert(actorID);
	}

	Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(5), [actorHandle, actorID]() {
		SKSE::GetTaskInterface()->AddTask([actorHandle, actorID]() {
			ProcessActorImpulses(actorHandle, actorID);
			});
		});
}


// =========================================================================
// SISTEMA DE ROTAÇÃO (ROTATION)
// =========================================================================

void ProcessActorRotations(RE::ActorHandle a_actorHandle, RE::FormID a_actorID)
{
	auto actorPtr = a_actorHandle.get();
	if (!actorPtr) {
		std::lock_guard<std::mutex> lock(g_RotationMutex);
		g_ActiveRotations.erase(a_actorID);
		g_ActiveRotationLoops.erase(a_actorID);
		return;
	}

	std::vector<ActiveRotation> remainingRotations;
	float deltaYawTotal = 0.0f; // Acumulador de rotação incremental para este frame

	{
		std::lock_guard<std::mutex> lock(g_RotationMutex);
		auto it = g_ActiveRotations.find(a_actorID);

		if (it == g_ActiveRotations.end() || it->second.empty()) {
			g_ActiveRotationLoops.erase(a_actorID);
			return;
		}

		// Processa todas as rotações ativas de forma cumulativa (permite blends de combos)
		for (auto& rot : it->second) {
			if (rot.elapsedFrames < rot.totalFrames) {
				rot.elapsedFrames++;

				float progress = static_cast<float>(rot.elapsedFrames) / static_cast<float>(rot.totalFrames);
				float currentEased = GetEasingProgress(progress);

				// CALCULO DELTA: Descobre o quanto o ângulo deve mudar APENAS neste frame
				float deltaEased = currentEased - rot.lastEasedProgress;
				deltaYawTotal += rot.totalYaw * deltaEased;

				// Atualiza o histórico para o próximo frame
				rot.lastEasedProgress = currentEased;

				if (rot.elapsedFrames < rot.totalFrames) {
					remainingRotations.push_back(rot);
				}
			}
		}

		if (remainingRotations.empty()) {
			g_ActiveRotations.erase(a_actorID);
			g_ActiveRotationLoops.erase(a_actorID);
		}
		else {
			it->second = remainingRotations;
		}
	}

	// Aplica a variação acumulada diretamente em cima do ângulo dinâmico atual do ator
	if (std::abs(deltaYawTotal) > 0.00001f) {
		actorPtr->GetActorRuntimeData().boolBits.reset(RE::Actor::BOOL_BITS::kHeadingFixed);
		actorPtr->SetHeading(actorPtr->data.angle.z + deltaYawTotal);
	}

	// Sincronização idêntica ao sistema de impulso (Task + 16ms Delayed Dispatcher)
	Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(5), [a_actorHandle, a_actorID]() {
		SKSE::GetTaskInterface()->AddTask([a_actorHandle, a_actorID]() {
			ProcessActorRotations(a_actorHandle, a_actorID);
			});
		});
}

void ApplyCustomRotation(RE::Actor* a_actor, float a_yawDegrees, float a_time)
{
	if (!a_actor || a_actor->IsDead()) return;

	if (a_actor->IsPlayerRef()) {
		// Obtém o ponteiro da API do TDM (Versão 3)
		if (auto* tdmAPI = static_cast<TDM_API::IVTDM3*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V3))) {
			// Verifica se o player está com algum alvo travado
			if (tdmAPI->GetTargetLockState()) {
				auto myPluginHandle = SKSE::GetPluginHandle();

				// Força a desativação temporária (limpa o lock-on instantaneamente)
				tdmAPI->RequestDisableDirectionalMovement(myPluginHandle);
				// Libera logo em seguida para que o movimento direcional volte a funcionar sem o lock
				tdmAPI->ReleaseDisableDirectionalMovement(myPluginHandle);
			}
		}
	}

	float yawRadians = a_yawDegrees * 3.14159265f / 180.0f;
	RE::FormID actorID = a_actor->GetFormID();
	RE::ActorHandle actorHandle = a_actor->GetHandle();

	// Rotação instantânea (tempo zero) segura
	if (a_time <= 0.0f) {
		a_actor->GetActorRuntimeData().boolBits.reset(RE::Actor::BOOL_BITS::kHeadingFixed);
		a_actor->SetHeading(a_actor->data.angle.z + yawRadians);
		return;
	}

	int durationFrames = static_cast<int>(a_time * 60.0f);
	if (durationFrames < 1) durationFrames = 1;

	// =========================================================================
	// APLICAÇÃO IMEDIATA (FRAME 0) - ESPELHANDO O COMPORTAMENTO DO IMPULSO
	// =========================================================================
	float initialProgress = 1.0f / static_cast<float>(durationFrames);
	float initialEased = GetEasingProgress(initialProgress);
	float deltaYawFrameZero = yawRadians * initialEased;

	a_actor->GetActorRuntimeData().boolBits.reset(RE::Actor::BOOL_BITS::kHeadingFixed);
	a_actor->SetHeading(a_actor->data.angle.z + deltaYawFrameZero);
	// =========================================================================

	{
		std::lock_guard<std::mutex> lock(g_RotationMutex);
		// Armazena com elapsedFrames = 1 e o progresso inicial salvo
		g_ActiveRotations[actorID].push_back({ yawRadians, 1, durationFrames, initialEased });

		if (g_ActiveRotationLoops.contains(actorID)) {
			return;
		}
		g_ActiveRotationLoops.insert(actorID);
	}

	// Insere o delay de 16ms antes do Frame 1 para garantir a cadência do framerate
	Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(5), [actorHandle, actorID]() {
		SKSE::GetTaskInterface()->AddTask([actorHandle, actorID]() {
			ProcessActorRotations(actorHandle, actorID);
			});
		});
}

// =========================================================================
// HANDLERS DOS EVENTOS DE ANIMAÇÃO (PAYLOAD PARSING)
// =========================================================================

void ApplyImpulseFFC::Process(RE::TESObjectREFR* a_holder, const std::string_view& a_payload, RE::BShkbAnimationGraph* a_animationGraph)
{
	auto actor = a_holder ? a_holder->As<RE::Actor>() : nullptr;
	if (!actor) return;
	logger::debug("string_view recebida (Force): {}", a_payload);

	auto args = SplitString(a_payload, '|');
	if (args.empty()) {
		SKSE::log::error("ApplyImpulseFFC: O payload está totalmente vazio.");
		return;
	}

	try {
		// Valores padrão caso não sejam fornecidos no payload
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float time = 0.0f;
		bool inflictDamage = true;

		// Preenchimento incremental e seguro baseando-se no tamanho do vetor
		if (args.size() >= 1) x = std::stof(args[0]);
		if (args.size() >= 2) y = std::stof(args[1]);
		if (args.size() >= 3) z = std::stof(args[2]);
		if (args.size() >= 4) time = std::stof(args[3]);
		if (args.size() >= 5) {
			std::string damageArg = args[4];
			std::transform(damageArg.begin(), damageArg.end(), damageArg.begin(), ::tolower);
			if (damageArg == "false" || damageArg == "0") {
				inflictDamage = false;
			}
		}

		ApplyCustomVelocityImpulse(actor, x, y, z, time, inflictDamage);
		SKSE::log::debug("ApplyForceFFC: Impulso processado [X:{}, Y:{}, Z:{}, Tempo:{}s, Dano:{}] no ator {:X}", x, y, z, time, inflictDamage, actor->GetFormID());
	}
	catch (...) {
		SKSE::log::error("ApplyForceFFC: Erro crítico de conversão. Certifique-se de usar números válidos no payload: {}", a_payload);
	}
}

void ApplyRotationFFCHandler::Process(RE::TESObjectREFR* a_holder, const std::string_view& a_payload, RE::BShkbAnimationGraph* a_animationGraph)
{
	auto actor = a_holder ? a_holder->As<RE::Actor>() : nullptr;
	if (!actor) return;
	logger::debug("string_view recebida (Rotation): {}", a_payload);
	auto args = SplitString(a_payload, '|');
	if (args.empty()) {
		SKSE::log::error("ApplyRotationFFC: O payload está vazio.");
		return;
	}

	try {
		float yawDegrees = std::stof(args[0]);

		float time = 0.0f;
		if (args.size() >= 2) {
			time = std::stof(args[1]);
		}

		ApplyCustomRotation(actor, yawDegrees, time);
		SKSE::log::debug("ApplyRotationFFC: Rotação processada [Graus:{}, Tempo:{}s] no ator {:X}", yawDegrees, time, actor->GetFormID());
	}
	catch (...) {
		SKSE::log::error("ApplyRotationFFC: Erro crítico ao converter valor de rotação: {}", a_payload);
	}
}
