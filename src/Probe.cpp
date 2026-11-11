// 実験: 女性 NPC の 3D が読み込まれたら、着ている物のスキン頂点（NiSkinPartition）を読み、
// スカートと判定したら腰より下を縮めて書き戻す。何が読めて何が書けるかを MiniSkirt.log に出す。

#include "PCH.h"

#include "Band.h"
#include "DevFlags.h"
#include "Layout.h"
#include "Skirt.h"
#include "Underwear.h"
#include "Verts.h"

#include <d3d11.h>
#include <set>

#include <chrono>
#include <deque>
#include <mutex>
#include <fstream>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <functional>
#include <unordered_map>

namespace Probe
{
	namespace
	{
		struct Cfg
		{
			bool  apply{ true };
			bool  enabled{ true };         // MCM の「有効」。無効にすると全員を元のロングに戻して帯を外す
			float waist{ 70.0f };
			float lengthCm{ 22.0f };       // スカート丈（ウエストバンドの上端からすそまで、cm。標準の体での目安）
			float hem{ 65.0f };            // 縮めたあとのすその高さ（足元 0・頭のてっぺん ≒ 128）。lengthCm から求める
			float hipEase{ 1.0f };         // A ライン: お尻まわりのゆとり（帯の体のいちばん外からの幅）
			float flareAngle{ 5.0f };      // A ライン: お尻の線より下の広がり角度（度。0 でまっすぐ下）
			float bandTop{ 78.0f };      // 帯（足首から）の上端。全 NPC 共通（体 nif ごとにファイル 1 個）
			bool  remapWeights{ true };  // 縮めた頂点の骨の重みを、動かした先の高さの重みに写し替える
			float skirtGap{ 1.0f };        // スカートを帯の体から離す幅（スカートにお尻の揺れ骨があるとき）
			float skirtGapNoBone{ 1.0f };  // 同（お尻の揺れ骨が無く、お尻の揺れに追従できないとき）
			bool  freezeJiggle{ false };   // 帯の揺れ骨を止める（最後の手段。全 NPC 共通）
			bool  underwear{ true };       // スカートの中に下着を着ける（帯の元を下着つきの体にする）
			bool  bodyWeights{ true };     // お尻の揺れ骨が無い服で、体に近いスカートの頂点に体の重みを混ぜる（姿勢でお尻が突き抜けない）
			bool  weldCeiling{ true };     // スカートの中の「下向きの水平な面」（覗くと黒い天井になる）を面積 0 にする
			bool  twoSided{ true };        // スカートの shape を両面描画にする（裏側の布が見えるように）
			bool  cutCloth{ true };        // 裾より下の布を縮めず切る（縮めると布が腰に溜まり、覗くと黒い塊になる）
		};
		Cfg g_cfg;

		std::string Trim(std::string a_s)
		{
			const auto b = a_s.find_first_not_of(" \t\r");
			const auto e = a_s.find_last_not_of(" \t\r");
			return b == std::string::npos ? std::string() : a_s.substr(b, e - b + 1);
		}

		// 「キー=値」の ini を読む（[節] と ; のコメントは飛ばす）。読めたら true
		bool ReadIni(const char* a_path, const std::function<void(const std::string&, const std::string&)>& a_fn)
		{
			std::ifstream f(a_path);
			if (!f) {
				return false;
			}
			std::string line;
			while (std::getline(f, line)) {
				line = Trim(line);
				if (line.empty() || line[0] == ';' || line[0] == '[') {
					continue;
				}
				const auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				a_fn(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
			}
			return true;
		}

		float F(const std::string& a_v) { return static_cast<float>(std::atof(a_v.c_str())); }

		// スカート丈（cm）→ すその高さ。物差しは身長 165 cm（= 頭のてっぺん 128）の標準の体で、ウエストバンドの上端を高さ 78 とする。
		// 1 単位 ≒ 1.29 cm。目安: 16 cm → 65.6（お尻がぎりぎり隠れる）、40 → 47（太ももの真ん中）、45 → 43（ひざの少し上）、52 → 38（ひざ）
		constexpr float kLengthMinCm = 16.0f;
		constexpr float kLengthMaxCm = 55.0f;
		float           HemFromLength(float a_cm)
		{
			constexpr float kWaistbandZ = 78.0f;
			constexpr float kCmPerUnit = 165.0f / 128.0f;
			return kWaistbandZ - (std::clamp)(a_cm, kLengthMinCm, kLengthMaxCm) / kCmPerUnit;
		}
		bool  B(const std::string& a_v) { return std::atoi(a_v.c_str()) != 0 || a_v == "true" || a_v == "True"; }

		// MCM Helper がユーザーの変えた値を書くファイル（MCM が無い環境では無い）
		const char* kMcmSettings = "Data/MCM/Settings/MiniSkirt.ini";

		// 既定の ini（SKSE\Plugins\MiniSkirt.ini）を読み、そのあと MCM の設定ファイルがあれば上書きする
		void LoadCfg()
		{
			g_cfg = Cfg{};
			ReadIni("Data/SKSE/Plugins/MiniSkirt.ini", [](const std::string& key, const std::string& val) {
				if (key == "apply") {
					g_cfg.apply = B(val);
				} else if (key == "waist") {
					g_cfg.waist = F(val);
				} else if (key == "skirt_length_cm") {
					g_cfg.lengthCm = F(val);
				} else if (key == "hip_ease") {
					g_cfg.hipEase = (std::max)(0.0f, F(val));
				} else if (key == "flare_angle") {
					g_cfg.flareAngle = (std::clamp)(F(val), 0.0f, 45.0f);
				} else if (key == "remap_weights") {
					g_cfg.remapWeights = B(val);
				} else if (key == "band_top") {
					g_cfg.bandTop = F(val);
				} else if (key == "skirt_gap") {
					g_cfg.skirtGap = F(val);
				} else if (key == "skirt_gap_nobone") {
					g_cfg.skirtGapNoBone = F(val);
				} else if (key == "band_freeze_jiggle") {
					g_cfg.freezeJiggle = B(val);
				} else if (key == "underwear") {
					g_cfg.underwear = B(val);
				} else if (key == "two_sided") {
					g_cfg.twoSided = B(val);
				} else if (key == "body_weights") {
					g_cfg.bodyWeights = B(val);
				} else if (key == "weld_ceiling") {
					g_cfg.weldCeiling = B(val);
				} else if (key == "cut_cloth") {
					g_cfg.cutCloth = B(val);
				}
			});
			const bool mcm = ReadIni(kMcmSettings, [](const std::string& key, const std::string& val) {
				if (key == "bEnabled") {
					g_cfg.enabled = B(val);
				} else if (key == "fSkirtLength") {
					g_cfg.lengthCm = F(val);
				} else if (key == "fHipEase") {
					g_cfg.hipEase = (std::max)(0.0f, F(val));
				} else if (key == "fFlareAngle") {
					g_cfg.flareAngle = (std::clamp)(F(val), 0.0f, 45.0f);
				} else if (key == "fBandTop") {
					g_cfg.bandTop = F(val);
				} else if (key == "fSkirtGap") {
					g_cfg.skirtGap = F(val);
				} else if (key == "fSkirtGapNoBone") {
					g_cfg.skirtGapNoBone = F(val);
				} else if (key == "bUnderwear") {
					g_cfg.underwear = B(val);
				}
			});
			g_cfg.lengthCm = (std::clamp)(g_cfg.lengthCm, kLengthMinCm, kLengthMaxCm);
			g_cfg.hem = HemFromLength(g_cfg.lengthCm);
			logger::info("設定: 有効={} スカート丈 {} cm（すその高さ hem={:.1f}）、お尻のゆとり hip_ease={}、広がり角度 flare_angle={}°、下着を着ける={}（MCM の設定ファイル {}）", g_cfg.enabled,
				g_cfg.lengthCm, g_cfg.hem, g_cfg.hipEase, g_cfg.flareAngle,
				g_cfg.underwear, mcm ? "あり" : "なし");
			logger::info("設定: apply={} waist={} hem={} band_top={} remap_weights={} skirt_gap={} skirt_gap_nobone={} band_freeze_jiggle={}", g_cfg.apply,
				g_cfg.waist, g_cfg.hem, g_cfg.bandTop, g_cfg.remapWeights, g_cfg.skirtGap, g_cfg.skirtGapNoBone, g_cfg.freezeJiggle);
		}

		// 書き換え済みの頂点バッファ（二重に縮めない）
		bool                  g_shortened = false;  // いま処理中の NPC で縮めたか（判定だけの回では、スカートがあるか）
		bool                  g_dry = false;        // 判定だけの回（縮めない。帯を用意できてから縮める）
		bool                  g_pants = false;      // 肌でない shape で脚が分かれている物（ズボン・脚甲）があったか。あれば、スカートの中が素肌でない（下にズボンをはいている）ので対象外。服も鎧も
		bool                  g_armorMode = false;  // いま処理中の NPC の体スロットが鎧（MCM で鎧も対象にしているとき）。判定の広がりを問わない
		std::vector<RE::BSGeometry*> g_skirts;     // いま処理中の NPC のスカートの shape
		float                        g_hemNew = 1e9f;  // いま処理中の NPC の縮めたあとの裾の高さ（いちばん低いもの）
		// 判定だけの回にかかった時間（マイクロ秒）: 袖の見分け・形の判定・ズボンの検出。重さを確かめるためにログに出す
		long long g_usSleeve = 0, g_usAnalyze = 0, g_usTubes = 0;
		auto      UsSince(std::chrono::steady_clock::time_point a_t0)
		{
			return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - a_t0).count();
		}


		// 縮めて動いた頂点の皮の重みを、動かした先の高さに合わせる（viewer/skirt.py の remap_weights）。
		// 動いた頂点ごとに、縮める前の頂点のうち新しい位置にいちばん近い頂点の重みと骨を写す。
		// 頂点の骨番号は区画ごとの骨の一覧（Partition::bones）の番号なので、写す元と先の区画が違えば番号を付け替える。
		// 付け替えられない頂点（先の区画にその骨が無い）は元のまま
		void RemapWeights(RE::BSGraphics::TriShape* a_buf, RE::NiSkinPartition* a_part, const std::vector<Skirt::Vec3>& a_pos0,
			const std::vector<Skirt::Vec3>& a_pos1, const std::vector<float>& a_sleeve)
		{
			auto       desc = a_buf->vertexDesc;
			const auto stride = desc.GetSize();
			if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED)) {
				logger::info("    重み: 皮の重みが無い頂点なので写さない");
				return;
			}
			const auto off = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
			const auto n = static_cast<std::uint32_t>(a_pos0.size());
			if (off + 12 > stride) {
				logger::info("    重み: 重みの位置 {} が頂点の大きさ {} を超えるので写さない", off, stride);
				return;
			}
			// 頂点 → 区画（区画ごとの面の番号から）
			std::vector<int> partOf(n, -1);
			for (std::uint32_t i = 0; i < a_part->numPartitions; ++i) {
				auto& p = a_part->partitions[i];
				if (!p.buffData || !p.buffData->rawIndexData || !p.bones) {
					logger::info("    重み: 区画{} の面か骨の一覧が無いので写さない", i);
					return;
				}
				for (std::uint32_t t = 0; t < static_cast<std::uint32_t>(p.triangles) * 3; ++t) {
					const auto v = p.buffData->rawIndexData[t];
					if (v < n) {
						partOf[v] = static_cast<int>(i);
					}
				}
			}
			// 元の重み（写している途中で元が書き換わらないように）
			auto* raw = a_buf->rawVertexData;
			std::vector<std::uint8_t> orig(static_cast<std::size_t>(n) * 12);
			for (std::uint32_t v = 0; v < n; ++v) {
				std::memcpy(&orig[static_cast<std::size_t>(v) * 12], raw + static_cast<std::size_t>(v) * stride + off, 12);
			}
			// 縮める前の位置を高さ順に並べ、近い頂点を高さの近い方から探す
			std::vector<std::uint32_t> order(n);
			for (std::uint32_t v = 0; v < n; ++v) {
				order[v] = v;
			}
			std::sort(order.begin(), order.end(), [&](auto a, auto b) { return a_pos0[a].z < a_pos0[b].z; });
			std::vector<float> zs(n);
			for (std::uint32_t k = 0; k < n; ++k) {
				zs[k] = a_pos0[order[k]].z;
			}
			int moved = 0, copied = 0, failed = 0, noPart = 0;
			for (std::uint32_t v = 0; v < n; ++v) {
				if (std::fabs(a_pos1[v].z - a_pos0[v].z) <= 0.01f || (a_sleeve.size() == n && a_sleeve[v] > 0.01f)) {
					continue;
				}
				++moved;
				const auto& q = a_pos1[v];
				const auto  start = static_cast<std::ptrdiff_t>(std::lower_bound(zs.begin(), zs.end(), q.z) - zs.begin());
				float       best = 1e30f;
				std::int64_t bu = -1;
				for (std::ptrdiff_t k = start; k < static_cast<std::ptrdiff_t>(n); ++k) {
					const float dz = zs[k] - q.z;
					if (dz * dz >= best) {
						break;
					}
					const auto& p = a_pos0[order[k]];
					const float d = (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + dz * dz;
					if (d < best) {
						best = d;
						bu = order[k];
					}
				}
				for (std::ptrdiff_t k = start - 1; k >= 0; --k) {
					const float dz = q.z - zs[k];
					if (dz * dz >= best) {
						break;
					}
					const auto& p = a_pos0[order[k]];
					const float d = (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + dz * dz;
					if (d < best) {
						best = d;
						bu = order[k];
					}
				}
				if (bu < 0) {
					continue;
				}
				const auto u = static_cast<std::uint32_t>(bu);
				if (partOf[u] < 0 || partOf[v] < 0) {
					++noPart;
					continue;
				}
				auto&         pu = a_part->partitions[partOf[u]];
				auto&         pv = a_part->partitions[partOf[v]];
				const auto*   src = &orig[static_cast<std::size_t>(u) * 12];
				std::uint8_t  out[12];
				std::memcpy(out, src, 12);
				bool ok = true;
				for (int k = 0; k < 4 && ok; ++k) {
					std::uint16_t w;
					std::memcpy(&w, src + k * 2, 2);
					const std::uint8_t li = src[8 + k];
					if ((w & 0x7FFF) == 0) {
						out[8 + k] = 0;  // 重み 0 の枠は番号を使わない
						continue;
					}
					if (true) {  // 骨番号は皮の骨の一覧の番号そのもの（区画ごとではない）なので付け替えない
						continue;
					}
					if (li >= pu.numBones) {
						ok = false;
						break;
					}
					const auto g = pu.bones[li];
					int        lj = -1;
					for (std::uint16_t j = 0; j < pv.numBones; ++j) {
						if (pv.bones[j] == g) {
							lj = j;
							break;
						}
					}
					if (lj < 0 || lj > 255) {
						ok = false;
						break;
					}
					out[8 + k] = static_cast<std::uint8_t>(lj);
				}
				if (!ok) {
					++failed;
					continue;
				}
				std::memcpy(raw + static_cast<std::size_t>(v) * stride + off, out, 12);
				++copied;
			}
			logger::info("    重み: 動いた頂点 {} 個のうち {} 個に近い頂点の重みを写した（骨を付け替えられず元のまま {}、区画不明 {}）", moved, copied, failed, noPart);
		}

		void ProcessGeometry(RE::BSGeometry* a_geo, const std::string& a_who)
		{
			auto& rd = a_geo->GetGeometryRuntimeData();
			auto  skin = rd.skinInstance.get();
			const std::string name = a_geo->name.c_str();
			if (!skin || !skin->skinPartition) {
				logger::info("  [{}] 皮なし（または区画なし）", name);
				return;
			}
			if (std::string why; !Layout::Check(skin, why)) {
				logger::info("  [{}] 皮の構造体が前提と合わないので処理しない（{}）", name, why);
				return;
			}
			auto part = skin->skinPartition.get();
			if (g_dry) logger::info("  [{}] 区画 {} 個、頂点 {}、rendererData={}", name, part->numPartitions, part->vertexCount,
				static_cast<const void*>(rd.rendererData));

			// 区画は同じ頂点バッファを共有しているので、重複を除く
			std::set<RE::BSGraphics::TriShape*> bufs;
			for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
				auto& p = part->partitions[i];
				if (p.buffData) {
					bufs.insert(p.buffData);
				}
			}
			for (auto buf : bufs) {
				auto  desc = buf->vertexDesc;
				const auto stride = desc.GetSize();
				D3D11_BUFFER_DESC bd{};
				auto              vb = reinterpret_cast<ID3D11Buffer*>(buf->vertexBuffer);
				if (vb) {
					vb->GetDesc(&bd);
				}
				if (g_dry) logger::info("    バッファ {}: stride={} 頂点の写し(CPU)={} vb={} usage={} cpuAccess={} byteWidth={} flags=0x{:X}",
					static_cast<const void*>(buf), stride, static_cast<const void*>(buf->rawVertexData), static_cast<const void*>(vb),
					static_cast<int>(bd.Usage), bd.CPUAccessFlags, bd.ByteWidth, static_cast<unsigned>(desc.GetFlags()));
				if (!buf->rawVertexData || !stride) {
					logger::info("    → CPU 側の頂点が無いので読めない");
					continue;
				}
				const std::uint32_t n = part->vertexCount ? part->vertexCount : (bd.ByteWidth / stride);
				// shape 自身の移動を持つ服（頂点が局所座標）は、足元 0 の座標に直して見る。書き戻すときに引く
				const auto               fo = Band::FrameOffset(a_geo);
				std::vector<Skirt::Vec3> pos(n);
				for (std::uint32_t v = 0; v < n; ++v) {
					const float* f = reinterpret_cast<const float*>(buf->rawVertexData + static_cast<std::size_t>(v) * stride);
					pos[v] = { f[0] + fo[0], f[1] + fo[1], f[2] + fo[2] };
				}
				// 袖（垂れた袖を含む）は判定・縮め・広げ・押し出しから外す。書き換え済みなら、そのとき求めた物を使う
				std::vector<float> sleeve;
				const auto         tSleeve = std::chrono::steady_clock::now();
				if (auto e = Verts::Owned(buf, n); e && e->sleeve.size() == n) {
					sleeve = e->sleeve;
					if (e->dead.size() == n) {
						for (std::uint32_t v = 0; v < n; ++v) {
							sleeve[v] = e->dead[v] ? 1.0f : sleeve[v];  // 消した頂点（NaN）は判定に入れない
						}
					}
				} else {
					sleeve = Band::SleeveMask(a_geo, buf, g_cfg.waist);
				}
				if (sleeve.size() != n) {
					sleeve.assign(n, 0.0f);
				}
				if (g_dry) {
					g_usSleeve += UsSince(tSleeve);
				}
				std::vector<Skirt::Vec3> body;
				body.reserve(n);
				for (std::uint32_t v = 0; v < n; ++v) {
					if (sleeve[v] < 0.5f) {
						body.push_back(pos[v]);
					}
				}
				// 同じ頂点バッファを別の NPC と共有している（先に処理されて切られている）: 切ったあとの形は判定できない（短くなっていてスカートと見なされない）ので、
				// 縮めたスカートとして扱う。帯はこの NPC の 3D に付ける必要がある
				if (auto e = Verts::Owned(buf, n); e && e->shortened && e->trim && g_cfg.apply) {
					if (!g_dry) {
						logger::info("    → 裾の飾りは書き換え済み（別の NPC と共有している頂点バッファ）");
					}
					continue;  // 全部切ってある。スカートとしては扱わない
				}
				if (auto e = Verts::Owned(buf, n); e && e->shortened && g_cfg.apply) {
					if (std::find(g_skirts.begin(), g_skirts.end(), a_geo) == g_skirts.end()) {
						g_skirts.push_back(a_geo);
					}
					if (!g_dry) {
						logger::info("    → 書き換え済み（別の NPC と共有している頂点バッファ）");
						g_hemNew = (std::min)(g_hemNew, e->hemNew);
					}
					g_shortened = true;
					continue;
				}
				// 鎧のスカート状の部品は、まっすぐ垂れて広がらないことがあるので、広がりは問わない（裾の輪がつながっている＝ズボンでない、は同じに見る）
				const auto tAnalyze = std::chrono::steady_clock::now();
				auto       res = Skirt::Analyze(body, g_cfg.waist, g_armorMode ? 0.0f : 1.05f);
				if (g_dry) {
					g_usAnalyze += UsSince(tAnalyze);
					logger::info("    判定: {}（裾 z={:.1f}、広がり {:.2f}）", res.reason, res.hemZ, res.flare);
				}
				auto       pr0 = a_geo->GetGeometryRuntimeData().shaderProperty.get();
				const bool isSkin = pr0 && pr0->material && pr0->material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint;
				// スカート状の部品があるだけでなく、下半身にズボン状の物（肌でない shape で脚が分かれている）が無いことも条件にする（服も鎧も。
				// 物乞いのローブのように、スカートの下にズボンをはいている服は、短くしても脚は布のままで、中に素肌が無い）
				// 脚を筒のように包んでいる物だけをズボンとみなす（左右に垂れた薄いマントの布は脚が分かれて見えるが、ズボンではない）
				if (g_dry && res.reason.rfind("脚が分かれている", 0) == 0) {
					const bool isSkinShape = isSkin;
					if (!isSkinShape && !res.wrapsLegs) {
						logger::info("    [{}] は脚が分かれて見えるが、脚を包んでいない（垂れた布など）→ ズボンとはみなさない", name);
					} else if (!isSkinShape) {
						g_pants = true;
						logger::info("    {}: [{}] は肌でなく脚が分かれている（ズボン状）→ この{}は対象外", g_armorMode ? "鎧" : "服", name, g_armorMode ? "鎧" : "服");
					}
				}
				// スカートと判定された shape でも、中に片脚ずつ囲む筒（ズボン）があれば対象外（首長の服 02・サルモールのローブは、
				// ドレスとズボンが 1 つの shape。体重によっては裾の布で輪がつながって見え、スカートと判定される）。
				// 区画ごとの頂点の写しは同じ中身なので、1 つの shape で 1 回だけ見る。
				// すでにズボンと分かっている時と、形の判定で早く外れた shape（腰まで届かない・腰より下が短いなど。筒も作れない）は見ない
				const bool early = res.reason.rfind("頂点なし", 0) == 0 || res.reason.rfind("腰より下の頂点が少ない", 0) == 0 ||
				                   res.reason.rfind("腰から裾までが短い", 0) == 0 || res.reason.rfind("腰まで届かない", 0) == 0;
				if (g_dry && !isSkin && !g_pants && !early && buf == *bufs.begin()) {
					const auto                                              tTubes = std::chrono::steady_clock::now();
					thread_local std::vector<std::array<std::uint32_t, 3>> tris;  // 確保し直さず使い回す
					tris.clear();
					for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
						auto& p = part->partitions[i];
						if (!p.buffData || !p.buffData->rawIndexData) {
							continue;
						}
						const auto* ix = p.buffData->rawIndexData;
						for (std::uint32_t t = 0; t < p.triangles; ++t) {
							const std::uint32_t a = ix[t * 3], b = ix[t * 3 + 1], c = ix[t * 3 + 2];
							if (a < n && b < n && c < n && sleeve[a] < 0.5f && sleeve[b] < 0.5f && sleeve[c] < 0.5f) {
								tris.push_back({ a, b, c });
							}
						}
					}
					const bool tubes = Skirt::LegTubes(pos, tris, g_cfg.waist);
					g_usTubes += UsSince(tTubes);
					if (tubes) {
						g_pants = true;
						logger::info("    {}: [{}] の中に片脚ずつ囲む筒がある（ズボン）→ この{}は対象外", g_armorMode ? "鎧" : "服", name, g_armorMode ? "鎧" : "服");
					}
				}
				// 鎧の裾の飾り（毛皮の縁取りなど）: 腰まで届かず、まるごと切り線（裾の目標）より下にある、肌でない輪の部品。
				// スカートだけ切ると、切った裾の下に取り残されて宙に浮くので、スカートと同じ切り方で全部消す。
				// 脚が分かれた部品（脚甲）なら、ズボン状なので、鎧ごと対象外にする（膝より上まである物だけ。ブーツは数えない）
				bool trim = false;
				if (g_armorMode && g_cfg.apply && !res.isSkirt && res.reason.rfind("腰まで届かない", 0) == 0 && !body.empty()) {
					auto pr = a_geo->GetGeometryRuntimeData().shaderProperty.get();
					const bool isSkinShape = pr && pr->material && pr->material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint;
					if (!isSkinShape) {
						float maxZ = -1e9f;
						for (auto& p : body) {
							maxZ = (std::max)(maxZ, p.z);
						}
						const auto r2 = Skirt::Analyze(body, maxZ + 6.0f, 0.0f);  // 自分の上端を腰とみなして、輪か（つながっているか）、脚が分かれているかを見る
						if (r2.isSkirt && maxZ < g_cfg.hem) {
							trim = true;
							if (g_dry) logger::info("    鎧: [{}] は裾の飾り（輪、上端 z={:.1f} が切り線 {:.0f} より下）→ スカートと同じに切って消す", name, maxZ, g_cfg.hem);
						} else if (g_dry && maxZ >= 45.0f && r2.reason.rfind("脚が分かれている", 0) == 0) {
							g_pants = true;
							logger::info("    鎧: [{}] は肌でなく脚が分かれている（脚甲など、上端 z={:.1f}）→ この鎧は対象外", name, maxZ);
						}
					}
				}
				if ((!res.isSkirt && !trim) || !g_cfg.apply) {
					continue;
				}
				if (!trim && std::find(g_skirts.begin(), g_skirts.end(), a_geo) == g_skirts.end()) {
					g_skirts.push_back(a_geo);  // 帯を付けたあとで、帯の体から離す
				}
				if (g_dry) {
					if (!trim) {
						g_shortened = true;  // 判定だけの回: スカートがある（縮めるのは帯を用意できてから）
					}
					continue;
				}
				if (auto e = Verts::Owned(buf, n); e && e->shortened) {
					logger::info("    → 書き換え済み");
					g_hemNew = (std::min)(g_hemNew, e->hemNew);
					g_shortened = true;  // 縮めたスカートを着ている（帯が外れていれば付け直す）
					continue;
				}
				auto&      ent = Verts::Remember(buf, n);  // 元を覚える（設定を変えたときに元から作り直す）
				// 切ると内側の布が無くなり、外側の布の裏が描かれない（カリング）ので、スカートの shape を両面描画にする
				if (g_cfg.twoSided && !trim) {
					if (auto prop = a_geo->GetGeometryRuntimeData().shaderProperty.get()) {
						if (!prop->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided)) {
							prop->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
							logger::info("    [{}] 両面描画にした", a_geo->name.c_str());
						}
					}
				}
				if (g_cfg.weldCeiling && !trim && !g_armorMode) {  // 鎧は革の板が重なっていて、下向きの面も見える所にある（消すと穴になる）
					Band::WeldCeiling(a_geo, buf, g_cfg.waist, -0.7f, sleeve);  // 縮める前の向きで決める
				}
				float hemNew;
				if (g_cfg.cutCloth) {
					// 縮めず、裾より下の布を切る（重みは動かさないので写し替えも要らない）。CPU 側の頂点は CutCloth が直接書く
					hemNew = Band::CutCloth(a_geo, buf, g_cfg.hem, sleeve, ent.dead);
					pos.clear();
				} else {
					const auto pos0 = pos;
					hemNew = Skirt::Shorten(pos, g_cfg.waist, g_cfg.hem, &sleeve);
					if (g_cfg.remapWeights && fo[0] == 0.0f && fo[1] == 0.0f && fo[2] == 0.0f) {  // 座標系がずれた服では位置で頂点を引けない
						RemapWeights(buf, part, pos0, pos, sleeve);  // 重みは縦に縮めた位置で選ぶ（広げる前）
					}
				}
				if (!trim) {
					g_hemNew = (std::min)(g_hemNew, hemNew);  // 裾の飾りは全部消えるので、裾の高さには入れない
				}
				ent.sleeve = sleeve;
				// 形（A ライン）は、全部のスカートを縮めたあとで Band::ShapeSkirt がまとめて作る。すき間の押し出し（Band::FitSkirt）はそのあと
				logger::info("    → 裾 z={:.1f} → {:.1f}", res.hemZ, hemNew);
				for (std::uint32_t v = 0; v < pos.size(); ++v) {
					float* f = reinterpret_cast<float*>(buf->rawVertexData + static_cast<std::size_t>(v) * stride);
					f[0] = pos[v].x - fo[0];
					f[1] = pos[v].y - fo[1];
					f[2] = pos[v].z - fo[2];
				}
				ent.shortened = true;
				ent.trim = trim;
				ent.hemNew = trim ? g_cfg.hem : hemNew;
				Verts::Seal(buf, n);
				g_shortened = true;
				// GPU 側へ反映（新しい頂点バッファを作って差し替える）
				if (!vb) {
					logger::info("    → CPU 側だけ書き換えた（GPU の頂点バッファが無い）");
					continue;
				}
				logger::info("    → {}", Verts::Upload(buf, n, part) ? "GPU の頂点バッファを作り直して差し替えた" : "GPU に反映できない");
			}
		}

		void Walk(RE::NiAVObject* a_obj, const std::string& a_who)
		{
			if (!a_obj) {
				return;
			}
			if (auto geo = a_obj->AsGeometry()) {
				ProcessGeometry(geo, a_who);
			}
			if (auto node = a_obj->AsNode()) {
				for (auto& c : node->GetChildren()) {
					Walk(c.get(), a_who);
				}
			}
		}

		std::set<RE::FormID>      g_skirtActors;  // 縮めた（帯を付けた）NPC。3D が作り直されたら、時間を置かずにもう一度処理する
		std::set<RE::FormID>      g_seen;   // 処理した（スカートを縮めうる）女性 NPC。設定を変えたときに作り直す
		int                       g_gen = 0;  // 設定の世代（ゲーム中に設定を変えるたびに 1 増やす）
		std::map<RE::FormID, int> g_genOf;  // NPC ごとに、最後に処理した設定の世代

		// NPC の体スロットの服で自分が書き換えた頂点（スカート・服の脚）を、覚えておいた元に戻す。戻した頂点の写しの数
		int RestoreActor(RE::Actor* a_actor)
		{
			auto& biped = a_actor->GetCurrentBiped();
			if (!biped) {
				return 0;
			}
			int                                  n = 0;
			std::function<void(RE::NiAVObject*)> walk = [&](RE::NiAVObject* a) {
				if (!a) {
					return;
				}
				if (auto geo = a->AsGeometry()) {
					n += Verts::RestoreGeometry(geo);
				}
				if (auto node = a->AsNode()) {
					for (auto& c : node->GetChildren()) {
						walk(c.get());
					}
				}
			};
			walk(biped->objects[RE::BIPED_OBJECT::kBody].partClone.get());
			return n;
		}

		// 服の皮付き shape の頂点に、前の書き換えの痕（記録に無いのに NaN の頂点）が残っていれば、同じ服の全部の shape を覚えておいた元に戻す
		void RefreshOutfit(RE::NiAVObject* a_root, const std::string& a_who, std::uint32_t a_actor)
		{
			std::vector<std::pair<RE::BSGeometry*, RE::BSGraphics::TriShape*>> bufs;
			std::function<void(RE::NiAVObject*)> walk = [&](RE::NiAVObject* a) {
				if (!a) {
					return;
				}
				if (auto geo = a->AsGeometry()) {
					auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
					auto part = skin ? skin->skinPartition.get() : nullptr;
					if (part) {
						// 区画ごとに頂点の写し（TriShape）が別のことがある（GPU の頂点バッファは共有）。全部を見る
						std::set<RE::BSGraphics::TriShape*> seen;
						for (std::uint32_t i = 0; i < part->numPartitions; ++i) {
							if (auto b = part->partitions[i].buffData; b && seen.insert(b).second) {
								bufs.emplace_back(geo, b);
							}
						}
					}
				}
				if (auto node = a->AsNode()) {
					for (auto& c : node->GetChildren()) {
						walk(c.get());
					}
				}
			};
			walk(a_root);
			int restored = 0;
			for (auto& [geo, buf] : bufs) {
				restored += Verts::Refresh(geo, buf, a_actor);
			}
			if (restored) {
				logger::info("  {}: 3D が作り直されて消した頂点（NaN・1 点に集めた頂点）が残っていたので、覚えておいた元に戻した（頂点 {} 個）", a_who, restored);
			}
		}

		// 服の頂点バッファに、自分が切った頂点（位置が NaN）が残っているか。残っていれば縮めた服（元に戻せなかった物を含む）
		bool HasCutBuffers(RE::NiAVObject* a_root)
		{
			bool found = false;
			std::function<void(RE::NiAVObject*)> walk = [&](RE::NiAVObject* a) {
				if (!a || found) {
					return;
				}
				if (auto geo = a->AsGeometry()) {
					auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
					auto part = skin ? skin->skinPartition.get() : nullptr;
					if (part && part->numPartitions) {
						auto b = part->partitions[0].buffData;
						if (b && b->rawVertexData && part->vertexCount && Verts::HasNaN(b, part->vertexCount)) {
							found = true;
						}
					}
				}
				if (auto node = a->AsNode()) {
					for (auto& c : node->GetChildren()) {
						walk(c.get());
					}
				}
			};
			walk(a_root);
			return found;
		}

		// 女性 NPC を処理せずに飛ばしたとき、理由を 1 人につき 1 回だけログに出す（ログに無い NPC が、処理されなかったのか見えなかったのかを区別する）
		void SkipLog(RE::Actor* a_actor, RE::FormID a_id, int a_reason, const std::string& a_text)
		{
			static std::set<std::uint64_t> s_done;
			if (!s_done.insert((static_cast<std::uint64_t>(a_id) << 8) | static_cast<std::uint64_t>(a_reason)).second) {
				return;
			}
			logger::info("女性 NPC（処理しない）: {} ({:08X}) {}", a_actor->GetDisplayFullName(), a_id, a_text);
		}

		void Later(int a_ms, std::function<void()> a_fn);
		bool GameBusy();
		void Enqueue(int a_delayMs, RE::FormID a_id);

		// 1 人の処理（3D の書き換え）は数フレームに分ける。VR（3BA の体）では 1 人で 20 ms 前後かかり、1 フレーム（約 11 ms）に収まらず、
		// 場面が変わって何人も現れると引っかかった。段は 判定・帯 → 縮め → A ライン → 押し出し の 4 つ。
		// 段の間に 3D が作り直された・着替えた・ほかの処理が割り込んだときは、残りをやめて最初からやり直す
		struct Job
		{
			RE::FormID                    id{ 0 };
			RE::NiPointer<RE::NiAVObject> root;  // 体スロットの服の 3D（作り直されたら別の物になる）
			RE::TESObjectARMO*            armor{ nullptr };
			std::string                   who;
			std::uint64_t                 token{ 0 };
			long long                     us[4]{};
		};
		std::uint64_t g_jobToken = 0;  // 処理を始めるたびに増やす。途中の段は、自分の番号と違えば（別の処理が割り込んだ）やめる

		// 1 段目: 判定と帯。縮める段へ進むなら true
		bool StartActor(RE::FormID a_id, Job& a_job)
		{
			auto actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
			if (!actor || actor->IsPlayerRef() || actor->IsChild() || Layout::Disabled()) {
				if (actor && !actor->IsPlayerRef()) {
					SkipLog(actor, a_id, 4, actor->IsChild() ? "子供" : "構造体の確認で止めてある");
				}
				return false;
			}
			// 前の設定で書き換えた NPC: 元に戻してから今の設定で作り直す（帯は Attach が上端を見て付け直す）
			if (auto g = g_genOf.find(a_id); g != g_genOf.end() && g->second != g_gen) {
				const int r = RestoreActor(actor);
				logger::info("設定が変わったので {} ({:08X}) を元に戻した（頂点の写し {} 個）", actor->GetDisplayFullName(), a_id, r);
				if (!g_cfg.enabled) {
					Band::Detach(actor, "設定で無効にした");
				}
			}
			g_genOf[a_id] = g_gen;
			if (!g_cfg.enabled) {
				return false;
			}
			auto base = actor->GetActorBase();
			if (!base || base->GetSex() != RE::SEX::kFemale) {
				return false;
			}
			// 体スロット（32）に着ている物が「衣服」のときだけ調べる（鎧・手袋・ブーツ・頭は見ない）
			// 服を脱いだ・鎧に着替えたなど、縮める服を着ていなければ、前に付けた帯を外す
			auto armor = actor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
			const bool isClothing = armor && armor->GetArmorType() == RE::BGSBipedObjectForm::ArmorType::kClothing;
			const bool isArmor = armor && !isClothing;  // 鎧も対象。スカート状の部品があって、ズボン状の部品が無いときだけ縮める
			g_armorMode = isArmor;
			if (!armor || (!isClothing && !isArmor)) {
				SkipLog(actor, a_id, 1,
					armor ? std::format("体スロットの物 {} ({:08X}) が衣服でない（種類 {}）", armor->GetName(), armor->GetFormID(), static_cast<int>(armor->GetArmorType())) :
					        std::string("体スロットに何も着ていない"));
				Band::Detach(actor);
				return false;
			}
			auto& biped = actor->GetCurrentBiped();
			if (!biped) {
				SkipLog(actor, a_id, 2, "biped（装備の 3D の記録）がまだ無い");
				return false;
			}
			auto& obj = biped->objects[RE::BIPED_OBJECT::kBody];
			if (!obj.partClone) {
				SkipLog(actor, a_id, 3, std::format("体スロットの服 {} の 3D（partClone）がまだ無い", armor->GetName()));
				return false;
			}
			const std::string who = std::format("{} ({:08X})", actor->GetDisplayFullName(), a_id);
			logger::info("女性 NPC: {} 衣服: {} ({:08X})", who, armor->GetName(), armor->GetFormID());
			g_seen.insert(a_id);
			// 3D が作り直されて、書き換え済みの頂点が写されていないか調べ、あれば元に戻す（脚が消えたままになるのを防ぐ）
			RefreshOutfit(obj.partClone.get(), who, a_id);
			// 1 回目: 判定だけ（スカートがあるか）
			g_shortened = false;
			g_skirts.clear();
			g_dry = true;
			g_pants = false;
			g_usSleeve = g_usAnalyze = g_usTubes = 0;
			Walk(obj.partClone.get(), who);
			g_dry = false;
			logger::info("  判定の時間: 袖の見分け {} µs、形の判定 {} µs、ズボンの検出 {} µs", g_usSleeve, g_usAnalyze, g_usTubes);
			if (g_pants) {
				logger::info("  下にズボン状の物を着ているので対象外（スカートの中が素肌でない）");
				Band::Detach(actor);  // 前に帯を付けていれば外す
				return false;
			}
			if (!g_shortened) {
				if (HasCutBuffers(obj.partClone.get())) {
					// 切られた頂点が残っている（元に戻せず、切り済みのまま判定できなかった）。縮めた服のままなので、帯は外さない。
					// 3D が作り直されて帯が外れていれば付け直す（付けないと、短いスカートの下に脚が無いままになる）
					std::string underwear;
					if (g_cfg.underwear) {
						underwear = Underwear::Find(actor, armor, static_cast<int>(std::lround(g_cfg.bandTop))).path;
					}
					const bool ok = Band::Attach(actor, underwear);
					if (ok) {
						Band::MatchSkin(actor, obj.partClone.get());
					}
					logger::info("  切られた服（元の頂点が見つからず戻せない）なので、帯は外さない（帯 {}）", ok ? "あり" : "を用意できない");
					return false;
				}
				Band::Detach(actor);  // スカートでない服に着替えた
				return false;
			}
			// 帯（足首〜上端の裸の体）を先に用意する。用意できなければ縮めない（縮めると脚が消えるので、ロングのままにする）
			// スカートの中に着ける下着（所持品・服装・ランダム）。無効なら裸の体だけ
			std::string underwear;
			if (g_cfg.underwear && !DevFlags::On(DevFlags::kNoUnderwear)) {
				underwear = Underwear::Find(actor, armor, static_cast<int>(std::lround(g_cfg.bandTop))).path;
			}
			if (!DevFlags::On(DevFlags::kNoBand)) {
				if (!Band::Attach(actor, underwear)) {
					logger::info("  → 帯（裸の体）を用意できないので、この NPC のスカートは縮めない（ロングのまま）。帯ファイルが次の起動から読めれば短くなる");
					return false;
				}
				// 帯の肌の材質の付け替えは、帯を付けたのと同じフレームで（まだ一度も描かれていないうちに）する。
				// 段を分けて次のフレームにしたら、描かれた後の材質を取り替えることになり、VR でゲームが固まった（ドラゴンズリーチ・バナード・メアで 2 回とも、この直後に止まった）
				if (!DevFlags::On(DevFlags::kNoSkin)) {
					Band::MatchSkin(actor, obj.partClone.get());
				}
			}
			if (DevFlags::On(DevFlags::kNoShorten)) {
				logger::info("  開発用フラグ: 帯だけ付けて、スカートは縮めない");
				return false;
			}
			a_job.id = a_id;
			a_job.root = obj.partClone;
			a_job.armor = armor;
			a_job.who = who;
			return true;
		}

		// 2〜4 段目の前に、1 段目と同じ 3D のままかを確かめる
		bool StillSame(const Job& a_job)
		{
			if (a_job.token != g_jobToken || GameBusy()) {
				return false;
			}
			auto actor = RE::TESForm::LookupByID<RE::Actor>(a_job.id);
			if (!actor || !actor->Get3D() || actor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody) != a_job.armor) {
				return false;
			}
			auto& biped = actor->GetCurrentBiped();
			return biped && biped->objects[RE::BIPED_OBJECT::kBody].partClone.get() == a_job.root.get();
		}

		// 2 段目: 縮める（そのあと服の中の肌の脚（帯と重なる所）を消し、肌の色を合わせる）／3 段目: A ライン／4 段目: スカートを帯の体から離す
		void RunStage(Job& a_job, int a_k)
		{
			auto       actor = RE::TESForm::LookupByID<RE::Actor>(a_job.id);
			auto       root = a_job.root.get();
			const auto t0 = std::chrono::steady_clock::now();
			if (a_k == 1) {
				g_shortened = false;
				g_skirts.clear();
				g_hemNew = 1e9f;
				Walk(root, a_job.who);
				Band::HideOutfitLegs(actor, root);
			} else if (a_k == 2) {
				// 鎧は重なった板の間隔を保って広げる（比で縮尺すると板が同じ半径に潰れて背中に穴が空いた）
				Band::ShapeSkirt(actor, g_skirts, g_cfg.waist, g_hemNew < 1e8f ? g_hemNew : g_cfg.hem, g_cfg.hipEase, g_cfg.flareAngle, g_armorMode);
			} else {
				Band::FitSkirt(actor, g_skirts, g_hemNew < 1e8f ? g_hemNew : g_cfg.hem, g_cfg.skirtGap, g_cfg.skirtGapNoBone, g_cfg.bodyWeights);
				g_skirtActors.insert(a_job.id);
			}
			a_job.us[a_k] = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
			if (a_k == 3) {
				logger::info("  処理の内訳（段ごと）: 判定・帯 {} µs、縮め {} µs、A ライン {} µs、押し出し {} µs", a_job.us[0], a_job.us[1], a_job.us[2], a_job.us[3]);
			}
		}

		void Step(std::shared_ptr<Job> a_job, int a_k, std::function<void(bool)> a_done)
		{
			logger::info("  {}: {} 段目", a_job->who, a_k + 1);  // ゲームが固まったとき、どの段の最中だったかを見分ける
			if (!StillSame(*a_job)) {
				logger::info("  {}: 処理の途中で 3D が変わった（作り直し・着替え・別の処理）ので、やり直す", a_job->who);
				Enqueue(500, a_job->id);
				a_done(false);
				return;
			}
			RunStage(*a_job, a_k);
			if (a_k < 3) {
				Later(1, [a_job, a_k, a_done] { Step(a_job, a_k + 1, a_done); });
			} else {
				a_done(true);
			}
		}

		// a_done が無ければ全部の段をその場で続けて行う（設定の読み直し）。あれば 1 段ずつ次のフレームへ回し、終わったら a_done(最後まで済んだか)
		void ProcessActor(RE::FormID a_id, std::function<void(bool)> a_done = {})
		{
			auto job = std::make_shared<Job>();
			job->token = ++g_jobToken;
			const auto t0 = std::chrono::steady_clock::now();
			const bool go = StartActor(a_id, *job);
			job->us[0] = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
			if (!go) {
				if (a_done) {
					a_done(true);
				}
				return;
			}
			if (!a_done) {
				for (int k = 1; k <= 3; ++k) {
					RunStage(*job, k);
				}
				return;
			}
			Later(1, [job, a_done] { Step(job, 1, a_done); });
		}

		void Later(int a_ms, std::function<void()> a_fn)
		{
			std::thread([a_ms, fn = std::move(a_fn)] {
				std::this_thread::sleep_for(std::chrono::milliseconds(a_ms));
				SKSE::GetTaskInterface()->AddTask(fn);
			}).detach();
		}

		// 読み込み画面（ドアの出入り・場面転換）の最中は、NPC の 3D を触らない。ゲームが NPC の 3D・装備を作り直している最中に
		// こちらが走査・書き換えをすると、ゲームの処理とぶつかる（場面転換の直後にゲームが固まる件の予防）。終わるまで待つ
		// UI::IsMenuOpen は VR で読み込み画面を見逃した（ドラゴンズリーチに入るときの読み込み中に NPC を書き換え、ゲームが固まった。
		// ほかの MOD のログでは、その 5 秒に 3 フレームしか進んでいない）。メニューの開閉の通知で自分でも覚え、閉じてから 2 秒も待つ
		std::atomic<bool>      g_loadingOpen{ false };
		std::atomic<long long> g_loadingClosedMs{ 0 };
		long long              NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		bool GameBusy()
		{
			if (g_loadingOpen || NowMs() - g_loadingClosedMs < 2000) {
				return true;
			}
			auto ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		// 体スロットの服のうち、縮めた頂点バッファの shape の数
		int CountShortened(RE::Actor* a_actor)
		{
			auto& biped = a_actor->GetCurrentBiped();
			if (!biped) {
				return 0;
			}
			auto& obj = biped->objects[RE::BIPED_OBJECT::kBody];
			int   n = 0;
			std::function<void(RE::NiAVObject*)> walk = [&](RE::NiAVObject* a) {
				if (!a) {
					return;
				}
				if (auto geo = a->AsGeometry()) {
					auto skin = geo->GetGeometryRuntimeData().skinInstance.get();
					if (skin && skin->skinPartition && skin->skinPartition->numPartitions) {
						auto e = Verts::Owned(skin->skinPartition->partitions[0].buffData, skin->skinPartition->vertexCount);
						n += (e && e->shortened) ? 1 : 0;
					}
				}
				if (auto node = a->AsNode()) {
					for (auto& c : node->GetChildren()) {
						walk(c.get());
					}
				}
			};
			walk(obj.partClone.get());
			return n;
		}

		// 帯を付けたあとで外れていないか（3D の作り直し・装備の付け直しなど）。外れていたらもう一度処理する
		void CheckActor(RE::FormID a_id, int a_sec)
		{
			if (GameBusy()) {
				Later(1000, [a_id, a_sec] { CheckActor(a_id, a_sec); });
				return;
			}
			auto actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
			if (!actor || !actor->Get3D()) {
				return;
			}
			const auto st = Band::Check(actor);
			const int  shortened = CountShortened(actor);
			logger::info("確認 {} 秒後: {} ({:08X}) 帯の記録={} 3D の根が同じ={} 帯の shape 付いている {}/{} 縮めたスカート shape {} 個", a_sec,
				actor->GetDisplayFullName(), a_id, st.known ? "あり" : "なし", st.rootSame ? "はい" : "いいえ", st.attached, st.total, shortened);
			if ((st.known && (!st.rootSame || st.attached < st.total)) || shortened == 0) {
				logger::info("  → 外れた／作り直されたので、もう一度処理する");
				Enqueue(0, a_id);
			}
		}

		void Enqueue(int a_delayMs, RE::FormID a_id);

		// 読み込みの通知（TESObjectLoadedEvent）が来なかった女性 NPC を探して処理に回す。
		// 通知が来ない NPC がいた（ドラゴンズリーチの掃除係）。いま読み込まれている NPC を直接見て、まだ処理していない女性を拾う
		std::set<RE::FormID> g_scanFound;  // 走査で拾った NPC（有効に戻したときは空にして拾い直す）

		void ScanLoadedActors()
		{
			auto& s_found = g_scanFound;
			auto                        pl = RE::ProcessLists::GetSingleton();
			if (!pl) {
				return;
			}
			for (auto& h : pl->highActorHandles) {
				auto ptr = h.get();
				auto a = ptr.get();
				if (!a || a->IsPlayerRef() || !a->Get3D()) {
					continue;
				}
				const auto id = a->GetFormID();
				if (g_genOf.find(id) != g_genOf.end() || s_found.count(id)) {
					continue;
				}
				auto base = a->GetActorBase();
				if (!base || base->GetSex() != RE::SEX::kFemale || a->IsChild()) {
					continue;
				}
				s_found.insert(id);
				logger::info("走査: 読み込みの通知が来ていない女性 NPC {} ({:08X}、元 {:08X}) を見つけた。処理に回す", a->GetDisplayFullName(), id, base->GetFormID());
				Enqueue(500, id);
			}
		}

		// 縮めた NPC の 3D が作り直されていないか、2 秒ごとに調べる（作り直しは登場の 3 秒後・30 秒後だけでなく、あとからも起きる。
		// 作り直された 3D は元のロングに戻っていて、設定を変えて全員を処理し直すまで直らなかった）
		void Tick()
		{
			if (GameBusy()) {
				Later(2000, [] { Tick(); });
				return;
			}
			static int s_ticks = 0;
			if (s_ticks % 5 == 4) {
				ScanLoadedActors();  // 10 秒ごと
			}
			if (++s_ticks % 30 == 0) {
				logger::info("生存確認: 縮めた NPC {} 人", g_skirtActors.size());  // ゲームが止まったとき、このプラグインが動いていたかを見分ける
			}
			for (const auto id : std::vector<RE::FormID>(g_skirtActors.begin(), g_skirtActors.end())) {
				auto actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!actor || !actor->Get3D()) {
					continue;
				}
				const auto st = Band::Check(actor);
				if (CountShortened(actor) > 0 && !(st.known && (!st.rootSame || st.attached < st.total))) {
					continue;
				}
				logger::info("定期確認: {} ({:08X}) の 3D が作り直された／帯が外れたので、もう一度処理する（帯の記録={} 根が同じ={} 帯 {}/{}）", actor->GetDisplayFullName(), id,
					st.known ? "あり" : "なし", st.rootSame ? "はい" : "いいえ", st.attached, st.total);
				Enqueue(0, id);  // 処理は待ち行列で 1 人ずつ（数フレームに分けて）。縮める服でなくなったら、処理のあとで g_skirtActors から外す
			}
			Later(2000, [] { Tick(); });
		}

		void ProcessAndCheck(RE::FormID a_id, std::function<void()> a_after)
		{
			ProcessActor(a_id, [a_id, a_after](bool a_completed) {
				auto actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
				if (a_completed && actor) {
					if (CountShortened(actor) > 0) {
						Later(3000, [a_id] { CheckActor(a_id, 3); });
						Later(30000, [a_id] { CheckActor(a_id, 30); });
					} else {
						g_skirtActors.erase(a_id);  // 縮める服でなくなった（着替えなど）
					}
				}
				a_after();
			});
		}

		// 登場・着替えの処理は 1 人ずつ間を空けて行う。場面転換では 10 人前後が同時に読み込まれ、まとめて処理するとゲームが止まって見える
		std::mutex             g_qMutex;
		std::deque<RE::FormID> g_queue;
		bool                   g_pumping = false;

		void Pump()
		{
			if (GameBusy()) {
				Later(500, [] { Pump(); });  // g_pumping は立てたまま（待っている間に新しい Enqueue が Pump を重ねて起こさない）
				return;
			}
			RE::FormID id = 0;
			{
				std::lock_guard l(g_qMutex);
				if (g_queue.empty()) {
					g_pumping = false;
					return;
				}
				id = g_queue.front();
				g_queue.pop_front();
			}
			// 1 人の全部の段が済んでから次の人へ（途中の段のあいだに別の人を始めない）
			const auto t0 = std::chrono::steady_clock::now();
			ProcessAndCheck(id, [id, t0] {
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
				bool        more;
				std::size_t waiting;
				{
					std::lock_guard l(g_qMutex);
					waiting = g_queue.size();
					more = waiting > 0;
					if (!more) {
						g_pumping = false;
					}
				}
				if (ms >= 15) {
					logger::info("処理の経過: {:08X} 始めてから終わるまで {} ms（段ごとにフレームを分けた。あと {} 人待ち）", id, ms, waiting);
				}
				if (more) {
					Later(50, [] { Pump(); });
				}
			});
		}

		void Enqueue(int a_delayMs, RE::FormID a_id)
		{
			Later(a_delayMs, [a_id] {
				bool start = false;
				{
					std::lock_guard l(g_qMutex);
					if (std::find(g_queue.begin(), g_queue.end(), a_id) == g_queue.end()) {
						g_queue.push_back(a_id);
					}
					if (!g_pumping) {
						g_pumping = true;
						start = true;
					}
				}
				if (start) {
					Pump();
				}
			});
		}

		int g_dbgEnabled = -1;  // 開発用（DebugCmd の @enabled）。0/1 なら設定ファイルの「有効」を上書きする
#ifdef MINISKIRT_DEVTOOLS
		float g_dbgLengthCm = -1;  // 開発用（DebugCmd の @hem）。0 より大きければ設定ファイルの「スカート丈」を上書きする
#endif

		// ゲーム中に設定を変えた（MCM を閉じた）: 設定を読み直し、読み込まれている対象の NPC 全員を元から作り直す
		void ApplySettings()
		{
			const auto t0 = std::chrono::steady_clock::now();
			const Cfg  old = g_cfg;
			LoadCfg();
			if (g_dbgEnabled >= 0) {
				g_cfg.enabled = g_dbgEnabled != 0;
			}
#ifdef MINISKIRT_DEVTOOLS
			if (g_dbgLengthCm > 0) {
				g_cfg.lengthCm = (std::clamp)(g_dbgLengthCm, kLengthMinCm, kLengthMaxCm);
				g_cfg.hem = HemFromLength(g_cfg.lengthCm);
			}
#endif
			const bool topChanged = std::lround(old.bandTop) != std::lround(g_cfg.bandTop);
			const bool changed = topChanged || old.enabled != g_cfg.enabled || old.hem != g_cfg.hem || old.hipEase != g_cfg.hipEase || old.flareAngle != g_cfg.flareAngle ||
			                     old.skirtGap != g_cfg.skirtGap || old.skirtGapNoBone != g_cfg.skirtGapNoBone || old.waist != g_cfg.waist ||
			                     old.remapWeights != g_cfg.remapWeights || old.underwear != g_cfg.underwear;
			if (!changed) {
				return;  // メニューを閉じるたびに読むので、変わっていなければ何も書かない
			}
			logger::info("設定の読み直し: 値が変わった（上端 {}）", topChanged ? "も変わった" : "は同じ");
			if (topChanged) {
				Band::SetTop(g_cfg.bandTop);
			}
			++g_gen;
			if (!old.enabled && g_cfg.enabled) {
				// 無効のまま起動すると、NPC を処理していないので作り直す相手の記録が空。読み込み済みの女性 NPC を拾い直す
				g_scanFound.clear();
				ScanLoadedActors();
			}
			int loaded = 0;
			for (const auto id : std::vector<RE::FormID>(g_seen.begin(), g_seen.end())) {
				auto actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!actor || !actor->Get3D()) {
					continue;  // 読み込まれていない NPC は、次に読み込まれたときに今の設定で処理する
				}
				++loaded;
				ProcessActor(id);
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			logger::info("設定の読み直し: 読み込まれている NPC {} 人を{}（{} ms）", loaded, g_cfg.enabled ? "今の設定で作り直した" : "元のロングに戻し、帯を外した", ms);
		}

		// メニュー（MCM のある Journal Menu）を閉じたら設定ファイルを読み直す（MCM Helper はスクリプト無しでも値をファイルに書くので、
		// ESP には MCM Helper の MCM_ConfigBase をそのまま付けている）。ModEvent MiniSkirt_SettingChanged が来たらログに出すだけ（将来の自前スクリプト用）
		class SettingSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>, public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (a_event && a_event->eventName == "MiniSkirt_SettingChanged") {
					logger::info("MCM で設定が変わった（{}）。メニューを閉じたら反映する", a_event->strArg.c_str());
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
					g_loadingOpen = a_event->opening;
					if (!a_event->opening) {
						g_loadingClosedMs = NowMs();
					}
					logger::info("読み込み画面が{}（NPC の書き換えは、閉じて 2 秒たつまで待つ）", a_event->opening ? "開いた" : "閉じた");
				}
				if (a_event && !a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME) {
					SKSE::GetTaskInterface()->AddTask([] { ApplySettings(); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		SettingSink g_settingSink;

		// 通知が来ないゲルダ（ACHR 000D16E1、元 000C247E）とフィアナ（000D16E2、元 000D16DD）の調査用
		bool IsWatched(RE::FormID a_id)
		{
			return a_id == 0x000D16E1 || a_id == 0x000D16E2 || a_id == 0x000C247E || a_id == 0x000D16DD;
		}

		class Sink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
					 public RE::BSTEventSink<RE::TESEquipEvent>,
					 public RE::BSTEventSink<RE::TESCellAttachDetachEvent>,
					 public RE::BSTEventSink<RE::TESMoveAttachDetachEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (a_event && IsWatched(a_event->formID)) {
					auto f = RE::TESForm::LookupByID(a_event->formID);
					logger::info("通知（無条件）: 読み込みイベント {:08X} loaded={} 形={}", a_event->formID, a_event->loaded, f ? static_cast<int>(f->GetFormType()) : -1);
				}
				if (a_event && !a_event->loaded) {
					Band::Forget(a_event->formID);
				}
				if (a_event && a_event->loaded) {
					const auto id = a_event->formID;
					if (auto form = RE::TESForm::LookupByID(id); form && form->Is(RE::FormType::ActorCharacter)) {
						if (auto a = form->As<RE::Actor>()) {
							auto base = a->GetActorBase();
							if (base && !a->IsPlayerRef()) {
								logger::info("読み込み: {} NPC {} ({:08X}、元 {:08X}) 子供={} 3D={}", base->GetSex() == RE::SEX::kFemale ? "女性" : "女性でない", a->GetDisplayFullName(), id,
									base->GetFormID(), a->IsChild(), a->Get3D() != nullptr);
							}
						}
						// 装備の 3D が付くのを少し待ってから、ゲームの主スレッドで処理する
						Enqueue(1500, id);
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			// セルにアタッチされた女性 NPC を拾う（読み込みの通知が来ない NPC の取りこぼし対策。調査用ログ付き）
			static void OnAttach(const char* a_kind, RE::TESObjectREFR* a_ref, bool a_attached)
			{
				if (!a_ref) {
					return;
				}
				auto a = a_ref->As<RE::Actor>();
				if (!a || a->IsPlayerRef()) {
					return;
				}
				const auto id = a->GetFormID();
				auto       base = a->GetActorBase();
				if (IsWatched(id) || (base && IsWatched(base->GetFormID()))) {
					logger::info("通知（{}）: {} ({:08X}、元 {:08X}) attached={} 3D={}", a_kind, a->GetDisplayFullName(), id, base ? base->GetFormID() : 0, a_attached, a->Get3D() != nullptr);
				}
				if (!a_attached || !base || base->GetSex() != RE::SEX::kFemale || a->IsChild()) {
					return;
				}
				static std::set<RE::FormID> s_seen;
				if (g_genOf.find(id) != g_genOf.end() || !s_seen.insert(id).second) {
					return;
				}
				logger::info("通知（{}）: 女性 NPC {} ({:08X}) がアタッチされた。処理に回す", a_kind, a->GetDisplayFullName(), id);
				Enqueue(1500, id);
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				if (a_event) {
					OnAttach("セル", a_event->reference.get(), a_event->attached);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESMoveAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESMoveAttachDetachEvent>*) override
			{
				if (a_event) {
					OnAttach("移動", a_event->movedRef.get(), a_event->isCellAttached);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			// 体スロットの服を着替えた（付け直した）ら、もう一度処理する（帯の範囲が変わる・3D が作り直される）
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (!a_event || !a_event->actor) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto actor = a_event->actor->As<RE::Actor>();
				auto armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(a_event->baseObject);
				if (!actor || actor->IsPlayerRef() || !armor || !armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto base = actor->GetActorBase();
				if (!base || base->GetSex() != RE::SEX::kFemale || !actor->Get3D()) {
					return RE::BSEventNotifyControl::kContinue;  // 3D が無ければ読み込みのときに処理する
				}
				const auto id = actor->GetFormID();
				logger::info("装備の変更: {:08X} の体スロット {} ({:08X}) {}", id, armor->GetName(), armor->GetFormID(), a_event->equipped ? "装備" : "解除");
				Enqueue(1500, id);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sink g_sink;
	}

	// 開発用: 有効／無効を切り替えて全員を作り直す
	void DebugSetEnabled(bool a_on)
	{
		g_dbgEnabled = a_on ? 1 : 0;
		ApplySettings();
	}

#ifdef MINISKIRT_DEVTOOLS
	// 開発用: スカート丈（cm、MCM と同じ値）を上書きして全員を作り直す。0 以下で上書きをやめ、設定ファイルの値に戻す
	void DebugSetLength(float a_cm)
	{
		g_dbgLengthCm = a_cm;
		if (a_cm > 0) {
			logger::info("デバッグコマンド: スカート丈を {:.0f} cm に上書き（範囲 {:.0f}〜{:.0f} に収める）", a_cm, kLengthMinCm, kLengthMaxCm);
		} else {
			logger::info("デバッグコマンド: スカート丈の上書きをやめ、設定ファイルの値に戻す");
		}
		ApplySettings();
	}
#endif

	// プラグイン読み込み時（ゲームの初期化より前）: 設定を読み、帯ファイルを先に書き出す
	void Early()
	{
		LoadCfg();
		Band::EarlyPrepare(g_cfg.bandTop);
	}

	void Install()
	{
		Band::Prepare(g_cfg.bandTop, g_cfg.freezeJiggle);
		Later(5000, [] { Tick(); });
		Underwear::Prepare();
		if (auto src = RE::ScriptEventSourceHolder::GetSingleton()) {
			src->AddEventSink<RE::TESObjectLoadedEvent>(&g_sink);
			src->AddEventSink<RE::TESEquipEvent>(&g_sink);
			src->AddEventSink<RE::TESCellAttachDetachEvent>(&g_sink);
			src->AddEventSink<RE::TESMoveAttachDetachEvent>(&g_sink);
			logger::info("TESObjectLoadedEvent を購読した");
		}
		if (auto mod = SKSE::GetModCallbackEventSource()) {
			mod->AddEventSink(&g_settingSink);
		}
		if (auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_settingSink);
		}
		logger::info("メニュー（Journal Menu）を閉じたときに MCM の設定ファイル（{}）を読み直す", kMcmSettings);
	}
}
