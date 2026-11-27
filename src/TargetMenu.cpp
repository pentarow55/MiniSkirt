#include "PCH.h"

#include "TargetMenu.h"

#include "Overrides.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace Probe
{
	int  TargetHotkey();
	bool GlobalEnabled();
	bool GlobalExcludeElderly();
	float GlobalLengthPct();
	bool IsActorElderly(RE::Actor* a_actor);
	void ReprocessActorNow(RE::FormID a_id);
	void ReprocessByArmorNow(RE::FormID a_armorId);
}

namespace TargetMenu
{
	namespace
	{
		constexpr const char* kPluginName = "MiniSkirt.esp";
		constexpr RE::FormID  kAdjustPowerLocalId = 0x801;

		class Callback : public RE::IMessageBoxCallback
		{
		public:
			explicit Callback(std::function<void(std::int32_t)> a_fn) : _fn(std::move(a_fn)) {}

			void Run(std::uint8_t a_button) override
			{
				if (_fn) {
					_fn(static_cast<std::int32_t>(a_button));
				}
			}

		private:
			std::function<void(std::int32_t)> _fn;
		};

		void ShowMenu(const std::string& a_body, const std::vector<std::string>& a_buttons, std::function<void(std::int32_t)> a_onSelect)
		{
			auto tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				return;
			}
			tasks->AddUITask([a_body, a_buttons, a_onSelect = std::move(a_onSelect)]() mutable {
				auto factory = RE::MessageDataFactoryManager::GetSingleton();
				auto uiStr = RE::InterfaceStrings::GetSingleton();
				if (!factory || !uiStr) {
					return;
				}
				auto creator = factory->GetCreator<RE::MessageBoxData>(uiStr->messageBoxData);
				if (!creator) {
					return;
				}
				auto msg = creator->Create();
				if (!msg) {
					return;
				}
				msg->bodyText = a_body;
				for (const auto& b : a_buttons) {
					msg->buttonText.push_back(RE::BSString(b));
				}
				msg->warningType = 25;
				msg->menuDepth = 10;
				msg->verticalButtons = true;
				msg->isCancellable = true;
				msg->cancelButtonIndex = static_cast<std::int32_t>(a_buttons.size()) - 1;
				msg->callback = RE::make_smart<Callback>(std::move(a_onSelect));
				RE::MessageBoxMenu::QueueMessage(msg);
			});
		}

		bool IsValidFemaleTarget(RE::Actor* a_actor)
		{
			if (!a_actor || a_actor->IsPlayerRef() || a_actor->IsChild() || a_actor->IsDead()) {
				return false;
			}
			auto base = a_actor->GetActorBase();
			return base && base->GetSex() == RE::SEX::kFemale;
		}

		RE::Actor* FindLookedAtFemale()
		{
			// 1. クロスヘア／VR ポインターの対象を最優先
			if (auto pick = RE::CrosshairPickData::GetSingleton()) {
				auto handle = pick->GetActiveTarget();
				if (auto ref = handle.get()) {
					if (auto actor = ref->As<RE::Actor>(); IsValidFemaleTarget(actor)) {
						return actor;
					}
				}
			}

			// 2. 見つからなければプレイヤーの正面（±35°・距離 450u 以内）の最寄り女性 NPC
			auto player = RE::PlayerCharacter::GetSingleton();
			auto pl = RE::ProcessLists::GetSingleton();
			if (!player || !pl) {
				return nullptr;
			}
			const auto  pPos = player->GetPosition();
			const float pYaw = player->GetAngleZ();
			const float fwdX = std::sin(pYaw);
			const float fwdY = std::cos(pYaw);

			RE::Actor* best = nullptr;
			float      bestScore = 1e9f;
			for (auto& h : pl->highActorHandles) {
				auto a = h.get().get();
				if (!IsValidFemaleTarget(a) || !a->Get3D()) {
					continue;
				}
				const auto  pos = a->GetPosition();
				const float dx = pos.x - pPos.x;
				const float dy = pos.y - pPos.y;
				const float dz = pos.z - pPos.z;
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (dist < 10.0f || dist > 450.0f) {
					continue;
				}
				const float horiz = std::sqrt(dx * dx + dy * dy);
				if (horiz < 1.0f) {
					continue;
				}
				const float dot = (dx * fwdX + dy * fwdY) / horiz;
				if (dot < 0.80f) {  // 約 ±37° 以内
					continue;
				}
				const float score = dist * (1.5f - dot);
				if (score < bestScore) {
					bestScore = score;
					best = a;
				}
			}
			return best;
		}

		const char* SourceLabel(Overrides::Source a_src)
		{
			switch (a_src) {
			case Overrides::Source::kNpc:
				return "NPC";
			case Overrides::Source::kOutfit:
				return "Outfit";
			default:
				return "Global";
			}
		}

		void OpenLengthSubMenu(RE::FormID a_actorId, RE::FormID a_armorId, bool a_forNpc)
		{
			auto actor = RE::TESForm::LookupByID<RE::Actor>(a_actorId);
			auto armor = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
			if (!actor) {
				return;
			}
			if (!a_forNpc && !armor) {
				RE::SendHUDMessage::ShowHUDMessage("MiniSkirt: Target is not wearing a body outfit.");
				return;
			}

			const std::string targetLabel = a_forNpc ?
			                                    std::format("NPC: {}", actor->GetDisplayFullName()) :
			                                    std::format("Outfit: {}", armor->GetName() && *armor->GetName() ? armor->GetName() : "(Unnamed)");
			const std::string body = std::format("MiniSkirt - Select Skirt Length\n{}", targetLabel);

			static constexpr float kPresets[] = { 15.0f, 25.0f, 35.0f, 50.0f, 70.0f, 100.0f };
			std::vector<std::string> buttons = {
				"15% (Micro Mini)",
				"25% (Mini)",
				"35% (Short)",
				"50% (Knee)",
				"70% (Long)",
				"100% (Original Length)",
				"Reset Length to Default",
				"Back",
			};

			ShowMenu(body, buttons, [a_actorId, a_armorId, a_forNpc](std::int32_t a_idx) {
				auto act = RE::TESForm::LookupByID<RE::Actor>(a_actorId);
				auto arm = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
				if (!act) {
					return;
				}
				if (a_idx >= 0 && a_idx < 6) {
					const float pct = kPresets[a_idx];
					if (a_forNpc) {
						Overrides::SetNpcLength(act, pct);
						Probe::ReprocessActorNow(a_actorId);
						RE::SendHUDMessage::ShowHUDMessage(std::format("MiniSkirt: {} -> {:.0f}%", act->GetDisplayFullName(), pct).c_str());
					} else if (arm) {
						Overrides::SetOutfitLength(arm, pct);
						Probe::ReprocessByArmorNow(a_armorId);
						RE::SendHUDMessage::ShowHUDMessage(std::format("MiniSkirt: Outfit {} -> {:.0f}%", arm->GetName(), pct).c_str());
					}
				} else if (a_idx == 6) {
					if (a_forNpc) {
						Overrides::SetNpcLength(act, std::nullopt);
						Probe::ReprocessActorNow(a_actorId);
						RE::SendHUDMessage::ShowHUDMessage(std::format("MiniSkirt: {} length reset", act->GetDisplayFullName()).c_str());
					} else if (arm) {
						Overrides::SetOutfitLength(arm, std::nullopt);
						Probe::ReprocessByArmorNow(a_armorId);
						RE::SendHUDMessage::ShowHUDMessage(std::format("MiniSkirt: Outfit {} length reset", arm->GetName()).c_str());
					}
				} else if (a_idx == 7) {
					OpenForActor(act);
				}
			});
		}

		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const int hotkey = Probe::TargetHotkey();
				if (hotkey <= 0) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto ui = RE::UI::GetSingleton();
				if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME) || ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (auto* ev = *a_event; ev; ev = ev->next) {
					if (ev->eventType != RE::INPUT_EVENT_TYPE::kButton) {
						continue;
					}
					auto* btn = ev->AsButtonEvent();
					if (!btn || !btn->IsDown()) {
						continue;
					}
					if (btn->GetDevice() == RE::INPUT_DEVICE::kKeyboard && static_cast<int>(btn->GetIDCode()) == hotkey) {
						OpenForLookedAt();
						break;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class SpellCastSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (!a_event || !a_event->object || !a_event->object->IsPlayerRef()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto dh = RE::TESDataHandler::GetSingleton();
				auto power = dh ? dh->LookupForm<RE::SpellItem>(kAdjustPowerLocalId, kPluginName) : nullptr;
				if (power && a_event->spell == power->GetFormID()) {
					OpenForLookedAt();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		InputSink     g_inputSink;
		SpellCastSink g_spellSink;
		bool          g_installed = false;
	}

	void Install()
	{
		if (g_installed) {
			return;
		}
		g_installed = true;

		if (auto idm = RE::BSInputDeviceManager::GetSingleton()) {
			idm->AddEventSink(&g_inputSink);
			logger::info("TargetMenu: ホットキー入力を購読しました");
		}
		if (auto src = RE::ScriptEventSourceHolder::GetSingleton()) {
			src->AddEventSink<RE::TESSpellCastEvent>(&g_spellSink);
			logger::info("TargetMenu: 調整パワー（VR/パッド向け）の詠唱イベントを購読しました");
		}
	}

	void OpenForLookedAt()
	{
		auto actor = FindLookedAtFemale();
		if (!actor) {
			RE::SendHUDMessage::ShowHUDMessage("MiniSkirt: No female NPC in front of you.");
			return;
		}
		OpenForActor(actor);
	}

	void OpenForActor(RE::Actor* a_actor)
	{
		if (!IsValidFemaleTarget(a_actor)) {
			RE::SendHUDMessage::ShowHUDMessage("MiniSkirt: Target is not a valid female NPC.");
			return;
		}
		const auto actorId = a_actor->GetFormID();
		auto       armor = a_actor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
		const auto armorId = armor ? armor->GetFormID() : 0u;

		const bool elderlyExcl = Probe::GlobalExcludeElderly() && Probe::IsActorElderly(a_actor);
		const auto res = Overrides::Resolve(a_actor, armor, Probe::GlobalEnabled(), elderlyExcl, Probe::GlobalLengthPct());

		const std::string npcName = a_actor->GetDisplayFullName();
		const std::string armName = armor && armor->GetName() && *armor->GetName() ? armor->GetName() : "(None)";
		const std::string status = res.enabled ?
		                               std::format("Enabled ({:.0f}% [{}])", res.lengthPct, SourceLabel(res.lengthSource)) :
		                               std::format("Excluded ([{}])", SourceLabel(res.enabledSource));

		const std::string body = std::format("MiniSkirt: {}\nOutfit: {}\nCurrent: {}", npcName, armName, status);

		const auto npcRule = Overrides::GetNpcRule(a_actor);
		const auto armRule = Overrides::GetOutfitRule(armor);
		const bool npcExcluded = npcRule && npcRule->enabled.has_value() && !*npcRule->enabled;
		const bool armExcluded = armRule && armRule->enabled.has_value() && !*armRule->enabled;

		std::vector<std::string> buttons = {
			"NPC: Change Length...",
			npcExcluded ? "NPC: Enable (Clear Exclude)" : "NPC: Exclude (Keep Original)",
			"Outfit: Change Length...",
			armExcluded ? "Outfit: Enable (Clear Exclude)" : "Outfit: Exclude (Keep Original)",
			"Clear NPC & Outfit Overrides",
			"Close",
		};

		ShowMenu(body, buttons, [actorId, armorId, npcExcluded, armExcluded](std::int32_t a_idx) {
			auto act = RE::TESForm::LookupByID<RE::Actor>(actorId);
			auto arm = armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(armorId) : nullptr;
			if (!act) {
				return;
			}
			switch (a_idx) {
			case 0:
				OpenLengthSubMenu(actorId, armorId, true);
				break;
			case 1:
				Overrides::SetNpcEnabled(act, npcExcluded ? std::optional<bool>(true) : std::optional<bool>(false));
				Probe::ReprocessActorNow(actorId);
				RE::SendHUDMessage::ShowHUDMessage(
					std::format("MiniSkirt: {} -> {}", act->GetDisplayFullName(), npcExcluded ? "Enabled" : "Excluded").c_str());
				break;
			case 2:
				OpenLengthSubMenu(actorId, armorId, false);
				break;
			case 3:
				if (arm) {
					Overrides::SetOutfitEnabled(arm, armExcluded ? std::optional<bool>(true) : std::optional<bool>(false));
					Probe::ReprocessByArmorNow(armorId);
					RE::SendHUDMessage::ShowHUDMessage(
						std::format("MiniSkirt: Outfit {} -> {}", arm->GetName(), armExcluded ? "Enabled" : "Excluded").c_str());
				}
				break;
			case 4:
				Overrides::ClearNpc(act);
				if (arm) {
					Overrides::ClearOutfit(arm);
					Probe::ReprocessByArmorNow(armorId);
				} else {
					Probe::ReprocessActorNow(actorId);
				}
				RE::SendHUDMessage::ShowHUDMessage(std::format("MiniSkirt: Cleared overrides for {}", act->GetDisplayFullName()).c_str());
				break;
			default:
				break;
			}
		});
	}
}
