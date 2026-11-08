#include "PCH.h"

#include "Band.h"
#include "Layout.h"
#include "Skirt.h"
#include "Verts.h"

#include <d3d11.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Band
{
	namespace
	{
		// NPC ごとに付けた帯（3D の根・範囲・付けた shape）。根はアドレスを比べるだけで、中身は触らない（作り直されて消えているかもしれない）
		struct State
		{
			const void*                                root{ nullptr };
			int                                        zHi{ -1 };
			std::vector<RE::NiPointer<RE::NiAVObject>> shapes;
			RE::TESObjectARMA*                         arma{ nullptr };  // 帯の体の ArmorAddon（肌のテクスチャの予備に使う）
			bool                                       skinDone{ false };  // 肌の色・テクスチャを NPC に合わせた
			std::string                                src;  // 下着の nif（空なら裸の体だけ）。変わったら付け直す
		};
		std::unordered_map<RE::FormID, State> g_state;

		// 皮付きの shape を集める
		void CollectSkinned(RE::NiAVObject* a_obj, std::vector<RE::BSGeometry*>& a_out)
		{
			if (!a_obj) {
				return;
			}
			if (auto geo = a_obj->AsGeometry()) {
				if (geo->GetGeometryRuntimeData().skinInstance) {
					a_out.push_back(geo);
				}
			}
			if (auto node = a_obj->AsNode()) {
				for (auto& c : node->GetChildren()) {
					CollectSkinned(c.get(), a_out);
				}
			}
		}

		// 腕の骨（上腕・前腕・手・指）の印（皮の骨の番号ごと）。帯の上端を腰まで上げると、体 nif の手首（前腕）が帯の高さに入るので、
		// 帯からも服の脚を消す所からも外す。見分けは Skyrim の人型の骨格に共通の骨の名前（服・体の名前は見ない）
		std::vector<char> ArmBones(RE::NiSkinInstance* a_skin)
		{
			std::vector<char> out;
			if (!a_skin || !a_skin->skinData || !a_skin->bones) {
				return out;
			}
			const auto n = a_skin->skinData->GetBoneCount();
			out.assign(n, 0);
			for (std::uint32_t i = 0; i < n; ++i) {
				auto b = a_skin->bones[i];
				if (!b || !b->name.c_str()) {
					continue;
				}
				std::string s = b->name.c_str();
				for (auto& c : s) {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				out[i] = s.find("arm") != std::string::npos || s.find("hand") != std::string::npos || s.find("finger") != std::string::npos;
			}
			return out;
		}

		// 頂点（区画の頂点の写しの中の 1 つ。骨の番号は区画の中の番号）が腕の骨に重みを持つか
		bool OnArm(const std::uint8_t* a_vp, const RE::BSGraphics::VertexDesc& a_desc, const RE::NiSkinPartition::Partition& a_p, const std::vector<char>& a_arm)
		{
			// 頂点の骨番号は、区画ごとの骨の一覧（Partition::bones）の番号ではなく、皮（skinInstance）の骨の一覧の番号そのもの
			// （複数区画で骨の一覧が飛び飛びの服で、区画の一覧を通すと範囲外や別の骨になるのを、手元の nif と実機のログで確かめた）
			if (a_arm.empty() || !a_desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED)) {
				return false;
			}
			const auto off = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
			for (int k = 0; k < 4; ++k) {
				std::uint16_t h;
				std::memcpy(&h, a_vp + off + k * 2, 2);
				if ((h & 0x7FFF) == 0) {
					continue;  // 重み 0
				}
				const auto li = a_vp[off + 8 + k];
				if (li < a_arm.size() && a_arm[li]) {
					return true;
				}
			}
			return false;
		}

		// 区画の頂点バッファのアドレス（共有しているか調べる用）
		const void* FirstBuf(RE::BSGeometry* a_geo)
		{
			auto skin = a_geo->GetGeometryRuntimeData().skinInstance.get();
			if (!skin || !skin->skinPartition || !skin->skinPartition->numPartitions) {
				return nullptr;
			}
			return skin->skinPartition->partitions[0].buffData;
		}
	}

	void Forget(RE::FormID a_id)
	{
		g_state.erase(a_id);
	}

	namespace
	{
		// 付けた帯の shape のうち、いまも NPC の 3D の根に付いている数
		int CountAttached(const State& a_s, RE::NiNode* a_root)
		{
			int n = 0;
			for (auto& s : a_s.shapes) {
				if (s && a_root && s->parent == a_root) {
					++n;
				}
			}
			return n;
		}
	}

	void Detach(RE::Actor* a_actor, const char* a_why)
	{
		auto it = g_state.find(a_actor->GetFormID());
		if (it == g_state.end()) {
			return;
		}
		auto root = a_actor->Get3D();
		auto rootNode = root ? root->AsNode() : nullptr;
		int  n = 0;
		if (rootNode && static_cast<const void*>(rootNode) == it->second.root) {
			for (auto& s : it->second.shapes) {
				if (s && s->parent == rootNode) {
					rootNode->DetachChild(s.get());
					++n;
				}
			}
		}
		logger::info("  帯: {}ので、付けた帯を外した（{} 個）", a_why, n);
		g_state.erase(it);
	}

	Status Check(RE::Actor* a_actor)
	{
		Status st;
		auto   it = g_state.find(a_actor->GetFormID());
		if (it == g_state.end()) {
			return st;
		}
		st.known = true;
		auto root = a_actor->Get3D();
		auto rootNode = root ? root->AsNode() : nullptr;
		st.rootSame = rootNode && static_cast<const void*>(rootNode) == it->second.root;
		st.total = static_cast<int>(it->second.shapes.size());
		st.attached = st.rootSame ? CountAttached(it->second, rootNode) : 0;
		return st;
	}

	namespace
	{
		struct Cut
		{
			RE::NiPointer<RE::NiNode> root;  // 切り詰め済みの体（作り直さないよう持ち続ける）
		};
		std::map<std::string, Cut> g_cuts;  // 体 nif のパス＋範囲 → 切り詰め済みの体

		ID3D11DeviceContext* Context()
		{
			return Verts::GameContext();
		}

		// 腰から膝の帯の外の三角形を消す（面の番号を 0,0,0 にして何も描かせない）。頂点は触らない。
		// 戻り値: 消せた shape の数。区画ごとに面のバッファが別でなければ何もしない（ログに構造を出す）
		int CutShapes(RE::NiNode* a_root, float a_zLo, float a_zHi)
		{
			std::vector<RE::BSGeometry*> shapes;
			CollectSkinned(a_root, shapes);
			auto ctx = Context();
			int  cut = 0;
			for (auto geo : shapes) {
				auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
				auto part = skin->skinPartition.get();
				if (!part) {
					continue;
				}
				// 構造をログに出す
				std::set<void*> ibs;
				for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
					auto& p = part->partitions[i];
					auto  b = p.buffData;
					D3D11_BUFFER_DESC ibd{};
					if (b && b->indexBuffer) {
						reinterpret_cast<ID3D11Buffer*>(b->indexBuffer)->GetDesc(&ibd);
					}
					logger::info("    区画{}: 三角 {} 頂点 {} ib={} ibByteWidth={} ibUsage={} rawIndex={} rawVertex={}", i, p.triangles, p.vertices,
						b ? static_cast<const void*>(b->indexBuffer) : nullptr, ibd.ByteWidth, static_cast<int>(ibd.Usage),
						b ? static_cast<const void*>(b->rawIndexData) : nullptr, b ? static_cast<const void*>(b->rawVertexData) : nullptr);
					if (b) {
						ibs.insert(b->indexBuffer);
					}
				}
				if (ibs.size() != part->numPartitions) {
					logger::info("  帯: [{}] 面のバッファが区画ごとに別ではないので切らない（構造を確認する）", geo->name.c_str());
					continue;
				}
				bool ok = true;
				for (std::uint32_t i = 0; i < part->numPartitions && ok; ++i) {
					auto& p = part->partitions[i];
					auto  b = p.buffData;
					if (!b || !b->rawIndexData || !b->rawVertexData || !b->indexBuffer || !ctx) {
						ok = false;
						break;
					}
					D3D11_BUFFER_DESC ibd{};
					reinterpret_cast<ID3D11Buffer*>(b->indexBuffer)->GetDesc(&ibd);
					if (ibd.ByteWidth != static_cast<UINT>(p.triangles) * 6 || ibd.Usage != D3D11_USAGE_DEFAULT) {
						ok = false;
						break;
					}
					const auto stride = b->vertexDesc.GetSize();
					const auto arm = ArmBones(skin);
					const float oz = FrameOffset(geo)[2];  // 体 nif の shape が持つ移動（標準の体は +120）。足すと足元 z=0 の座標
					int        removed = 0;
					for (std::uint32_t t = 0; t < p.triangles; ++t) {
						auto  idx = b->rawIndexData + t * 3;
						float zs = 0.0f;
						bool  onArm = false;
						for (int k = 0; k < 3; ++k) {
							const auto* vp = b->rawVertexData + static_cast<std::size_t>(idx[k]) * stride;
							zs += reinterpret_cast<const float*>(vp)[2];
							onArm = onArm || OnArm(vp, b->vertexDesc, p, arm);
						}
						zs = zs / 3.0f + oz;
						if (zs < a_zLo || zs > a_zHi || onArm) {
							idx[0] = idx[1] = idx[2] = 0;
							++removed;
						}
					}
					reinterpret_cast<ID3D11DeviceContext*>(ctx)->UpdateSubresource(reinterpret_cast<ID3D11Buffer*>(b->indexBuffer), 0, nullptr, b->rawIndexData, 0, 0);
					logger::info("  帯: [{}] 区画{} 三角 {} 個のうち {} 個を消した", geo->name.c_str(), i, p.triangles, removed);
				}
				if (ok) {
					++cut;
				}
			}
			return cut;
		}

		struct Reader : RE::BSResourceNiBinaryStream
		{
			using RE::BSResourceNiBinaryStream::BSResourceNiBinaryStream;
			using RE::NiBinaryStream::binary_read;  // protected を公開する
		};

		// ゲームのファイルシステム（BSResource）から、このパス（Meshes\ からの相対）が開けるか
		bool StreamOpens(const std::string& a_rel)
		{
			Reader in(std::string("Meshes\\") + a_rel);
			return in.good();
		}

		std::set<std::string> g_failed;     // 読めなかった帯のファイル（同じ起動中に何度も試さない）
		std::set<std::string> g_prepared;   // 起動時（kDataLoaded）に用意した（書き出した／前からあった）帯のファイル
		std::set<std::string> g_early;      // プラグイン読み込み時（ゲームの初期化より前）に用意した帯のファイル
		std::set<std::string> g_bodyPaths;  // 体 nif のパスの一覧（次の起動のプラグイン読み込み時に書き出すため、ファイルに保存する）
		std::map<std::string, std::uint64_t> g_contentHash;  // 体 nif のパス（小文字） → 中身のハッシュ（同じ起動中は読み直さない）

		std::string Lower(std::string a_s)
		{
			for (auto& c : a_s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a_s;
		}

		// FNV-1a 64 ビット（ビルドや実行環境で変わらないハッシュ）
		std::uint64_t Fnv(const void* a_data, std::size_t a_size)
		{
			std::uint64_t h = 0xcbf29ce484222325ull;
			const auto*   p = static_cast<const unsigned char*>(a_data);
			for (std::size_t i = 0; i < a_size; ++i) {
				h ^= p[i];
				h *= 0x100000001b3ull;
			}
			return h;
		}

		// 帯のファイル名: パスのハッシュ＋中身のハッシュ（BodySlide で体を作り直すと別名になる）＋範囲
		// （範囲ごとに別のファイル名にする。同じファイル名 ＝ 同じキャッシュ項目なので、範囲を変えると二重に切れる）
		std::string BandRel(const std::string& a_path, std::uint64_t a_content, int a_zLo, int a_zHi)
		{
			const auto low = Lower(a_path);
			return std::format("MiniSkirt\\band_{:016x}_{:016x}_{}_{}.nif", Fnv(low.data(), low.size()), a_content, a_zLo, a_zHi);
		}

		// 体の nif をゲームのファイルシステム（BSResource。BSA の中も）から読む
		bool ReadStream(const std::string& a_path, std::vector<char>& a_out)
		{
			Reader in(std::string("Meshes\\") + a_path);
			if (!in.good()) {
				return false;
			}
			std::vector<char> chunk(65536);
			for (;;) {
				std::uint32_t one = 1;
				const auto    n = in.binary_read(chunk.data(), static_cast<std::uint32_t>(chunk.size()), &one);
				if (n == 0) {
					break;
				}
				a_out.insert(a_out.end(), chunk.begin(), chunk.begin() + n);
				if (n < chunk.size()) {
					break;
				}
			}
			return !a_out.empty();
		}

		// 帯のファイルを Data\Meshes\<a_rel> に書き出す（同じ大きさの物が既にあれば書かない）
		bool WriteBand(const std::string& a_rel, const std::vector<char>& a_bytes, bool& a_existed)
		{
			const std::filesystem::path disk = std::filesystem::path("Data\\Meshes") / a_rel;
			std::error_code             ec;
			std::filesystem::create_directories(disk.parent_path(), ec);
			a_existed = std::filesystem::exists(disk, ec) && std::filesystem::file_size(disk, ec) == a_bytes.size();
			if (a_existed) {
				return true;
			}
			std::ofstream out(disk, std::ios::binary | std::ios::trunc);
			out.write(a_bytes.data(), static_cast<std::streamsize>(a_bytes.size()));
			if (!out) {
				logger::info("  帯: 書き出しに失敗: {}", disk.string());
				return false;
			}
			return true;
		}

		// 体の nif（Meshes\ からの相対）の帯ファイルを用意する。a_rel に帯ファイルの名前を返す
		bool EnsureFile(const std::string& a_path, int a_zLo, int a_zHi, std::string& a_rel, bool& a_existed)
		{
			const auto low = Lower(a_path);
			if (auto it = g_contentHash.find(low); it != g_contentHash.end()) {
				a_rel = BandRel(a_path, it->second, a_zLo, a_zHi);
				std::error_code ec;
				if (std::filesystem::exists(std::filesystem::path("Data\\Meshes") / a_rel, ec)) {
					a_existed = true;
					return true;
				}
			}
			std::vector<char> bytes;
			if (!ReadStream(a_path, bytes)) {
				logger::info("  帯: 元の nif を読めなかった: {}", a_path);
				return false;
			}
			const auto h = Fnv(bytes.data(), bytes.size());
			g_contentHash[low] = h;
			a_rel = BandRel(a_path, h, a_zLo, a_zHi);
			return WriteBand(a_rel, bytes, a_existed);
		}

		const char* kBodyList = "Data\\SKSE\\Plugins\\MiniSkirt_bodies.txt";

		void SaveBodyList()
		{
			std::ofstream out(kBodyList, std::ios::trunc);
			out << "; MiniSkirt が自動で作る一覧（帯に使う女性の体 nif のパス）。次の起動の最初に、ここにある体の帯ファイルを書き出す\n";
			for (auto& p : g_bodyPaths) {
				out << p << "\n";
			}
		}

		std::string Origin(const std::string& a_rel)
		{
			return g_early.contains(a_rel)    ? "プラグイン読み込み時に用意した物" :
			       g_prepared.contains(a_rel) ? "起動時（kDataLoaded）に用意した物" :
                                                "いま用意した物";
		}

		RE::NiNode* LoadCut(const char* a_path, int a_zLo, int a_zHi)
		{
			// 別名のパスは読めなかった。体の nif の中身を、別名のファイルとして Data\Meshes に書き出し、それを読む
			// （別のファイル名 ＝ モデルキャッシュの別の項目 ＝ ほかの NPC の体と共有しない）
			std::string rel;
			bool        existed = false;
			if (!EnsureFile(a_path, a_zLo, a_zHi, rel, existed)) {
				return nullptr;
			}
			if (g_bodyPaths.insert(a_path).second) {
				SaveBodyList();  // 起動時の一覧に無かった体（次の起動から最初に書き出す）
			}
			if (auto it = g_cuts.find(rel); it != g_cuts.end()) {
				return it->second.root.get();
			}
			if (g_failed.contains(rel)) {
				logger::info("  帯: [{}] はこの起動中に読めなかった（再起動後に読める見込み）", rel);
				return nullptr;
			}
			// 普通の読み込み（ほかの NPC が使っている共有の体）と、別名で読んだ体を比べて、別の物かを確かめる
			RE::NiPointer<RE::NiNode>         normal;
			RE::BSModelDB::DBTraits::ArgsType args;
			const auto                        ecn = RE::BSModelDB::Demand(a_path, normal, args);
			std::vector<RE::BSGeometry*>      normalShapes;
			CollectSkinned(normal.get(), normalShapes);
			logger::info("  帯: 普通の読み込み [{}] ErrorCode={} 読めた={} 皮付き shape {} 個", a_path, static_cast<int>(ecn), normal ? "はい" : "いいえ",
				normalShapes.size());

			logger::info("  帯: 別名のファイル {}: {}（{}）", existed ? "はある" : "をいま書き出した", rel, Origin(rel));
			// ゲームのファイルシステムから見えるか（見えなければ Demand も読めないはず）
			const bool opens = StreamOpens(rel);
			logger::info("  帯: [Meshes\\{}] をストリームで開けたか: {}", rel, opens ? "はい" : "いいえ");

			RE::NiPointer<RE::NiNode> copy;
			auto                      ecd = RE::BSModelDB::Demand(rel.c_str(), copy, args);
			logger::info("  帯: Demand [{}] ErrorCode={} 読めた={}", rel, static_cast<int>(ecd), copy ? "はい" : "いいえ");
			if (ecd != RE::BSResource::ErrorCode::kNone || !copy) {
				// 書き方の違いの備え（Meshes\ を付ける）
				const std::string withMeshes = std::string("Meshes\\") + rel;
				copy.reset();
				ecd = RE::BSModelDB::Demand(withMeshes.c_str(), copy, args);
				logger::info("  帯: Demand [{}] ErrorCode={} 読めた={}", withMeshes, static_cast<int>(ecd), copy ? "はい" : "いいえ");
			}
			if (ecd != RE::BSResource::ErrorCode::kNone || !copy) {
				logger::info("  帯: 書き出したファイル [{}] を読めなかった（{}。{}）", rel, Origin(rel),
					g_early.contains(rel) ? "ゲームの初期化より前からあったのに読めない" : "この起動中に作ったファイルなら、次の起動から読める見込み");
				g_failed.insert(rel);
				return nullptr;
			}
			std::vector<RE::BSGeometry*> copyShapes;
			CollectSkinned(copy.get(), copyShapes);
			bool distinct = !copyShapes.empty();
			for (auto c : copyShapes) {
				for (auto n : normalShapes) {
					if (FirstBuf(c) == FirstBuf(n)) {
						distinct = false;
					}
				}
			}
			logger::info("  帯: [{}] で読めた。普通の読み込みとは{}", rel, distinct ? "別の物" : "同じ物（共有）");
			if (distinct && CutShapes(copy.get(), static_cast<float>(a_zLo), static_cast<float>(a_zHi)) > 0) {
				g_cuts[rel].root = copy;
				return copy.get();
			}
			g_failed.insert(rel);  // 共有だった／切れなかった物を二度切らない
			return nullptr;
		}

		// ---- ファイル無しの帯 ----
		// 元の体 nif を普通に Demand し（いつでも読める）、複製の NiSkinInstance の skinPartition を自前の物に差し替える。
		// 自前の NiSkinPartition: 元の物のバイト列の写し（vtable も同じ）。区画の配列と、区画ごとの TriShape（buffData）を自前で持ち、
		// 面（rawIndexData と GPU の面のバッファ）だけ自前で作って帯の外の三角形を 0,0,0 にする。頂点（rawVertexData・頂点バッファ）と
		// 骨・重みなどの配列は元と共有する（触らない）。
		// 注意（二重解放を防ぐ）: 自前の物のデストラクタが走ると、元と共有している配列を解放してしまう。そこで
		//  - 自前の NiSkinPartition は体 nif ごとに 1 つだけ作って NPC 間で共有し、参照を 1 つ余分に持ったまま決して手放さない（参照数が 0 にならない）
		//  - 元の体（Demand の根）と元の NiSkinPartition も参照を持ち続ける（共有している配列を元の側が解放しないように）
		// ゲームの終了時まで持ち続ける（少しのメモリと引き換えに安全にする）
		struct Fileless
		{
			RE::NiPointer<RE::NiNode>                   root;   // 元の体（Demand の根。複製の元）
			std::vector<RE::NiPointer<RE::NiSkinPartition>> origs;  // 元の skinPartition（shape の順）
			std::vector<RE::NiPointer<RE::NiSkinPartition>> mine;   // 自前の skinPartition（同じ順）
			std::vector<const void*>                    skins;  // 元の NiSkinInstance（複製がこれを共有していたら差し替えない）
			std::vector<char>                           empty;  // 範囲の中に何も残らない shape（付けない）
			bool                                        hasSkin{ false };  // 肌（体）の shape がある
		};
		std::map<std::string, Fileless> g_fileless;      // 体 nif のパス（小文字）＋範囲 → ファイル無しの帯
		std::map<std::string, std::string> g_filelessNg;  // 作れなかった物 → 理由（同じ起動中に何度も試さない）

		ID3D11Device* Device()
		{
			auto r = RE::BSGraphics::Renderer::GetSingleton();
			return r ? reinterpret_cast<ID3D11Device*>(r->GetRuntimeData().forwarder) : nullptr;
		}

		// 元の skinPartition から、帯の外の三角形を消した自前の skinPartition を作る。失敗したら nullptr と理由
		RE::NiSkinPartition* MakeCutPartition(RE::NiSkinPartition* a_orig, const std::vector<char>& a_arm, float a_zLo, float a_zHi, std::string& a_why,
			int& a_removed, int& a_total, int& a_armCut, float a_zOff)
		{
			auto rtti = a_orig->GetRTTI();
			if (!rtti || !rtti->GetName() || std::string_view(rtti->GetName()) != "NiSkinPartition") {
				a_why = "skinPartition の型が NiSkinPartition でない";
				return nullptr;
			}
			auto dev = Device();
			if (!dev) {
				a_why = "D3D11 のデバイスが無い";
				return nullptr;
			}
			const std::uint32_t np = a_orig->numPartitions;
			if (!np || a_orig->partitions.size() != np) {
				a_why = std::format("区画の数が合わない（{} と {}）", np, a_orig->partitions.size());
				return nullptr;
			}
			// 区画ごとに面のバッファが別で、大きさが三角形の数どおりであること（今の切り方と同じ前提）
			std::set<const void*> bufs;
			for (std::uint32_t i = 0; i < np; ++i) {
				auto& p = a_orig->partitions[i];
				auto  b = p.buffData;
				if (!b || !b->rawIndexData || !b->rawVertexData || !b->indexBuffer || !b->vertexBuffer) {
					a_why = std::format("区画{} の面・頂点のデータが無い", i);
					return nullptr;
				}
				D3D11_BUFFER_DESC ibd{};
				reinterpret_cast<ID3D11Buffer*>(b->indexBuffer)->GetDesc(&ibd);
				if (ibd.ByteWidth != static_cast<UINT>(p.triangles) * 6) {
					a_why = std::format("区画{} の面のバッファの大きさが三角形の数と合わない（{} バイト、三角 {}）", i, ibd.ByteWidth, p.triangles);
					return nullptr;
				}
				bufs.insert(b);
			}
			if (bufs.size() != np) {
				a_why = "区画どうしで buffData を共有している";
				return nullptr;
			}

			// 面を作る（区画ごと）。途中で失敗したら作った物を解放して戻る
			std::vector<std::uint16_t*> idxs(np, nullptr);
			std::vector<ID3D11Buffer*>  ibs(np, nullptr);
			auto                        undo = [&]() {
				for (auto p : idxs) {
					if (p) {
						RE::free(p);
					}
				}
				for (auto b : ibs) {
					if (b) {
						b->Release();
					}
				}
			};
			a_removed = a_total = a_armCut = 0;
			for (std::uint32_t i = 0; i < np; ++i) {
				auto&      p = a_orig->partitions[i];
				auto       b = p.buffData;
				const auto stride = b->vertexDesc.GetSize();
				const auto bytes = static_cast<std::size_t>(p.triangles) * 6;
				idxs[i] = static_cast<std::uint16_t*>(RE::malloc(bytes));
				if (!idxs[i]) {
					undo();
					a_why = "メモリが取れない";
					return nullptr;
				}
				std::memcpy(idxs[i], b->rawIndexData, bytes);
				for (std::uint32_t t = 0; t < p.triangles; ++t) {
					auto  idx = idxs[i] + t * 3;
					float zs = 0.0f;
					bool  onArm = false;
					for (int k = 0; k < 3; ++k) {
						const auto* vp = b->rawVertexData + static_cast<std::size_t>(idx[k]) * stride;
						zs += reinterpret_cast<const float*>(vp)[2];
						onArm = onArm || OnArm(vp, b->vertexDesc, p, a_arm);
					}
					zs = zs / 3.0f + a_zOff;  // 体 nif の shape の移動を足した座標で範囲を見る
					const bool outside = zs < a_zLo || zs > a_zHi;
					if (outside || onArm) {
						idx[0] = idx[1] = idx[2] = 0;
						++a_removed;
						a_armCut += (!outside && onArm) ? 1 : 0;
					}
				}
				a_total += p.triangles;
				D3D11_BUFFER_DESC desc{};
				reinterpret_cast<ID3D11Buffer*>(b->indexBuffer)->GetDesc(&desc);  // 元と同じ作り（大きさ・用途）
				D3D11_SUBRESOURCE_DATA init{};
				init.pSysMem = idxs[i];
				const HRESULT hr = dev->CreateBuffer(&desc, &init, &ibs[i]);
				if (FAILED(hr) || !ibs[i]) {
					ibs[i] = nullptr;
					undo();
					a_why = std::format("面のバッファを作れない（HRESULT {:08X}）", static_cast<std::uint32_t>(hr));
					return nullptr;
				}
			}

			// 区画の配列（SimpleArray: 先頭に要素数、その後に区画）と TriShape を作る
			auto head = static_cast<std::size_t*>(RE::malloc(sizeof(std::size_t) + sizeof(RE::NiSkinPartition::Partition) * np));
			auto obj = RE::malloc(sizeof(RE::NiSkinPartition));
			std::vector<RE::BSGraphics::TriShape*> tris(np, nullptr);
			for (auto& t : tris) {
				t = static_cast<RE::BSGraphics::TriShape*>(RE::malloc(sizeof(RE::BSGraphics::TriShape)));
			}
			if (!head || !obj || std::find(tris.begin(), tris.end(), nullptr) != tris.end()) {
				undo();
				a_why = "メモリが取れない";
				return nullptr;  // 取れた分はわずかなので捨てる（解放しない）
			}
			*head = np;
			auto parts = reinterpret_cast<RE::NiSkinPartition::Partition*>(head + 1);
			for (std::uint32_t i = 0; i < np; ++i) {
				const auto& src = a_orig->partitions[i];
				std::memcpy(&parts[i], &src, sizeof(RE::NiSkinPartition::Partition));  // 骨・重みなどの配列は元と共有
				std::memcpy(tris[i], src.buffData, sizeof(RE::BSGraphics::TriShape));   // 頂点は元と共有
				tris[i]->refCount = 1;
				tris[i]->indexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(ibs[i]);
				tris[i]->rawIndexData = idxs[i];
				reinterpret_cast<ID3D11Buffer*>(tris[i]->vertexBuffer)->AddRef();  // 共有の頂点バッファ。参照を 1 つ持つ（手放さない）
				parts[i].buffData = tris[i];
			}
			// 本体: 元のバイト列の写し（vtable・区画の数・頂点の数など）。区画の配列だけ自前の物にし、参照数は 0 から始める
			std::memcpy(obj, a_orig, sizeof(RE::NiSkinPartition));
			auto mine = static_cast<RE::NiSkinPartition*>(obj);
			*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(obj) + 0x08) = 0;  // NiRefObject::_refCount
			*reinterpret_cast<RE::NiSkinPartition::Partition**>(&mine->partitions) = parts;      // SimpleArray の中身（先頭の要素数の次）
			static_assert(sizeof(RE::SimpleArray<RE::NiSkinPartition::Partition>) == sizeof(void*));
			mine->IncRefCount();  // 余分の参照（決して手放さない。デストラクタが走らないように）
			return mine;
		}

		// 肌の体の点（足元 z=0 の座標）と外向きの法線。下着を体に寄せるときの基準
		struct SkinPt
		{
			float p[3];
			float n[3];
		};

		// 肌でない shape（下着）の、体から a_limit より外に出ている頂点を、体から a_limit の所まで寄せた自前の skinPartition を作る。
		// スカートはお尻のあたりで体から 1 以上離してあるので、寄せた下着はどの服の下でも内側に収まる（立体的な飾りがスカートを突き抜けない）。
		// 頂点は元の nif と共有なので、頂点の写しと頂点バッファを自前で作る（面・骨・重みの配列は元と共有）。
		// 寄せる頂点が無ければ nullptr（a_why は空）。作れなければ nullptr と理由
		RE::NiSkinPartition* MakeTuckedPartition(RE::NiSkinPartition* a_orig, float a_zOff, const std::vector<SkinPt>& a_skin, float a_limit, std::string& a_why,
			int& a_moved, float& a_maxMove)
		{
			a_moved = 0;
			a_maxMove = 0.0f;
			auto rtti = a_orig->GetRTTI();
			if (!rtti || !rtti->GetName() || std::string_view(rtti->GetName()) != "NiSkinPartition") {
				a_why = "skinPartition の型が NiSkinPartition でない";
				return nullptr;
			}
			auto dev = Device();
			if (!dev) {
				a_why = "D3D11 のデバイスが無い";
				return nullptr;
			}
			const std::uint32_t np = a_orig->numPartitions;
			const std::uint32_t nv = a_orig->vertexCount;
			if (a_skin.empty()) {
				a_why = "基準の肌の点が無い";
				return nullptr;
			}
			if (!np || a_orig->partitions.size() != np || !nv) {
				a_why = std::format("区画・頂点が無い（区画 {}・{}、頂点 {}）", np, a_orig->partitions.size(), nv);
				return nullptr;
			}
			auto b0 = a_orig->partitions[0].buffData;
			if (!b0 || !b0->rawVertexData || !b0->vertexBuffer) {
				a_why = "頂点のデータが無い";
				return nullptr;
			}
			const auto            stride = b0->vertexDesc.GetSize();
			std::set<const void*> bufs;
			for (std::uint32_t i = 0; i < np; ++i) {
				auto b = a_orig->partitions[i].buffData;
				if (!b || !b->rawVertexData || !b->vertexBuffer || !b->indexBuffer || b->vertexDesc.GetSize() != stride) {
					a_why = std::format("区画{} の頂点のデータが無い・作りが違う", i);
					return nullptr;
				}
				D3D11_BUFFER_DESC vbd{};
				reinterpret_cast<ID3D11Buffer*>(b->vertexBuffer)->GetDesc(&vbd);
				if (vbd.ByteWidth != static_cast<UINT>(nv) * stride) {
					a_why = std::format("区画{} の頂点バッファの大きさが頂点の数と合わない（{} バイト、頂点 {}）", i, vbd.ByteWidth, nv);
					return nullptr;
				}
				bufs.insert(b);
			}
			if (bufs.size() != np) {
				a_why = "区画どうしで buffData を共有している";
				return nullptr;
			}
			// 寄せる量: 頂点ごとに、いちばん近い体の点の法線の向きのすき間 g が a_limit を超えた分だけ、法線の逆へ動かす
			std::vector<std::array<float, 3>> moved(nv);
			std::vector<char>                 hit(nv, 0);
			for (std::uint32_t v = 0; v < nv; ++v) {
				const float*  f = reinterpret_cast<const float*>(b0->rawVertexData + static_cast<std::size_t>(v) * stride);
				const float   q[3] = { f[0], f[1], f[2] + a_zOff };
				float         best = 1e30f;
				const SkinPt* sb = nullptr;
				// 体の点は z の順。q の高さから上下へ、z の差だけで今の最良より遠くなったら打ち切る
				const auto start = std::lower_bound(a_skin.begin(), a_skin.end(), q[2], [](const SkinPt& a, float z) { return a.p[2] < z; }) - a_skin.begin();
				auto       test = [&](std::ptrdiff_t k) {
                    const auto& sp = a_skin[k];
                    const float d = (sp.p[0] - q[0]) * (sp.p[0] - q[0]) + (sp.p[1] - q[1]) * (sp.p[1] - q[1]) + (sp.p[2] - q[2]) * (sp.p[2] - q[2]);
                    if (d < best) {
                        best = d;
                        sb = &sp;
                    }
				};
				for (std::ptrdiff_t k = start; k < static_cast<std::ptrdiff_t>(a_skin.size()); ++k) {
					if ((a_skin[k].p[2] - q[2]) * (a_skin[k].p[2] - q[2]) >= best) {
						break;
					}
					test(k);
				}
				for (std::ptrdiff_t k = start - 1; k >= 0; --k) {
					if ((q[2] - a_skin[k].p[2]) * (q[2] - a_skin[k].p[2]) >= best) {
						break;
					}
					test(k);
				}
				if (!sb || best > 100.0f) {
					continue;  // 体から 10 より遠い（体の無い所）は動かさない。お尻に掛かる布（アハラムの下着の Cloth）は 6 以上離れている
				}
				const float g = (q[0] - sb->p[0]) * sb->n[0] + (q[1] - sb->p[1]) * sb->n[1] + (q[2] - sb->p[2]) * sb->n[2];
				if (g <= a_limit) {
					continue;
				}
				const float m = g - a_limit;
				moved[v] = { f[0] - sb->n[0] * m, f[1] - sb->n[1] * m, f[2] - sb->n[2] * m };
				hit[v] = 1;
				++a_moved;
				a_maxMove = (std::max)(a_maxMove, m);
			}
			if (!a_moved) {
				return nullptr;
			}
			// 頂点の写し（区画ごと）と頂点バッファ（区画で共有、元と同じ作り）
			std::vector<std::uint8_t*> raws(np, nullptr);
			for (std::uint32_t i = 0; i < np; ++i) {
				raws[i] = static_cast<std::uint8_t*>(RE::malloc(static_cast<std::size_t>(nv) * stride));
				if (!raws[i]) {
					a_why = "メモリが取れない";
					return nullptr;  // 取れた分はわずかなので捨てる（解放しない）
				}
				std::memcpy(raws[i], a_orig->partitions[i].buffData->rawVertexData, static_cast<std::size_t>(nv) * stride);
				for (std::uint32_t v = 0; v < nv; ++v) {
					if (hit[v]) {
						std::memcpy(raws[i] + static_cast<std::size_t>(v) * stride, moved[v].data(), 12);
					}
				}
			}
			D3D11_BUFFER_DESC vbd{};
			reinterpret_cast<ID3D11Buffer*>(b0->vertexBuffer)->GetDesc(&vbd);  // 元と同じ作り（大きさ・用途）
			D3D11_SUBRESOURCE_DATA init{};
			init.pSysMem = raws[0];
			ID3D11Buffer* vb = nullptr;
			const HRESULT hr = dev->CreateBuffer(&vbd, &init, &vb);
			if (FAILED(hr) || !vb) {
				a_why = std::format("頂点バッファを作れない（HRESULT {:08X}）", static_cast<std::uint32_t>(hr));
				return nullptr;
			}
			auto head = static_cast<std::size_t*>(RE::malloc(sizeof(std::size_t) + sizeof(RE::NiSkinPartition::Partition) * np));
			auto obj = RE::malloc(sizeof(RE::NiSkinPartition));
			std::vector<RE::BSGraphics::TriShape*> tris(np, nullptr);
			for (auto& t : tris) {
				t = static_cast<RE::BSGraphics::TriShape*>(RE::malloc(sizeof(RE::BSGraphics::TriShape)));
			}
			if (!head || !obj || std::find(tris.begin(), tris.end(), nullptr) != tris.end()) {
				vb->Release();
				a_why = "メモリが取れない";
				return nullptr;
			}
			*head = np;
			auto parts = reinterpret_cast<RE::NiSkinPartition::Partition*>(head + 1);
			for (std::uint32_t i = 0; i < np; ++i) {
				const auto& src = a_orig->partitions[i];
				std::memcpy(&parts[i], &src, sizeof(RE::NiSkinPartition::Partition));  // 骨・重みなどの配列は元と共有
				std::memcpy(tris[i], src.buffData, sizeof(RE::BSGraphics::TriShape));   // 面は元と共有
				tris[i]->refCount = 1;
				tris[i]->rawVertexData = raws[i];
				if (i) {
					vb->AddRef();  // 区画ごとに参照を 1 つ持つ（手放さない）
				}
				tris[i]->vertexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(vb);
				reinterpret_cast<ID3D11Buffer*>(tris[i]->indexBuffer)->AddRef();  // 共有の面のバッファ。参照を 1 つ持つ（手放さない）
				parts[i].buffData = tris[i];
			}
			std::memcpy(obj, a_orig, sizeof(RE::NiSkinPartition));
			auto mine = static_cast<RE::NiSkinPartition*>(obj);
			*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(obj) + 0x08) = 0;  // NiRefObject::_refCount
			*reinterpret_cast<RE::NiSkinPartition::Partition**>(&mine->partitions) = parts;
			mine->IncRefCount();  // 余分の参照（決して手放さない。デストラクタが走らないように）
			return mine;
		}

		// ファイル無しの帯を用意する（体 nif ごとに 1 回）。失敗したら nullptr（a_why に理由）
		Fileless* GetFileless(const char* a_path, int a_zLo, int a_zHi, std::string& a_why)
		{
			const auto key = std::format("{}|{}|{}", Lower(a_path), a_zLo, a_zHi);
			if (auto it = g_fileless.find(key); it != g_fileless.end()) {
				return &it->second;
			}
			if (auto it = g_filelessNg.find(key); it != g_filelessNg.end()) {
				a_why = it->second + "（前に試して作れなかった）";
				return nullptr;
			}
			auto fail = [&](std::string a_reason) -> Fileless* {
				a_why = a_reason;
				g_filelessNg[key] = std::move(a_reason);
				return nullptr;
			};
			RE::NiPointer<RE::NiNode>         normal;
			RE::BSModelDB::DBTraits::ArgsType args;
			const auto                        ec = RE::BSModelDB::Demand(a_path, normal, args);
			if (ec != RE::BSResource::ErrorCode::kNone || !normal) {
				return fail(std::format("元の体 nif を Demand で読めない（ErrorCode={}）", static_cast<int>(ec)));
			}
			std::vector<RE::BSGeometry*> shapes;
			CollectSkinned(normal.get(), shapes);
			if (shapes.empty()) {
				return fail("元の体 nif に皮付きの shape が無い");
			}
			Fileless f;
			f.root = normal;
			int kept = 0;
			// 肌の体の点（腕・手は除く）。下着を体に寄せる基準。z の順に並べる（近い点を探すとき z で打ち切る）
			std::vector<SkinPt> skinPts;
			for (auto geo : shapes) {
				auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
				auto sp = skin ? skin->skinPartition.get() : nullptr;
				auto pr = geo->GetGeometryRuntimeData().shaderProperty.get();
				if (!pr || !pr->material || pr->material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGenRGBTint || !sp || !sp->numPartitions) {
					continue;
				}
				auto b = sp->partitions[0].buffData;
				if (!b || !b->rawVertexData) {
					continue;
				}
				// 法線は三角形から求める（体はモデル空間の法線マップを使うので、頂点に法線が無いことが多い）
				const auto           stride = b->vertexDesc.GetSize();
				const auto           nv = sp->vertexCount;
				auto                 at = [&](std::uint32_t v) { return reinterpret_cast<const float*>(b->rawVertexData + static_cast<std::size_t>(v) * stride); };
				std::vector<float>   acc(static_cast<std::size_t>(nv) * 3, 0.0f);
				for (std::uint32_t i = 0; i < sp->numPartitions; ++i) {
					auto pb = sp->partitions[i].buffData;
					if (!pb || !pb->rawIndexData) {
						continue;
					}
					const auto* ix = pb->rawIndexData;
					for (std::uint32_t t = 0; t < sp->partitions[i].triangles; ++t) {
						const std::uint16_t a = ix[t * 3], c1 = ix[t * 3 + 1], c2 = ix[t * 3 + 2];
						if (a >= nv || c1 >= nv || c2 >= nv || (a == c1 && c1 == c2)) {
							continue;
						}
						const float* pa = at(a);
						const float* pb1 = at(c1);
						const float* pc = at(c2);
						const float  u[3] = { pb1[0] - pa[0], pb1[1] - pa[1], pb1[2] - pa[2] };
						const float  w[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
						const float  fn[3] = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
						for (auto v : { a, c1, c2 }) {
							for (int k = 0; k < 3; ++k) {
								acc[static_cast<std::size_t>(v) * 3 + k] += fn[k];
							}
						}
					}
				}
				const auto arm = ArmBones(skin);
				const auto fo = FrameOffset(geo);
				const auto first = skinPts.size();
				float      cx = 0.0f, cy = 0.0f;
				for (std::uint32_t v = 0; v < nv; ++v) {
					const auto*  vp = b->rawVertexData + static_cast<std::size_t>(v) * stride;
					const float* fp = reinterpret_cast<const float*>(vp);
					const float  z = fp[2] + fo[2];
					if (z < static_cast<float>(a_zLo) - 8.0f || z > static_cast<float>(a_zHi) + 8.0f || OnArm(vp, b->vertexDesc, sp->partitions[0], arm)) {
						continue;
					}
					const float* n = &acc[static_cast<std::size_t>(v) * 3];
					const float  l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
					if (l < 1e-8f) {
						continue;
					}
					skinPts.push_back({ { fp[0] + fo[0], fp[1] + fo[1], z }, { n[0] / l, n[1] / l, n[2] / l } });
					cx += fp[0] + fo[0];
					cy += fp[1] + fo[1];
				}
				// 三角形の向き（表裏）は nif 次第なので、法線が体の外を向く頂点が多くなる向きにそろえる（腰まわりで数える）
				const auto cnt = skinPts.size() - first;
				if (cnt) {
					cx /= cnt;
					cy /= cnt;
					int out = 0, in = 0;
					for (auto k = first; k < skinPts.size(); ++k) {
						auto& pt = skinPts[k];
						if (pt.p[2] < 60.0f || pt.p[2] > 80.0f) {
							continue;
						}
						((pt.p[0] - cx) * pt.n[0] + (pt.p[1] - cy) * pt.n[1] > 0.0f ? out : in)++;
					}
					if (in > out) {
						for (auto k = first; k < skinPts.size(); ++k) {
							for (auto& c : skinPts[k].n) {
								c = -c;
							}
						}
					}
					logger::info("  帯（ファイル無し）: [{}] 下着を寄せる基準の肌の点 {} 個（法線は三角形から。外向き {}・内向き {}{}）", geo->name.c_str(), cnt, out, in,
						in > out ? "、裏返した" : "");
				}
			}
			std::sort(skinPts.begin(), skinPts.end(), [](const SkinPt& a, const SkinPt& b) { return a.p[2] < b.p[2]; });
			for (auto geo : shapes) {
				auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
				if (std::string why; !Layout::Check(skin, why)) {
					return fail(std::format("皮の構造体が前提と合わない（{}）", why));
				}
				auto orig = skin ? skin->skinPartition.get() : nullptr;
				auto pr = geo->GetGeometryRuntimeData().shaderProperty.get();
				const bool isSkin = pr && pr->material && pr->material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint;
				f.hasSkin |= isSkin;
				// 肌でない shape（下着など）: 帯の範囲に掛かっていて、上が範囲の少し上までに収まる物は切らずにそのまま出す。
				// 範囲に掛からない物（ブラなど）は付けない。上に長く伸びる物（ボディスーツなど）だけ範囲で切る
				float lo = 1e9f, hi = -1e9f;
				if (!isSkin && orig && orig->numPartitions && orig->partitions[0].buffData && orig->partitions[0].buffData->rawVertexData) {
					auto       b = orig->partitions[0].buffData;
					const auto stride = b->vertexDesc.GetSize();
					const float oz = FrameOffset(geo)[2];
					for (std::uint32_t v = 0; v < orig->vertexCount; ++v) {
						const float z = reinterpret_cast<const float*>(b->rawVertexData + static_cast<std::size_t>(v) * stride)[2] + oz;
						lo = (std::min)(lo, z);
						hi = (std::max)(hi, z);
					}
					if (hi < a_zLo || lo > a_zHi) {
						logger::info("  帯（ファイル無し）: [{}] 肌でない shape（z {:.1f}〜{:.1f}）は範囲の外なので付けない", geo->name.c_str(), lo, hi);
						f.origs.emplace_back(nullptr);
						f.mine.emplace_back(nullptr);
						f.skins.push_back(skin);
						f.empty.push_back(1);
						continue;
					}
					if (hi <= a_zHi + 6) {
						// 体から外に出ている所（立体的な飾りなど）は体に寄せる。スカートはお尻のあたりで体から 1 以上離すので、0.4 ならどの服でも内側に収まる
						std::string why;
						int         moved = 0;
						float       maxMove = 0.0f;
						auto        tucked = MakeTuckedPartition(orig, FrameOffset(geo)[2], skinPts, 0.4f, why, moved, maxMove);
						if (tucked) {
							logger::info("  帯（ファイル無し）: [{}] 肌でない shape（z {:.1f}〜{:.1f}）は切らずに付ける。体から 0.4 より外の頂点 {} 個を体に寄せた（最大 {:.1f}）",
								geo->name.c_str(), lo, hi, moved, maxMove);
							orig->IncRefCount();  // 元の物も手放さない（自前の物と配列を共有しているので）
							f.origs.emplace_back(orig);
							f.mine.emplace_back(tucked);
							f.skins.push_back(skin);
							f.empty.push_back(0);
							++kept;
							continue;
						}
						logger::info("  帯（ファイル無し）: [{}] 肌でない shape（z {:.1f}〜{:.1f}）は切らずにそのまま付ける{}", geo->name.c_str(), lo, hi,
							why.empty() ? std::string("（体から出ている所は無い）") : std::format("（体に寄せられない: {}）", why));
						f.origs.emplace_back(nullptr);
						f.mine.emplace_back(nullptr);
						f.skins.push_back(skin);
						f.empty.push_back(0);
						++kept;
						continue;
					}
				}
				if (!orig) {
					return fail(std::format("[{}] に skinPartition が無い", geo->name.c_str()));
				}
				std::string why;
				int         removed = 0, total = 0, armCut = 0;
				auto        mine = MakeCutPartition(orig, isSkin ? ArmBones(skin) : std::vector<char>{}, static_cast<float>(a_zLo), static_cast<float>(a_zHi), why,
					removed, total, armCut, FrameOffset(geo)[2]);
				if (!mine) {
					if (!isSkin) {
						// 肌でない物は切れなければ付けない（体は付ける）
						logger::info("  帯（ファイル無し）: [{}] 肌でない shape を切れないので付けない（{}）", geo->name.c_str(), why);
						f.origs.emplace_back(nullptr);
						f.mine.emplace_back(nullptr);
						f.skins.push_back(skin);
						f.empty.push_back(1);
						continue;
					}
					return fail(std::format("[{}] {}", geo->name.c_str(), why));
				}
				logger::info("  帯（ファイル無し）: [{}] {}区画 {} 個、三角 {} 個のうち {} 個を消した（うち範囲の中の腕・手首 {} 個）", geo->name.c_str(),
					isSkin ? "" : std::format("肌でない shape（z {:.1f}〜{:.1f}）を範囲で切った。", lo, hi), orig->numPartitions, total, removed, armCut);
				if (!isSkin) {
					// 切った下着（飾りなど）も、体から外に出ている所は体に寄せる
					std::string twhy;
					int         moved = 0;
					float       maxMove = 0.0f;
					if (auto tucked = MakeTuckedPartition(mine, FrameOffset(geo)[2], skinPts, 0.4f, twhy, moved, maxMove)) {
						logger::info("  帯（ファイル無し）: [{}] 体から 0.4 より外の頂点 {} 個を体に寄せた（最大 {:.1f}）", geo->name.c_str(), moved, maxMove);
						mine = tucked;
					} else if (!twhy.empty()) {
						logger::info("  帯（ファイル無し）: [{}] 体に寄せられない（{}）", geo->name.c_str(), twhy);
					}
				}
				orig->IncRefCount();  // 元の物も手放さない（自前の物と配列を共有しているので）
				f.origs.emplace_back(orig);
				f.mine.emplace_back(mine);
				f.skins.push_back(skin);
				f.empty.push_back(removed >= total ? 1 : 0);
				++kept;
			}
			if (!kept) {
				return fail("範囲の中に残る shape が無い");
			}
			normal->IncRefCount();  // 元の体の根も手放さない
			return &(g_fileless[key] = std::move(f));
		}

		// 複製の皮付き shape の skinPartition を、自前の物に差し替える。shape の数・区画の数が合わなければ false
		bool UseFileless(Fileless& a_f, RE::NiNode* a_clone, std::string& a_why)
		{
			std::vector<RE::BSGeometry*> shapes;
			CollectSkinned(a_clone, shapes);
			if (shapes.size() != a_f.mine.size()) {
				a_why = std::format("複製の皮付き shape の数が合わない（{} と {}）", shapes.size(), a_f.mine.size());
				return false;
			}
			for (std::size_t i = 0; i < shapes.size(); ++i) {
				if (!a_f.mine[i]) {
					continue;  // 切らない・付けない shape
				}
				auto skin = shapes[i]->GetGeometryRuntimeData().skinInstance.get();
				auto cur = skin ? skin->skinPartition.get() : nullptr;
				if (static_cast<const void*>(skin) == a_f.skins[i]) {
					a_why = "複製が元の NiSkinInstance を共有している（差し替えるとほかの NPC の体まで切れる）";
					return false;
				}
				if (!cur || cur->numPartitions != a_f.mine[i]->numPartitions || cur->vertexCount != a_f.mine[i]->vertexCount) {
					a_why = std::format("[{}] の区画が元と合わない", shapes[i]->name.c_str());
					return false;
				}
			}
			for (std::size_t i = 0; i < shapes.size(); ++i) {
				if (a_f.mine[i]) {
					shapes[i]->GetGeometryRuntimeData().skinInstance->skinPartition = a_f.mine[i];
				}
			}
			return true;
		}
	}
	namespace
	{
		int g_zHi = 75;  // 帯の上端（全 NPC 共通。ファイルを増やさないため）。下端はいつも 0（足首から）

		// 服装（または段階リスト）に、体スロットの衣服（鎧でない）が入っているか
		bool HasClothingForm(RE::TESForm* a_form, int a_depth)
		{
			if (!a_form || a_depth > 4) {
				return false;
			}
			if (auto armo = a_form->As<RE::TESObjectARMO>()) {
				return armo->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody) &&
				       armo->GetArmorType() == RE::BGSBipedObjectForm::ArmorType::kClothing;
			}
			if (auto lvl = a_form->As<RE::TESLevItem>()) {
				for (auto& e : lvl->entries) {
					if (HasClothingForm(e.form, a_depth + 1)) {
						return true;
					}
				}
			}
			return false;
		}

		bool HasClothingOutfit(RE::BGSOutfit* a_outfit)
		{
			if (!a_outfit) {
				return false;
			}
			for (auto f : a_outfit->outfitItems) {
				if (HasClothingForm(f, 0)) {
					return true;
				}
			}
			return false;
		}

		void BodyModels(RE::TESObjectARMO* a_skin, RE::TESRace* a_race, std::set<std::string>& a_out)
		{
			if (!a_skin) {
				return;
			}
			for (auto a : a_skin->armorAddons) {
				if (a && a->IsValidRace(a_race) && a->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) {
					if (auto m = a->bipedModels[RE::SEXES::kFemale].GetModel(); m && *m) {
						a_out.insert(m);
					}
				}
			}
		}


		// 帯の揺れ骨を止める: NPC の骨格で子の骨（NiNode）を持たない末端の骨で、親の骨も同じ皮の骨の一覧にあるもの
		// （お尻・胸・股などの揺れ用の骨。名前は見ない）を、親の骨で動かす。帯はスカートの下なので揺れは要らない。
		// 皮の骨の基準（skinToBone）は親の物を写す（NiSkinData は同じ帯ファイルの複製で共有しているが、何度写しても同じ）
		int FreezeLeafBones(RE::NiSkinInstance* a_skin)
		{
			auto       data = a_skin->skinData.get();
			const auto n = data->GetBoneCount();
			int        frozen = 0;
			for (std::uint32_t i = 0; i < n; ++i) {
				auto b = a_skin->bones[i];
				auto node = b ? b->AsNode() : nullptr;
				if (!node || !node->parent) {
					continue;
				}
				bool hasChildBone = false;
				for (auto& c : node->GetChildren()) {
					if (c && c->AsNode()) {
						hasChildBone = true;
						break;
					}
				}
				if (hasChildBone) {
					continue;
				}
				int pj = -1;
				for (std::uint32_t j = 0; j < n; ++j) {
					if (j != i && a_skin->bones[j] == node->parent) {
						pj = static_cast<int>(j);
						break;
					}
				}
				if (pj < 0) {
					continue;
				}
				a_skin->bones[i] = node->parent;
				a_skin->boneWorldTransforms[i] = &node->parent->world;
				data->GetBoneDataSkinToBone(i) = data->GetBoneDataSkinToBone(static_cast<std::uint32_t>(pj));
				++frozen;
			}
			return frozen;
		}

		// 帯の揺れ骨を止めるか（ini band_freeze_jiggle、既定は止めない）。帯は体 nif ごとに 1 つの複製を共有しているので、止めると全 NPC 共通になる
		bool g_freeze = false;
	}

	void EarlyPrepare(float a_top)
	{
		// ゲームの初期化より前（SKSEPluginLoad）。ゲームは起動の最初にあったファイルしか Demand で見つけられないようなので、
		// ここで帯ファイルを書き出しておくと、この起動から読める。フォームはまだ読めないので、前回の起動で保存した体の一覧と、
		// 標準の女性の体を使う。BSResource もまだ使えないので、ルーズファイルをディスクから直接読む（BSA の中だけの体は kDataLoaded に任せる）
		const int zHi = static_cast<int>(std::lround(a_top));
		g_bodyPaths.insert("Actors\\Character\\Character Assets\\FemaleBody_1.nif");
		g_bodyPaths.insert("Actors\\Character\\Character Assets\\FemaleBody_0.nif");
		int listed = 0;
		{
			std::ifstream in(kBodyList);
			std::string   line;
			while (std::getline(in, line)) {
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
					line.pop_back();
				}
				if (!line.empty() && line[0] != ';') {
					g_bodyPaths.insert(line);
					++listed;
				}
			}
		}
		// 入れた直後でも NPC 専用の体を拾う: Data\Meshes\Actors\Character 以下のルーズファイルから、ファイル名に femalebody を含む nif を探す。
		// 名前で探すのは「先に用意しておく」ためだけ（実際に使う体は skin → ArmorAddon からたどる）。顔・動きのフォルダは見ない
		int found = 0;
		{
			const auto                  t0 = std::chrono::steady_clock::now();
			const std::filesystem::path base("Data\\Meshes");
			std::error_code             ec;
			std::filesystem::recursive_directory_iterator it(base / "Actors" / "Character", std::filesystem::directory_options::skip_permission_denied, ec);
			const std::filesystem::recursive_directory_iterator end;
			int seen = 0;
			for (; !ec && it != end; it.increment(ec)) {
				++seen;
				const auto name = Lower(it->path().filename().string());
				if (it->is_directory(ec)) {
					if (it.depth() >= 4 || name == "facegendata" || name == "animations" || name == "behaviors" || name == "_1stperson" ||
						name == "animationdata" || name == "animationsetdata") {
						it.disable_recursion_pending();
					}
					continue;
				}
				if (name.size() < 4 || name.substr(name.size() - 4) != ".nif" || name.find("femalebody") == std::string::npos || name.rfind("1stperson", 0) == 0) {
					continue;
				}
				const auto rel = it->path().lexically_relative(base).string();
				if (g_bodyPaths.insert(rel).second) {
					++found;
				}
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			logger::info("プラグイン読み込み時: Actors\\Character 以下を探した（{} ms、{} 項目を見た）→ 女性の体らしい nif を新しく {} 個", ms, seen, found);
		}
		// 大文字小文字だけ違う同じパスをまとめる
		std::map<std::string, std::string> unique;
		for (auto& p : g_bodyPaths) {
			unique.emplace(Lower(p), p);
		}
		int written = 0, existed = 0, missing = 0;
		for (auto& [low, path] : unique) {
			const std::filesystem::path disk = std::filesystem::path("Data\\Meshes") / path;
			std::ifstream               in(disk, std::ios::binary);
			if (!in) {
				++missing;
				logger::info("プラグイン読み込み時: {} はディスクに無い（BSA の中かもしれない。データ読み込み完了時に書き出す）", path);
				continue;
			}
			std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (bytes.empty()) {
				++missing;
				continue;
			}
			const auto h = Fnv(bytes.data(), bytes.size());
			g_contentHash[low] = h;
			const auto rel = BandRel(path, h, 0, zHi);
			bool       ex = false;
			if (WriteBand(rel, bytes, ex)) {
				g_early.insert(rel);
				ex ? ++existed : ++written;
				logger::info("プラグイン読み込み時: {} → {}（{}）", path, rel, ex ? "前からあった" : "書き出した");
			}
		}
		logger::info("プラグイン読み込み時: 体 {} 種（一覧から {} 行）→ 書き出した {} / 前からあった {} / ディスクに無い {}", unique.size(), listed, written, existed,
			missing);
	}

	void Prepare(float a_top, bool a_freezeJiggle)
	{
		g_zHi = static_cast<int>(std::lround(a_top));
		g_freeze = a_freezeJiggle;
		logger::info("起動時: 帯の範囲 z=0〜{}、帯の揺れ骨を止める={}", g_zHi, g_freeze ? "はい（全 NPC 共通）" : "いいえ");

		// 女性の体になりうる nif。この環境の物を前提にせず、毎回フォームからたどる
		std::set<std::string> models;
		auto                  dh = RE::TESDataHandler::GetSingleton();
		if (dh) {
			// 対象: 女性・子供でない人型（ActorTypeNPC）の種族で、既定の服装に体スロットの衣服がある NPC（＝このプラグインが縮めうる NPC）。
			// その NPC の skin（無ければ種族の skin）の、種族に合う体の ArmorAddon の女性モデル。名前での決め打ちはしない
			int npcs = 0;
			for (auto npc : dh->GetFormArray<RE::TESNPC>()) {
				if (!npc || npc->GetSex() != RE::SEX::kFemale) {
					continue;
				}
				auto race = npc->race;
				if (!race || race->data.flags.all(RE::RACE_DATA::Flag::kChild) || !race->HasKeywordString("ActorTypeNPC")) {
					continue;
				}
				if (!HasClothingOutfit(npc->defaultOutfit) && !HasClothingOutfit(npc->sleepOutfit)) {
					continue;
				}
				++npcs;
				BodyModels(npc->skin ? npc->skin : race->skin, race, models);
			}
			logger::info("起動時: 対象になりうる女性 NPC {} 人", npcs);
		}
		int written = 0, existed = 0, failed = 0, early = 0;
		for (auto& m : models) {
			std::string rel;
			bool        ex = false;
			if (EnsureFile(m, 0, g_zHi, rel, ex)) {
				const bool e = g_early.contains(rel);
				if (!e) {
					g_prepared.insert(rel);
				}
				early += e ? 1 : 0;
				ex ? ++existed : ++written;
				logger::info("起動時: 体の nif {} → 帯のファイル {}（{}）", m, rel,
					e ? "プラグイン読み込み時に用意した物。この起動で読める見込み" :
					ex ? "前からある" : "いま書き出した。この起動では読めないかもしれない");
			} else {
				++failed;
			}
			g_bodyPaths.insert(m);
		}
		logger::info("起動時: 体の nif {} 種 → プラグイン読み込み時に用意済み {} / ほかに前からあった {} / いま書き出した {} / 失敗 {}", models.size(), early,
			existed - early, written, failed);
		SaveBodyList();  // 次の起動のプラグイン読み込み時に、ここにある体の帯ファイルを書き出す

		// 帯のファイルがゲームのファイルシステムから見えるか（いま書き出した物も含む）
		const std::filesystem::path dir("Data\\Meshes\\MiniSkirt");
		std::error_code             ec;
		int                         n = 0, visible = 0;
		for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
			const auto name = e.path().filename().string();
			if (name.rfind("band_", 0) != 0 || e.path().extension() != ".nif") {
				continue;
			}
			++n;
			visible += StreamOpens("MiniSkirt\\" + name) ? 1 : 0;
		}
		logger::info("起動時: 帯のファイル {} 個のうち、ストリームで開けた {} 個（使うのは z=0〜{} の物だけ）", n, visible, g_zHi);
	}

	void SetTop(float a_top)
	{
		g_zHi = static_cast<int>(std::lround(a_top));
		logger::info("帯の上端を z={} に変えた（新しい上端の帯は、体ごとに作り直す）", g_zHi);
	}

	namespace
	{
		// モデルのパスから先頭の "meshes\" を外す（Demand は Meshes\ からの相対で受け取る）
		std::string StripMeshes(std::string a_p)
		{
			const auto low = Lower(a_p.substr(0, 7));
			if (low == "meshes\\" || low == "meshes/") {
				a_p.erase(0, 7);
			}
			return a_p;
		}

		// ファイル無しで複製を作る。a_take に付ける shape（範囲の中に何も残らない物は除く）。失敗したら nullptr と理由
		RE::NiNode* FilelessClone(const std::string& a_path, int a_zLo, int a_zHi, std::vector<RE::BSGeometry*>& a_take, bool& a_hasSkin, std::string& a_why)
		{
			auto f = GetFileless(a_path.c_str(), a_zLo, a_zHi, a_why);
			if (!f) {
				return nullptr;
			}
			auto c = static_cast<RE::NiNode*>(f->root->Clone());
			if (!c) {
				a_why = "複製に失敗";
				return nullptr;
			}
			if (!UseFileless(*f, c, a_why)) {
				RE::NiPointer<RE::NiNode> drop(c);  // 使わない複製は捨てる
				return nullptr;
			}
			std::vector<RE::BSGeometry*> shapes;
			CollectSkinned(c, shapes);
			for (std::size_t i = 0; i < shapes.size(); ++i) {
				if (!f->empty[i]) {
					a_take.push_back(shapes[i]);
				}
			}
			a_hasSkin = f->hasSkin;
			return c;
		}
	}

	bool Attach(RE::Actor* a_actor, const std::string& a_underwear)
	{
		const auto id = a_actor->GetFormID();
		auto       root = a_actor->Get3D();
		auto       rootNode = root ? root->AsNode() : nullptr;
		const int  zLo = 0;
		const int  zHi = g_zHi;
		const auto uw = StripMeshes(a_underwear);
		// 前に付けた帯: 同じ根に付いたままで、範囲も下着も同じなら何もしない。根が変わった（3D が作り直された）なら、古い帯は古い 3D と一緒に消えているので記録だけ捨てる
		if (auto it = g_state.find(id); it != g_state.end()) {
			const bool sameRoot = rootNode && static_cast<const void*>(rootNode) == it->second.root;
			const int  alive = sameRoot ? CountAttached(it->second, rootNode) : 0;
			if (sameRoot && alive == static_cast<int>(it->second.shapes.size()) && it->second.zHi == zHi && it->second.src == uw) {
				logger::info("  帯: 前に付けた帯が付いたまま");
				return true;
			}
			logger::info("  帯: 前に付けた帯を付け直す（根が同じ={}、付いている {}/{}、上端 {}→{}、下着 [{}]→[{}]）", sameRoot ? "はい" : "いいえ", alive,
				it->second.shapes.size(), it->second.zHi, zHi, it->second.src, uw);
			if (sameRoot) {
				for (auto& s : it->second.shapes) {
					if (s && s->parent == rootNode) {
						rootNode->DetachChild(s.get());
					}
				}
			}
			g_state.erase(it);
		}
		logger::info("  帯: 範囲 z={}〜{}", zLo, zHi);
		auto race = a_actor->GetRace();
		auto skinArmor = a_actor->GetSkin();
		if (!rootNode || !race || !skinArmor) {
			logger::info("  帯: 3D / 種族 / skin が無い");
			return false;
		}
		// skin には体・手・足など複数の ArmorAddon がある。種族に合い、体スロット（32）を持つものを選ぶ
		RE::TESObjectARMA* arma = nullptr;
		for (auto a : skinArmor->armorAddons) {
			if (a && a->IsValidRace(race) && a->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) {
				arma = a;
				break;
			}
		}
		if (!arma) {
			logger::info("  帯: skin ({:08X}) に種族に合う体の ArmorAddon が無い", skinArmor->GetFormID());
			return false;
		}
		const char* bodyPath = arma->bipedModels[RE::SEXES::kFemale].GetModel();
		if (!bodyPath || !*bodyPath) {
			logger::info("  帯: ArmorAddon ({:08X}) の女性モデルが無い", arma->GetFormID());
			return false;
		}
		logger::info("  帯: 体の nif = {}（ArmorAddon {:08X}）", bodyPath, arma->GetFormID());

		std::vector<RE::BSGeometry*> take;  // 付ける皮付きの shape（体と下着）

		// 下着: 下着の nif を同じ範囲で用意する。体を含む nif（肌の shape がある）なら、それだけで帯にする（裸の体は使わない）
		bool needBody = true;
		bool uwUsed = false;
		if (!uw.empty()) {
			std::string                  why;
			bool                         hasSkin = false;
			std::vector<RE::BSGeometry*> uwTake;
			if (FilelessClone(uw, zLo, zHi, uwTake, hasSkin, why)) {
				uwUsed = true;
				needBody = !hasSkin;
				take.insert(take.end(), uwTake.begin(), uwTake.end());
				logger::info("  帯: 下着の nif {} を使う（shape {} 個。{}）", uw, uwTake.size(), hasSkin ? "体を含むので、裸の体は足さない" : "体を含まないので、裸の体と重ねる");
			} else {
				logger::info("  帯: 下着の nif {} を用意できないので、裸の帯だけにする（{}）", uw, why);
			}
		}

		// 裸の体: まずファイル無し（元の体を複製して、面だけ自前の物に差し替える）。できなければ帯ファイルの方式に戻る
		if (needBody) {
			std::string why;
			bool        hasSkin = false;
			std::vector<RE::BSGeometry*> bodyTake;
			if (FilelessClone(bodyPath, zLo, zHi, bodyTake, hasSkin, why)) {
				logger::info("  帯: 体はファイル無しで作れた");
			} else {
				logger::info("  帯: 体はファイルの方式に戻した（{}）", why);
				bodyTake.clear();
				auto cutRoot = LoadCut(bodyPath, zLo, zHi);
				auto clone = cutRoot ? static_cast<RE::NiNode*>(cutRoot->Clone()) : nullptr;
				if (!clone) {
					logger::info("  帯: 切り詰めた体を用意できなかった");
				} else {
					CollectSkinned(clone, bodyTake);
				}
			}
			if (bodyTake.empty() && !uwUsed) {
				logger::info("  帯: 体を足せない");
				return false;
			}
			take.insert(take.end(), bodyTake.begin(), bodyTake.end());
		}
		logger::info("  帯: 皮付きの shape {} 個", take.size());

		int bound = 0;
		for (auto geo : take) {
			auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
			if (!skin || !skin->skinData || !skin->bones) {
				continue;
			}
			const auto n = skin->skinData->GetBoneCount();
			int        miss = 0;
			for (std::uint32_t i = 0; i < n; ++i) {
				auto b = skin->bones[i];
				if (!b) {
					++miss;
					continue;
				}
				auto target = rootNode->GetObjectByName(b->name);
				if (!target) {
					++miss;
					continue;
				}
				skin->bones[i] = target;
				skin->boneWorldTransforms[i] = &target->world;
			}
			skin->rootParent = rootNode;
			const int frozen = (g_freeze && miss == 0) ? FreezeLeafBones(skin) : 0;
			++bound;
			logger::info("  帯: [{}] 骨 {} 本（見つからない {}、揺れ骨を親で動かす {}）", geo->name.c_str(), n, miss, frozen);
			const auto fo = FrameOffset(geo);
			const auto& lt = geo->local.translate;
			logger::info("  帯: [{}] 座標のずれ ({:.2f}, {:.2f}, {:.2f})（shape 自身の移動 ({:.2f}, {:.2f}, {:.2f})）", geo->name.c_str(), fo[0], fo[1], fo[2], lt.x, lt.y,
				lt.z);
		}
		if (!bound) {
			return false;
		}

		int   attached = 0;
		State st;
		st.root = rootNode;
		st.zHi = zHi;
		st.arma = arma;
		st.src = uw;
		for (auto geo : take) {
			RE::NiPointer<RE::NiAVObject> k(geo);
			if (auto parent = geo->parent) {
				parent->DetachChild(geo);  // 複製の中から外して NPC の根に付け直す
			}
			rootNode->AttachChild(geo, true);
			// 付け直しただけでは、位置と範囲（world・worldBound）が複製の時のまま（原点のあたり）。
			// ライトは範囲で照らす物を選ぶので、NPC が動いて更新されるまで近くのライトに照らされず暗く見えた（バナード・メアのフルダ）。
			RE::NiUpdateData ud;
			geo->Update(ud);
			const auto& wb = geo->worldBound;
			logger::info("  帯: [{}] 位置と範囲を更新した（中心 ({:.0f}, {:.0f}, {:.0f})、半径 {:.0f}）", geo->name.c_str(), wb.center.x, wb.center.y, wb.center.z,
				wb.radius);
			st.shapes.push_back(k);
			++attached;
		}
		g_state[id] = std::move(st);
		logger::info("  帯: shape を {} 個付けた（下着 {}）", attached, uwUsed ? uw : std::string("なし"));
		return true;
	}

	void HideOutfitLegs(RE::Actor* a_actor, RE::NiAVObject* a_outfit)
	{
		auto it = g_state.find(a_actor->GetFormID());
		if (it == g_state.end() || !a_outfit) {
			return;
		}
		const float zHi = static_cast<float>(it->second.zHi);
		// 肌の shape の、帯の範囲（z < 上端 − 1）の頂点を全部「消す候補」にする。帯の体までの近さは見ない
		// （服と裸の体が別の体型の配布先でも、確実に消えるように）。上端の近くは残して、帯の切り口との間に穴をあけない

		std::vector<RE::BSGeometry*> geos;
		CollectSkinned(a_outfit, geos);
		auto ctx = Context();
		int  skinShapes = 0;
		for (auto geo : geos) {
			auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
			auto part = skin ? skin->skinPartition.get() : nullptr;
			if (!part || !part->numPartitions) {
				continue;
			}
			// 肌の shape（肌の色の付く材質）だけを見る。タイツ・エプロン・ベルトなどは残す
			auto prop = geo->GetGeometryRuntimeData().shaderProperty.get();
			auto mat = prop ? prop->material : nullptr;
			if (!mat || mat->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
				continue;
			}
			++skinShapes;
			const float                         oz = FrameOffset(geo)[2];
			std::set<RE::BSGraphics::TriShape*> bufs;
			for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
				if (part->partitions[i].buffData) {
					bufs.insert(part->partitions[i].buffData);
				}
			}
			for (auto buf : bufs) {
				if (!buf->rawVertexData) {
					continue;
				}
				const std::uint32_t n = part->vertexCount;
				if (auto e = Verts::Owned(buf, n); e && e->hidden) {
					continue;  // もう消してある
				}
				auto&             ent = Verts::Remember(buf, n);  // 元を覚える（設定を変えたときに戻す）
				Verts::SealGuard  seal{ buf, n };
				const auto          stride = buf->vertexDesc.GetSize();
				auto*               raw = buf->rawVertexData;
				auto pos = [&](std::uint32_t v) { return reinterpret_cast<float*>(raw + static_cast<std::size_t>(v) * stride); };
				// 体と重なる頂点（帯の範囲の内側。上端の近くは残して、帯の切り口との間に穴をあけない）
				std::vector<char> over(n, 0);
				int               nOver = 0, nLow = 0, nArm = 0;
				// 頂点ごとの区画（骨の番号は区画の中の番号なので、腕の骨かを見るのに使う）
				std::vector<int> vpart(n, -1);
				for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
					auto& p = part->partitions[i];
					if (p.buffData && p.buffData->rawIndexData) {
						for (std::uint32_t t = 0; t < static_cast<std::uint32_t>(p.triangles) * 3; ++t) {
							if (p.buffData->rawIndexData[t] < n) {
								vpart[p.buffData->rawIndexData[t]] = static_cast<int>(i);
							}
						}
					}
				}
				const auto arm = ArmBones(skin);
				for (std::uint32_t v = 0; v < n; ++v) {
					const float* f = pos(v);
					if (f[2] + oz >= zHi - 1.0f) {
						continue;
					}
					if (vpart[v] >= 0 && OnArm(raw + static_cast<std::size_t>(v) * stride, buf->vertexDesc, part->partitions[vpart[v]], arm)) {
						++nArm;  // 腕・手首（帯の上端を腰まで上げると、手首がこの高さに入る）は消さない
						continue;
					}
					++nLow;
					over[v] = 1;
					++nOver;
				}
				if (!nOver) {
					logger::info("  服の脚: [{}] 帯の範囲（z<{}）の頂点なし → 残す", geo->name.c_str(), zHi - 1.0f);
					continue;
				}
				// 三角形の 3 頂点とも重なる物だけを消す。そういう三角形にしか使われていない頂点を、区画ごとに 1 点に集める
				// （縁の頂点は動かさない。動かすと残る三角形が伸びてとげになる）
				std::vector<char> keep(n, 0);
				std::vector<int>  partOf(n, -1);
				bool              ok = true;
				for (std::uint32_t i = 0; i < part->numPartitions && ok; ++i) {
					auto& p = part->partitions[i];
					if (!p.buffData || !p.buffData->rawIndexData) {
						ok = false;
						break;
					}
					const auto* idx = p.buffData->rawIndexData;
					for (std::uint32_t t = 0; t < p.triangles; ++t) {
						const std::uint16_t a = idx[t * 3], b = idx[t * 3 + 1], c = idx[t * 3 + 2];
						if (a >= n || b >= n || c >= n) {
							continue;
						}
						partOf[a] = partOf[b] = partOf[c] = static_cast<int>(i);
						if (!(over[a] && over[b] && over[c])) {
							keep[a] = keep[b] = keep[c] = 1;
						}
					}
				}
				if (!ok) {
					logger::info("  服の脚: [{}] 区画の面が読めないので消さない", geo->name.c_str());
					continue;
				}
				// 集める頂点が 1 つだけの三角形は、残る 2 頂点と集めた点の間に伸びる（とげ）。そういう三角形が無くなるまで、その頂点を残す側に戻す。
				// これで三角形は「集める頂点 0 個（そのまま）」か「2〜3 個（同じ点に重なって面積 0 ＝ 描かれない）」だけになる
				std::vector<char> col(n, 0);
				for (std::uint32_t v = 0; v < n; ++v) {
					col[v] = over[v] && !keep[v] && partOf[v] >= 0;
				}
				for (bool changed = true; changed;) {
					changed = false;
					for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
						auto&       p = part->partitions[i];
						const auto* idx = p.buffData->rawIndexData;
						for (std::uint32_t t = 0; t < p.triangles; ++t) {
							const std::uint16_t a = idx[t * 3], b = idx[t * 3 + 1], c = idx[t * 3 + 2];
							if (a >= n || b >= n || c >= n) {
								continue;
							}
							if (col[a] + col[b] + col[c] == 1) {
								col[a] = col[b] = col[c] = 0;
								changed = true;
							}
						}
					}
				}
				// 集めた頂点は、区画ごとに位置も骨の重みも同じにする（区画の最初の頂点の位置、骨は区画の 0 番に重み 1）。
				// 同じ区画の三角形の頂点は必ず同じ点になるので、動いても面積 0 のまま（とげが出ない）
				const auto off = buf->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
				const bool skinned = buf->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) && off + 12 <= stride;
				std::vector<std::int64_t> anchor(part->numPartitions, -1);
				int                       collapsed = 0, edge = 0;
				for (std::uint32_t v = 0; v < n; ++v) {
					if (!over[v] || partOf[v] < 0) {
						continue;
					}
					if (!col[v]) {
						++edge;
						continue;
					}
					auto& a = anchor[partOf[v]];
					if (a < 0) {
						a = v;
					}
					std::memcpy(pos(v), pos(static_cast<std::uint32_t>(a)), sizeof(float) * 3);
					if (skinned) {
						std::uint8_t sk[12]{};
						const std::uint16_t one = 0x3C00;  // half の 1.0
						std::memcpy(sk, &one, 2);
						std::memcpy(raw + static_cast<std::size_t>(v) * stride + off, sk, 12);
					}
					++collapsed;
				}
				ent.hidden = true;
				D3D11_BUFFER_DESC bd{};
				auto              vb = reinterpret_cast<ID3D11Buffer*>(buf->vertexBuffer);
				if (vb) {
					vb->GetDesc(&bd);
				}
				if (vb && ctx && bd.Usage == D3D11_USAGE_DEFAULT) {
					ctx->UpdateSubresource(vb, 0, nullptr, raw, 0, 0);
				}
				logger::info("  服の脚: [{}] 帯の範囲の頂点 {} 個（腕・手首 {} 個は残す）→ 1 点に集めた {} 個、縁なので残した {} 個{}",
					geo->name.c_str(), nLow + nArm, nArm, collapsed, edge, vb && bd.Usage == D3D11_USAGE_DEFAULT ? "" : "（GPU に反映できない）");
			}
		}
		if (!skinShapes) {
			logger::info("  服の脚: 服に肌の shape（肌の色の付く材質）が無いので何もしない");
		}
	}


	namespace
	{
		std::string TexPath(RE::BSShaderMaterial* a_mat)
		{
			auto base = static_cast<RE::BSLightingShaderMaterialBase*>(a_mat);
			auto ts = base ? base->textureSet.get() : nullptr;
			auto p = ts ? ts->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
			return p && *p ? p : "（なし）";
		}

		std::string TintText(RE::BSShaderMaterial* a_mat)
		{
			if (!a_mat || a_mat->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
				return "（ティントの材質でない）";
			}
			auto& c = static_cast<RE::BSLightingShaderMaterialFacegenTint*>(a_mat)->tintColor;
			return std::format("({:.3f}, {:.3f}, {:.3f})", c.red, c.green, c.blue);
		}

		// ほかの NPC の帯が、この材質の持ち主（property / material）を共有しているか
		bool SharedWithOthers(RE::FormID a_self, const void* a_prop, const void* a_mat, bool& a_matShared)
		{
			bool shared = false;
			a_matShared = false;
			for (auto& [id, st] : g_state) {
				if (id == a_self) {
					continue;
				}
				for (auto& s : st.shapes) {
					auto g = s ? s->AsGeometry() : nullptr;
					auto pr = g ? g->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
					if (!pr) {
						continue;
					}
					shared |= static_cast<const void*>(pr) == a_prop;
					a_matShared |= static_cast<const void*>(pr->material) == a_mat;
				}
			}
			return shared;
		}
	}

	void MatchSkin(RE::Actor* a_actor, RE::NiAVObject* a_outfit)
	{
		const auto id = a_actor->GetFormID();
		auto       it = g_state.find(id);
		if (it == g_state.end() || it->second.skinDone) {
			return;
		}
		it->second.skinDone = true;
		// 第一案の写す元: 同じ NPC の服の中の肌の shape（ゲームがその NPC の色・テクスチャを付けている）
		RE::BSGeometry*              src = nullptr;
		std::vector<RE::BSGeometry*> geos;
		CollectSkinned(a_outfit, geos);
		for (auto g : geos) {
			auto pr = g->GetGeometryRuntimeData().shaderProperty.get();
			if (pr && pr->material && pr->material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
				src = g;
				break;
			}
		}
		auto base = a_actor->GetActorBase();
		for (auto& s : it->second.shapes) {
			auto geo = s ? s->AsGeometry() : nullptr;
			auto prop = geo ? netimmerse_cast<RE::BSLightingShaderProperty*>(geo->GetGeometryRuntimeData().shaderProperty.get()) : nullptr;
			if (!prop || !prop->material || prop->material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
				logger::info("  帯の肌: [{}] 肌のティントの材質でないので変えない", geo ? geo->name.c_str() : "?");
				continue;
			}
			bool       matShared = false;
			const bool propShared = SharedWithOthers(id, prop, prop->material, matShared);
			logger::info("  帯の肌: [{}] 変える前 property はほかの NPC の帯と{}、材質は{}（色 {}、テクスチャ {}）", geo->name.c_str(), propShared ? "共有" : "別",
				matShared ? "共有" : "別", TintText(prop->material), TexPath(prop->material));
			if (propShared) {
				// SetMaterial は property ごとなので、property が共有だと全員変わる。変えずに残す
				logger::info("  帯の肌: [{}] property がほかの NPC と共有なので変えない（全員の色が変わるため）", geo->name.c_str());
				continue;
			}
			if (src) {
				// 服の肌の材質の写し（この NPC 専用の新しい材質。a_unique=true）を帯に付ける
				auto srcMat = src->GetGeometryRuntimeData().shaderProperty->material;
				prop->SetMaterial(srcMat, true);
				logger::info("  帯の肌: [{}] 服の肌の shape [{}] から写した（色 {}、テクスチャ {}）。材質は{}", geo->name.c_str(), src->name.c_str(),
					TintText(prop->material), TexPath(prop->material), prop->material == srcMat ? "服と同じ物（写しにならなかった）" : "この NPC 専用");
				continue;
			}
			// 第二案: 自分の材質の写しにしてから、NPC の体の色（QNAM）と ArmorAddon の肌のテクスチャ（NAM1）を付ける
			auto own = prop->material;
			prop->SetMaterial(own, true);
			if (prop->material == own && matShared) {
				logger::info("  帯の肌: [{}] 材質を写しにできなかった（共有のまま）ので変えない", geo->name.c_str());
				continue;
			}
			auto mat = static_cast<RE::BSLightingShaderMaterialFacegenTint*>(prop->material);
			if (base) {
				const auto& c = base->bodyTintColor;
				mat->tintColor = RE::NiColor(c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f);
			}
			auto ts = it->second.arma ? it->second.arma->skinTextures[RE::SEXES::kFemale] : nullptr;
			if (ts) {
				mat->OnLoadTextureSet(0, ts);
			}
			logger::info("  帯の肌: [{}] 服に肌の shape が無いので、NPC の体の色 (bodyTintColor) {} を付けた。肌のテクスチャ: {}", geo->name.c_str(), TintText(mat),
				ts ? std::format("ArmorAddon の skinTextures ({:08X}) {}", ts->GetFormID(), TexPath(mat)) : std::string("ArmorAddon に指定なし（体 nif のまま）"));
		}
	}

	namespace
	{
		float HalfToFloat(std::uint16_t h)
		{
			const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
			float               f;
			if (e == 0) {
				f = std::ldexp(static_cast<float>(m), -24);
			} else if (e == 31) {
				f = m ? 0.0f : 65504.0f;
			} else {
				f = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
			}
			return s ? -f : f;
		}

		std::uint16_t FloatToHalf(float f)
		{
			// 重み（0〜1）用。0 と正の正規化数だけを扱う
			if (!(f > 6.1e-5f)) {
				return 0;
			}
			if (f >= 1.0f) {
				return 0x3C00;
			}
			int         e = 0;
			const float m = std::frexp(f, &e);  // f = m * 2^e、m は 0.5〜1
			const int   he = e - 1 + 15;
			const auto  hm = static_cast<std::uint32_t>(std::lround((m * 2.0f - 1.0f) * 1024.0f));
			if (hm >= 1024) {
				return static_cast<std::uint16_t>((he + 1) << 10);
			}
			return static_cast<std::uint16_t>((he << 10) | hm);
		}

		struct BandVert
		{
			Skirt::Vec3 p;
			Skirt::Vec3 n;
			float       butt[4]{};  // 見つけたお尻の揺れ骨（最大 4 本）ごとの重み
			std::array<std::pair<RE::NiAVObject*, float>, 4> all{};  // 全部の骨の重み（C で写す）
			bool        skin = true;  // 肌の shape の頂点。下着など肌でない物は、すき間を空けず覆うだけにする
		};

	}

	namespace
	{
		// 頂点ごとの区画（骨の番号を読むため）。描く三角形 → 区画の頂点の対応表 → 元の三角形の一覧の順に探す（切った三角形は 0,0,0 なので）
		std::vector<int> PartitionMap(RE::NiSkinPartition* a_part, std::uint32_t a_n)
		{
			std::vector<int> partOf(a_n, -1);
			for (std::uint32_t i = 0; i < a_part->numPartitions; ++i) {
				auto& p = a_part->partitions[i];
				if (p.buffData && p.buffData->rawIndexData) {
					const auto* ix = p.buffData->rawIndexData;
					for (std::uint32_t t = 0; t < p.triangles * 3u; ++t) {
						if (ix[t] < a_n && partOf[ix[t]] < 0) {
							partOf[ix[t]] = static_cast<int>(i);
						}
					}
				}
				if (p.vertexMap) {
					for (std::uint32_t j = 0; j < p.vertices; ++j) {
						if (p.vertexMap[j] < a_n && partOf[p.vertexMap[j]] < 0) {
							partOf[p.vertexMap[j]] = static_cast<int>(i);
						}
					}
				}
				if (p.triList) {
					for (std::uint32_t t = 0; t < p.triangles * 3u; ++t) {
						if (p.triList[t] < a_n && partOf[p.triList[t]] < 0) {
							partOf[p.triList[t]] = static_cast<int>(i);
						}
					}
				}
			}
			return partOf;
		}

		// 頂点の、腕の骨への重みの和
		float ArmWeight(const std::uint8_t* a_vp, const RE::BSGraphics::VertexDesc& a_desc, const RE::NiSkinPartition::Partition& a_p, const std::vector<char>& a_arm)
		{
			if (a_arm.empty() || !a_desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED)) {
				return 0.0f;
			}
			const auto off = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
			float      w = 0.0f;
			for (int k = 0; k < 4; ++k) {
				std::uint16_t h;
				std::memcpy(&h, a_vp + off + k * 2, 2);
				const auto li = a_vp[off + 8 + k];
				if (li < a_arm.size() && a_arm[li]) {
					w += HalfToFloat(h);
				}
			}
			return w;
		}
	}

	namespace
	{
		// 標準の骨格の、主な骨の基準の位置（足元 z=0 の座標）。標準の体・服の nif の skinToBone から求めた値
		struct RefBone
		{
			const char* name;
			float       x, y, z;
		};
		constexpr RefBone kRefBones[] = {
			{ "NPC Pelvis [Pelv]", 0.0f, 0.0f, 68.91f },
			{ "NPC L Thigh [LThg]", -6.62f, 0.0f, 68.91f },
			{ "NPC R Thigh [RThg]", 6.62f, 0.0f, 68.91f },
			{ "NPC L Calf [LClf]", -10.12f, -1.31f, 33.51f },
			{ "NPC R Calf [RClf]", 10.12f, -1.31f, 33.51f },
			{ "NPC Spine [Spn0]", 0.0f, -5.24f, 72.70f },
			{ "NPC Spine1 [Spn1]", 0.0f, -4.86f, 81.44f },
			{ "NPC Spine2 [Spn2]", 0.0f, -5.93f, 91.25f },
		};

		// 皮の骨の基準の位置（skinToBone の逆）と標準の位置の差の平均。主な骨が 1 本も無いか、骨ごとの差がそろわない（体型ごと骨を動かした nif）なら nullopt
		std::optional<std::array<float, 3>> OffsetFromBones(RE::BSGeometry* a_geo)
		{
			auto skin = a_geo->GetGeometryRuntimeData().skinInstance.get();
			auto data = skin ? skin->skinData.get() : nullptr;
			if (!data || !skin->bones) {
				return std::nullopt;
			}
			const auto           count = data->GetBoneCount();
			std::array<float, 3> sum{ 0.0f, 0.0f, 0.0f }, lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
			int                  found = 0;
			for (std::uint32_t i = 0; i < count; ++i) {
				auto bone = skin->bones[i];
				if (!bone) {
					continue;
				}
				for (const auto& r : kRefBones) {
					if (bone->name != r.name) {
						continue;
					}
					// skinToBone: 骨の座標 = s·R·v + t。骨の原点の、頂点の座標での位置は −Rᵀt / s
					const auto& st = data->GetBoneDataSkinToBone(i);
					const auto& m = st.rotate.entry;
					const auto& t = st.translate;
					const float sc = std::fabs(st.scale) > 1e-6f ? st.scale : 1.0f;
					const float p[3] = { -(m[0][0] * t.x + m[1][0] * t.y + m[2][0] * t.z) / sc, -(m[0][1] * t.x + m[1][1] * t.y + m[2][1] * t.z) / sc,
						-(m[0][2] * t.x + m[1][2] * t.y + m[2][2] * t.z) / sc };
					const float d[3] = { r.x - p[0], r.y - p[1], r.z - p[2] };
					for (int k = 0; k < 3; ++k) {
						sum[k] += d[k];
						lo[k] = (std::min)(lo[k], d[k]);
						hi[k] = (std::max)(hi[k], d[k]);
					}
					++found;
					break;
				}
			}
			if (!found) {
				return std::nullopt;
			}
			for (int k = 0; k < 3; ++k) {
				if (hi[k] - lo[k] > 0.5f) {
					return std::nullopt;
				}
			}
			return std::array<float, 3>{ sum[0] / found, sum[1] / found, sum[2] / found };
		}
	}

	std::array<float, 3> FrameOffset(RE::BSGeometry* a_geo)
	{
		if (!a_geo) {
			return { 0.0f, 0.0f, 0.0f };
		}
		// 皮付きの shape は、骨の基準の位置から求める。描かれる位置は骨と skinToBone で決まり、shape 自身の移動は使われない。
		// 標準の nif は移動と同じ値になる（確認済み）。Bijin NPCs の体（LE の nif）は移動が 0 なのに、骨の基準が (0, −2.54, +3.29) ずれていた
		// （移動だけ見ると、お尻が実際より 2.5 前にあることになり、スカートを突き抜けた）
		if (auto o = OffsetFromBones(a_geo)) {
			return *o;
		}
		const auto& l = a_geo->local;
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 3; ++c) {
				if (std::fabs(l.rotate.entry[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-3f) {
					return { 0.0f, 0.0f, 0.0f };
				}
			}
		}
		if (std::fabs(l.scale - 1.0f) > 1e-3f) {
			return { 0.0f, 0.0f, 0.0f };
		}
		return { l.translate.x, l.translate.y, l.translate.z };
	}

	std::vector<float> SleeveMask(RE::BSGeometry* a_geo, RE::BSGraphics::TriShape* a_buf, float a_waist)
	{
		auto skin = a_geo->GetGeometryRuntimeData().skinInstance.get();
		auto part = skin ? skin->skinPartition.get() : nullptr;
		if (!part || !a_buf || !a_buf->rawVertexData) {
			return {};
		}
		const auto fo = FrameOffset(a_geo);
		const auto n = part->vertexCount;
		const auto stride = a_buf->vertexDesc.GetSize();
		const auto arm = ArmBones(skin);
		const auto partOf = PartitionMap(part, n);
		std::vector<float> a(n, 0.0f);
		std::vector<float> xs(n);
		std::vector<char>  low(n, 0);
		for (std::uint32_t v = 0; v < n; ++v) {
			const auto*  vp = a_buf->rawVertexData + static_cast<std::size_t>(v) * stride;
			const float* f = reinterpret_cast<const float*>(vp);
			xs[v] = f[0] + fo[0];
			low[v] = f[2] + fo[2] < a_waist;
			if (partOf[v] >= 0) {
				a[v] = (std::min)(ArmWeight(vp, a_buf->vertexDesc, part->partitions[partOf[v]], arm), 1.0f);
			}
		}
		// 腰より下の頂点だけでつながった三角形のまとまり。垂れた袖はスカートとつながらない（肩の側でだけ体とつながる）。
		// 片側（x の符号が同じ）で体の外にあるまとまりは袖（骨の重みが腰に付いている服もあるので形でも見る）
		std::vector<std::uint32_t> parent(n);
		for (std::uint32_t v = 0; v < n; ++v) {
			parent[v] = v;
		}
		auto find = [&](std::uint32_t x) {
			while (parent[x] != x) {
				parent[x] = parent[parent[x]];
				x = parent[x];
			}
			return x;
		};
		for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
			auto& p = part->partitions[i];
			if (!p.buffData || !p.buffData->rawIndexData) {
				continue;
			}
			const auto* ix = p.buffData->rawIndexData;
			for (std::uint32_t t = 0; t < p.triangles; ++t) {
				const std::uint16_t v0 = ix[t * 3], v1 = ix[t * 3 + 1], v2 = ix[t * 3 + 2];
				if (v0 >= n || v1 >= n || v2 >= n || !low[v0] || !low[v1] || !low[v2]) {
					continue;
				}
				parent[find(v0)] = find(v1);
				parent[find(v1)] = find(v2);
			}
		}
		struct Comp
		{
			float xmin{ 1e9f }, xmax{ -1e9f }, arm{ 0.0f };
			int   n{ 0 };
		};
		std::map<std::uint32_t, Comp> comps;
		for (std::uint32_t v = 0; v < n; ++v) {
			if (!low[v] || partOf[v] < 0) {
				continue;
			}
			auto& c = comps[find(v)];
			c.xmin = (std::min)(c.xmin, xs[v]);
			c.xmax = (std::max)(c.xmax, xs[v]);
			c.arm += a[v];
			++c.n;
		}
		int sleeveComps = 0;
		std::map<std::uint32_t, bool> isSleeve;
		for (auto& [r, c] : comps) {
			const bool oneSided = c.xmin > 8.0f || c.xmax < -8.0f;
			const bool armic = c.n > 0 && c.arm / c.n >= 0.5f;
			isSleeve[r] = c.n >= 8 && (oneSided || armic);
			sleeveComps += isSleeve[r] ? 1 : 0;
		}
		int total = 0;
		for (std::uint32_t v = 0; v < n; ++v) {
			if (low[v] && partOf[v] >= 0 && isSleeve[find(v)]) {
				a[v] = 1.0f;
			}
			total += a[v] >= 0.5f ? 1 : 0;
		}
		logger::info("    袖: [{}] 袖として外す頂点 {} 個（腰より下のまとまり {} 個のうち袖 {} 個）", a_geo->name.c_str(), total, comps.size(), sleeveComps);
		return a;
	}

	int WeldCeiling(RE::BSGeometry* a_geo, RE::BSGraphics::TriShape* a_buf, float a_waist, float a_nz, const std::vector<float>& a_sleeve)
	{
		auto skin = a_geo->GetGeometryRuntimeData().skinInstance.get();
		auto part = skin ? skin->skinPartition.get() : nullptr;
		const auto n = part ? part->vertexCount : 0u;
		if (!part || !a_buf || !a_buf->rawVertexData || a_sleeve.size() != n) {
			return 0;
		}
		const auto stride = a_buf->vertexDesc.GetSize();
		auto*      raw = a_buf->rawVertexData;
		auto       pos = [&](std::uint32_t v) { return reinterpret_cast<float*>(raw + static_cast<std::size_t>(v) * stride); };
		const float oz = FrameOffset(a_geo)[2];
		const auto off = a_buf->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
		const bool skinned = a_buf->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) && off + 12 <= stride;
		const auto partOf = PartitionMap(part, n);
		// 先に全部の対象を決める（溶接で位置が変わる前の法線で）
		std::vector<std::array<std::uint32_t, 3>> targets;
		for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
			auto& p = part->partitions[i];
			if (!p.buffData || !p.buffData->rawIndexData) {
				continue;
			}
			const auto* ix = p.buffData->rawIndexData;
			for (std::uint32_t t = 0; t < p.triangles; ++t) {
				const std::uint32_t a = ix[t * 3], b = ix[t * 3 + 1], c = ix[t * 3 + 2];
				if (a >= n || b >= n || c >= n || a == b || b == c || a == c) {
					continue;
				}
				if (a_sleeve[a] >= 0.5f || a_sleeve[b] >= 0.5f || a_sleeve[c] >= 0.5f) {
					continue;
				}
				if (partOf[a] != static_cast<int>(i) || partOf[b] != static_cast<int>(i) || partOf[c] != static_cast<int>(i)) {
					continue;  // 骨の番号は区画ごとなので、同じ区画の頂点だけ溶接する
				}
				const float *pa = pos(a), *pb = pos(b), *pc = pos(c);
				if (pa[2] + oz >= a_waist || pb[2] + oz >= a_waist || pc[2] + oz >= a_waist) {
					continue;
				}
				const float e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] }, e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
				const float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nzz = e1[0] * e2[1] - e1[1] * e2[0];
				const float l = std::sqrt(nx * nx + ny * ny + nzz * nzz);
				if (l < 1e-9f || nzz / l >= a_nz) {
					continue;
				}
				targets.push_back({ a, b, c });
			}
		}
		// 3 頂点を最初の頂点の位置・骨の重みに重ねて、面積 0（描かれない）にする
		for (auto& t : targets) {
			for (int k = 1; k < 3; ++k) {
				std::memcpy(pos(t[k]), pos(t[0]), sizeof(float) * 3);
				if (skinned) {
					std::memcpy(raw + static_cast<std::size_t>(t[k]) * stride + off, raw + static_cast<std::size_t>(t[0]) * stride + off, 12);
				}
			}
		}
		logger::info("    下向きの水平な面: [{}] nz<{} の三角形 {} 個を面積 0 にした（スカートの中の天井になる黒い面）", a_geo->name.c_str(), a_nz, targets.size());
		return static_cast<int>(targets.size());
	}

	float CutCloth(RE::BSGeometry* a_geo, RE::BSGraphics::TriShape* a_buf, float a_hem, const std::vector<float>& a_sleeve, std::vector<char>& a_dead)
	{
		auto skin = a_geo->GetGeometryRuntimeData().skinInstance.get();
		auto part = skin ? skin->skinPartition.get() : nullptr;
		const auto n = part ? part->vertexCount : 0u;
		a_dead.assign(n, 0);
		if (!part || !a_buf || !a_buf->rawVertexData || a_sleeve.size() != n) {
			return a_hem;
		}
		const auto stride = a_buf->vertexDesc.GetSize();
		auto*      raw = a_buf->rawVertexData;
		auto       pos = [&](std::uint32_t v) { return reinterpret_cast<float*>(raw + static_cast<std::size_t>(v) * stride); };
		const float       oz = FrameOffset(a_geo)[2];
		std::vector<char> cand(n, 0), kept(n, 0);
		for (std::uint32_t v = 0; v < n; ++v) {
			cand[v] = pos(v)[2] + oz < a_hem && a_sleeve[v] < 0.5f;
		}
		const auto partOf = PartitionMap(part, n);
		// 3 頂点とも候補の三角形は消す。ほかの三角形の頂点は残す
		for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
			auto& p = part->partitions[i];
			if (!p.buffData || !p.buffData->rawIndexData) {
				continue;
			}
			const auto* ix = p.buffData->rawIndexData;
			for (std::uint32_t t = 0; t < p.triangles; ++t) {
				const std::uint16_t a = ix[t * 3], b = ix[t * 3 + 1], c = ix[t * 3 + 2];
				if (a >= n || b >= n || c >= n) {
					continue;
				}
				if (!(cand[a] && cand[b] && cand[c])) {
					kept[a] = kept[b] = kept[c] = 1;
				}
			}
		}
		// 消す三角形にしか使われていない頂点（候補で、残る三角形に使われていない）の位置を NaN にする。
		// D3D11 は位置が NaN の頂点を含む三角形を捨てるので、面のバッファを触らずに（同じ服を着たほかの人の面を壊さずに）三角形を消せる。
		// 縁の頂点（残る三角形にも使われる）は動かさない。別の頂点にまとめる方式は、まとめ先が散らばって、縁どうしをつなぐ面（スカートの底のふた）になった
		int collapsed = 0, nEdge = 0;
		const float nan = std::numeric_limits<float>::quiet_NaN();
		for (std::uint32_t v = 0; v < n; ++v) {
			if (!cand[v] || partOf[v] < 0) {
				continue;
			}
			if (kept[v]) {
				++nEdge;
				continue;
			}
			float* f = pos(v);
			f[0] = f[1] = f[2] = nan;
			a_dead[v] = 1;
			++collapsed;
		}
		float hemNew = 1e9f;
		for (std::uint32_t v = 0; v < n; ++v) {
			if (!a_dead[v] && a_sleeve[v] < 0.5f) {
				hemNew = (std::min)(hemNew, pos(v)[2] + oz);
			}
		}
		logger::info("    布を切る: [{}] z<{:.1f} の頂点のうち、NaN にして消した {} 個、縁として残した {} 個。裾 z={:.1f}", a_geo->name.c_str(), a_hem, collapsed, nEdge, hemNew);
		return hemNew;
	}

	void ShapeSkirt(RE::Actor* a_actor, const std::vector<RE::BSGeometry*>& a_skirts, float a_waist, float a_hem, float a_ease, float a_angle, bool a_keepLayers)
	{
		auto it = g_state.find(a_actor->GetFormID());
		if (it == g_state.end() || a_skirts.empty()) {
			return;
		}
		const auto  t0 = std::chrono::steady_clock::now();
		const float top = static_cast<float>(it->second.zHi);
		// 帯の体: 描く三角形の頂点（腕・手首・上端より上は切ってある）と、腕・手の頂点（袖の見分け用）。
		// 下着など肌でない shape は使わない（飾りが体より外に出ていて、お尻の線が横に広がった）。肌の shape が無いときだけ使う。
		// 下着の突き抜けは FitSkirt の押し出しが全部の shape で防ぐ
		std::vector<Skirt::Vec3> band, arm, other;
		for (auto& s : it->second.shapes) {
			auto geo = s ? s->AsGeometry() : nullptr;
			auto skin = geo ? geo->GetGeometryRuntimeData().skinInstance.get() : nullptr;
			auto part = skin ? skin->skinPartition.get() : nullptr;
			if (!part || !part->numPartitions || !part->partitions[0].buffData || !part->partitions[0].buffData->rawVertexData) {
				continue;
			}
			auto       pr = geo->GetGeometryRuntimeData().shaderProperty.get();
			const bool isSkin = pr && pr->material && pr->material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint;
			auto&      dst = isSkin ? band : other;
			auto             b = part->partitions[0].buffData;
			const auto       stride = b->vertexDesc.GetSize();
			const auto       nv = part->vertexCount;
			std::vector<char> drawn(nv, 0);
			for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
				auto& p = part->partitions[i];
				if (!p.buffData || !p.buffData->rawIndexData) {
					continue;
				}
				const auto* ix = p.buffData->rawIndexData;
				for (std::uint32_t t = 0; t < p.triangles; ++t) {
					if (ix[t * 3] == ix[t * 3 + 1] && ix[t * 3 + 1] == ix[t * 3 + 2]) {
						continue;
					}
					for (int k = 0; k < 3; ++k) {
						if (ix[t * 3 + k] < nv) {
							drawn[ix[t * 3 + k]] = 1;
						}
					}
				}
			}
			const auto armBones = ArmBones(skin);
			const auto bandFo = FrameOffset(geo);  // 帯の体 nif の shape の移動（標準の体は z +120）。足すと足元 z=0 の座標
			const auto partOf = PartitionMap(part, nv);
			for (std::uint32_t v = 0; v < nv; ++v) {
				const auto*  vp = b->rawVertexData + static_cast<std::size_t>(v) * stride;
				const float* f = reinterpret_cast<const float*>(vp);
				if (drawn[v]) {
					dst.push_back({ f[0] + bandFo[0], f[1] + bandFo[1], f[2] + bandFo[2] });
				} else if (partOf[v] >= 0 && ArmWeight(vp, b->vertexDesc, part->partitions[partOf[v]], armBones) >= 0.5f) {
					arm.push_back({ f[0] + bandFo[0], f[1] + bandFo[1], f[2] + bandFo[2] });
				}
			}
		}
		if (band.empty()) {
			band.swap(other);
		}
		const auto tBand = std::chrono::steady_clock::now();
		long long  usSleeve = 0;
		// スカート: 縮めた頂点の写しごと。もう A ラインにしてあれば作り直さない（設定が変わったときは元に戻してから来る）
		struct Item
		{
			RE::BSGraphics::TriShape* buf;
			RE::NiSkinPartition*      part;
			RE::NiSkinInstance*       skin;
			std::uint32_t             n;
			std::array<float, 3>      fo;  // 頂点の座標系のずれ（FrameOffset）
		};
		std::vector<Item> items;
		std::set<const void*> seen;
		for (auto geo : a_skirts) {
			auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
			auto part = skin ? skin->skinPartition.get() : nullptr;
			if (!part) {
				continue;
			}
			for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
				auto buf = part->partitions[i].buffData;
				if (!buf || !buf->rawVertexData || !seen.insert(buf).second) {
					continue;
				}
				auto e = Verts::Owned(buf, part->vertexCount);
				if (!e || !e->shortened) {
					continue;
				}
				if (e->shaped) {
					logger::info("  A ライン: もう作ってある");
					return;
				}
				items.push_back({ buf, part, skin, part->vertexCount, FrameOffset(geo) });
			}
		}
		if (items.empty()) {
			return;
		}
		std::vector<std::vector<Skirt::Vec3>> pos(items.size());
		std::vector<std::vector<float>>       sleeve(items.size());
		int                                   sleeves = 0;
		for (std::size_t k = 0; k < items.size(); ++k) {
			auto&      it2 = items[k];
			const auto stride = it2.buf->vertexDesc.GetSize();
			const auto armBones = ArmBones(it2.skin);
			const auto partOf = PartitionMap(it2.part, it2.n);
			pos[k].resize(it2.n);
			sleeve[k].assign(it2.n, 0.0f);
			for (std::uint32_t v = 0; v < it2.n; ++v) {
				const auto*  vp = it2.buf->rawVertexData + static_cast<std::size_t>(v) * stride;
				const float* f = reinterpret_cast<const float*>(vp);
				pos[k][v] = { f[0] + it2.fo[0], f[1] + it2.fo[1], f[2] + it2.fo[2] };
				if (partOf[v] >= 0) {
					sleeve[k][v] = ArmWeight(vp, it2.buf->vertexDesc, it2.part->partitions[partOf[v]], armBones);
				}
			}
			const auto tS = std::chrono::steady_clock::now();
			Skirt::Sleeve(pos[k], sleeve[k], band, arm);
			usSleeve += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tS).count();
			if (auto e = Verts::Owned(it2.buf, it2.n)) {
				for (std::uint32_t v = 0; v < it2.n; ++v) {
					if (e->sleeve.size() == it2.n) {
						sleeve[k][v] = (std::max)(sleeve[k][v], e->sleeve[v]);
					}
					if (e->dead.size() == it2.n && e->dead[v]) {
						sleeve[k][v] = 1.0f;  // 切り捨てた布の頂点は動かさない（外形にも入れない）
					}
				}
			}
			for (float w : sleeve[k]) {
				sleeves += w >= 0.5f ? 1 : 0;
			}
		}
		std::string log;
		const auto  tRead = std::chrono::steady_clock::now();
		const int   moved = Skirt::ALine(pos, sleeve, band, a_waist, a_hem, a_ease, a_angle, top, log, a_keepLayers);
		const auto  tShape = std::chrono::steady_clock::now();
		for (std::size_t k = 0; k < items.size(); ++k) {
			auto&            it2 = items[k];
			auto&            ent = Verts::Remember(it2.buf, it2.n);  // 縮めたときに覚えた元
			Verts::SealGuard seal{ it2.buf, it2.n };
			const auto       stride = it2.buf->vertexDesc.GetSize();
			for (std::uint32_t v = 0; v < it2.n; ++v) {
				float* f = reinterpret_cast<float*>(it2.buf->rawVertexData + static_cast<std::size_t>(v) * stride);
				f[0] = pos[k][v].x - it2.fo[0];
				f[1] = pos[k][v].y - it2.fo[1];
			}
			ent.shaped = true;
			Verts::Upload(it2.buf, it2.n);
		}
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
		logger::info("  A ライン: 頂点の写し {} 個、動かした頂点 {}、袖とみなして動かさない頂点 {}（帯の体の頂点 {}、腕・手 {}）。{}（{} ms。帯の読み取り {} µs、スカートの読み取り {} µs、"
					 "うち袖の見分け {} µs、形 {} µs、書き込み {} µs）",
			items.size(), moved, sleeves, band.size(), arm.size(), log, ms, us(t0, tBand), us(tBand, tRead) - usSleeve, usSleeve, us(tRead, tShape),
			us(tShape, std::chrono::steady_clock::now()));
	}

	void FitSkirt(RE::Actor* a_actor, const std::vector<RE::BSGeometry*>& a_skirts, float a_hem, float a_gap, float a_gapNoBone, bool a_bodyWeights)
	{
		auto it = g_state.find(a_actor->GetFormID());
		if (it == g_state.end() || a_skirts.empty()) {
			return;
		}
		const float top = static_cast<float>(it->second.zHi);
		// 帯の体の頂点: 位置・法線（初めの姿勢）と、末端の骨（子の骨を持たない＝揺れ骨）への重み
		std::vector<BandVert>     band;
		struct Raw
		{
			Skirt::Vec3                                    p, n;
			std::vector<std::pair<RE::NiAVObject*, float>> leaf;
			std::vector<std::pair<RE::NiAVObject*, float>> all;  // 全部の骨
			bool                                           skin = true;
		};
		std::vector<Raw> raws;
		for (auto& s : it->second.shapes) {
			auto geo = s ? s->AsGeometry() : nullptr;
			auto skin = geo ? geo->GetGeometryRuntimeData().skinInstance.get() : nullptr;
			auto part = skin ? skin->skinPartition.get() : nullptr;
			if (!part || !part->numPartitions || !skin->bones) {
				continue;
			}
			auto b = part->partitions[0].buffData;
			if (!b || !b->rawVertexData) {
				continue;
			}
			auto       pr = geo->GetGeometryRuntimeData().shaderProperty.get();
			const bool isSkin = pr && pr->material && pr->material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint;
			auto       desc = b->vertexDesc;
			const auto stride = desc.GetSize();
			const bool hasN = desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL);
			const auto offN = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
			const bool hasS = desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED);
			const auto offS = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
			const auto nv = part->vertexCount;
			std::vector<int> partOf(nv, -1);
			for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
				auto& p = part->partitions[i];
				if (!p.buffData || !p.buffData->rawIndexData) {
					continue;
				}
				// 描く三角形（切った物は 0,0,0）の頂点だけ。帯の外（腕・手首・上端より上）の頂点は使わない
				const auto* ix = p.buffData->rawIndexData;
				for (std::uint32_t t = 0; t < p.triangles; ++t) {
					if (ix[t * 3] == ix[t * 3 + 1] && ix[t * 3 + 1] == ix[t * 3 + 2]) {
						continue;
					}
					for (int k = 0; k < 3; ++k) {
						if (ix[t * 3 + k] < nv) {
							partOf[ix[t * 3 + k]] = static_cast<int>(i);
						}
					}
				}
			}
			const auto boneCount = skin->skinData ? skin->skinData->GetBoneCount() : 0;
			const auto bandFo = FrameOffset(geo);  // 帯の体 nif の shape の移動。スカートの頂点（FrameOffset を足した座標）と同じ座標で比べる
			for (std::uint32_t v = 0; v < nv; ++v) {
				const auto*  vp = b->rawVertexData + static_cast<std::size_t>(v) * stride;
				const float* f = reinterpret_cast<const float*>(vp);
				if (f[2] + bandFo[2] > top + 8.0f || partOf[v] < 0) {
					continue;
				}
				Raw r;
				r.skin = isSkin;
				r.p = { f[0] + bandFo[0], f[1] + bandFo[1], f[2] + bandFo[2] };
				r.n = { 0.0f, 0.0f, 0.0f };
				if (hasN) {
					r.n = { vp[offN] / 127.5f - 1.0f, vp[offN + 1] / 127.5f - 1.0f, vp[offN + 2] / 127.5f - 1.0f };
					const float l = std::sqrt(r.n.x * r.n.x + r.n.y * r.n.y + r.n.z * r.n.z);
					if (l > 1e-3f) {
						r.n = { r.n.x / l, r.n.y / l, r.n.z / l };
					}
				}
				if (hasS && partOf[v] >= 0) {
					auto& p = part->partitions[partOf[v]];
					for (int k = 0; k < 4; ++k) {
						std::uint16_t h;
						std::memcpy(&h, vp + offS + k * 2, 2);
						const float w = HalfToFloat(h);
						const auto  li = vp[offS + 8 + k];
						if (w <= 0.0f || li >= boneCount) {
							continue;
						}
						auto bone = skin->bones[li];
						auto node = bone ? bone->AsNode() : nullptr;
						if (!node) {
							continue;
						}
						r.all.emplace_back(node, w);
						bool child = false;
						for (auto& c : node->GetChildren()) {
							if (c && c->AsNode()) {
								child = true;
								break;
							}
						}
						if (!child) {
							r.leaf.emplace_back(node, w);
						}
					}
				}
				raws.push_back(std::move(r));
			}
		}
		if (raws.empty()) {
			logger::info("  スカート: 帯の頂点が読めないので広げない");
			return;
		}
		// お尻の揺れ骨: 裾〜上端+8 の後ろ側（y が平均より小さい）の帯の頂点で、末端の骨への重みの合計が大きい物（名前は見ない）
		float meanY = 0.0f;
		int   cnt = 0;
		for (auto& r : raws) {
			if (r.p.z >= a_hem && r.p.z <= top + 8.0f) {
				meanY += r.p.y;
				++cnt;
			}
		}
		meanY = cnt ? meanY / cnt : 0.0f;
		std::map<RE::NiAVObject*, float> sum;
		for (auto& r : raws) {
			if (r.p.z >= a_hem && r.p.z <= top + 8.0f && r.p.y < meanY) {
				for (auto& [node, w] : r.leaf) {
					sum[node] += w;
				}
			}
		}
		std::vector<RE::NiAVObject*> butt;
		for (auto& [node, w] : sum) {
			if (w >= 3.0f && butt.size() < 4) {
				butt.push_back(node);
			}
		}
		std::string buttNames;
		for (auto b : butt) {
			buttNames += std::format("[{}] ", b->name.c_str());
		}
		// お尻の揺れ骨に重み（0.1 以上）を持つ帯の頂点のいちばん上。これより上は、上端に向かって押し出しを弱める（腰まわりが膨らまない・境で段にならない）
		float buttTop = -1e9f;
		for (auto& r : raws) {
			float w = 0.0f;
			for (auto& [node, lw] : r.leaf) {
				if (std::find(butt.begin(), butt.end(), node) != butt.end()) {
					w += lw;
				}
			}
			if (w >= 0.1f) {
				buttTop = (std::max)(buttTop, r.p.z);
			}
		}
		// 弱め始める高さ t0 から上端+3（t1）で目標のすき間 0（体に食い込んでいる所は 0 までは押し出す）。viewer/skirt.py の fit_skirt と同じ
		const float t1 = top + 3.0f;
		const float t0 = butt.empty() ? top - 6.0f : (std::max)(top - 6.0f, buttTop + 1.0f);  // 揺れ骨が無ければ上端−6 から
		band.reserve(raws.size());
		for (auto& r : raws) {
			BandVert bv;
			bv.p = r.p;
			bv.n = r.n;
			bv.skin = r.skin;
			for (std::size_t k = 0; k < r.all.size() && k < bv.all.size(); ++k) {
				bv.all[k] = r.all[k];
			}
			for (auto& [node, w] : r.leaf) {
				for (std::size_t k = 0; k < butt.size(); ++k) {
					if (butt[k] == node) {
						bv.butt[k] += w;
					}
				}
			}
			band.push_back(bv);
		}
		std::sort(band.begin(), band.end(), [](auto& a, auto& b) { return a.p.z < b.p.z; });
		auto nearest = [&](const Skirt::Vec3& q) -> const BandVert* {
			const auto start = static_cast<std::ptrdiff_t>(
				std::lower_bound(band.begin(), band.end(), q.z, [](const BandVert& a, float z) { return a.p.z < z; }) - band.begin());
			float           best = 1e30f;
			const BandVert* bb = nullptr;
			auto            test = [&](std::ptrdiff_t k) {
                const auto& p = band[k].p;
                const float d = (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z);
                if (d < best) {
                    best = d;
                    bb = &band[k];
                }
			};
			for (std::ptrdiff_t k = start; k < static_cast<std::ptrdiff_t>(band.size()); ++k) {
				if ((band[k].p.z - q.z) * (band[k].p.z - q.z) >= best) {
					break;
				}
				test(k);
			}
			for (std::ptrdiff_t k = start - 1; k >= 0; --k) {
				if ((q.z - band[k].p.z) * (q.z - band[k].p.z) >= best) {
					break;
				}
				test(k);
			}
			return bb;
		};

		auto ctx = Context();
		for (auto geo : a_skirts) {
			auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
			auto part = skin ? skin->skinPartition.get() : nullptr;
			if (!part || !part->numPartitions || !skin->bones || !skin->skinData) {
				continue;
			}
			// B: お尻の揺れ骨と同じ名前の骨がスカートの骨の一覧にあるか（骨は足せないので、あるときだけ使う）
			const auto     boneCount = skin->skinData->GetBoneCount();
			std::vector<int> skirtBone(butt.size(), -1);
			std::string      found;
			for (std::size_t k = 0; k < butt.size(); ++k) {
				for (std::uint32_t i = 0; i < boneCount; ++i) {
					if (skin->bones[i] && skin->bones[i]->name == butt[k]->name) {
						skirtBone[k] = static_cast<int>(i);
						found += std::format("[{}] ", butt[k]->name.c_str());
						break;
					}
				}
			}
			const bool  useB = !found.empty();
			const float gapW = useB ? a_gap : a_gapNoBone;

			std::set<RE::BSGraphics::TriShape*> bufs;
			for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
				if (part->partitions[i].buffData) {
					bufs.insert(part->partitions[i].buffData);
				}
			}
			for (auto buf : bufs) {
				if (!buf->rawVertexData) {
					continue;
				}
				const std::uint32_t n = part->vertexCount;
				if (auto e = Verts::Owned(buf, n); e && e->fitted) {
					continue;  // もう離してある
				}
				auto&            ent = Verts::Remember(buf, n);  // 縮めたときに覚えた元をそのまま使う
				Verts::SealGuard seal{ buf, n };
				auto                desc = buf->vertexDesc;
				const auto          stride = desc.GetSize();
				auto*               raw = buf->rawVertexData;
				auto pos = [&](std::uint32_t v) { return reinterpret_cast<float*>(raw + static_cast<std::size_t>(v) * stride); };
					const auto fo = FrameOffset(geo);
				// 隣の頂点と区画
				std::vector<std::vector<std::uint32_t>> adj(n);
				std::vector<int>                        partOf(n, -1);
				for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
					auto& p = part->partitions[i];
					if (!p.buffData || !p.buffData->rawIndexData) {
						continue;
					}
					const auto* idx = p.buffData->rawIndexData;
					for (std::uint32_t t = 0; t < p.triangles; ++t) {
						const std::uint16_t tv[3] = { idx[t * 3], idx[t * 3 + 1], idx[t * 3 + 2] };
						if (tv[0] >= n || tv[1] >= n || tv[2] >= n) {
							continue;
						}
						for (int a = 0; a < 3; ++a) {
							partOf[tv[a]] = static_cast<int>(i);
							adj[tv[a]].push_back(tv[(a + 1) % 3]);
							adj[tv[a]].push_back(tv[(a + 2) % 3]);
						}
					}
				}
				// A: 帯の範囲（上端+3 まで）の頂点の、帯の体のいちばん近い頂点の法線の向きのすき間。足りない分だけ押し出す
				std::vector<float>           push(n, 0.0f), gap0(n, 1e9f);
				std::vector<const BandVert*> nb(n, nullptr);
				int                          dgRange = 0, dgDead = 0, dgSleeve = 0, dgEval = 0, dgInside = 0;
				float                        dgMinGap = 1e9f;
				for (std::uint32_t v = 0; v < n; ++v) {
					const float* f = pos(v);
					// 測るのは帯の上端まで。上端より上に帯の体は無い（上の頂点を下の体の頂点と比べると、食い込んでいると誤って押し出し、
					// 隣へ広がって腰の後ろが膨らんだ。イソルダの農民の服 04）
					if (f[2] + fo[2] > top) {
						continue;
					}
					++dgRange;
					if (ent.dead.size() == n && ent.dead[v]) {
						++dgDead;
						continue;  // 切り捨てた布（縁の頂点に重ねてある）は動かさない
					}
					const float sl = ent.sleeve.size() == n ? ent.sleeve[v] : 0.0f;
					if (sl >= 0.5f) {
						++dgSleeve;
						continue;  // 袖
					}
					const Skirt::Vec3 q{ f[0] + fo[0], f[1] + fo[1], f[2] + fo[2] };
					auto              b = nearest(q);
					if (!b) {
						continue;
					}
					nb[v] = b;
					Skirt::Vec3 dir = b->n;
					if (dir.x == 0.0f && dir.y == 0.0f && dir.z == 0.0f) {
						// 法線が無いときは体から頂点への向き
						dir = { q.x - b->p.x, q.y - b->p.y, q.z - b->p.z };
						const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
						dir = l > 1e-4f ? Skirt::Vec3{ dir.x / l, dir.y / l, dir.z / l } : Skirt::Vec3{ 0.0f, 0.0f, 0.0f };
					}
					const float g = (q.x - b->p.x) * dir.x + (q.y - b->p.y) * dir.y + (q.z - b->p.z) * dir.z;
					gap0[v] = g;
					++dgEval;
					dgMinGap = (std::min)(dgMinGap, g);
					dgInside += g < 0.0f;
					float want = t1 > t0 ? gapW * (std::clamp)((t1 - q.z) / (t1 - t0), 0.0f, 1.0f) : gapW;
					if (!b->skin) {
						// 下着（肌でない shape）は突き抜けない程度に覆うだけ。体と同じに離すと、飾りの所でスカートが横に膨らんだ（オルフィナ）
						want = (std::min)(want, 0.3f);
					}
					push[v] = (std::clamp)(want - g, 0.0f, gapW + 3.0f) * (1.0f - sl);
				}
				// 押し出し量を隣へ 0.6 倍ずつ広げる（大きいほうを取る）。段差・しわを防ぐ
				for (int iter = 0; iter < 4; ++iter) {
					auto next = push;
					for (std::uint32_t v = 0; v < n; ++v) {
						if (!nb[v]) {
							continue;
						}
						for (auto u : adj[v]) {
							next[v] = (std::max)(next[v], 0.6f * push[u]);
						}
					}
					push.swap(next);
				}
				logger::info("    押し出しの内訳: 頂点 {}、範囲内 {}、切り捨て {}、袖 {}、測った {}（体に食い込み {}、最小のすき間 {:.1f}）、帯の頂点 {}、上端 {:.1f}、目標 {:.1f}", n, dgRange, dgDead,
					dgSleeve, dgEval, dgInside, dgEval ? dgMinGap : 0.0f, band.size(), top, gapW);
				int   pushed = 0;
				float maxPush = 0.0f;
				for (std::uint32_t v = 0; v < n; ++v) {
					if (!nb[v] || push[v] <= 0.01f) {
						continue;
					}
					Skirt::Vec3 dir = nb[v]->n;
					if (dir.x == 0.0f && dir.y == 0.0f && dir.z == 0.0f) {
						continue;
					}
					float* f = pos(v);
					f[0] += dir.x * push[v];
					f[1] += dir.y * push[v];
					f[2] += dir.z * push[v];
					++pushed;
					maxPush = (std::max)(maxPush, push[v]);
				}
				// B: お尻を覆う頂点に、帯の体のいちばん近い頂点のお尻の揺れ骨の重みを混ぜる（すき間が狭かった所ほど強く。合計は 1 に戻す）
				int blended = 0;
				const auto offS = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
				if (useB && desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) && offS + 12 <= stride) {
					for (std::uint32_t v = 0; v < n; ++v) {
						if (!nb[v] || partOf[v] < 0) {
							continue;
						}
						const float s = (std::clamp)((gapW - gap0[v]) / gapW, 0.0f, 1.0f);
						if (s <= 0.0f) {
							continue;
						}
						auto&         p = part->partitions[partOf[v]];
						std::uint8_t* sk = raw + static_cast<std::size_t>(v) * stride + offS;
						float         w[4];
						std::uint8_t  li[4];
						for (int k = 0; k < 4; ++k) {
							std::uint16_t h;
							std::memcpy(&h, sk + k * 2, 2);
							w[k] = HalfToFloat(h);
							li[k] = sk[8 + k];
						}
						bool changed = false;
						for (std::size_t k = 0; k < butt.size(); ++k) {
							const float add = s * nb[v]->butt[k];
							if (skirtBone[k] < 0 || add <= 0.01f) {
								continue;
							}
							const int lj = skirtBone[k];  // 骨番号は皮の骨の一覧の番号そのもの
							if (lj < 0 || lj > 255) {
								continue;  // この区画にはその骨が無い
							}
							for (auto& x : w) {
								x *= (1.0f - add);
							}
							int slot = -1;
							for (int q = 0; q < 4; ++q) {
								if (li[q] == lj && w[q] > 0.0f) {
									slot = q;
								}
							}
							if (slot < 0) {
								slot = 0;
								for (int q = 1; q < 4; ++q) {
									if (w[q] < w[slot]) {
										slot = q;
									}
								}
								w[slot] = 0.0f;
								li[slot] = static_cast<std::uint8_t>(lj);
							}
							w[slot] += add;
							changed = true;
						}
						if (!changed) {
							continue;
						}
						const float total = w[0] + w[1] + w[2] + w[3];
						for (int k = 0; k < 4; ++k) {
							const std::uint16_t h = FloatToHalf(total > 0.0f ? w[k] / total : (k == 0 ? 1.0f : 0.0f));
							std::memcpy(sk + k * 2, &h, 2);
							sk[8 + k] = w[k] > 0.0f ? li[k] : 0;
						}
						++blended;
					}
				}
				// C: お尻の揺れ骨が無い服。体に近い頂点に、帯の体のいちばん近い頂点の重み（全部の骨）を混ぜる。
				// 体とスカートの重みが違うと、立ち姿勢などで骨が少し動いただけで体だけが外へ出る（イソルダの農民の服 04。重みの違いは平均 0.49）。
				// 体に触れている所（すき間 0 以下）は体と同じ重み、すき間 gapW+2 で混ぜない。上端の近くは押し出しと同じく弱める。
				// スカートの骨の一覧に無い骨は使わない（骨は足せない）
				int copied = 0;
				if (!useB && a_bodyWeights && desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) && offS + 12 <= stride) {
					std::map<std::string_view, int> idxOf;
					for (std::uint32_t i = 0; i < boneCount && i < 256; ++i) {
						if (skin->bones[i]) {
							idxOf.emplace(skin->bones[i]->name.c_str(), static_cast<int>(i));
						}
					}
					const float reach = gapW + 2.0f;
					for (std::uint32_t v = 0; v < n; ++v) {
						if (!nb[v] || partOf[v] < 0 || gap0[v] > 1e8f) {
							continue;
						}
						const float z = pos(v)[2] + fo[2];
						const float fade = t1 > t0 ? (std::clamp)((t1 - z) / (t1 - t0), 0.0f, 1.0f) : 1.0f;
						const float s = (std::clamp)(1.0f - (std::max)(gap0[v], 0.0f) / reach, 0.0f, 1.0f) * fade;
						if (s <= 0.01f) {
							continue;
						}
						// 体の重みをスカートの骨の番号に直す（無い骨は落として、残りで 1 に）
						std::pair<int, float> bw[4];
						int                   nbw = 0;
						float                 bsum = 0.0f;
						for (auto& [node, w] : nb[v]->all) {
							if (!node || w <= 0.0f) {
								continue;
							}
							auto f = idxOf.find(node->name.c_str());
							if (f == idxOf.end()) {
								continue;
							}
							bw[nbw++] = { f->second, w };
							bsum += w;
						}
						if (!nbw || bsum < 0.5f) {
							continue;  // 体の重みの大半がスカートに無い骨
						}
						std::uint8_t* sk = raw + static_cast<std::size_t>(v) * stride + offS;
						std::map<int, float> mix;
						for (int k = 0; k < 4; ++k) {
							std::uint16_t h;
							std::memcpy(&h, sk + k * 2, 2);
							const float w = HalfToFloat(h);
							if (w > 0.0f) {
								mix[sk[8 + k]] += (1.0f - s) * w;
							}
						}
						for (int k = 0; k < nbw; ++k) {
							mix[bw[k].first] += s * bw[k].second / bsum;
						}
						std::vector<std::pair<float, int>> top4;
						for (auto& [i, w] : mix) {
							top4.emplace_back(w, i);
						}
						std::sort(top4.begin(), top4.end(), std::greater<>());
						top4.resize((std::min)(top4.size(), std::size_t{ 4 }));
						float total = 0.0f;
						for (auto& [w, i] : top4) {
							total += w;
						}
						if (total <= 0.0f) {
							continue;
						}
						for (int k = 0; k < 4; ++k) {
							const bool has = k < static_cast<int>(top4.size());
							const std::uint16_t h = FloatToHalf(has ? top4[k].first / total : 0.0f);
							std::memcpy(sk + k * 2, &h, 2);
							sk[8 + k] = has ? static_cast<std::uint8_t>(top4[k].second) : 0;
						}
						++copied;
					}
				}
				if (!useB) {
					logger::info("    体の重みを写した頂点 {} 個（{}）", copied, a_bodyWeights ? "体に近いほど強く" : "body_weights=0 なので写さない");
				}
				ent.fitted = true;
				D3D11_BUFFER_DESC bd{};
				auto              vb = reinterpret_cast<ID3D11Buffer*>(buf->vertexBuffer);
				if (vb) {
					vb->GetDesc(&bd);
				}
				if (vb && ctx && bd.Usage == D3D11_USAGE_DEFAULT) {
					ctx->UpdateSubresource(vb, 0, nullptr, raw, 0, 0);
				}
				logger::info("  スカート: [{}] 帯から {:.1f} 以上離す（z {:.1f} から上端+3 へ弱める）: 押し出した頂点 {} 個（最大 {:.1f}）、お尻の揺れ骨 {} → {}（重みを混ぜた頂点 {} 個）",
					geo->name.c_str(), gapW, t0, pushed, maxPush, buttNames.empty() ? "見つからない" : buttNames,
					useB ? std::format("スカートにある {}", found) : std::string("スカートの骨の一覧に無い"), blended);
			}
		}
	}
}
