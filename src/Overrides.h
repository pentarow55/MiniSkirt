#pragma once

#include <optional>
#include <string>

namespace Overrides
{
	struct Rule
	{
		std::string          name;       // 表示用メモ（JSON で確認しやすくするため）
		std::optional<bool>  enabled;    // true=強制有効, false=除外, nullopt=下位に委譲
		std::optional<float> lengthPct;  // 10..100, nullopt=下位に委譲

		bool empty() const { return !enabled.has_value() && !lengthPct.has_value(); }
	};

	enum class Source
	{
		kGlobal,
		kOutfit,
		kNpc,
	};

	struct Resolved
	{
		bool   enabled{ true };
		float  lengthPct{ 30.0f };
		Source enabledSource{ Source::kGlobal };
		Source lengthSource{ Source::kGlobal };
		bool   hasNpcRule{ false };  // この NPC 専用の個別設定がある（共有バッファ分離の目安）
	};

	// 起動時・再読込時に Data/SKSE/Plugins/MiniSkirt_Overrides.json を読む
	void Load();

	// 現在の設定を Data/SKSE/Plugins/MiniSkirt_Overrides.json へ保存する
	bool Save();

	// 優先順位（NPC 個別 > 衣装別 > 全体設定）で最終的な有効／無効とスカート丈（%）を決める
	Resolved Resolve(RE::Actor* a_actor, RE::TESObjectARMO* a_armor, bool a_globalEnabled, bool a_elderlyExcluded, float a_globalLengthPct);

	// 参照用（メニュー表示など）
	std::optional<Rule> GetNpcRule(RE::Actor* a_actor);
	std::optional<Rule> GetOutfitRule(RE::TESObjectARMO* a_armor);

	// NPC 個別設定の更新（即時 Save）
	void SetNpcLength(RE::Actor* a_actor, std::optional<float> a_lengthPct);
	void SetNpcEnabled(RE::Actor* a_actor, std::optional<bool> a_enabled);
	void ClearNpc(RE::Actor* a_actor);

	// 衣装別設定の更新（即時 Save）
	void SetOutfitLength(RE::TESObjectARMO* a_armor, std::optional<float> a_lengthPct);
	void SetOutfitEnabled(RE::TESObjectARMO* a_armor, std::optional<bool> a_enabled);
	void ClearOutfit(RE::TESObjectARMO* a_armor);

	// 永続キー（"Skyrim.esm|013BAB" 形式）
	std::string FormKey(const RE::TESForm* a_form);
}
