#pragma once

// スカートの中に着ける下着を NPC ごとに選ぶ（一般的な方式。特定の下着 MOD 専用の処理はしない）。
//  1. NPC の所持品と既定の服装の、体スロット（32）の防具のうち、いま着ている服以外で「下着らしい」物
//     （強い順に: ①名前・キーワード・EditorID に下着の言葉 ②ArmorAddon のモデルのパスに下着の言葉 ③形: 脚の無い短い物）
//  2. 無ければ、ロード順の全防具から作った下着らしい物（①か②）の一覧から、NPC の FormID で決まる 1 つ（同じ NPC は毎回同じ）
//  3. Dynamic Armor Variants（DAV）の設定があれば、その差し替え（ArmorAddon）を当てる（条件は HasKeyword / WornHasKeyword だけ読む）
namespace Underwear
{
	struct Choice
	{
		std::string        path;  // 下着の女性モデルの nif（Meshes\ からの相対）。空なら下着なし
		std::string        how;   // どうやって見つけたか（ログ用）
		RE::TESObjectARMO* armo{ nullptr };
		RE::TESObjectARMA* arma{ nullptr };
	};

	// 起動時（kDataLoaded）: ランダムに割り当てる下着の一覧と、DAV の設定を読む
	void Prepare();
	// NPC の下着を選ぶ（NPC ごとに一度決めたら覚えておく）。a_worn: いま体スロットに着ている服、a_top: 帯の上端。
	// ランダムに選ぶときは、NPC の素の体と同じ体型（下着の nif の肌の shape の形で比べる）の下着だけから選ぶ
	const Choice& Find(RE::Actor* a_actor, RE::TESObjectARMO* a_worn, int a_top);
}
