#pragma once

#include <atomic>

// 開発用の切り替え（DebugCmd の「@flag <番号> <0|1>」で変える。配布用のビルドでは変える手段が無く、すべて 0 のまま）。
// 不具合の切り分けで、処理の一部を一時的に変えるために使う
namespace DevFlags
{
	enum : int
	{
		kNoNan,        // 0 番。1: 切った布の頂点を NaN にせず、1 点に集めて消す
		kNoBand,       // 1 番。1: 帯（裸の体・下着）を付けずに、スカートだけ縮める
		kNoSkin,       // 2 番。1: 帯の肌の材質の付け替え（MatchSkin）をしない
		kNoUnderwear,  // 3 番。1: 下着を付けない（帯は裸の体だけ）
		kNoShorten,    // 4 番。1: 帯は付けるが、スカートは縮めない
		kUnused5,      // 5 番。（D3D11 の多スレッド保護を入れない、に使っていた。保護はもう入れない）
		kNoUpload,     // 6 番。1: 書き換えた頂点・面を GPU に書かない（見た目は変わらない）
		kCount
	};

	inline std::atomic<int> g[kCount]{};

	inline bool On(int a_i) { return g[a_i].load() != 0; }
}
