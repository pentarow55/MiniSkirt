#pragma once

// ゲームの内部構造（皮の構造体）が、このプラグインの前提と合っているかの確認。
// MakeCutPartition や頂点の書き換えは、NiSkinPartition などのレイアウトが版（SE/AE/VR）で同じことを前提にしている。
// 合っていない版で動かすと落ちる／壊れるので、実物を見て確かめ、合わなければこの機能だけ止める（ゲームは落とさない）
namespace Layout
{
	// プラグイン読み込み時: ゲームの版と、前提にしている構造体の大きさ・オフセットをログに出す
	void LogStartup();
	// 皮の構造体を実物で確かめる。合わなければ false（a_why に理由）。皮が無いなど対象外なら true。
	// 合わないことが 3 回続き、一度も合ったことが無ければ、機能を止める（Disabled が true になる）
	bool Check(RE::NiSkinInstance* a_skin, std::string& a_why);
	bool Disabled();
}
