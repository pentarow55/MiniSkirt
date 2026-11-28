#include "PCH.h"

#include "Overrides.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace Overrides
{
	namespace
	{
		constexpr const char* kFilePath = "Data/SKSE/Plugins/MiniSkirt_Overrides.json";
		constexpr float       kMinLength = 10.0f;
		constexpr float       kMaxLength = 100.0f;

		std::mutex                  g_mutex;
		std::map<std::string, Rule> g_npcs;
		std::map<std::string, Rule> g_outfits;

		float Round1(float a_v) { return std::round(a_v * 10.0f) / 10.0f; }

		void ClampRule(Rule& a_r)
		{
			if (a_r.lengthPct.has_value()) {
				a_r.lengthPct = Round1(std::clamp(*a_r.lengthPct, kMinLength, kMaxLength));
			}
			if (a_r.hemStyle.has_value()) {
				a_r.hemStyle = std::clamp(*a_r.hemStyle, 0, 1);
			}
			if (a_r.hipEase.has_value()) {
				a_r.hipEase = Round1(std::clamp(*a_r.hipEase, 0.0f, 8.0f));
			}
			if (a_r.flareAngle.has_value()) {
				a_r.flareAngle = Round1(std::clamp(*a_r.flareAngle, 0.0f, 20.0f));
			}
			if (a_r.flareStart.has_value()) {
				a_r.flareStart = Round1(std::clamp(*a_r.flareStart, -50.0f, 50.0f));
			}
			if (a_r.flareCurve.has_value()) {
				a_r.flareCurve = Round1(std::clamp(*a_r.flareCurve, -10.0f, 15.0f));
			}
			if (a_r.skirtGap.has_value()) {
				a_r.skirtGap = Round1(std::clamp(*a_r.skirtGap, 0.0f, 8.0f));
			}
			if (a_r.skirtGapNoBone.has_value()) {
				a_r.skirtGapNoBone = Round1(std::clamp(*a_r.skirtGapNoBone, 0.0f, 10.0f));
			}
		}

		std::string NpcKey(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return {};
			}
			if (auto base = a_actor->GetActorBase()) {
				return FormKey(base);
			}
			return FormKey(a_actor);
		}

		std::string NpcName(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return {};
			}
			if (const char* n = a_actor->GetDisplayFullName(); n && *n) {
				return n;
			}
			return {};
		}

		std::string ArmorName(RE::TESObjectARMO* a_armor)
		{
			if (!a_armor) {
				return {};
			}
			if (const char* n = a_armor->GetName(); n && *n) {
				return n;
			}
			return {};
		}

		void ParseSection(const nlohmann::json& a_obj, std::map<std::string, Rule>& a_out)
		{
			if (!a_obj.is_object()) {
				return;
			}
			for (auto it = a_obj.begin(); it != a_obj.end(); ++it) {
				if (!it.value().is_object()) {
					continue;
				}
				const auto& v = it.value();
				Rule        r;
				if (auto n = v.find("name"); n != v.end() && n->is_string()) {
					r.name = n->get<std::string>();
				}
				if (auto e = v.find("enabled"); e != v.end() && e->is_boolean()) {
					r.enabled = e->get<bool>();
				}
				if (auto l = v.find("length"); l != v.end() && l->is_number()) {
					r.lengthPct = l->get<float>();
				}
				if (auto h = v.find("hemStyle"); h != v.end() && h->is_number_integer()) {
					r.hemStyle = h->get<int>();
				}
				if (auto he = v.find("hipEase"); he != v.end() && he->is_number()) {
					r.hipEase = he->get<float>();
				}
				if (auto fa = v.find("flareAngle"); fa != v.end() && fa->is_number()) {
					r.flareAngle = fa->get<float>();
				}
				if (auto fs = v.find("flareStart"); fs != v.end() && fs->is_number()) {
					r.flareStart = fs->get<float>();
				}
				if (auto fc = v.find("flareCurve"); fc != v.end() && fc->is_number()) {
					r.flareCurve = fc->get<float>();
				}
				if (auto rl = v.find("replaceLegs"); rl != v.end() && rl->is_boolean()) {
					r.replaceLegs = rl->get<bool>();
				}
				if (auto uw = v.find("underwear"); uw != v.end() && uw->is_boolean()) {
					r.underwear = uw->get<bool>();
				}
				if (auto sg = v.find("skirtGap"); sg != v.end() && sg->is_number()) {
					r.skirtGap = sg->get<float>();
				}
				if (auto sgn = v.find("skirtGapNoBone"); sgn != v.end() && sgn->is_number()) {
					r.skirtGapNoBone = sgn->get<float>();
				}
				ClampRule(r);
				if (!r.empty()) {
					a_out[it.key()] = std::move(r);
				}
			}
		}

		nlohmann::json DumpSection(const std::map<std::string, Rule>& a_map)
		{
			nlohmann::json obj = nlohmann::json::object();
			for (const auto& [k, r] : a_map) {
				if (r.empty()) {
					continue;
				}
				nlohmann::json item = nlohmann::json::object();
				if (!r.name.empty()) {
					item["name"] = r.name;
				}
				if (r.enabled.has_value()) {
					item["enabled"] = *r.enabled;
				}
				if (r.lengthPct.has_value()) {
					item["length"] = Round1(*r.lengthPct);
				}
				if (r.hemStyle.has_value()) {
					item["hemStyle"] = *r.hemStyle;
				}
				if (r.hipEase.has_value()) {
					item["hipEase"] = Round1(*r.hipEase);
				}
				if (r.flareAngle.has_value()) {
					item["flareAngle"] = Round1(*r.flareAngle);
				}
				if (r.flareStart.has_value()) {
					item["flareStart"] = Round1(*r.flareStart);
				}
				if (r.flareCurve.has_value()) {
					item["flareCurve"] = Round1(*r.flareCurve);
				}
				if (r.replaceLegs.has_value()) {
					item["replaceLegs"] = *r.replaceLegs;
				}
				if (r.underwear.has_value()) {
					item["underwear"] = *r.underwear;
				}
				if (r.skirtGap.has_value()) {
					item["skirtGap"] = Round1(*r.skirtGap);
				}
				if (r.skirtGapNoBone.has_value()) {
					item["skirtGapNoBone"] = Round1(*r.skirtGapNoBone);
				}
				obj[k] = std::move(item);
			}
			return obj;
		}

		bool SaveUnlocked()
		{
			try {
				std::filesystem::path p(kFilePath);
				if (p.has_parent_path()) {
					std::error_code ec;
					std::filesystem::create_directories(p.parent_path(), ec);
				}
				nlohmann::json root = nlohmann::json::object();
				root["version"] = 1;
				root["npcs"] = DumpSection(g_npcs);
				root["outfits"] = DumpSection(g_outfits);

				std::ofstream out(p, std::ios::binary | std::ios::trunc);
				if (!out) {
					logger::warn("Overrides: 保存先を開けない: {}", kFilePath);
					return false;
				}
				out << root.dump(2) << "\n";
				return static_cast<bool>(out);
			} catch (const std::exception& e) {
				logger::warn("Overrides: 保存に失敗: {}", e.what());
				return false;
			}
		}

		void ApplyRule(const Rule& a_r, Source a_src, Resolved& a_out)
		{
			if (a_r.enabled.has_value()) {
				a_out.enabled = *a_r.enabled;
				a_out.enabledSource = a_src;
			}
			if (a_r.lengthPct.has_value()) {
				a_out.lengthPct = *a_r.lengthPct;
				a_out.lengthSource = a_src;
			}
			if (a_r.hemStyle.has_value()) {
				a_out.hemStyle = *a_r.hemStyle;
				a_out.hemStyleSource = a_src;
			}
			if (a_r.hipEase.has_value()) {
				a_out.hipEase = *a_r.hipEase;
				a_out.hipEaseSource = a_src;
			}
			if (a_r.flareAngle.has_value()) {
				a_out.flareAngle = *a_r.flareAngle;
				a_out.flareAngleSource = a_src;
			}
			if (a_r.flareStart.has_value()) {
				a_out.flareStart = *a_r.flareStart;
				a_out.flareStartSource = a_src;
			}
			if (a_r.flareCurve.has_value()) {
				a_out.flareCurve = *a_r.flareCurve;
				a_out.flareCurveSource = a_src;
			}
			if (a_r.replaceLegs.has_value()) {
				a_out.replaceLegs = *a_r.replaceLegs;
				a_out.replaceLegsSource = a_src;
			}
			if (a_r.underwear.has_value()) {
				a_out.underwear = *a_r.underwear;
				a_out.underwearSource = a_src;
			}
			if (a_r.skirtGap.has_value()) {
				a_out.skirtGap = *a_r.skirtGap;
				a_out.skirtGapSource = a_src;
			}
			if (a_r.skirtGapNoBone.has_value()) {
				a_out.skirtGapNoBone = *a_r.skirtGapNoBone;
				a_out.skirtGapNoBoneSource = a_src;
			}
		}
	}

	std::string FormKey(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		if (auto file = a_form->GetFile(0); file && file->fileName[0] != '\0') {
			return std::format("{}|{:06X}", file->fileName, a_form->GetLocalFormID());
		}
		return std::format("DYNAMIC|{:08X}", a_form->GetFormID());
	}

	void Load()
	{
		std::lock_guard l(g_mutex);
		g_npcs.clear();
		g_outfits.clear();

		std::ifstream in(kFilePath);
		if (!in) {
			logger::info("Overrides: 個別設定ファイルなし（{}）", kFilePath);
			return;
		}
		try {
			nlohmann::json root;
			in >> root;
			if (root.contains("npcs")) {
				ParseSection(root["npcs"], g_npcs);
			}
			if (root.contains("outfits")) {
				ParseSection(root["outfits"], g_outfits);
			}
			logger::info("Overrides: 個別設定を読み込みました（NPC {} 件、衣装 {} 件）", g_npcs.size(), g_outfits.size());
		} catch (const std::exception& e) {
			logger::warn("Overrides: {} の読み込みに失敗: {}", kFilePath, e.what());
		}
	}

	bool Save()
	{
		std::lock_guard l(g_mutex);
		return SaveUnlocked();
	}

	Resolved Resolve(RE::Actor* a_actor, RE::TESObjectARMO* a_armor, const GlobalDefaults& a_global)
	{
		Resolved out;
		out.enabled = a_global.enabled && !a_global.elderlyExcluded;
		out.lengthPct = std::clamp(a_global.lengthPct, kMinLength, kMaxLength);
		out.hemStyle = std::clamp(a_global.hemStyle, 0, 1);
		out.hipEase = std::clamp(a_global.hipEase, 0.0f, 8.0f);
		out.flareAngle = std::clamp(a_global.flareAngle, 0.0f, 20.0f);
		out.flareStart = std::clamp(a_global.flareStart, -50.0f, 50.0f);
		out.flareCurve = std::clamp(a_global.flareCurve, -10.0f, 15.0f);
		out.replaceLegs = a_global.replaceLegs;
		out.underwear = a_global.underwear;
		out.skirtGap = std::clamp(a_global.skirtGap, 0.0f, 8.0f);
		out.skirtGapNoBone = std::clamp(a_global.skirtGapNoBone, 0.0f, 10.0f);

		const auto npcKey = NpcKey(a_actor);
		const auto outfitKey = FormKey(a_armor);

		std::lock_guard l(g_mutex);

		if (!outfitKey.empty()) {
			if (auto it = g_outfits.find(outfitKey); it != g_outfits.end()) {
				ApplyRule(it->second, Source::kOutfit, out);
			}
		}

		if (!npcKey.empty()) {
			if (auto it = g_npcs.find(npcKey); it != g_npcs.end()) {
				out.hasNpcRule = true;
				ApplyRule(it->second, Source::kNpc, out);
			}
		}

		return out;
	}

	std::optional<Rule> GetNpcRule(RE::Actor* a_actor)
	{
		const auto key = NpcKey(a_actor);
		if (key.empty()) {
			return std::nullopt;
		}
		std::lock_guard l(g_mutex);
		if (auto it = g_npcs.find(key); it != g_npcs.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	std::optional<Rule> GetOutfitRule(RE::TESObjectARMO* a_armor)
	{
		const auto key = FormKey(a_armor);
		if (key.empty()) {
			return std::nullopt;
		}
		std::lock_guard l(g_mutex);
		if (auto it = g_outfits.find(key); it != g_outfits.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	void UpdateNpc(RE::Actor* a_actor, const std::function<void(Rule&)>& a_fn)
	{
		const auto key = NpcKey(a_actor);
		if (key.empty() || !a_fn) {
			return;
		}
		std::lock_guard l(g_mutex);
		auto&           r = g_npcs[key];
		r.name = NpcName(a_actor);
		a_fn(r);
		ClampRule(r);
		if (r.empty()) {
			g_npcs.erase(key);
		}
		SaveUnlocked();
	}

	void UpdateOutfit(RE::TESObjectARMO* a_armor, const std::function<void(Rule&)>& a_fn)
	{
		const auto key = FormKey(a_armor);
		if (key.empty() || !a_fn) {
			return;
		}
		std::lock_guard l(g_mutex);
		auto&           r = g_outfits[key];
		r.name = ArmorName(a_armor);
		a_fn(r);
		ClampRule(r);
		if (r.empty()) {
			g_outfits.erase(key);
		}
		SaveUnlocked();
	}

	void SetNpcLength(RE::Actor* a_actor, std::optional<float> a_lengthPct)
	{
		UpdateNpc(a_actor, [&](Rule& r) {
			if (a_lengthPct.has_value()) {
				r.lengthPct = *a_lengthPct;
				if (r.enabled.has_value() && !*r.enabled) {
					r.enabled = true;
				}
			} else {
				r.lengthPct.reset();
			}
		});
	}

	void SetNpcEnabled(RE::Actor* a_actor, std::optional<bool> a_enabled)
	{
		UpdateNpc(a_actor, [&](Rule& r) { r.enabled = a_enabled; });
	}

	void ClearNpc(RE::Actor* a_actor)
	{
		const auto key = NpcKey(a_actor);
		if (key.empty()) {
			return;
		}
		std::lock_guard l(g_mutex);
		g_npcs.erase(key);
		SaveUnlocked();
	}

	void SetOutfitLength(RE::TESObjectARMO* a_armor, std::optional<float> a_lengthPct)
	{
		UpdateOutfit(a_armor, [&](Rule& r) {
			if (a_lengthPct.has_value()) {
				r.lengthPct = *a_lengthPct;
				if (r.enabled.has_value() && !*r.enabled) {
					r.enabled = true;
				}
			} else {
				r.lengthPct.reset();
			}
		});
	}

	void SetOutfitEnabled(RE::TESObjectARMO* a_armor, std::optional<bool> a_enabled)
	{
		UpdateOutfit(a_armor, [&](Rule& r) { r.enabled = a_enabled; });
	}

	void ClearOutfit(RE::TESObjectARMO* a_armor)
	{
		const auto key = FormKey(a_armor);
		if (key.empty()) {
			return;
		}
		std::lock_guard l(g_mutex);
		g_outfits.erase(key);
		SaveUnlocked();
	}
}
