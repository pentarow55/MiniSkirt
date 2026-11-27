#pragma once

namespace TargetMenu
{
	// kDataLoaded 後に呼び、ホットキー（SE/AE）および調整パワー（VR/パッド向け）の入力を受け付ける
	void Install();

	// 目の前の女性 NPC を対象に個別設定メニューを開く（ホットキー・パワー・デバッグコマンド共通）
	void OpenForLookedAt();

	// 指定した NPC を対象に個別設定メニューを開く
	void OpenForActor(RE::Actor* a_actor);
}
