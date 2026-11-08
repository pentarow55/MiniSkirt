#pragma once

namespace Band
{
	// スカートを縮めた NPC に、裸の体（そのNPCの skin の体 nif）の帯（足首〜上端）を足して、スカート下の脚を補う
	// a_underwear: スカートの中に着ける下着の女性モデルの nif（空なら裸の体だけ）。体を含む nif ならそれだけ、含まなければ裸の体と重ねる
	bool Attach(RE::Actor* a_actor, const std::string& a_underwear = {});
	// 帯を付けた NPC の服の中の肌の shape のうち、帯の範囲（z < 上端 − 1）の頂点（脚）を 1 点に集めて消す（体型が違っても確実に消える）
	void HideOutfitLegs(RE::Actor* a_actor, RE::NiAVObject* a_outfit);
	// 帯の肌の色・テクスチャを NPC に合わせる。服の中の肌の shape の材質を写す（無ければ NPC の体の色と ArmorAddon の肌のテクスチャ）。
	// 材質は NPC ごとに別の写しにしてから付ける（共有の材質を変えると全員変わる）
	void MatchSkin(RE::Actor* a_actor, RE::NiAVObject* a_outfit);
	// 起動時（kDataLoaded）: 女性の体 nif（衣服を着うる人型の女性 NPC の skin）ごとに、帯ファイル（z=0〜a_top）を先に書き出す。
	// あわせて、帯のファイルがゲームのファイルシステムから見えるかをログに出す
	// プラグイン読み込み時（ゲームの初期化より前）: 前回保存した体の一覧と標準の女性の体の帯ファイルを、ディスクから直接読んで書き出す
	// （この時点で書いたファイルは、その起動から Demand で読める見込み）
	void EarlyPrepare(float a_top);
	// a_freezeJiggle: 帯の揺れ骨（末端の骨）を親の骨で動かす（帯は体 nif ごとの共有の複製なので全 NPC 共通になる）
	void Prepare(float a_top, bool a_freezeJiggle);
	// 縮めたスカート（全部の shape をまとめて）を A ライン（お尻で膨らみ、その下は斜めにまっすぐ広がる）にする。Skirt::ALine。
	// 角度・高さは帯の体から測る。袖（腕の骨の重み、または体の腕・手に近い頂点）は動かさない。一度作ったら設定が変わるまで作り直さない
	void ShapeSkirt(RE::Actor* a_actor, const std::vector<RE::BSGeometry*>& a_skirts, float a_waist, float a_hem, float a_ease, float a_angle, bool a_keepLayers = false);
	// 帯を付けた NPC の縮めたスカートを、帯の体から離す（A: 足りない分だけ外へ押し出す。
	// B: スカートの骨の一覧にお尻の揺れ骨があれば、お尻を覆う頂点にその重みを混ぜる）。
	// a_gap: B が使えるときのすき間、a_gapNoBone: 使えないときのすき間
	// a_bodyWeights: お尻の揺れ骨が無い服で、体に近い頂点に体の重みを混ぜる（C）
	void FitSkirt(RE::Actor* a_actor, const std::vector<RE::BSGeometry*>& a_skirts, float a_hem, float a_gap, float a_gapNoBone, bool a_bodyWeights);
	// 服の shape の頂点の座標系のずれ。足す（頂点 + ずれ）と、足元 z=0・頭のてっぺん約 128 の座標になる。
	// 皮付きの shape は骨の基準の位置（skinToBone）と標準の骨格の差から求める。主な骨が無いときは shape 自身の移動
	// （NiAVObject::local.translate。回転・拡大があるときは 0）。
	// 判定・丈・A ライン・押し出しは、この座標で見て、書き戻すときに引く
	std::array<float, 3> FrameOffset(RE::BSGeometry* a_geo);
	// 頂点の写しごとの袖の度合い（0〜1）。腕の骨の重み、または腰より下の布のまとまり（つながった三角形）が片側に寄っていて体の外にある（垂れた袖）なら 1。
	// 失敗したら空
	std::vector<float> SleeveMask(RE::BSGeometry* a_geo, RE::BSGraphics::TriShape* a_buf, float a_waist);
	// スカートの中を覗くと黒く見える「下向きの水平な面」（法線の z が a_nz 未満で、3 頂点とも腰より下の三角形）を、3 頂点を重ねて面積 0 にする。
	// 縮めても面の向きは変わらないので、縮める前に呼ぶ。同じ位置・同じ骨の重みになるので、そのあとの縮め・広げ・押し出しでも重なったまま。戻り値: 対象の三角形の数
	int WeldCeiling(RE::BSGeometry* a_geo, RE::BSGraphics::TriShape* a_buf, float a_waist, float a_nz, const std::vector<float>& a_sleeve);
	// スカートの布を「縮める」のではなく「切る」: z < a_hem の頂点だけでできた三角形を消す（面は触らず、その頂点を同じ区画の縁の頂点の位置・重みに重ねて面積ほぼ 0 にする）。
	// 縮めると下の布が全部腰に溜まり、覗くと黒い布の塊になるため。a_sleeve が 0.5 以上の頂点は切らない。
	// a_dead に重ねた頂点の印を返す。戻り値: 切ったあとの裾の高さ（残った頂点でいちばん低い z。袖は除く）。切る物が無ければ元の裾
	float CutCloth(RE::BSGeometry* a_geo, RE::BSGraphics::TriShape* a_buf, float a_hem, const std::vector<float>& a_sleeve, std::vector<char>& a_dead);
	// 付けた帯がまだ付いているか（切り分け用）
	struct Status
	{
		bool known{ false };     // この NPC に帯を付けた記録がある
		bool rootSame{ false };  // 3D の根が付けたときと同じ
		int  attached{ 0 };      // いまも根に付いている帯の shape の数
		int  total{ 0 };         // 付けた帯の shape の数
	};
	Status Check(RE::Actor* a_actor);
	// 縮める服を着ていなくなった・設定で無効にした・帯の上端を変えたときに、付けた帯を外す（a_why はログ用の理由）
	void Detach(RE::Actor* a_actor, const char* a_why = "縮める服を着ていない");
	// 帯の上端を変える（ゲーム中に MCM で変えたとき。次に付ける帯から新しい上端）
	void SetTop(float a_top);
	// NPC の 3D が外れたとき（次に読み込まれたらまた足せるように）
	void Forget(RE::FormID a_id);
}
