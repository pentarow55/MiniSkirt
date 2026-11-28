#pragma once

#include <functional>
#include <optional>
#include <string>

namespace Overrides
{
	struct Rule
	{
		std::string          name;            // 表示用メモ（JSON で確認しやすくするため）
		std::optional<bool>  enabled;         // true=強制有効, false=除外, nullopt=下位に委譲
		std::optional<float> lengthPct;       // 10..100
		std::optional<int>   hemStyle;        // 0=ラフ, 1=水平
		std::optional<float> hipEase;         // 0..8
		std::optional<float> flareAngle;      // 0..30
		std::optional<bool>  underwear;       // true=下着あり, false=なし
		std::optional<float> skirtGap;        // 0..8
		std::optional<float> skirtGapNoBone;  // 0..10

		bool empty() const
		{
			return !enabled.has_value() && !lengthPct.has_value() && !hemStyle.has_value() && !hipEase.has_value() &&
			       !flareAngle.has_value() && !underwear.has_value() && !skirtGap.has_value() && !skirtGapNoBone.has_value();
		}
	};

	enum class Source
	{
		kGlobal,
		kOutfit,
		kNpc,
	};

	struct GlobalDefaults
	{
		bool  enabled{ true };
		bool  elderlyExcluded{ false };
		float lengthPct{ 30.0f };
		int   hemStyle{ 1 };
		float hipEase{ 1.0f };
		float flareAngle{ 5.0f };
		bool  underwear{ true };
		float skirtGap{ 1.0f };
		float skirtGapNoBone{ 1.0f };
	};

	struct Resolved
	{
		bool   enabled{ true };
		float  lengthPct{ 30.0f };
		int    hemStyle{ 1 };
		float  hipEase{ 1.0f };
		float  flareAngle{ 5.0f };
		bool   underwear{ true };
		float  skirtGap{ 1.0f };
		float  skirtGapNoBone{ 1.0f };

		Source enabledSource{ Source::kGlobal };
		Source lengthSource{ Source::kGlobal };
		Source hemStyleSource{ Source::kGlobal };
		Source hipEaseSource{ Source::kGlobal };
		Source flareAngleSource{ Source::kGlobal };
		Source underwearSource{ Source::kGlobal };
		Source skirtGapSource{ Source::kGlobal };
		Source skirtGapNoBoneSource{ Source::kGlobal };

		bool hasNpcRule{ false };  // この NPC 専用の個別設定がある（共有バッファ分離の目安）
	};

	// 起動時・再読込時に Data/SKSE/Plugins/MiniSkirt_Overrides.json を読む
	void Load();

	// 現在の設定を Data/SKSE/Plugins/MiniSkirt_Overrides.json へ保存する
	bool Save();

	// 優先順位（NPC 個別 > 衣装別 > 全体設定）で最終的な各設定値を決める
	Resolved Resolve(RE::Actor* a_actor, RE::TESObjectARMO* a_armor, const GlobalDefaults& a_global);

	// 参照用（メニュー表示など）
	std::optional<Rule> GetNpcRule(RE::Actor* a_actor);
	std::optional<Rule> GetOutfitRule(RE::TESObjectARMO* a_armor);

	// 汎用更新（範囲正規化・空エントリ削除・即時 Save を行う）
	void UpdateNpc(RE::Actor* a_actor, const std::function<void(Rule&)>& a_fn);
	void UpdateOutfit(RE::TESObjectARMO* a_armor, const std::function<void(Rule&)>& a_fn);

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
