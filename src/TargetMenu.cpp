#include "PCH.h"

#include "TargetMenu.h"

#include "Overrides.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Probe
{
	int                       TargetHotkey();
	Overrides::GlobalDefaults GetGlobalDefaults(RE::Actor* a_actor);
	void                      ReprocessActorNow(RE::FormID a_id);
	void                      ReprocessByArmorNow(RE::FormID a_armorId);
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

		bool LoadTranslationFile(const std::string& a_relPath)
		{
			StreamReader st(a_relPath);
			if (!st.good()) {
				return false;
			}
			std::vector<std::uint8_t> raw;
			std::uint8_t              buf[4096];
			std::uint32_t             comp = 1;
			while (true) {
				const auto n = st.binary_read(buf, sizeof(buf), &comp, 1);
				if (n == 0) {
					break;
				}
				raw.insert(raw.end(), buf, buf + n);
			}
			if (raw.size() < 4) {
				return false;
			}
			std::string text;
			if (raw[0] == 0xFF && raw[1] == 0xFE) {
				const auto* w = reinterpret_cast<const wchar_t*>(raw.data() + 2);
				const int   wlen = static_cast<int>((raw.size() - 2) / 2);
				text = WideToUtf8(w, wlen);
			} else {
				const std::size_t off = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
				text.assign(reinterpret_cast<const char*>(raw.data() + off), raw.size() - off);
			}
			std::size_t pos = 0;
			while (pos < text.size()) {
				std::size_t nl = text.find('\n', pos);
				if (nl == std::string::npos) {
					nl = text.size();
				}
				std::string line = text.substr(pos, nl - pos);
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (const auto tab = line.find('\t'); tab != std::string::npos && tab > 0) {
					g_i18n[line.substr(0, tab)] = line.substr(tab + 1);
				}
				pos = nl + 1;
			}
			return !g_i18n.empty();
		}

		std::string DetectGameLanguage()
		{
			if (auto ini = RE::INISettingCollection::GetSingleton()) {
				if (auto s = ini->GetSetting("sLanguage:General")) {
					if (const char* v = s->GetString(); v && *v) {
						return Upper(v);
					}
				}
			}
			return "ENGLISH";
		}

		void EnsureTranslations()
		{
			if (g_i18nLoaded) {
				return;
			}
			g_i18nLoaded = true;
			const auto lang = DetectGameLanguage();
			const auto path = std::format("Interface\\Translations\\MiniSkirt_{}.txt", lang);
			if (LoadTranslationFile(path)) {
				logger::info("TargetMenu: 翻訳ファイルを読み込みました: {} ({} 件)", path, g_i18n.size());
				return;
			}
			if (lang != "ENGLISH" && LoadTranslationFile("Interface\\Translations\\MiniSkirt_ENGLISH.txt")) {
				logger::info("TargetMenu: {} の翻訳が無いため ENGLISH を読み込みました ({} 件)", lang, g_i18n.size());
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

		// ---- ネイティブ選択肢メッセージボックス（MiniSkirt のメニューだけ左寄せ表示） ----
		constexpr double  kMenuShiftLeft = 360.0;
		std::atomic<bool> g_ourMenuActive{ false };
		double            g_origMenuX = -99999.0;

		void ApplyMenuOffset(bool a_left)
		{
			auto ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			auto menu = ui->GetMenu<RE::MessageBoxMenu>();
			if (!menu || !menu->uiMovie) {
				return;
			}
			RE::GFxValue mc;
			if (!menu->uiMovie->GetVariable(&mc, "_root.MessageMenu") || !mc.IsDisplayObject()) {
				// 互換フォールバック（一部 UI Mod で名前が違う場合）
				if (!menu->uiMovie->GetVariable(&mc, "_root") || !mc.IsDisplayObject()) {
					return;
				}
			}
			RE::GFxValue::DisplayInfo info;
			if (!mc.GetDisplayInfo(&info)) {
				return;
			}
			const double curX = info.GetX();
			if (g_origMenuX < -90000.0) {
				g_origMenuX = curX;
			}
			const double targetX = a_left ? (g_origMenuX - kMenuShiftLeft) : g_origMenuX;
			if (std::abs(curX - targetX) > 0.5) {
				info.SetX(targetX);
				mc.SetDisplayInfo(info);
			}
		}

		void ScheduleApplyOffset(bool a_left)
		{
			auto apply = [a_left] {
				if (auto tasks = SKSE::GetTaskInterface()) {
					tasks->AddUITask([a_left] { ApplyMenuOffset(a_left); });
				}
			};
			apply();
			// SWF の setup() 完了後にもう一度確実に適用する
			std::thread([apply] {
				std::this_thread::sleep_for(std::chrono::milliseconds(60));
				apply();
			}).detach();
		}

		class MenuOpenCloseSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || a_event->menuName != RE::MessageBoxMenu::MENU_NAME) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->opening) {
					const bool ours = g_ourMenuActive.load();
					ScheduleApplyOffset(ours);
				} else {
					g_ourMenuActive.store(false);
					ApplyMenuOffset(false);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

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
				g_ourMenuActive.store(true);
				RE::MessageBoxMenu::QueueMessage(msg);
				ScheduleApplyOffset(true);
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
			RE::NiPoint3 origin = player->GetPosition();
			float        yaw = player->GetAngleZ();
			if (!REL::Module::IsVR()) {
				if (auto cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFreeCameraMode()) {
					if (auto st = static_cast<RE::FreeCameraState*>(cam->GetRuntimeData().cameraStates[RE::CameraState::kFree].get())) {
						origin = st->translation;
						yaw = st->rotation.y;
					}
				}
			}
			const float fwdX = std::sin(yaw);
			const float fwdY = std::cos(yaw);

			RE::Actor* best = nullptr;
			float      bestScore = 1e9f;
			for (auto& h : pl->highActorHandles) {
				auto a = h.get().get();
				if (!IsValidFemaleTarget(a) || !a->Get3D()) {
					continue;
				}
				const auto  pos = a->GetPosition();
				const float dx = pos.x - origin.x;
				const float dy = pos.y - origin.y;
				const float dz = pos.z - origin.z;
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

		std::string TargetHeader(RE::Actor* a_actor, RE::TESObjectARMO* a_armor, bool a_forNpc)
		{
			if (a_forNpc) {
				return std::format("{}: {}", Tr("$MSK_MenuTargetNpc", "NPC"), a_actor->GetDisplayFullName());
			}
			return std::format("{}: {}", Tr("$MSK_MenuTargetOutfit", "Outfit"),
				a_armor && a_armor->GetName() && *a_armor->GetName() ? a_armor->GetName() : "(Unnamed)");
		}

		void ApplyReprocess(RE::FormID a_actorId, RE::FormID a_armorId, bool a_forNpc)
		{
			if (a_forNpc) {
				Probe::ReprocessActorNow(a_actorId);
			} else if (a_armorId) {
				Probe::ReprocessByArmorNow(a_armorId);
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

			const auto res = Overrides::Resolve(actor, armor, Probe::GetGlobalDefaults(actor));
			const int  curPct = std::clamp(static_cast<int>(std::lround(res.lengthPct)), 10, 100);

			const std::string body = std::format("{}\n{}\n{}: {}% [{}]", Tr("$MSK_MenuLenTitle", "Adjust Skirt Length (10-100%)"),
				TargetHeader(actor, armor, a_forNpc), Tr("$MSK_MenuCurrentLabel", "Current"), curPct, SourceLabel(res.lengthSource));

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
					} else if (arm) {
						Overrides::SetOutfitLength(arm, nextPct);
					}
					ApplyReprocess(a_actorId, a_armorId, a_forNpc);
					OpenLengthSubMenu(a_actorId, a_armorId, a_forNpc);
				} else if (a_idx == 6) {
					if (a_forNpc) {
						Overrides::SetNpcLength(act, 100.0f);
					} else if (arm) {
						Overrides::SetOutfitLength(arm, 100.0f);
					}
					ApplyReprocess(a_actorId, a_armorId, a_forNpc);
					OpenLengthSubMenu(a_actorId, a_armorId, a_forNpc);
				} else if (a_idx == 7) {
					if (a_forNpc) {
						Overrides::SetNpcLength(act, std::nullopt);
					} else if (arm) {
						Overrides::SetOutfitLength(arm, std::nullopt);
					}
					ApplyReprocess(a_actorId, a_armorId, a_forNpc);
					OpenLengthSubMenu(a_actorId, a_armorId, a_forNpc);
				}
			});
		}

		enum class FloatField
		{
			kHipEase,
			kFlareAngle,
			kSkirtGap,
			kSkirtGapNoBone,
		};

		void OpenDetailSubMenu(RE::FormID a_actorId, RE::FormID a_armorId, bool a_forNpc);

		void OpenFloatSubMenu(RE::FormID a_actorId, RE::FormID a_armorId, bool a_forNpc, FloatField a_field)
		{
			auto actor = RE::TESForm::LookupByID<RE::Actor>(a_actorId);
			auto armor = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
			if (!actor || (!a_forNpc && !armor)) {
				return;
			}
			const auto res = Overrides::Resolve(actor, armor, Probe::GetGlobalDefaults(actor));

			std::string       title;
			float             curVal = 0.0f;
			float             minV = 0.0f, maxV = 8.0f, smallStep = 0.5f, bigStep = 1.0f;
			bool              isIntDeg = false;
			Overrides::Source src = Overrides::Source::kGlobal;

			switch (a_field) {
			case FloatField::kHipEase:
				title = Tr("$MSK_HipEaseText", "Hip Ease");
				curVal = res.hipEase;
				src = res.hipEaseSource;
				minV = 0.0f;
				maxV = 8.0f;
				smallStep = 0.5f;
				bigStep = 1.0f;
				break;
			case FloatField::kFlareAngle:
				title = Tr("$MSK_FlareText", "Flare Angle");
				curVal = res.flareAngle;
				src = res.flareAngleSource;
				minV = 0.0f;
				maxV = 30.0f;
				smallStep = 1.0f;
				bigStep = 5.0f;
				isIntDeg = true;
				break;
			case FloatField::kSkirtGap:
				title = Tr("$MSK_GapText", "Skirt / Leg Gap");
				curVal = res.skirtGap;
				src = res.skirtGapSource;
				minV = 0.0f;
				maxV = 8.0f;
				smallStep = 0.5f;
				bigStep = 1.0f;
				break;
			case FloatField::kSkirtGapNoBone:
				title = Tr("$MSK_GapStiffText", "Gap (Stiff Skirts)");
				curVal = res.skirtGapNoBone;
				src = res.skirtGapNoBoneSource;
				minV = 0.0f;
				maxV = 10.0f;
				smallStep = 0.5f;
				bigStep = 1.0f;
				break;
			}

			const std::string valStr = isIntDeg ? std::format("{:.0f}°", curVal) : std::format("{:.1f}", curVal);
			const std::string body = std::format("{} ({:.0f}-{:.0f})\n{}\n{}: {} [{}]", title, minV, maxV,
				TargetHeader(actor, armor, a_forNpc), Tr("$MSK_MenuCurrentLabel", "Current"), valStr, SourceLabel(src));

			const float              deltas[4] = { -bigStep, -smallStep, +smallStep, +bigStep };
			std::vector<std::string> buttons;
			for (int i = 0; i < 4; ++i) {
				if (isIntDeg) {
					buttons.push_back(std::format("{:+.0f}°", deltas[i]));
				} else {
					buttons.push_back(std::format("{:+.1f}", deltas[i]));
				}
			}
			buttons.push_back(Tr("$MSK_MenuBtnResetItem", "Reset to Default"));
			buttons.push_back(Tr("$MSK_MenuBtnBack", "Back"));

			ShowMenu(body, buttons, [a_actorId, a_armorId, a_forNpc, a_field, curVal, minV, maxV, smallStep, bigStep](std::int32_t a_idx) {
				auto act = RE::TESForm::LookupByID<RE::Actor>(a_actorId);
				auto arm = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
				if (!act) {
					return;
				}
				const float deltasLocal[4] = { -bigStep, -smallStep, +smallStep, +bigStep };
				auto        setField = [&](std::optional<float> v) {
                    auto fn = [&](Overrides::Rule& r) {
                        switch (a_field) {
                        case FloatField::kHipEase:
                            r.hipEase = v;
                            break;
                        case FloatField::kFlareAngle:
                            r.flareAngle = v;
                            break;
                        case FloatField::kSkirtGap:
                            r.skirtGap = v;
                            break;
                        case FloatField::kSkirtGapNoBone:
                            r.skirtGapNoBone = v;
                            break;
                        }
                    };
                    if (a_forNpc) {
                        Overrides::UpdateNpc(act, fn);
                    } else if (arm) {
                        Overrides::UpdateOutfit(arm, fn);
                    }
                    ApplyReprocess(a_actorId, a_armorId, a_forNpc);
				};

				if (a_idx >= 0 && a_idx < 4) {
					setField(std::clamp(curVal + deltasLocal[a_idx], minV, maxV));
					OpenFloatSubMenu(a_actorId, a_armorId, a_forNpc, a_field);
				} else if (a_idx == 4) {
					setField(std::nullopt);
					OpenFloatSubMenu(a_actorId, a_armorId, a_forNpc, a_field);
				} else {
					OpenDetailSubMenu(a_actorId, a_armorId, a_forNpc);
				}
			});
		}

		void OpenDetailSubMenu(RE::FormID a_actorId, RE::FormID a_armorId, bool a_forNpc)
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

			const auto res = Overrides::Resolve(actor, armor, Probe::GetGlobalDefaults(actor));
			const auto cutStr = res.hemStyle == 0 ? Tr("$MSK_CutOptRoughText", "Rough") : Tr("$MSK_CutOptLevelText", "Level");
			const auto uwStr = res.underwear ? Tr("$MSK_MenuOn", "On") : Tr("$MSK_MenuOff", "Off");

			const std::string body = std::format("{}\n{}", Tr("$MSK_MenuMoreTitle", "More Settings"), TargetHeader(actor, armor, a_forNpc));

			std::vector<std::string> buttons = {
				std::format("{}: {} [{}]", Tr("$MSK_CutStyleText", "Hem Cut Style"), cutStr, SourceLabel(res.hemStyleSource)),
				std::format("{}: {:.1f} [{}]...", Tr("$MSK_HipEaseText", "Hip Ease"), res.hipEase, SourceLabel(res.hipEaseSource)),
				std::format("{}: {:.0f}° [{}]...", Tr("$MSK_FlareText", "Flare Angle"), res.flareAngle, SourceLabel(res.flareAngleSource)),
				std::format("{}: {} [{}]", Tr("$MSK_UnderwearText", "Wear Underwear"), uwStr, SourceLabel(res.underwearSource)),
				std::format("{}: {:.1f} [{}]...", Tr("$MSK_GapText", "Skirt / Leg Gap"), res.skirtGap, SourceLabel(res.skirtGapSource)),
				std::format("{}: {:.1f} [{}]...", Tr("$MSK_GapStiffText", "Gap (Stiff Skirts)"), res.skirtGapNoBone, SourceLabel(res.skirtGapNoBoneSource)),
				Tr("$MSK_MenuBtnResetMore", "Reset Detail Settings"),
				Tr("$MSK_MenuBtnBack", "Back"),
			};

			ShowMenu(body, buttons, [a_actorId, a_armorId, a_forNpc, curCut = res.hemStyle, curUw = res.underwear](std::int32_t a_idx) {
				auto act = RE::TESForm::LookupByID<RE::Actor>(a_actorId);
				auto arm = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
				if (!act) {
					return;
				}
				switch (a_idx) {
				case 0: {
					const int nextCut = curCut == 0 ? 1 : 0;
					auto      fn = [nextCut](Overrides::Rule& r) { r.hemStyle = nextCut; };
					if (a_forNpc) {
						Overrides::UpdateNpc(act, fn);
					} else if (arm) {
						Overrides::UpdateOutfit(arm, fn);
					}
					ApplyReprocess(a_actorId, a_armorId, a_forNpc);
					OpenDetailSubMenu(a_actorId, a_armorId, a_forNpc);
					break;
				}
				case 1:
					OpenFloatSubMenu(a_actorId, a_armorId, a_forNpc, FloatField::kHipEase);
					break;
				case 2:
					OpenFloatSubMenu(a_actorId, a_armorId, a_forNpc, FloatField::kFlareAngle);
					break;
				case 3: {
					const bool nextUw = !curUw;
					auto       fn = [nextUw](Overrides::Rule& r) { r.underwear = nextUw; };
					if (a_forNpc) {
						Overrides::UpdateNpc(act, fn);
					} else if (arm) {
						Overrides::UpdateOutfit(arm, fn);
					}
					ApplyReprocess(a_actorId, a_armorId, a_forNpc);
					OpenDetailSubMenu(a_actorId, a_armorId, a_forNpc);
					break;
				}
				case 4:
					OpenFloatSubMenu(a_actorId, a_armorId, a_forNpc, FloatField::kSkirtGap);
					break;
				case 5:
					OpenFloatSubMenu(a_actorId, a_armorId, a_forNpc, FloatField::kSkirtGapNoBone);
					break;
				case 6: {
					auto fn = [](Overrides::Rule& r) {
						r.hemStyle.reset();
						r.hipEase.reset();
						r.flareAngle.reset();
						r.underwear.reset();
						r.skirtGap.reset();
						r.skirtGapNoBone.reset();
					};
					if (a_forNpc) {
						Overrides::UpdateNpc(act, fn);
					} else if (arm) {
						Overrides::UpdateOutfit(arm, fn);
					}
					ApplyReprocess(a_actorId, a_armorId, a_forNpc);
					OpenDetailSubMenu(a_actorId, a_armorId, a_forNpc);
					break;
				}
				default:
					OpenForActor(act);
					break;
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

		InputSink         g_inputSink;
		SpellCastSink     g_spellSink;
		MenuOpenCloseSink g_menuSink;
		bool              g_installed = false;
	}

	void Install()
	{
		if (g_installed) {
			return;
		}
		g_installed = true;

		EnsureTranslations();

		if (auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
		}
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

		const auto res = Overrides::Resolve(a_actor, armor, Probe::GetGlobalDefaults(a_actor));

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
			Tr("$MSK_MenuBtnNpcMore", "NPC: More Settings..."),
			npcExcluded ? Tr("$MSK_MenuBtnNpcEn", "NPC: Enable (Clear Exclude)") : Tr("$MSK_MenuBtnNpcExcl", "NPC: Exclude (Keep Original)"),
			Tr("$MSK_MenuBtnOutLen", "Outfit: Adjust Length..."),
			Tr("$MSK_MenuBtnOutMore", "Outfit: More Settings..."),
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
				OpenDetailSubMenu(actorId, armorId, true);
				break;
			case 2:
				Overrides::SetNpcEnabled(act, npcExcluded ? std::optional<bool>(true) : std::optional<bool>(false));
				Probe::ReprocessActorNow(actorId);
				RE::SendHUDMessage::ShowHUDMessage(std::format("MiniSkirt: {} -> {}", act->GetDisplayFullName(),
					npcExcluded ? Tr("$MSK_MenuEnabled", "Enabled") : Tr("$MSK_MenuExcluded", "Excluded"))
					                                   .c_str());
				break;
			case 3:
				OpenLengthSubMenu(actorId, armorId, false);
				break;
			case 4:
				OpenDetailSubMenu(actorId, armorId, false);
				break;
			case 5:
				if (arm) {
					Overrides::SetOutfitEnabled(arm, armExcluded ? std::optional<bool>(true) : std::optional<bool>(false));
					Probe::ReprocessByArmorNow(armorId);
					RE::SendHUDMessage::ShowHUDMessage(std::format("MiniSkirt: {} -> {}", arm->GetName(),
						armExcluded ? Tr("$MSK_MenuEnabled", "Enabled") : Tr("$MSK_MenuExcluded", "Excluded"))
						                                   .c_str());
				}
				break;
			case 6:
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
