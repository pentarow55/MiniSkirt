#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace BodyMorph
{
	struct Entry
	{
		std::string name;   // 小文字化したモーフ名
		float       value{ 0.0f };
	};

	struct Snapshot
	{
		std::vector<Entry> morphs;
		std::uint64_t      hash{ 0 };

		bool empty() const { return morphs.empty(); }
	};

	// kPostPostLoad で SKEE（RaceMenu）の IBodyMorphInterface を取得する（未導入環境では何もしない）
	void Init();

	// SKEE の IBodyMorphInterface が利用可能か
	bool Available();

	// NPC に掛かっている有効なボディモーフ一覧と署名ハッシュを取得する
	Snapshot Query(RE::Actor* a_actor);

	// 署名ハッシュのみ取得する（定期チェック用）
	std::uint64_t Hash(RE::Actor* a_actor);

	// NiAVObject ツリー（または指定 geometry）から "BODYTRI" の NiStringExtraData を探して相対パスを返す
	std::string FindBodyTri(RE::NiAVObject* a_root, RE::BSGeometry* a_geo = nullptr);

	// 指定 .tri ファイル（Meshes\ からの相対パス）の指定 shape に、Snapshot のモーフデルタを加算する。
	// a_pos / a_hit は vertexCount 要素。1 頂点でも動いたら true を返し、a_moved / a_maxMove を埋める。
	bool ApplyToPositions(const std::string& a_triRelPath, const std::string& a_shapeName, const Snapshot& a_morphs,
		std::vector<std::array<float, 3>>& a_pos, std::vector<char>& a_hit, int& a_moved, float& a_maxMove);
}
