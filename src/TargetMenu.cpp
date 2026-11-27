#include "PCH.h"

#include "TargetMenu.h"

#include "Overrides.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Probe
{
	int   TargetHotkey();
	bool  GlobalEnabled();
	bool  GlobalExcludeElderly();
	float GlobalLengthPct();
	bool  IsActorElderly(RE::Actor* a_actor);
	void  ReprocessActorNow(RE::FormID a_id);
	void  ReprocessByArmorNow(RE::FormID a_armorId);
}

namespace TargetMenu
{
	namespace
	{
		constexpr const char* kPluginName = "MiniSkirt.esp";
		constexpr RE::FormID  kAdjustPowerLocalId = 0x801;

		// ---- 翻訳ファイル読み込み（ゲームの sLanguage に連動） ----
		std::unordered_map<std::string, std::string> g_i18n;
		bool                                         g_i18nLoaded = false;

		struct StreamReader : RE::BSResourceNiBinaryStream
		{
			using RE::BSResourceNiBinaryStream::BSResourceNiBinaryStream;
			using RE::NiBinaryStream::binary_read;
		};

		std::string Upper(std::string a_s)
		{
			for (auto& c : a_s) {
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			}
			return a_s;
		}

		std::string WideToUtf8(const wchar_t* a_w, int a_len)
		{
			if (!a_w || a_len <= 0) {
				return {};
			}
			const int need = WideCharToMultiByte(CP_UTF8, 0, a_w, a_len, nullptr, 0, nullptr, nullptr);
			if (need <= 0) {
				return {};
			}
			std::string out(static_cast<std::size_t>(need), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_w, a_len, out.data(), need, nullptr, nullptr);
			return out;
		}

		bool TryLoadTranslationFile(const std::string& a_lang)
		{
			const std::string path = std::format("Interface\\Translations\\MiniSkirt_{}.txt", a_lang);
			StreamReader      in(path);
			if (!in.good()) {
				return false;
			}
			std::vector<std::uint8_t> bytes;
			std::uint8_t              buf[4096];
			for (;;) {
				std::uint32_t sz = 1;
				const auto    n = in.binary_read(buf, sizeof(buf), &sz, 1);
				if (n == 0) {
					break;
				}
				bytes.insert(bytes.end(), buf, buf + n);
			}
			if (bytes.size() < 4) {
				return false;
			}
			std::size_t off = 0;
			if (bytes[0] == 0xFF && bytes[1] == 0xFE) {
				off = 2;
			}
			const auto     wcount = (bytes.size() - off) / 2;
			std::wstring   wtext(wcount, L'\0');
			std::memcpy(wtext.data(), bytes.data() + off, wcount * 2);
			const auto utf8 = WideToUtf8(wtext.data(), static_cast<int>(wtext.size()));

			g_i18n.clear();
			std::size_t pos = 0;
			while (pos < utf8.size()) {
				auto nl = utf8.find('\n', pos);
				if (nl == std::string::npos) {
					nl = utf8.size();
				}
				std::string line = utf8.substr(pos, nl - pos);
				pos = nl + 1;
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				const auto tab = line.find('\t');
				if (tab == std::string::npos || tab == 0) {
					continue;
				}
				g_i18n[line.substr(0, tab)] = line.substr(tab + 1);
			}
			logger::info("TargetMenu: 翻訳ファイルを読み込みました: {} ({} 件)", path, g_i18n.size());
			return !g_i18n.empty();
		}

		void EnsureTranslations()
		{
			if (g_i18nLoaded) {
				return;
			}
			g_i18nLoaded = true;
			std::string lang = "ENGLISH";
			if (auto ini = RE::INISettingCollection::GetSingleton()) {
				if (auto s = ini->GetSetting("sLanguage:General"); s && s->GetType() == RE::Setting::Type::kString) {
					if (const char* v = s->GetString(); v && *v) {
						lang = Upper(v);
					}
				}
			}
			if (!TryLoadTranslationFile(lang) && lang != "ENGLISH") {
				TryLoadTranslationFile("ENGLISH");
			}
		}

		std::string Tr(const char* a_key, const char* a_fallback)
		{
			EnsureTranslations();
			if (auto it = g_i18n.find(a_key); it != g_i18n.end() && !it->second.empty()) {
				return it->second;
			}
			return a_fallback;
		}

		// ---- ネイティブ選択肢メッセージボックス ----
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
			if (auto pick = RE::CrosshairPickData::GetSingleton()) {
				auto handle = pick->GetActiveTarget();
				if (auto ref = handle.get()) {
					if (auto actor = ref->As<RE::Actor>(); IsValidFemaleTarget(actor)) {
						return actor;
					}
				}
			}

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
				if (dot < 0.80f) {
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

		std::string SourceLabel(Overrides::Source a_src)
		{
			switch (a_src) {
			case Overrides::Source::kNpc:
				return Tr("$MSK_MenuSrcNpc", "NPC");
			case Overrides::Source::kOutfit:
				return Tr("$MSK_MenuSrcOutfit", "Outfit");
			default:
				return Tr("$MSK_MenuSrcGlobal", "Global");
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
				RE::SendHUDMessage::ShowHUDMessage(Tr("$MSK_MenuNoOutfit", "MiniSkirt: Target is not wearing a body outfit.").c_str());
				return;
			}

			const bool elderlyExcl = Probe::GlobalExcludeElderly() && Probe::IsActorElderly(actor);
			const auto res = Overrides::Resolve(actor, armor, Probe::GlobalEnabled(), elderlyExcl, Probe::GlobalLengthPct());
			const int  curPct = std::clamp(static_cast<int>(std::lround(res.lengthPct)), 10, 100);

			const std::string targetLabel = a_forNpc ?
			                                    std::format("{}: {}", Tr("$MSK_MenuTargetNpc", "NPC"), actor->GetDisplayFullName()) :
			                                    std::format("{}: {}", Tr("$MSK_MenuTargetOutfit", "Outfit"),
			                                        armor->GetName() && *armor->GetName() ? armor->GetName() : "(Unnamed)");
			const std::string body = std::format("{}\n{}\n{}: {}% [{}]", Tr("$MSK_MenuLenTitle", "Adjust Skirt Length (10-100%)"), targetLabel,
				Tr("$MSK_MenuCurrentLabel", "Current"), curPct, SourceLabel(res.lengthSource));

			static constexpr int kDeltas[] = { -10, -5, -1, +1, +5, +10 };
			std::vector<std::string> buttons = {
				"-10%",
				"-5%",
				"-1%",
				"+1%",
				"+5%",
				"+10%",
				Tr("$MSK_MenuBtnLen100", "100% (Original Length)"),
				Tr("$MSK_MenuBtnResetLen", "Reset Length to Default"),
				Tr("$MSK_MenuBtnDone", "Done"),
			};

			ShowMenu(body, buttons, [a_actorId, a_armorId, a_forNpc, curPct](std::int32_t a_idx) {
				auto act = RE::TESForm::LookupByID<RE::Actor>(a_actorId);
				auto arm = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
				if (!act) {
					return;
				}
				if (a_idx >= 0 && a_idx < 6) {
					const float nextPct = static_cast<float>(std::clamp(curPct + kDeltas[a_idx], 10, 100));
					if (a_forNpc) {
						Overrides::SetNpcLength(act, nextPct);
						Probe::ReprocessActorNow(a_actorId);
					} else if (arm) {
						Overrides::SetOutfitLength(arm, nextPct);
						Probe::ReprocessByArmorNow(a_armorId);
					}
					// 即時反映させつつ同じ調整画面を開き直し、1% 刻みで追い込めるようにする
					OpenLengthSubMenu(a_actorId, a_armorId, a_forNpc);
				} else if (a_idx == 6) {
					if (a_forNpc) {
						Overrides::SetNpcLength(act, 100.0f);
						Probe::ReprocessActorNow(a_actorId);
					} else if (arm) {
						Overrides::SetOutfitLength(arm, 100.0f);
						Probe::ReprocessByArmorNow(a_armorId);
					}
					OpenLengthSubMenu(a_actorId, a_armorId, a_forNpc);
				} else if (a_idx == 7) {
					if (a_forNpc) {
						Overrides::SetNpcLength(act, std::nullopt);
						Probe::ReprocessActorNow(a_actorId);
					} else if (arm) {
						Overrides::SetOutfitLength(arm, std::nullopt);
						Probe::ReprocessByArmorNow(a_armorId);
					}
					OpenLengthSubMenu(a_actorId, a_armorId, a_forNpc);
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

		EnsureTranslations();

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
			RE::SendHUDMessage::ShowHUDMessage(Tr("$MSK_MenuNoTarget", "MiniSkirt: No female NPC in front of you.").c_str());
			return;
		}
		OpenForActor(actor);
	}

	void OpenForActor(RE::Actor* a_actor)
	{
		if (!IsValidFemaleTarget(a_actor)) {
			RE::SendHUDMessage::ShowHUDMessage(Tr("$MSK_MenuNoTarget", "MiniSkirt: No female NPC in front of you.").c_str());
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
		                               std::format("{} ({:.0f}% [{}])", Tr("$MSK_MenuEnabled", "Enabled"), res.lengthPct, SourceLabel(res.lengthSource)) :
		                               std::format("{} ([{}])", Tr("$MSK_MenuExcluded", "Excluded"), SourceLabel(res.enabledSource));

		const std::string body = std::format("MiniSkirt: {}\n{}: {}\n{}: {}", npcName, Tr("$MSK_MenuOutfitLabel", "Outfit"), armName,
			Tr("$MSK_MenuCurrentLabel", "Current"), status);

		const auto npcRule = Overrides::GetNpcRule(a_actor);
		const auto armRule = Overrides::GetOutfitRule(armor);
		const bool npcExcluded = npcRule && npcRule->enabled.has_value() && !*npcRule->enabled;
		const bool armExcluded = armRule && armRule->enabled.has_value() && !*armRule->enabled;

		std::vector<std::string> buttons = {
			Tr("$MSK_MenuBtnNpcLen", "NPC: Adjust Length..."),
			npcExcluded ? Tr("$MSK_MenuBtnNpcEn", "NPC: Enable (Clear Exclude)") : Tr("$MSK_MenuBtnNpcExcl", "NPC: Exclude (Keep Original)"),
			Tr("$MSK_MenuBtnOutLen", "Outfit: Adjust Length..."),
			armExcluded ? Tr("$MSK_MenuBtnOutEn", "Outfit: Enable (Clear Exclude)") : Tr("$MSK_MenuBtnOutExcl", "Outfit: Exclude (Keep Original)"),
			Tr("$MSK_MenuBtnClearAll", "Clear NPC & Outfit Overrides"),
			Tr("$MSK_MenuBtnClose", "Close"),
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
				RE::SendHUDMessage::ShowHUDMessage(std::format("MiniSkirt: {} -> {}", act->GetDisplayFullName(),
					npcExcluded ? Tr("$MSK_MenuEnabled", "Enabled") : Tr("$MSK_MenuExcluded", "Excluded"))
					                                   .c_str());
				break;
			case 2:
				OpenLengthSubMenu(actorId, armorId, false);
				break;
			case 3:
				if (arm) {
					Overrides::SetOutfitEnabled(arm, armExcluded ? std::optional<bool>(true) : std::optional<bool>(false));
					Probe::ReprocessByArmorNow(armorId);
					RE::SendHUDMessage::ShowHUDMessage(std::format("MiniSkirt: {} -> {}", arm->GetName(),
						armExcluded ? Tr("$MSK_MenuEnabled", "Enabled") : Tr("$MSK_MenuExcluded", "Excluded"))
						                                   .c_str());
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
				RE::SendHUDMessage::ShowHUDMessage(Tr("$MSK_MenuCleared", "MiniSkirt: Cleared overrides").c_str());
				break;
			default:
				break;
			}
		});
	}
}
