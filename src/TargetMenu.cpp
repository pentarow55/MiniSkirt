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
	bool                      GlobalExcludeElderly();
	Overrides::GlobalDefaults GetGlobalDefaults(RE::Actor* a_actor);
	void                      UpdateGlobalSetting(const std::string& a_key, const std::string& a_val);
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

		enum class Scope
		{
			kGlobal,
			kOutfit,
			kNpc,
		};

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

		std::string TargetHeader(RE::Actor* a_actor, RE::TESObjectARMO* a_armor, Scope a_scope)
		{
			if (a_scope == Scope::kGlobal) {
				return Tr("$MSK_MenuGlobalTitle", "Settings for All (Global)");
			}
			if (a_scope == Scope::kNpc && a_actor) {
				return std::format("{}: {}", Tr("$MSK_MenuTargetNpc", "NPC"), a_actor->GetDisplayFullName());
			}
			return std::format("{}: {}", Tr("$MSK_MenuTargetOutfit", "Outfit"),
				a_armor && a_armor->GetName() && *a_armor->GetName() ? a_armor->GetName() : "(Unnamed)");
		}

		void ApplyReprocess(RE::FormID a_actorId, RE::FormID a_armorId, Scope a_scope)
		{
			if (a_scope == Scope::kNpc && a_actorId) {
				Probe::ReprocessActorNow(a_actorId);
			} else if (a_scope == Scope::kOutfit && a_armorId) {
				Probe::ReprocessByArmorNow(a_armorId);
			}
		}

		void OpenGlobalMenu(RE::FormID a_returnActorId);
		void OpenDetailSubMenu(RE::FormID a_actorId, RE::FormID a_armorId, Scope a_scope);

		void OpenLengthSubMenu(RE::FormID a_actorId, RE::FormID a_armorId, Scope a_scope)
		{
			auto actor = a_actorId ? RE::TESForm::LookupByID<RE::Actor>(a_actorId) : nullptr;
			auto armor = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
			if (a_scope == Scope::kNpc && !actor) {
				return;
			}
			if (a_scope == Scope::kOutfit && !armor) {
				RE::SendHUDMessage::ShowHUDMessage(Tr("$MSK_MenuNoOutfit", "MiniSkirt: Target is not wearing a body outfit.").c_str());
				return;
			}

			int         curPct = 30;
			std::string srcStr = SourceLabel(Overrides::Source::kGlobal);
			if (a_scope == Scope::kGlobal) {
				const auto gdef = Probe::GetGlobalDefaults(nullptr);
				curPct = std::clamp(static_cast<int>(std::lround(gdef.lengthPct)), 10, 100);
			} else {
				const auto res = Overrides::Resolve(actor, armor, Probe::GetGlobalDefaults(actor));
				curPct = std::clamp(static_cast<int>(std::lround(res.lengthPct)), 10, 100);
				srcStr = SourceLabel(res.lengthSource);
			}

			const std::string body = std::format("{}\n{}\n{}: {}% [{}]", Tr("$MSK_MenuLenTitle", "Adjust Skirt Length (10-100%)"),
				TargetHeader(actor, armor, a_scope), Tr("$MSK_MenuCurrentLabel", "Current"), curPct, srcStr);

			static constexpr int     kDeltas[] = { -10, -5, -1, +1, +5, +10 };
			std::vector<std::string> buttons = {
				"-10%",
				"-5%",
				"-1%",
				"+1%",
				"+5%",
				"+10%",
				Tr("$MSK_MenuBtnLen100", "100% (Original Length)"),
			};
			const bool hasReset = a_scope != Scope::kGlobal;
			if (hasReset) {
				buttons.push_back(Tr("$MSK_MenuBtnResetLen", "Reset Length to Default"));
			}
			buttons.push_back(Tr("$MSK_MenuBtnBack", "Back"));

			ShowMenu(body, buttons, [a_actorId, a_armorId, a_scope, curPct, hasReset](std::int32_t a_idx) {
				auto act = a_actorId ? RE::TESForm::LookupByID<RE::Actor>(a_actorId) : nullptr;
				auto arm = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
				auto applyLen = [&](std::optional<float> v) {
					if (a_scope == Scope::kGlobal) {
						if (v.has_value()) {
							Probe::UpdateGlobalSetting("fSkirtLength", std::format("{:.6f}", *v));
						}
					} else if (a_scope == Scope::kNpc && act) {
						Overrides::SetNpcLength(act, v);
						ApplyReprocess(a_actorId, a_armorId, a_scope);
					} else if (a_scope == Scope::kOutfit && arm) {
						Overrides::SetOutfitLength(arm, v);
						ApplyReprocess(a_actorId, a_armorId, a_scope);
					}
				};

				if (a_idx >= 0 && a_idx < 6) {
					const float nextPct = static_cast<float>(std::clamp(curPct + kDeltas[a_idx], 10, 100));
					applyLen(nextPct);
					OpenLengthSubMenu(a_actorId, a_armorId, a_scope);
				} else if (a_idx == 6) {
					applyLen(100.0f);
					OpenLengthSubMenu(a_actorId, a_armorId, a_scope);
				} else if (hasReset && a_idx == 7) {
					applyLen(std::nullopt);
					OpenLengthSubMenu(a_actorId, a_armorId, a_scope);
				} else {
					if (a_scope == Scope::kGlobal) {
						OpenGlobalMenu(a_actorId);
					} else if (act) {
						OpenForActor(act);
					}
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

		void OpenFloatSubMenu(RE::FormID a_actorId, RE::FormID a_armorId, Scope a_scope, FloatField a_field)
		{
			auto actor = a_actorId ? RE::TESForm::LookupByID<RE::Actor>(a_actorId) : nullptr;
			auto armor = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
			if ((a_scope == Scope::kNpc && !actor) || (a_scope == Scope::kOutfit && !armor)) {
				return;
			}
			const auto gdef = Probe::GetGlobalDefaults(actor);
			const auto res = (a_scope == Scope::kGlobal) ? Overrides::Resolve(nullptr, nullptr, gdef) : Overrides::Resolve(actor, armor, gdef);

			std::string       title;
			const char*       iniKey = "";
			float             curVal = 0.0f;
			float             minV = 0.0f, maxV = 8.0f, smallStep = 0.5f, bigStep = 1.0f;
			bool              isIntDeg = false;
			Overrides::Source src = Overrides::Source::kGlobal;

			switch (a_field) {
			case FloatField::kHipEase:
				title = Tr("$MSK_HipEaseText", "Hip Ease");
				iniKey = "fHipEase";
				curVal = res.hipEase;
				src = res.hipEaseSource;
				minV = 0.0f;
				maxV = 8.0f;
				smallStep = 0.5f;
				bigStep = 1.0f;
				break;
			case FloatField::kFlareAngle:
				title = Tr("$MSK_FlareText", "Flare Angle");
				iniKey = "fFlareAngle";
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
				iniKey = "fSkirtGap";
				curVal = res.skirtGap;
				src = res.skirtGapSource;
				minV = 0.0f;
				maxV = 8.0f;
				smallStep = 0.5f;
				bigStep = 1.0f;
				break;
			case FloatField::kSkirtGapNoBone:
				title = Tr("$MSK_GapStiffText", "Gap (Stiff Skirts)");
				iniKey = "fSkirtGapNoBone";
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
				TargetHeader(actor, armor, a_scope), Tr("$MSK_MenuCurrentLabel", "Current"), valStr, SourceLabel(src));

			const float              deltas[4] = { -bigStep, -smallStep, +smallStep, +bigStep };
			std::vector<std::string> buttons;
			for (int i = 0; i < 4; ++i) {
				if (isIntDeg) {
					buttons.push_back(std::format("{:+.0f}°", deltas[i]));
				} else {
					buttons.push_back(std::format("{:+.1f}", deltas[i]));
				}
			}
			const bool hasReset = a_scope != Scope::kGlobal;
			if (hasReset) {
				buttons.push_back(Tr("$MSK_MenuBtnResetItem", "Reset to Default"));
			}
			buttons.push_back(Tr("$MSK_MenuBtnBack", "Back"));

			ShowMenu(body, buttons, [a_actorId, a_armorId, a_scope, a_field, iniKey, curVal, minV, maxV, smallStep, bigStep, hasReset](std::int32_t a_idx) {
				auto act = a_actorId ? RE::TESForm::LookupByID<RE::Actor>(a_actorId) : nullptr;
				auto arm = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
				const float deltasLocal[4] = { -bigStep, -smallStep, +smallStep, +bigStep };
				auto        setField = [&](std::optional<float> v) {
                    if (a_scope == Scope::kGlobal) {
                        if (v.has_value()) {
                            Probe::UpdateGlobalSetting(iniKey, std::format("{:.6f}", *v));
                        }
                        return;
                    }
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
                    if (a_scope == Scope::kNpc && act) {
                        Overrides::UpdateNpc(act, fn);
                    } else if (a_scope == Scope::kOutfit && arm) {
                        Overrides::UpdateOutfit(arm, fn);
                    }
                    ApplyReprocess(a_actorId, a_armorId, a_scope);
				};

				if (a_idx >= 0 && a_idx < 4) {
					setField(std::clamp(curVal + deltasLocal[a_idx], minV, maxV));
					OpenFloatSubMenu(a_actorId, a_armorId, a_scope, a_field);
				} else if (hasReset && a_idx == 4) {
					setField(std::nullopt);
					OpenFloatSubMenu(a_actorId, a_armorId, a_scope, a_field);
				} else {
					OpenDetailSubMenu(a_actorId, a_armorId, a_scope);
				}
			});
		}

		void OpenDetailSubMenu(RE::FormID a_actorId, RE::FormID a_armorId, Scope a_scope)
		{
			auto actor = a_actorId ? RE::TESForm::LookupByID<RE::Actor>(a_actorId) : nullptr;
			auto armor = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
			if (a_scope == Scope::kNpc && !actor) {
				return;
			}
			if (a_scope == Scope::kOutfit && !armor) {
				RE::SendHUDMessage::ShowHUDMessage(Tr("$MSK_MenuNoOutfit", "MiniSkirt: Target is not wearing a body outfit.").c_str());
				return;
			}

			const auto gdef = Probe::GetGlobalDefaults(actor);
			const auto res = (a_scope == Scope::kGlobal) ? Overrides::Resolve(nullptr, nullptr, gdef) : Overrides::Resolve(actor, armor, gdef);
			const auto cutStr = res.hemStyle == 0 ? Tr("$MSK_CutOptRoughText", "Rough") : Tr("$MSK_CutOptLevelText", "Level");
			const bool elderlyExcl = Probe::GlobalExcludeElderly();

			const std::string body = std::format("{}\n{}", Tr("$MSK_MenuMoreTitle", "More Settings"), TargetHeader(actor, armor, a_scope));

			enum class DAct
			{
				kCut,
				kHip,
				kFlare,
				kUnderwear,
				kGap,
				kGapStiff,
				kElderly,
				kReset,
				kBack,
			};
			std::vector<std::string> buttons;
			std::vector<DAct>        actions;

			buttons.push_back(std::format("{}: {} [{}]", Tr("$MSK_CutStyleText", "Hem Cut Style"), cutStr, SourceLabel(res.hemStyleSource)));
			actions.push_back(DAct::kCut);
			buttons.push_back(std::format("{}: {:.1f} [{}]...", Tr("$MSK_HipEaseText", "Hip Ease"), res.hipEase, SourceLabel(res.hipEaseSource)));
			actions.push_back(DAct::kHip);
			buttons.push_back(std::format("{}: {:.0f}° [{}]...", Tr("$MSK_FlareText", "Flare Angle"), res.flareAngle, SourceLabel(res.flareAngleSource)));
			actions.push_back(DAct::kFlare);
			buttons.push_back(std::format("[{}] {} [{}]", res.underwear ? "x" : " ", Tr("$MSK_UnderwearText", "Wear Underwear"), SourceLabel(res.underwearSource)));
			actions.push_back(DAct::kUnderwear);
			buttons.push_back(std::format("{}: {:.1f} [{}]...", Tr("$MSK_GapText", "Skirt / Leg Gap"), res.skirtGap, SourceLabel(res.skirtGapSource)));
			actions.push_back(DAct::kGap);
			buttons.push_back(std::format("{}: {:.1f} [{}]...", Tr("$MSK_GapStiffText", "Gap (Stiff Skirts)"), res.skirtGapNoBone, SourceLabel(res.skirtGapNoBoneSource)));
			actions.push_back(DAct::kGapStiff);

			if (a_scope == Scope::kGlobal) {
				buttons.push_back(std::format("[{}] {}", elderlyExcl ? "x" : " ", Tr("$MSK_ElderlyText", "Exclude Elderly Women")));
				actions.push_back(DAct::kElderly);
			} else {
				buttons.push_back(Tr("$MSK_MenuBtnResetMore", "Reset Detail Settings"));
				actions.push_back(DAct::kReset);
			}
			buttons.push_back(Tr("$MSK_MenuBtnBack", "Back"));
			actions.push_back(DAct::kBack);

			ShowMenu(body, buttons, [a_actorId, a_armorId, a_scope, curCut = res.hemStyle, curUw = res.underwear, elderlyExcl, actions](std::int32_t a_idx) {
				if (a_idx < 0 || static_cast<std::size_t>(a_idx) >= actions.size()) {
					return;
				}
				auto act = a_actorId ? RE::TESForm::LookupByID<RE::Actor>(a_actorId) : nullptr;
				auto arm = a_armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_armorId) : nullptr;
				switch (actions[static_cast<std::size_t>(a_idx)]) {
				case DAct::kCut: {
					const int nextCut = curCut == 0 ? 1 : 0;
					if (a_scope == Scope::kGlobal) {
						Probe::UpdateGlobalSetting("iHemStyle", std::to_string(nextCut));
					} else {
						auto fn = [nextCut](Overrides::Rule& r) { r.hemStyle = nextCut; };
						if (a_scope == Scope::kNpc && act) {
							Overrides::UpdateNpc(act, fn);
						} else if (a_scope == Scope::kOutfit && arm) {
							Overrides::UpdateOutfit(arm, fn);
						}
						ApplyReprocess(a_actorId, a_armorId, a_scope);
					}
					OpenDetailSubMenu(a_actorId, a_armorId, a_scope);
					break;
				}
				case DAct::kHip:
					OpenFloatSubMenu(a_actorId, a_armorId, a_scope, FloatField::kHipEase);
					break;
				case DAct::kFlare:
					OpenFloatSubMenu(a_actorId, a_armorId, a_scope, FloatField::kFlareAngle);
					break;
				case DAct::kUnderwear: {
					const bool nextUw = !curUw;
					if (a_scope == Scope::kGlobal) {
						Probe::UpdateGlobalSetting("bUnderwear", nextUw ? "1" : "0");
					} else {
						auto fn = [nextUw](Overrides::Rule& r) { r.underwear = nextUw; };
						if (a_scope == Scope::kNpc && act) {
							Overrides::UpdateNpc(act, fn);
						} else if (a_scope == Scope::kOutfit && arm) {
							Overrides::UpdateOutfit(arm, fn);
						}
						ApplyReprocess(a_actorId, a_armorId, a_scope);
					}
					OpenDetailSubMenu(a_actorId, a_armorId, a_scope);
					break;
				}
				case DAct::kGap:
					OpenFloatSubMenu(a_actorId, a_armorId, a_scope, FloatField::kSkirtGap);
					break;
				case DAct::kGapStiff:
					OpenFloatSubMenu(a_actorId, a_armorId, a_scope, FloatField::kSkirtGapNoBone);
					break;
				case DAct::kElderly:
					Probe::UpdateGlobalSetting("bExcludeElderly", elderlyExcl ? "0" : "1");
					OpenDetailSubMenu(a_actorId, a_armorId, a_scope);
					break;
				case DAct::kReset: {
					auto fn = [](Overrides::Rule& r) {
						r.hemStyle.reset();
						r.hipEase.reset();
						r.flareAngle.reset();
						r.underwear.reset();
						r.skirtGap.reset();
						r.skirtGapNoBone.reset();
					};
					if (a_scope == Scope::kNpc && act) {
						Overrides::UpdateNpc(act, fn);
					} else if (a_scope == Scope::kOutfit && arm) {
						Overrides::UpdateOutfit(arm, fn);
					}
					ApplyReprocess(a_actorId, a_armorId, a_scope);
					OpenDetailSubMenu(a_actorId, a_armorId, a_scope);
					break;
				}
				default:
					if (a_scope == Scope::kGlobal) {
						OpenGlobalMenu(a_actorId);
					} else if (act) {
						OpenForActor(act);
					}
					break;
				}
			});
		}

		void OpenGlobalMenu(RE::FormID a_returnActorId)
		{
			const auto        gdef = Probe::GetGlobalDefaults(nullptr);
			const std::string status = gdef.enabled ?
			                               std::format("{} ({:.0f}%)", Tr("$MSK_MenuEnabled", "Enabled"), gdef.lengthPct) :
			                               Tr("$MSK_MenuExcluded", "Excluded");
			const std::string body = std::format("{}\n{}: {}", Tr("$MSK_MenuGlobalTitle", "Settings for All (Global)"),
				Tr("$MSK_MenuCurrentLabel", "Current"), status);

			enum class GAct
			{
				kApply,
				kLen,
				kMore,
				kExit,
			};
			std::vector<std::string> buttons;
			std::vector<GAct>        actions;

			buttons.push_back(std::format("[{}] {}", gdef.enabled ? "x" : " ", Tr("$MSK_MenuBtnApply", "Apply MiniSkirt")));
			actions.push_back(GAct::kApply);
			if (gdef.enabled) {
				buttons.push_back(std::format("    - {}", Tr("$MSK_MenuBtnSubLen", "Adjust Length...")));
				actions.push_back(GAct::kLen);
				buttons.push_back(std::format("    - {}", Tr("$MSK_MenuBtnSubMore", "More Settings...")));
				actions.push_back(GAct::kMore);
			}
			buttons.push_back(a_returnActorId ? Tr("$MSK_MenuBtnBack", "Back") : Tr("$MSK_MenuBtnClose", "Close"));
			actions.push_back(GAct::kExit);

			ShowMenu(body, buttons, [a_returnActorId, enabled = gdef.enabled, actions](std::int32_t a_idx) {
				if (a_idx < 0 || static_cast<std::size_t>(a_idx) >= actions.size()) {
					return;
				}
				switch (actions[static_cast<std::size_t>(a_idx)]) {
				case GAct::kApply:
					Probe::UpdateGlobalSetting("bEnabled", enabled ? "0" : "1");
					OpenGlobalMenu(a_returnActorId);
					break;
				case GAct::kLen:
					OpenLengthSubMenu(a_returnActorId, 0, Scope::kGlobal);
					break;
				case GAct::kMore:
					OpenDetailSubMenu(a_returnActorId, 0, Scope::kGlobal);
					break;
				default:
					if (a_returnActorId) {
						if (auto act = RE::TESForm::LookupByID<RE::Actor>(a_returnActorId)) {
							OpenForActor(act);
						}
					}
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
			// 目の前に女性 NPC がいなくても、全体設定メニューをそのまま開けるようにする
			OpenGlobalMenu(0);
			return;
		}
		OpenForActor(actor);
	}

	void OpenForActor(RE::Actor* a_actor)
	{
		if (!IsValidFemaleTarget(a_actor)) {
			OpenGlobalMenu(0);
			return;
		}
		const auto actorId = a_actor->GetFormID();
		auto       armor = a_actor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
		const auto armorId = armor ? armor->GetFormID() : 0u;

		const auto        gdef = Probe::GetGlobalDefaults(a_actor);
		const std::string npcName = a_actor->GetDisplayFullName();
		const std::string armName = armor && armor->GetName() && *armor->GetName() ? armor->GetName() : "(None)";

		if (!gdef.enabled) {
			const std::string body = std::format("MiniSkirt: {}\n{}: {}\n\n{}", npcName, Tr("$MSK_MenuOutfitLabel", "Outfit"), armName,
				Tr("$MSK_MenuGlobalDisabled", "MiniSkirt is disabled in MCM. Enable it in MCM to adjust settings."));
			std::vector<std::string> buttons = {
				Tr("$MSK_MenuBtnGlobal", "Settings for All (Global)..."),
				Tr("$MSK_MenuBtnClose", "Close"),
			};
			ShowMenu(body, buttons, [actorId](std::int32_t a_idx) {
				if (a_idx == 0) {
					OpenGlobalMenu(actorId);
				}
			});
			return;
		}

		const auto        res = Overrides::Resolve(a_actor, armor, gdef);
		const std::string status = res.enabled ?
		                               std::format("{} ({:.0f}% [{}])", Tr("$MSK_MenuEnabled", "Enabled"), res.lengthPct, SourceLabel(res.lengthSource)) :
		                               std::format("{} ([{}])", Tr("$MSK_MenuExcluded", "Excluded"), SourceLabel(res.enabledSource));

		const std::string body = std::format("MiniSkirt: {}\n{}: {}\n{}: {}", npcName, Tr("$MSK_MenuOutfitLabel", "Outfit"), armName,
			Tr("$MSK_MenuCurrentLabel", "Current"), status);

		const auto npcRule = Overrides::GetNpcRule(a_actor);
		const auto armRule = Overrides::GetOutfitRule(armor);
		const bool npcOverride = npcRule.has_value();
		const bool armOverride = armRule.has_value();
		const bool npcApply = npcOverride ? (!npcRule->enabled.has_value() || *npcRule->enabled) : res.enabled;
		const bool armApply = armOverride ? (!armRule->enabled.has_value() || *armRule->enabled) : true;

		enum class Act
		{
			kGlobal,
			kOutOverride,
			kOutApply,
			kOutLen,
			kOutMore,
			kNpcOverride,
			kNpcApply,
			kNpcLen,
			kNpcMore,
			kClose,
		};
		std::vector<std::string> buttons;
		std::vector<Act>         actions;

		const std::string applyLabel = Tr("$MSK_MenuBtnApply", "Apply MiniSkirt");
		const std::string subLen = std::format("    - {}", Tr("$MSK_MenuBtnSubLen", "Adjust Length..."));
		const std::string subMore = std::format("    - {}", Tr("$MSK_MenuBtnSubMore", "More Settings..."));

		// 広い範囲 → 狭い範囲の順（全体 → この衣装 → この NPC）
		buttons.push_back(Tr("$MSK_MenuBtnGlobal", "Settings for All (Global)..."));
		actions.push_back(Act::kGlobal);

		if (armor) {
			buttons.push_back(std::format("[{}] {}", armOverride ? "x" : " ", Tr("$MSK_MenuBtnOutOverride", "Override for this Outfit")));
			actions.push_back(Act::kOutOverride);
			if (armOverride) {
				buttons.push_back(std::format("    - [{}] {}", armApply ? "x" : " ", applyLabel));
				actions.push_back(Act::kOutApply);
				if (armApply) {
					buttons.push_back(subLen);
					actions.push_back(Act::kOutLen);
					buttons.push_back(subMore);
					actions.push_back(Act::kOutMore);
				}
			}
		}

		buttons.push_back(std::format("[{}] {}", npcOverride ? "x" : " ", Tr("$MSK_MenuBtnNpcOverride", "Override for this NPC")));
		actions.push_back(Act::kNpcOverride);
		if (npcOverride) {
			buttons.push_back(std::format("    - [{}] {}", npcApply ? "x" : " ", applyLabel));
			actions.push_back(Act::kNpcApply);
			if (npcApply) {
				buttons.push_back(subLen);
				actions.push_back(Act::kNpcLen);
				buttons.push_back(subMore);
				actions.push_back(Act::kNpcMore);
			}
		}

		buttons.push_back(Tr("$MSK_MenuBtnClose", "Close"));
		actions.push_back(Act::kClose);

		const bool curEffective = res.enabled;
		ShowMenu(body, buttons, [actorId, armorId, npcOverride, armOverride, npcApply, armApply, curEffective, actions](std::int32_t a_idx) {
			if (a_idx < 0 || static_cast<std::size_t>(a_idx) >= actions.size()) {
				return;
			}
			auto act = RE::TESForm::LookupByID<RE::Actor>(actorId);
			auto arm = armorId ? RE::TESForm::LookupByID<RE::TESObjectARMO>(armorId) : nullptr;
			if (!act) {
				return;
			}
			switch (actions[static_cast<std::size_t>(a_idx)]) {
			case Act::kGlobal:
				OpenGlobalMenu(actorId);
				break;
			case Act::kOutOverride:
				if (arm) {
					if (armOverride) {
						Overrides::ClearOutfit(arm);
						Probe::ReprocessByArmorNow(armorId);
					} else {
						Overrides::SetOutfitEnabled(arm, true);
					}
				}
				OpenForActor(act);
				break;
			case Act::kOutApply:
				if (arm) {
					Overrides::SetOutfitEnabled(arm, !armApply);
					Probe::ReprocessByArmorNow(armorId);
				}
				OpenForActor(act);
				break;
			case Act::kOutLen:
				OpenLengthSubMenu(actorId, armorId, Scope::kOutfit);
				break;
			case Act::kOutMore:
				OpenDetailSubMenu(actorId, armorId, Scope::kOutfit);
				break;
			case Act::kNpcOverride:
				if (npcOverride) {
					Overrides::ClearNpc(act);
					Probe::ReprocessActorNow(actorId);
				} else {
					Overrides::SetNpcEnabled(act, curEffective);
				}
				OpenForActor(act);
				break;
			case Act::kNpcApply:
				Overrides::SetNpcEnabled(act, !npcApply);
				Probe::ReprocessActorNow(actorId);
				OpenForActor(act);
				break;
			case Act::kNpcLen:
				OpenLengthSubMenu(actorId, armorId, Scope::kNpc);
				break;
			case Act::kNpcMore:
				OpenDetailSubMenu(actorId, armorId, Scope::kNpc);
				break;
			default:
				break;
			}
		});
	}
}
