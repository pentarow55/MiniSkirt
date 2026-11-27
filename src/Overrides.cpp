#include "PCH.h"

#include "Overrides.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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
				Rule r;
				if (auto n = it.value().find("name"); n != it.value().end() && n->is_string()) {
					r.name = n->get<std::string>();
				}
				if (auto e = it.value().find("enabled"); e != it.value().end() && e->is_boolean()) {
					r.enabled = e->get<bool>();
				}
				if (auto l = it.value().find("length"); l != it.value().end() && l->is_number()) {
					r.lengthPct = std::clamp(l->get<float>(), kMinLength, kMaxLength);
				}
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
					item["length"] = std::round(*r.lengthPct * 10.0f) / 10.0f;
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

	Resolved Resolve(RE::Actor* a_actor, RE::TESObjectARMO* a_armor, bool a_globalEnabled, bool a_elderlyExcluded, float a_globalLengthPct)
	{
		Resolved out;
		out.enabled = a_globalEnabled && !a_elderlyExcluded;
		out.lengthPct = std::clamp(a_globalLengthPct, kMinLength, kMaxLength);
		out.enabledSource = Source::kGlobal;
		out.lengthSource = Source::kGlobal;

		const auto npcKey = NpcKey(a_actor);
		const auto outfitKey = FormKey(a_armor);

		std::lock_guard l(g_mutex);

		// 衣装別（中優先度）
		if (!outfitKey.empty()) {
			if (auto it = g_outfits.find(outfitKey); it != g_outfits.end()) {
				if (it->second.enabled.has_value()) {
					out.enabled = *it->second.enabled;
					out.enabledSource = Source::kOutfit;
				}
				if (it->second.lengthPct.has_value()) {
					out.lengthPct = *it->second.lengthPct;
					out.lengthSource = Source::kOutfit;
				}
			}
		}

		// NPC 個別（最高優先度）
		if (!npcKey.empty()) {
			if (auto it = g_npcs.find(npcKey); it != g_npcs.end()) {
				out.hasNpcRule = true;
				if (it->second.enabled.has_value()) {
					out.enabled = *it->second.enabled;
					out.enabledSource = Source::kNpc;
				}
				if (it->second.lengthPct.has_value()) {
					out.lengthPct = *it->second.lengthPct;
					out.lengthSource = Source::kNpc;
				}
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

	void SetNpcLength(RE::Actor* a_actor, std::optional<float> a_lengthPct)
	{
		const auto key = NpcKey(a_actor);
		if (key.empty()) {
			return;
		}
		std::lock_guard l(g_mutex);
		auto&           r = g_npcs[key];
		r.name = NpcName(a_actor);
		if (a_lengthPct.has_value()) {
			r.lengthPct = std::clamp(*a_lengthPct, kMinLength, kMaxLength);
			// 丈を明示設定したら、除外中だった場合は解除して有効にする
			if (r.enabled.has_value() && !*r.enabled) {
				r.enabled = true;
			}
		} else {
			r.lengthPct.reset();
		}
		if (r.empty()) {
			g_npcs.erase(key);
		}
		SaveUnlocked();
	}

	void SetNpcEnabled(RE::Actor* a_actor, std::optional<bool> a_enabled)
	{
		const auto key = NpcKey(a_actor);
		if (key.empty()) {
			return;
		}
		std::lock_guard l(g_mutex);
		auto&           r = g_npcs[key];
		r.name = NpcName(a_actor);
		r.enabled = a_enabled;
		if (r.empty()) {
			g_npcs.erase(key);
		}
		SaveUnlocked();
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
		const auto key = FormKey(a_armor);
		if (key.empty()) {
			return;
		}
		std::lock_guard l(g_mutex);
		auto&           r = g_outfits[key];
		r.name = ArmorName(a_armor);
		if (a_lengthPct.has_value()) {
			r.lengthPct = std::clamp(*a_lengthPct, kMinLength, kMaxLength);
			if (r.enabled.has_value() && !*r.enabled) {
				r.enabled = true;
			}
		} else {
			r.lengthPct.reset();
		}
		if (r.empty()) {
			g_outfits.erase(key);
		}
		SaveUnlocked();
	}

	void SetOutfitEnabled(RE::TESObjectARMO* a_armor, std::optional<bool> a_enabled)
	{
		const auto key = FormKey(a_armor);
		if (key.empty()) {
			return;
		}
		std::lock_guard l(g_mutex);
		auto&           r = g_outfits[key];
		r.name = ArmorName(a_armor);
		r.enabled = a_enabled;
		if (r.empty()) {
			g_outfits.erase(key);
		}
		SaveUnlocked();
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
