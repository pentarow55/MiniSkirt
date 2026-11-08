#include "PCH.h"

#include "Underwear.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

namespace Underwear
{
	namespace
	{
		std::string Lower(std::string a_s)
		{
			for (auto& c : a_s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a_s;
		}

		// 下着の言葉（部分一致。英語は小文字で比べる）
		constexpr std::string_view kWords[] = { "underwear", "lingerie", "undies", "panties", "panty", "thong", "knickers", "下着", "パンツ",
			"ショーツ", "ブラジャー", "ランジェリー" };

		// "bra" は単語として出てきたときだけ（Bra / _bra_ / BraTop / SilkBra。"Zebra" "bracer" "Brave" は違う）
		bool HasBraToken(const std::string& a_s)
		{
			const auto low = Lower(a_s);
			for (std::size_t i = low.find("bra"); i != std::string::npos; i = low.find("bra", i + 1)) {
				const bool beforeOk = i == 0 || !std::isalpha(static_cast<unsigned char>(a_s[i - 1])) ||
				                      (std::islower(static_cast<unsigned char>(a_s[i - 1])) && std::isupper(static_cast<unsigned char>(a_s[i])));
				const std::size_t j = i + 3;
				const bool afterOk = j >= a_s.size() || !std::isalpha(static_cast<unsigned char>(a_s[j])) || std::isupper(static_cast<unsigned char>(a_s[j])) ||
				                     (low[j] == 's' && (j + 1 >= a_s.size() || !std::isalpha(static_cast<unsigned char>(a_s[j + 1]))));
				if (beforeOk && afterOk) {
					return true;
				}
			}
			return false;
		}

		bool HasWord(const std::string& a_s)
		{
			if (a_s.empty()) {
				return false;
			}
			const auto low = Lower(a_s);
			for (auto w : kWords) {
				if (low.find(w) != std::string::npos) {
					return true;
				}
			}
			return HasBraToken(a_s);
		}

		// ①名前・キーワード・EditorID に下着の言葉があるか
		bool NamedUnderwear(RE::TESObjectARMO* a_armo)
		{
			if (HasWord(a_armo->GetName()) || HasWord(a_armo->GetFormEditorID())) {
				return true;
			}
			for (auto kw : a_armo->GetKeywords()) {
				if (kw && HasWord(kw->GetFormEditorID())) {
					return true;
				}
			}
			return false;
		}

		// ②ArmorAddon の女性モデルのパスに下着の言葉があるか（フォルダ名も含む）
		bool ModelUnderwear(RE::TESObjectARMO* a_armo)
		{
			for (auto a : a_armo->armorAddons) {
				if (!a) {
					continue;
				}
				auto m = a->bipedModels[RE::SEXES::kFemale].GetModel();
				if (m && HasWord(m)) {
					return true;
				}
			}
			return false;
		}

		// 体スロット（32）を持つ、遊べる（kNonPlayable でない）防具
		bool BodyArmor(RE::TESObjectARMO* a_armo)
		{
			return a_armo && a_armo->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody) &&
			       (a_armo->GetFormFlags() & RE::TESObjectARMO::RecordFlags::kNonPlayable) == 0;
		}

		// モデルのパスから先頭の "meshes\" を外す（Demand は Meshes\ からの相対で受け取る）
		std::string StripMeshes(std::string a_p)
		{
			const auto low = Lower(a_p.substr(0, 7));
			if (low == "meshes\\" || low == "meshes/") {
				a_p.erase(0, 7);
			}
			return a_p;
		}

		// 種族に合う、体スロットの ArmorAddon（女性モデルがある物）
		RE::TESObjectARMA* RaceArma(RE::TESObjectARMO* a_armo, RE::TESRace* a_race)
		{
			for (auto a : a_armo->armorAddons) {
				if (!a || !a->IsValidRace(a_race) || !a->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) {
					continue;
				}
				auto m = a->bipedModels[RE::SEXES::kFemale].GetModel();
				if (m && *m) {
					return a;
				}
			}
			return nullptr;
		}

		// ③形: nif の肌でない皮付きの shape が、脚の無い短い物か（縦の広がりが小さく、膝より上にある）。パスごとに覚える
		std::unordered_map<std::string, bool> g_shapeOk;

		void CollectGeo(RE::NiAVObject* a_obj, std::vector<RE::BSGeometry*>& a_out)
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
					CollectGeo(c.get(), a_out);
				}
			}
		}

		bool ShortShape(const std::string& a_path)
		{
			const auto key = Lower(a_path);
			if (auto it = g_shapeOk.find(key); it != g_shapeOk.end()) {
				return it->second;
			}
			bool                              ok = false;
			RE::NiPointer<RE::NiNode>         root;
			RE::BSModelDB::DBTraits::ArgsType args;
			if (RE::BSModelDB::Demand(a_path.c_str(), root, args) == RE::BSResource::ErrorCode::kNone && root) {
				std::vector<RE::BSGeometry*> geos;
				CollectGeo(root.get(), geos);
				float lo = 1e9f, hi = -1e9f;
				for (auto g : geos) {
					auto pr = g->GetGeometryRuntimeData().shaderProperty.get();
					if (pr && pr->material && pr->material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
						continue;  // 肌（体）は見ない
					}
					auto skin = g->GetGeometryRuntimeData().skinInstance.get();
					auto part = skin ? skin->skinPartition.get() : nullptr;
					if (!part || !part->numPartitions || !part->partitions[0].buffData || !part->partitions[0].buffData->rawVertexData) {
						continue;
					}
					auto       b = part->partitions[0].buffData;
					const auto stride = b->vertexDesc.GetSize();
					for (std::uint32_t v = 0; v < part->vertexCount; ++v) {
						const float z = reinterpret_cast<const float*>(b->rawVertexData + static_cast<std::size_t>(v) * stride)[2];
						lo = (std::min)(lo, z);
						hi = (std::max)(hi, z);
					}
				}
				ok = hi > lo && lo >= 40.0f && hi - lo <= 60.0f;
				logger::info("  下着: 形を調べた {} → 肌でない部分 z {:.1f}〜{:.1f}（{}）", a_path, lo, hi, ok ? "短い。下着とみなせる" : "下着の形でない");
			}
			g_shapeOk[key] = ok;
			return ok;
		}

		// ---- Dynamic Armor Variants（DAV） ----
		struct Cond
		{
			bool                worn{ false };  // WornHasKeyword（着ている物）か HasKeyword（NPC 本人）か
			RE::BGSKeyword*     kw{ nullptr };
			bool                want{ true };   // 条件が真になるのは、キーワードを持つ(true)／持たない(false)とき
		};
		struct DavState
		{
			std::string                                       variant;
			std::vector<Cond>                                 conds;
		};
		std::map<std::string, std::map<RE::TESObjectARMA*, RE::TESObjectARMA*>> g_variants;  // 変種の名前 → ArmorAddon の差し替え
		std::vector<DavState>                                                   g_states;    // 条件つきの変種（読んだ順）

		// "Plugin.esp|800" → フォーム
		template <class T>
		T* FormOf(const std::string& a_s)
		{
			const auto bar = a_s.find('|');
			if (bar == std::string::npos) {
				return nullptr;
			}
			const auto id = static_cast<RE::FormID>(std::strtoul(a_s.substr(bar + 1).c_str(), nullptr, 16));
			auto       dh = RE::TESDataHandler::GetSingleton();
			return dh ? dh->LookupForm<T>(id, a_s.substr(0, bar)) : nullptr;
		}

		// 条件 1 行（例 "WornHasKeyword wS_Underwear == 1 AND"）を読む。読めない形なら false と理由
		bool ParseCond(const std::string& a_line, Cond& a_out, bool& a_or, std::string& a_why)
		{
			std::vector<std::string> t;
			std::string              cur;
			for (char c : a_line) {
				if (c == ' ' || c == '\t') {
					if (!cur.empty()) {
						t.push_back(cur);
						cur.clear();
					}
				} else {
					cur += c;
				}
			}
			if (!cur.empty()) {
				t.push_back(cur);
			}
			a_or = false;
			if (!t.empty() && (t.back() == "AND" || t.back() == "OR")) {
				a_or = t.back() == "OR";
				t.pop_back();
			}
			if (t.size() != 4 || (t[0] != "HasKeyword" && t[0] != "WornHasKeyword") || (t[2] != "==" && t[2] != "!=") || (t[3] != "0" && t[3] != "1" &&
				t[3] != "1.0" && t[3] != "0.0")) {
				a_why = "読めない条件（HasKeyword / WornHasKeyword の == / != 0・1 だけ読む）";
				return false;
			}
			a_out.worn = t[0] == "WornHasKeyword";
			a_out.kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(t[1]);
			if (!a_out.kw) {
				a_why = std::format("キーワード {} が無い", t[1]);
				return false;
			}
			const bool one = t[3][0] == '1';
			a_out.want = (t[2] == "==") == one;
			return true;
		}

		void LoadDav()
		{
			const std::filesystem::path dir("Data\\SKSE\\Plugins\\DynamicArmorVariants");
			std::error_code             ec;
			int                         files = 0;
			for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (Lower(e.path().extension().string()) != ".json") {
					continue;
				}
				++files;
				const auto    name = e.path().filename().string();
				std::ifstream in(e.path());
				auto          j = nlohmann::json::parse(in, nullptr, false);
				if (j.is_discarded() || !j.is_object()) {
					logger::info("下着（DAV）: {} を JSON として読めない", name);
					continue;
				}
				int vars = 0, maps = 0, states = 0, skipped = 0;
				if (auto v = j.find("variants"); v != j.end() && v->is_array()) {
					for (auto& var : *v) {
						if (!var.is_object() || !var.contains("name") || !var["name"].is_string()) {
							continue;
						}
						auto& m = g_variants[var["name"].get<std::string>()];
						++vars;
						if (auto r = var.find("replaceByForm"); r != var.end() && r->is_object()) {
							for (auto& [from, to] : r->items()) {
								if (!to.is_string()) {
									continue;
								}
								auto a = FormOf<RE::TESObjectARMA>(from);
								auto b = FormOf<RE::TESObjectARMA>(to.get<std::string>());
								if (a && b) {
									m[a] = b;
									++maps;
								}
							}
						}
					}
				}
				if (auto s = j.find("states"); s != j.end() && s->is_array()) {
					for (auto& st : *s) {
						if (!st.is_object() || !st.contains("variant") || !st["variant"].is_string()) {
							continue;
						}
						DavState ds;
						ds.variant = st["variant"].get<std::string>();
						bool        ok = true;
						std::string why;
						if (auto c = st.find("conditions"); c != st.end() && c->is_array()) {
							for (auto& line : *c) {
								Cond cond;
								bool orNext = false;
								if (!line.is_string() || !ParseCond(line.get<std::string>(), cond, orNext, why)) {
									ok = false;
									if (line.is_string()) {
										why += std::format("「{}」", line.get<std::string>());
									}
									break;
								}
								if (orNext) {
									ok = false;
									why = "OR の条件は読まない";
									break;
								}
								ds.conds.push_back(cond);
							}
						}
						if (!ok) {
							++skipped;
							logger::info("下着（DAV）: {} の変種 {} の条件を飛ばした（{}）", name, ds.variant, why);
							continue;
						}
						g_states.push_back(std::move(ds));
						++states;
					}
				}
				logger::info("下着（DAV）: {} → 変種 {} 個（差し替え {} 組）、条件 {} 個（飛ばした {}）", name, vars, maps, states, skipped);
			}
			logger::info("下着（DAV）: 設定ファイル {} 個（{}）", files, ec ? "フォルダが無い" : "フォルダあり");
		}

		bool WornHas(RE::Actor* a_actor, RE::TESObjectARMO* a_extra, RE::BGSKeyword* a_kw)
		{
			if (a_extra && a_extra->HasKeyword(a_kw)) {
				return true;  // 選んだ下着（NPC は着ていないが、スカートの中に着けるので着ているとみなす）
			}
			for (std::uint32_t i = 0; i < 32; ++i) {
				auto armo = a_actor->GetWornArmor(static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << i));
				if (armo && armo->HasKeyword(a_kw)) {
					return true;
				}
			}
			return false;
		}

		// DAV の差し替え後の ArmorAddon（無ければ元の物）。a_how に何をしたか
		RE::TESObjectARMA* ApplyDav(RE::Actor* a_actor, RE::TESObjectARMO* a_armo, RE::TESObjectARMA* a_arma, RE::TESRace* a_race, std::string& a_how)
		{
			for (auto& st : g_states) {
				bool ok = true;
				for (auto& c : st.conds) {
					const bool has = c.worn ? WornHas(a_actor, a_armo, c.kw) : a_actor->HasKeyword(c.kw);
					if (has != c.want) {
						ok = false;
						break;
					}
				}
				if (!ok) {
					continue;
				}
				auto v = g_variants.find(st.variant);
				if (v == g_variants.end()) {
					continue;
				}
				auto r = v->second.find(a_arma);
				if (r == v->second.end()) {
					continue;
				}
				auto to = r->second;
				auto m = to->bipedModels[RE::SEXES::kFemale].GetModel();
				if (!to->IsValidRace(a_race) || !m || !*m) {
					a_how += std::format("、DAV の変種 {} の差し替え先 {:08X} は種族に合わない・女性モデルが無いので元の物", st.variant, to->GetFormID());
					return a_arma;
				}
				a_how += std::format("、DAV の変種 {} で ArmorAddon {:08X} → {:08X}", st.variant, a_arma->GetFormID(), to->GetFormID());
				return to;
			}
			return a_arma;
		}

		// ランダムに割り当てる下着の一覧（ロード順の全防具のうち、①か②の物。FormID の順）
		std::vector<RE::TESObjectARMO*>          g_pool;
		std::unordered_map<RE::FormID, Choice>   g_cache;
		int                                      g_cacheTop{ -1 };

		std::uint64_t Mix(std::uint64_t x)
		{
			x += 0x9E3779B97F4A7C15ull;
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			return x ^ (x >> 31);
		}

		// 服装（段階リストは中までたどる）の防具を集める
		void OutfitArmors(RE::TESForm* a_form, int a_depth, std::vector<RE::TESObjectARMO*>& a_out)
		{
			if (!a_form || a_depth > 4) {
				return;
			}
			if (auto armo = a_form->As<RE::TESObjectARMO>()) {
				a_out.push_back(armo);
			} else if (auto lvl = a_form->As<RE::TESLevItem>()) {
				for (auto& e : lvl->entries) {
					OutfitArmors(e.form, a_depth + 1, a_out);
				}
			}
		}
		// ---- 体型の比べ方（名前は見ない。形で） ----
		// nif の肌の材質（FaceGenRGBTint）の shape の頂点。いちばん大きい肌の shape の頂点と、全部の肌の shape の頂点
		struct SkinVerts
		{
			bool               ok{ false };
			std::vector<float> largest;  // x, y, z の並び
			std::vector<float> all;
		};
		std::unordered_map<std::string, SkinVerts> g_skinVerts;  // nif のパス（小文字）→ 肌の頂点

		const SkinVerts& ReadSkin(const std::string& a_path)
		{
			const auto key = Lower(a_path);
			if (auto it = g_skinVerts.find(key); it != g_skinVerts.end()) {
				return it->second;
			}
			SkinVerts                         sv;
			RE::NiPointer<RE::NiNode>         root;
			RE::BSModelDB::DBTraits::ArgsType args;
			if (RE::BSModelDB::Demand(a_path.c_str(), root, args) == RE::BSResource::ErrorCode::kNone && root) {
				std::vector<RE::BSGeometry*> geos;
				CollectGeo(root.get(), geos);
				for (auto g : geos) {
					auto pr = g->GetGeometryRuntimeData().shaderProperty.get();
					if (!pr || !pr->material || pr->material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
						continue;
					}
					auto skin = g->GetGeometryRuntimeData().skinInstance.get();
					auto part = skin ? skin->skinPartition.get() : nullptr;
					if (!part || !part->numPartitions || !part->partitions[0].buffData || !part->partitions[0].buffData->rawVertexData) {
						continue;
					}
					auto               b = part->partitions[0].buffData;
					const auto         stride = b->vertexDesc.GetSize();
					std::vector<float> v;
					v.reserve(static_cast<std::size_t>(part->vertexCount) * 3);
					for (std::uint32_t i = 0; i < part->vertexCount; ++i) {
						const float* f = reinterpret_cast<const float*>(b->rawVertexData + static_cast<std::size_t>(i) * stride);
						v.insert(v.end(), { f[0], f[1], f[2] });
					}
					sv.all.insert(sv.all.end(), v.begin(), v.end());
					if (v.size() > sv.largest.size()) {
						sv.largest = std::move(v);
					}
				}
				sv.ok = !sv.largest.empty();
			}
			return g_skinVerts[key] = std::move(sv);
		}

		// 高さ z（±1）の輪切りの左右・前後の幅。頂点が少なければ false
		bool Width(const std::vector<float>& a_v, float a_z, float& a_wx, float& a_wy)
		{
			float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
			int   n = 0;
			for (std::size_t i = 0; i + 2 < a_v.size(); i += 3) {
				if (a_v[i + 2] < a_z - 1.0f || a_v[i + 2] >= a_z + 1.0f) {
					continue;
				}
				x0 = (std::min)(x0, a_v[i]);
				x1 = (std::max)(x1, a_v[i]);
				y0 = (std::min)(y0, a_v[i + 1]);
				y1 = (std::max)(y1, a_v[i + 1]);
				++n;
			}
			if (n < 4) {
				return false;
			}
			a_wx = x1 - x0;
			a_wy = y1 - y0;
			return true;
		}

		// 下着の nif の体（肌の shape）が、NPC の素の体と同じ体型か。
		//  頂点の数が同じ: 頂点の位置の差の平均が 0.5 以内
		//  違う: 帯の範囲の高さ（z 15 から 5 ごと、上端−10 か 70 まで。上は手首が入るので見ない）の左右・前後の幅の差が全部 1.0 以内
		//  python（この環境）: 下着 11 着の体 ⇔ 3BA は 0.00、旧 CBBE は位置の差 0.69・幅の差 最大 1.8、Valerica の 3BA は 1.3、Sofia の体は 3.6
		// 下着の nif に肌の shape が無い（体を含まない）なら、裸の帯と重ねるので合うとみなす
		std::map<std::string, std::pair<bool, std::string>> g_fit;  // 体 nif|下着 nif|上端 → 合うか・理由

		const std::pair<bool, std::string>& BodyFits(const std::string& a_body, const std::string& a_uw, int a_top)
		{
			const auto key = std::format("{}|{}|{}", Lower(a_body), Lower(a_uw), a_top);
			if (auto it = g_fit.find(key); it != g_fit.end()) {
				return it->second;
			}
			std::pair<bool, std::string> r{ true, "" };
			const auto&                  u = ReadSkin(a_uw);
			const auto&                  b = ReadSkin(a_body);
			if (!u.ok) {
				r = { true, "下着の nif に体が無い（裸の帯と重ねる）" };
			} else if (!b.ok) {
				r = { false, "素の体の nif を読めない" };
			} else if (u.largest.size() == b.largest.size()) {
				double sum = 0.0;
				for (std::size_t i = 0; i + 2 < u.largest.size(); i += 3) {
					const double dx = u.largest[i] - b.largest[i], dy = u.largest[i + 1] - b.largest[i + 1], dz = u.largest[i + 2] - b.largest[i + 2];
					sum += std::sqrt(dx * dx + dy * dy + dz * dz);
				}
				const double mean = sum / static_cast<double>(u.largest.size() / 3);
				r = { mean <= 0.5, std::format("頂点の数が同じ（{}）、位置の差の平均 {:.2f}", u.largest.size() / 3, mean) };
			} else {
				float worst = 0.0f, worstZ = 0.0f;
				int   used = 0;
				const float zMax = (std::min)(70.0f, static_cast<float>(a_top) - 10.0f);
				for (float z = 15.0f; z <= zMax + 0.01f; z += 5.0f) {
					float ux, uy, bx, by;
					if (!Width(u.all, z, ux, uy) || !Width(b.all, z, bx, by)) {
						continue;
					}
					++used;
					const float d = (std::max)(std::abs(ux - bx), std::abs(uy - by));
					if (d > worst) {
						worst = d;
						worstZ = z;
					}
				}
				r = { used >= 3 && worst <= 1.0f,
					std::format("頂点の数が違う（{} と {}）、高さ {} か所の幅の差 最大 {:.2f}（z {:.0f}）", u.largest.size() / 3, b.largest.size() / 3, used, worst, worstZ) };
			}
			logger::info("  下着の体型: {} と素の体 {} → {}（{}）", a_uw, a_body, r.first ? "合う" : "合わない", r.second);
			return g_fit[key] = std::move(r);
		}

		// NPC の素の体の nif（skin → 種族に合う体の ArmorAddon の女性モデル）
		std::string BodyPath(RE::Actor* a_actor)
		{
			auto race = a_actor->GetRace();
			auto skin = a_actor->GetSkin();
			if (!race || !skin) {
				return {};
			}
			for (auto a : skin->armorAddons) {
				if (a && a->IsValidRace(race) && a->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) {
					auto m = a->bipedModels[RE::SEXES::kFemale].GetModel();
					return m ? StripMeshes(m) : std::string();
				}
			}
			return {};
		}
	}

	void Prepare()
	{
		g_pool.clear();
		g_cache.clear();
		g_variants.clear();
		g_states.clear();
		auto dh = RE::TESDataHandler::GetSingleton();
		if (dh) {
			for (auto armo : dh->GetFormArray<RE::TESObjectARMO>()) {
				if (BodyArmor(armo) && (NamedUnderwear(armo) || ModelUnderwear(armo))) {
					g_pool.push_back(armo);
				}
			}
		}
		std::sort(g_pool.begin(), g_pool.end(), [](auto a, auto b) { return a->GetFormID() < b->GetFormID(); });
		logger::info("下着: ランダムに割り当てる一覧 {} 個", g_pool.size());
		for (auto a : g_pool) {
			logger::info("  下着の候補: {} ({:08X})", a->GetName(), a->GetFormID());
		}
		LoadDav();
	}

	const Choice& Find(RE::Actor* a_actor, RE::TESObjectARMO* a_worn, int a_top)
	{
		const auto id = a_actor->GetFormID();
		if (a_top != g_cacheTop) {
			g_cache.clear();  // 帯の上端が変わると体型の比べる範囲が変わるので選び直す
			g_cacheTop = a_top;
		}
		if (auto it = g_cache.find(id); it != g_cache.end()) {
			return it->second;
		}
		Choice c;
		auto   race = a_actor->GetRace();
		auto   skin = a_actor->GetSkin();
		// 1. 所持品と既定の服装から、いちばん下着らしい物
		struct Cand
		{
			RE::TESObjectARMO* armo;
			const char*        from;
		};
		std::vector<Cand> cands;
		for (auto& [obj, data] : a_actor->GetInventory([](RE::TESBoundObject& o) { return o.Is(RE::FormType::Armor); })) {
			if (data.first > 0) {
				cands.push_back({ obj->As<RE::TESObjectARMO>(), "所持品" });
			}
		}
		if (auto base = a_actor->GetActorBase(); base && base->defaultOutfit) {
			std::vector<RE::TESObjectARMO*> out;
			for (auto f : base->defaultOutfit->outfitItems) {
				OutfitArmors(f, 0, out);
			}
			for (auto a : out) {
				cands.push_back({ a, "服装" });
			}
		}
		int                best = 0;
		RE::TESObjectARMO* bestArmo = nullptr;
		RE::TESObjectARMA* bestArma = nullptr;
		std::string        bestHow;
		std::set<RE::FormID> seen;
		for (auto& cd : cands) {
			auto a = cd.armo;
			if (!a || a == a_worn || a == skin || !BodyArmor(a) || !seen.insert(a->GetFormID()).second) {
				continue;
			}
			auto arma = RaceArma(a, race);
			if (!arma) {
				continue;
			}
			int         score = 0;
			const char* why = "";
			if (NamedUnderwear(a)) {
				score = 3, why = "名前・キーワード";
			} else if (ModelUnderwear(a)) {
				score = 2, why = "モデルのパス";
			} else if (ShortShape(StripMeshes(arma->bipedModels[RE::SEXES::kFemale].GetModel()))) {
				score = 1, why = "形";
			}
			if (score > best) {
				best = score;
				bestArmo = a;
				bestArma = arma;
				bestHow = std::format("{}（{}で下着と判断）", cd.from, why);
			}
		}
		const auto body = BodyPath(a_actor);
		if (bestArmo) {
			// NPC が自分で持っている下着はそのまま使う。体型が合わなければ警告だけ
			c.armo = bestArmo;
			c.how = bestHow;
			c.arma = ApplyDav(a_actor, bestArmo, bestArma, race, c.how);
			c.path = StripMeshes(c.arma->bipedModels[RE::SEXES::kFemale].GetModel());
			logger::info("  下着: {} ({:08X}) ArmorAddon {:08X} {} ← {}", bestArmo->GetName(), bestArmo->GetFormID(), c.arma->GetFormID(), c.path, c.how);
			if (!body.empty()) {
				if (auto& fit = BodyFits(body, c.path, a_top); !fit.first) {
					logger::info("  下着: 警告: NPC の持っている下着の体型が素の体 {} と合わない（{}）。そのまま使う", body, fit.second);
				}
			}
		} else if (!g_pool.empty()) {
			// 2. 無ければ、全防具の一覧のうち、NPC の素の体と同じ体型の下着から FormID で決まる 1 つ。合う物が無ければ裸の帯
			struct Fit
			{
				RE::TESObjectARMO* armo;
				RE::TESObjectARMA* arma;
				std::string        how;
				std::string        path;
			};
			std::vector<Fit> fits;
			int              raced = 0;
			for (auto a : g_pool) {
				if (a == a_worn) {
					continue;
				}
				auto arma = RaceArma(a, race);
				if (!arma) {
					continue;
				}
				++raced;
				Fit f{ a, nullptr, "", "" };
				f.arma = ApplyDav(a_actor, a, arma, race, f.how);
				f.path = StripMeshes(f.arma->bipedModels[RE::SEXES::kFemale].GetModel());
				if (!body.empty() && BodyFits(body, f.path, a_top).first) {
					fits.push_back(std::move(f));
				}
			}
			logger::info("  下着: ランダムの候補 {} 着（種族に合う物 {} 着）のうち体型が合う {} 着（素の体 {}）", g_pool.size(), raced, fits.size(),
				body.empty() ? std::string("不明") : body);
			if (!fits.empty()) {
				const auto k = static_cast<std::size_t>(Mix(id) % fits.size());
				auto&      f = fits[k];
				c.armo = f.armo;
				c.arma = f.arma;
				c.path = f.path;
				c.how = std::format("ランダム（体型が合う {} 着の {} 番目）{}", fits.size(), k + 1, f.how);
				logger::info("  下着: {} ({:08X}) ArmorAddon {:08X} {} ← {}", f.armo->GetName(), f.armo->GetFormID(), c.arma->GetFormID(), c.path, c.how);
			} else {
				c.how = "無し";
				logger::info("  下着: 体型が合うランダムの候補が無い → 裸の帯");
			}
		} else {
			c.how = "無し";
			logger::info("  下着: 見つからない（所持品・服装に無く、ランダムの一覧も空）→ 裸の帯");
		}
		return g_cache[id] = std::move(c);
	}
}
