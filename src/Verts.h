#pragma once

// 書き換えた頂点の写し（CPU 側の rawVertexData）の「元」を、頂点の写しごとに覚えておく。
// ゲーム中に設定を変えたとき、元に戻してから作り直すため（スカートを縮める・広げる・帯から離す、服の脚を消す）。
//
// 頂点の写しはアドレスで覚えるが、NPC の 3D が消えるとアドレスが別の物に使い回されることがある。
// そこで、最後に自分が書いた中身のハッシュも覚え、いまの中身と同じときだけ「自分が書き換えた物」とみなす
// （違えば古い記録なので捨てる。違う物に元を書き戻して壊さない）。

#include "DevFlags.h"

#include <d3d11.h>
#include <d3d11_4.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Verts
{
	// GPU への書き込みについて（経緯）:
	// ゲームの描画用の context（immediate context）は同時に 2 つのスレッドから使えない。VR では SKSE の task がいくつもの作業スレッドで動き、
	// ゲームの描画と同時に context を触ってドライバの中で固まった（ドラゴンズリーチ・バナード・メア）。そこで D3D11 の多スレッド保護を入れていたが、
	// それが Bottled Shaders と合わず画面が壊れた。いまは context を使わず、ID3D11Device（多スレッドから使ってよい）で
	// 新しいバッファを作って差し替える（Replace）。多スレッド保護も入れない

	struct Entry
	{
		std::vector<std::uint8_t> orig;     // 書き換える前の頂点（そのまま）
		std::uint64_t             sealed{ 0 };  // 最後に自分が書いた中身のハッシュ
		bool                      shortened{ false };  // スカートを縮めた・広げた
		bool                      fitted{ false };     // 帯の体から離した
		bool                      shaped{ false };     // A ラインにした
		bool                      trim{ false };       // 鎧の裾の飾り（切り線より下にある輪）。全部切って消した。スカートとしては扱わない
		bool                      hidden{ false };     // 服の脚を消した
		bool                      cut{ true };         // 裾を切った（元の裾が丈の目標より上で、切る所が無かったときは false。形を変えない）
		float                     hemNew{ 0.0f };      // 縮めたあとの裾の高さ
		std::vector<float>        sleeve;              // 頂点ごとの袖の度合い（0〜1）。縮め・広がり・押し出しをこの分だけ止める
		std::vector<char>         dead;                // 切り捨てた布の頂点（縁の頂点の位置に重ねてある）。動かさない
	};

	inline std::unordered_map<const void*, Entry> g_map;

	inline std::uint64_t Hash(const std::uint8_t* a_p, std::size_t a_n)
	{
		std::uint64_t h = 0xcbf29ce484222325ull;
		// 長い物は 4 バイトずつ混ぜる（速さのため。書き換えの有無が分かれば十分）
		std::size_t i = 0;
		for (; i + 4 <= a_n; i += 4) {
			std::uint32_t w;
			std::memcpy(&w, a_p + i, 4);
			h ^= w;
			h *= 0x100000001b3ull;
		}
		for (; i < a_n; ++i) {
			h ^= a_p[i];
			h *= 0x100000001b3ull;
		}
		return h;
	}

	inline std::size_t Bytes(RE::BSGraphics::TriShape* a_b, std::uint32_t a_count)
	{
		return static_cast<std::size_t>(a_count) * a_b->vertexDesc.GetSize();
	}

	// 自分が書き換えた物なら記録を返す。記録があっても中身・大きさが違えば古い記録なので捨てて nullptr
	inline Entry* Owned(RE::BSGraphics::TriShape* a_b, std::uint32_t a_count)
	{
		if (!a_b || !a_b->rawVertexData) {
			return nullptr;
		}
		auto it = g_map.find(a_b);
		if (it == g_map.end()) {
			return nullptr;
		}
		const auto n = Bytes(a_b, a_count);
		if (it->second.orig.size() != n || Hash(a_b->rawVertexData, n) != it->second.sealed) {
			g_map.erase(it);
			return nullptr;
		}
		return &it->second;
	}

	// 書き換える前に呼ぶ。自分の記録が無ければ、いまの中身を元として覚える
	inline Entry& Remember(RE::BSGraphics::TriShape* a_b, std::uint32_t a_count)
	{
		if (auto e = Owned(a_b, a_count)) {
			return *e;
		}
		const auto n = Bytes(a_b, a_count);
		auto&      e = g_map[a_b];
		e = Entry{};
		e.orig.assign(a_b->rawVertexData, a_b->rawVertexData + n);
		e.sealed = Hash(a_b->rawVertexData, n);
		return e;
	}

	// 書き換えたあとに呼ぶ（いまの中身を「自分が書いた物」として覚える）
	inline void Seal(RE::BSGraphics::TriShape* a_b, std::uint32_t a_count)
	{
		auto it = g_map.find(a_b);
		if (it != g_map.end()) {
			it->second.sealed = Hash(a_b->rawVertexData, Bytes(a_b, a_count));
		}
	}

	// 書き換えの途中で抜けても（continue など）必ず Seal する
	struct SealGuard
	{
		RE::BSGraphics::TriShape* b;
		std::uint32_t             n;
		~SealGuard() { Seal(b, n); }
	};

	inline ID3D11Device* Device()
	{
		auto r = RE::BSGraphics::Renderer::GetSingleton();
		return r ? reinterpret_cast<ID3D11Device*>(r->GetRuntimeData().forwarder) : nullptr;
	}

	// 差し替えて外した古いバッファ。描画がまだ使っているかもしれないので、しばらく待ってから手放す
	struct Retired
	{
		ID3D11Buffer*                         buf;
		std::chrono::steady_clock::time_point at;
	};
	inline std::mutex           g_retiredMutex;
	inline std::vector<Retired> g_retired;

	inline void Retire(ID3D11Buffer* a_buf, int a_refs)
	{
		std::lock_guard l(g_retiredMutex);
		const auto      now = std::chrono::steady_clock::now();
		for (auto it = g_retired.begin(); it != g_retired.end();) {
			if (now - it->at > std::chrono::seconds(10)) {
				it->buf->Release();
				it = g_retired.erase(it);
			} else {
				++it;
			}
		}
		for (int i = 0; i < a_refs; ++i) {
			g_retired.push_back({ a_buf, now });
		}
	}

	// CPU 側の写し（a_data）から GPU のバッファを新しく作り、a_b の頂点（a_index なら面）のバッファと差し替える。
	// 同じ区画の集まり（a_part）で同じバッファを指している区画もまとめて差し替える（区画ごとに参照を 1 つ持つ）。
	// ゲームの描画用の context（immediate context）は使わない（ID3D11Device は多スレッドから使ってよい）。
	// 以前は context に UpdateSubresource で書いていたが、作業スレッドから context を使うために入れた D3D11 の多スレッド保護が、
	// Bottled Shaders（Community Shaders のフォーク）と合わず、場所を移ると画面全体が灰色になりテクスチャが消えた（1.0.0、SE 1.7.104 で再現）
	inline bool Replace(RE::BSGraphics::TriShape* a_b, RE::NiSkinPartition* a_part, bool a_index, const void* a_data, std::size_t a_bytes)
	{
		if (DevFlags::On(DevFlags::kNoUpload)) {
			return false;  // 開発用: GPU に書かない
		}
		auto  dev = Device();
		auto& slot = a_index ? a_b->indexBuffer : a_b->vertexBuffer;
		auto  old = reinterpret_cast<ID3D11Buffer*>(slot);
		if (!dev || !old || !a_data) {
			return false;
		}
		D3D11_BUFFER_DESC bd{};
		old->GetDesc(&bd);
		if (a_bytes < bd.ByteWidth) {
			logger::warn("GPU のバッファを作り直せない: 写しが {} バイトで、バッファの {} バイトより小さい", a_bytes, bd.ByteWidth);
			return false;
		}
		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = a_data;
		ID3D11Buffer* nb = nullptr;
		const HRESULT hr = dev->CreateBuffer(&bd, &init, &nb);
		if (FAILED(hr) || !nb) {
			logger::warn("GPU のバッファを作れない（HRESULT {:08X}）", static_cast<std::uint32_t>(hr));
			return false;
		}
		int holders = 0;
		auto swap = [&](RE::BSGraphics::TriShape* a_t) {
			auto& s = a_index ? a_t->indexBuffer : a_t->vertexBuffer;
			if (reinterpret_cast<ID3D11Buffer*>(s) != old) {
				return;
			}
			if (holders++) {
				nb->AddRef();
			}
			s = reinterpret_cast<REX::W32::ID3D11Buffer*>(nb);
		};
		if (a_part) {
			for (std::uint32_t i = 0; i < a_part->numPartitions; ++i) {
				if (auto t = a_part->partitions[i].buffData) {
					swap(t);
				}
			}
		}
		swap(a_b);  // a_part に入っていない（または a_part が無い）とき
		if (!holders) {
			nb->Release();
			return false;
		}
		Retire(old, holders);
		return true;
	}

	// CPU 側の頂点を GPU の頂点バッファへ書き戻す（新しいバッファを作って差し替える）
	inline bool Upload(RE::BSGraphics::TriShape* a_b, std::uint32_t a_count, RE::NiSkinPartition* a_part)
	{
		return a_b && Replace(a_b, a_part, false, a_b->rawVertexData, Bytes(a_b, a_count));
	}

	// 自分が書き換えた物なら元に戻して GPU に書き戻し、記録を消す。戻したら true
	inline bool Restore(RE::BSGraphics::TriShape* a_b, std::uint32_t a_count, RE::NiSkinPartition* a_part)
	{
		auto e = Owned(a_b, a_count);
		if (!e) {
			return false;
		}
		std::memcpy(a_b->rawVertexData, e->orig.data(), e->orig.size());
		g_map.erase(a_b);
		Upload(a_b, a_count, a_part);
		return true;
	}

	// shape の全区画の頂点の写しを元に戻す。戻した数
	inline int RestoreGeometry(RE::BSGeometry* a_geo)
	{
		auto skin = a_geo->GetGeometryRuntimeData().skinInstance.get();
		auto part = skin ? skin->skinPartition.get() : nullptr;
		if (!part) {
			return 0;
		}
		int n = 0;
		for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
			n += Restore(part->partitions[i].buffData, part->vertexCount, part) ? 1 : 0;
		}
		return n;
	}

	// ---- 書き換えが残った 3D への備え ----
	// ゲームが NPC の 3D を作り直すとき、書き換えたあとの頂点を写して新しい頂点の写しを作ることがある（頂点バッファのアドレスは変わるのに中身は書き換え済み）。
	// 記録（アドレス）が合わないので元に戻せず、判定も狂う（短いスカートと見なして帯を外し、服の脚は消えたまま＝脚が見えない）。
	// そこで、初めて見た（まだ書き換えていない）頂点を NPC ごと・位置と皮の重みを除いた中身＋面のハッシュごとに覚えておき、
	// 記録の無い頂点のうち、自分が「消した」頂点（位置が NaN＝切った布、または皮の重みが骨 0 番に 1＝服の脚を 1 点に集めた頂点）だけを、覚えておいた元に戻す。
	// 頂点バッファ全体は戻さない（同じ服でも NPC ごとに体型モーフ（OBody など）で位置が違い、全体を戻すと体が服を突き破るため）
	inline std::map<std::pair<std::uint32_t, std::uint64_t>, std::vector<std::uint8_t>> g_pristine;
	// 面（インデックス）を見ない鍵（頂点の数・大きさ・位置と皮の重みを除いた中身）の控え。NPC を問わない。
	// 面のハッシュまで含む鍵（TopoKey）で見つからなかったときの最後の手段
	inline std::map<std::uint64_t, std::vector<std::uint8_t>> g_pristineByVerts;

	inline std::uint64_t VertKey(RE::BSGraphics::TriShape* a_b, std::uint32_t a_n)
	{
		auto       desc = a_b->vertexDesc;
		const auto stride = desc.GetSize();
		std::uint64_t h = 0x84222325cbf29ce4ull ^ (static_cast<std::uint64_t>(a_n) << 20) ^ stride;
		const bool skinned = desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED);
		const auto off = skinned ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING) : 0u;
		for (std::uint32_t v = 0; v < a_n; ++v) {
			const auto* vp = a_b->rawVertexData + static_cast<std::size_t>(v) * stride;
			for (std::uint32_t k = 12; k < stride; ++k) {
				if (skinned && k >= off && k < off + 12) {
					continue;
				}
				h = (h ^ vp[k]) * 0x100000001b3ull;
			}
		}
		return h;
	}

	inline std::uint64_t TopoKey(RE::NiSkinPartition* a_part, RE::BSGraphics::TriShape* a_b, std::uint32_t a_n)
	{
		auto       desc = a_b->vertexDesc;
		const auto stride = desc.GetSize();
		std::uint64_t h = 0xcbf29ce484222325ull ^ (static_cast<std::uint64_t>(a_n) << 20) ^ stride;
		const bool skinned = desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED);
		const auto off = skinned ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING) : 0u;
		for (std::uint32_t v = 0; v < a_n; ++v) {
			const auto* vp = a_b->rawVertexData + static_cast<std::size_t>(v) * stride;
			for (std::uint32_t k = 12; k < stride; ++k) {
				if (skinned && k >= off && k < off + 12) {
					continue;
				}
				h = (h ^ vp[k]) * 0x100000001b3ull;
			}
		}
		for (std::uint32_t i = 0; i < a_part->numPartitions; ++i) {
			auto& p = a_part->partitions[i];
			if (p.buffData && p.buffData->rawIndexData) {
				h ^= Hash(reinterpret_cast<const std::uint8_t*>(p.buffData->rawIndexData), static_cast<std::size_t>(p.triangles) * 6);
				h *= 0x100000001b3ull;
			}
		}
		return h;
	}

	inline bool HasNaN(RE::BSGraphics::TriShape* a_b, std::uint32_t a_n)
	{
		const auto stride = a_b->vertexDesc.GetSize();
		for (std::uint32_t v = 0; v < a_n; ++v) {
			if (std::isnan(reinterpret_cast<const float*>(a_b->rawVertexData + static_cast<std::size_t>(v) * stride)[2])) {
				return true;
			}
		}
		return false;
	}

	// 記録の無い頂点: まだ書き換えていなければ元として覚える。書き換えの痕（NaN の頂点・1 点に集めた頂点）があれば、その頂点だけを覚えた元に戻す。戻した頂点の数
	inline int Refresh(RE::BSGeometry* a_geo, RE::BSGraphics::TriShape* a_b, std::uint32_t a_actor)
	{
		auto skin = a_geo->GetGeometryRuntimeData().skinInstance.get();
		auto part = skin ? skin->skinPartition.get() : nullptr;
		if (!part || !a_b || !a_b->rawVertexData || !part->vertexCount) {
			return 0;
		}
		const auto n = part->vertexCount;
		if (Owned(a_b, n)) {
			return 0;
		}
		const auto bytes = Bytes(a_b, n);
		const auto key = std::make_pair(a_actor, TopoKey(part, a_b, n));
		// 元の探し方: この NPC の元 → 別 NPC の元（面あり）→ 頂点だけの鍵。
		// 同じ服の NPC は頂点バッファを共有することがあり、後から処理される NPC は「初めて見る」ときにはもう別の NPC が切ったあと
		// （＝この NPC の元は記録できない）。なので、元は NPC を問わない控えにも必ず持ち、この NPC の元が無ければそれで戻す
		const std::vector<std::uint8_t>* src = nullptr;
		const char*                      how = "";
		bool                             own = false;
		if (auto it = g_pristine.find(key); it != g_pristine.end()) {
			src = &it->second;
			own = true;
		} else if (auto gt = g_pristine.find(std::make_pair(0u, key.second)); gt != g_pristine.end()) {
			src = &gt->second;
			how = "別 NPC の元（面あり）";
		} else if (auto jt = g_pristineByVerts.find(VertKey(a_b, n)); jt != g_pristineByVerts.end()) {
			src = &jt->second;
			how = "別 NPC の元（頂点のみ）";
		}
		if (!src) {
			if (!HasNaN(a_b, n)) {
				g_pristine[key].assign(a_b->rawVertexData, a_b->rawVertexData + bytes);
				logger::info("    元を覚えた: NPC {:08X}、頂点 {}、面ありの鍵 {:016X}", a_actor, n, key.second);
				g_pristine.try_emplace(std::make_pair(0u, key.second)).first->second.assign(a_b->rawVertexData, a_b->rawVertexData + bytes);
				g_pristineByVerts.try_emplace(VertKey(a_b, n)).first->second.assign(a_b->rawVertexData, a_b->rawVertexData + bytes);
			} else {
				logger::info("    元が無い: 切られた頂点（NaN）を渡されたが、覚えた元が見つからない（頂点 {}、NPC {:08X}、面ありの鍵 {:016X}）", n, a_actor, key.second);
			}
			return 0;
		}
		const std::vector<std::uint8_t>& orgv = *src;
		if (orgv.size() != bytes) {
			return 0;
		}
		auto       desc = a_b->vertexDesc;
		const auto stride = desc.GetSize();
		const bool skinned = desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED);
		const auto off = skinned ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING) : 0u;
		int        restored = 0;
		for (std::uint32_t v = 0; v < n; ++v) {
			auto*       cur = a_b->rawVertexData + static_cast<std::size_t>(v) * stride;
			const auto* org = orgv.data() + static_cast<std::size_t>(v) * stride;
			bool        nan = std::isnan(reinterpret_cast<const float*>(cur)[2]);
			bool        collapsed = false;
			if (skinned && off + 12 <= stride) {
				std::uint16_t w[4];
				std::memcpy(w, cur + off, 8);
				collapsed = w[0] == 0x3C00 && w[1] == 0 && w[2] == 0 && w[3] == 0 && cur[off + 8] == 0 && std::memcmp(cur + off, org + off, 12) != 0;
			}
			if (nan) {
				std::memcpy(cur, org, 12);  // 位置だけ
				++restored;
			} else if (collapsed) {
				std::memcpy(cur, org, 12);
				std::memcpy(cur + off, org + off, 12);
				++restored;
			}
			// 皮の重みは体型モーフで変わらないので、書き換えた分（お尻の揺れ骨の重みを混ぜる・重みの写し替え）を常に元に戻す。
			// 戻さないと、3D が作り直されるたびに処理し直す中で重みを混ぜ足し続け、お尻の揺れ骨に引かれてスカートが引きつる
			if (!nan && !collapsed && skinned && off + 12 <= stride && std::memcmp(cur + off, org + off, 12) != 0) {
				std::memcpy(cur + off, org + off, 12);
				++restored;
			}
		}
		if (restored) {
			Upload(a_b, n, part);
			if (!own) {
				logger::info("    切り済みの頂点を {} で戻した（NPC {:08X}、頂点 {} 個）", how, a_actor, restored);
			}
		} else if (!own) {
			// 別 NPC の元と比べて書き換えの痕が無い＝この NPC の元がそのまま見えている。この NPC の元として覚える（体型モーフの位置も含めて）
			g_pristine[key].assign(a_b->rawVertexData, a_b->rawVertexData + bytes);
			logger::info("    元を覚えた: NPC {:08X}、頂点 {}、面ありの鍵 {:016X}（別 NPC の元と同じ・痕なし）", a_actor, n, key.second);
		}
		return restored;
	}
}
