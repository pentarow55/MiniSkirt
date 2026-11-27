#include "PCH.h"

#include "BodyMorph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

namespace BodyMorph
{
	namespace
	{
		// RaceMenu / SKEE の公開インターフェース定義（expired6978/SKSE64Plugins skee64/IPluginInterface.h と完全一致）
		class IPluginInterface
		{
		public:
			IPluginInterface() = default;
			virtual ~IPluginInterface() = default;

			virtual std::uint32_t GetVersion() = 0;
			virtual void          Revert() = 0;
		};

		class IInterfaceMap
		{
		public:
			virtual IPluginInterface* QueryInterface(const char* name) = 0;
			virtual bool              AddInterface(const char* name, IPluginInterface* pluginInterface) = 0;
			virtual IPluginInterface* RemoveInterface(const char* name) = 0;
		};

		struct InterfaceExchangeMessage
		{
			enum
			{
				kMessage_ExchangeInterface = 0x9E3779B9
			};

			IInterfaceMap* interfaceMap{ nullptr };
		};

		class IBodyMorphInterface : public IPluginInterface
		{
		public:
			class MorphKeyVisitor
			{
			public:
				virtual void Visit(const char*, float) = 0;
			};

			class StringVisitor
			{
			public:
				virtual void Visit(const char*) = 0;
			};

			class ActorVisitor
			{
			public:
				virtual void Visit(RE::TESObjectREFR*) = 0;
			};

			class MorphValueVisitor
			{
			public:
				virtual void Visit(RE::TESObjectREFR*, const char*, const char*, float) = 0;
			};

			class MorphVisitor
			{
			public:
				virtual void Visit(RE::TESObjectREFR*, const char*) = 0;
			};

			virtual void  SetMorph(RE::TESObjectREFR* actor, const char* morphName, const char* morphKey, float relative) = 0;
			virtual float GetMorph(RE::TESObjectREFR* actor, const char* morphName, const char* morphKey) = 0;
			virtual void  ClearMorph(RE::TESObjectREFR* actor, const char* morphName, const char* morphKey) = 0;

			virtual float GetBodyMorphs(RE::TESObjectREFR* actor, const char* morphName) = 0;
			virtual void  ClearBodyMorphNames(RE::TESObjectREFR* actor, const char* morphName) = 0;

			virtual void VisitMorphs(RE::TESObjectREFR* actor, MorphVisitor& visitor) = 0;
			virtual void VisitKeys(RE::TESObjectREFR* actor, const char* name, MorphKeyVisitor& visitor) = 0;
			virtual void VisitMorphValues(RE::TESObjectREFR* actor, MorphValueVisitor& visitor) = 0;

			virtual void ClearMorphs(RE::TESObjectREFR* actor) = 0;

			virtual void ApplyVertexDiff(RE::TESObjectREFR* refr, RE::NiAVObject* rootNode, bool erase = false) = 0;

			virtual void ApplyBodyMorphs(RE::TESObjectREFR* refr, bool deferUpdate = true) = 0;
			virtual void UpdateModelWeight(RE::TESObjectREFR* refr, bool immediate = false) = 0;

			virtual void          SetCacheLimit(std::uint64_t limit) = 0;
			virtual bool          HasMorphs(RE::TESObjectREFR* actor) = 0;
			virtual std::uint32_t EvaluateBodyMorphs(RE::TESObjectREFR* actor) = 0;
		};

		IBodyMorphInterface* g_iface = nullptr;
		bool                 g_inited = false;

		std::string Lower(std::string a_s)
		{
			for (auto& c : a_s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a_s;
		}

		std::string NormalizeTriPath(std::string a_path)
		{
			for (auto& c : a_path) {
				if (c == '/') {
					c = '\\';
				}
			}
			while (!a_path.empty() && a_path.front() == '\\') {
				a_path.erase(a_path.begin());
			}
			const auto low = Lower(a_path);
			if (low.rfind("meshes\\", 0) == 0) {
				return low.substr(7);
			}
			return low;
		}

		struct Reader : RE::BSResourceNiBinaryStream
		{
			using RE::BSResourceNiBinaryStream::BSResourceNiBinaryStream;
			using RE::NiBinaryStream::binary_read;
		};

		bool ReadExact(Reader& a_in, void* a_dst, std::size_t a_bytes)
		{
			if (a_bytes == 0) {
				return true;
			}
			std::uint32_t compSize = 1;
			return a_in.binary_read(a_dst, static_cast<std::uint32_t>(a_bytes), &compSize, 1) == a_bytes;
		}

		template <class T>
		bool ReadVal(Reader& a_in, T& a_out)
		{
			return ReadExact(a_in, &a_out, sizeof(T));
		}

		struct VertexDelta
		{
			std::uint32_t index;
			float         dx;
			float         dy;
			float         dz;
		};

		struct TriFile
		{
			// shape 名（小文字） → morph 名（小文字） → 頂点デルタ列
			std::unordered_map<std::string, std::unordered_map<std::string, std::vector<VertexDelta>>> shapes;
		};

		std::mutex                                               g_triMutex;
		std::unordered_map<std::string, std::shared_ptr<TriFile>> g_triCache;

		std::shared_ptr<TriFile> LoadTri(const std::string& a_relPath)
		{
			const auto key = NormalizeTriPath(a_relPath);
			if (key.empty()) {
				return nullptr;
			}
			{
				std::lock_guard l(g_triMutex);
				if (auto it = g_triCache.find(key); it != g_triCache.end()) {
					return it->second;
				}
			}

			const std::string fullPath = "meshes\\" + key;
			Reader            in(fullPath);
			if (!in.good()) {
				logger::info("  BodyMorph: .tri を開けない: {}", fullPath);
				std::lock_guard l(g_triMutex);
				g_triCache.emplace(key, nullptr);
				return nullptr;
			}

			std::uint32_t magic = 0;
			if (!ReadVal(in, magic)) {
				std::lock_guard l(g_triMutex);
				g_triCache.emplace(key, nullptr);
				return nullptr;
			}

			// SKEE と同じ判定: 'TRI\0' (0x54524900) または 'TRIP' (0x54524950)
			constexpr std::uint32_t kMagicTri = 0x54524900u;
			constexpr std::uint32_t kMagicTrip = 0x54524950u;
			if (magic != kMagicTri && magic != kMagicTrip) {
				logger::info("  BodyMorph: 未対応の .tri 形式 ({:08X}): {}", magic, fullPath);
				std::lock_guard l(g_triMutex);
				g_triCache.emplace(key, nullptr);
				return nullptr;
			}
			const bool packed = (magic == kMagicTrip);

			std::uint32_t shapeCount = 0;
			if (!packed) {
				if (!ReadVal(in, shapeCount)) {
					std::lock_guard l(g_triMutex);
					g_triCache.emplace(key, nullptr);
					return nullptr;
				}
			} else {
				std::uint16_t sc16 = 0;
				if (!ReadVal(in, sc16)) {
					std::lock_guard l(g_triMutex);
					g_triCache.emplace(key, nullptr);
					return nullptr;
				}
				shapeCount = sc16;
			}

			auto        tri = std::make_shared<TriFile>();
			std::size_t totalMorphs = 0;
			bool        ok = true;

			for (std::uint32_t i = 0; i < shapeCount && ok; ++i) {
				std::uint8_t nameLen = 0;
				if (!ReadVal(in, nameLen)) {
					ok = false;
					break;
				}
				std::string shapeName(nameLen, '\0');
				if (nameLen > 0 && !ReadExact(in, shapeName.data(), nameLen)) {
					ok = false;
					break;
				}
				const auto shapeKey = Lower(shapeName);

				if (!packed) {
					std::uint32_t blockSize = 0;
					if (!ReadVal(in, blockSize)) {
						ok = false;
						break;
					}
				}

				std::uint32_t morphCount = 0;
				if (!packed) {
					if (!ReadVal(in, morphCount)) {
						ok = false;
						break;
					}
				} else {
					std::uint16_t mc16 = 0;
					if (!ReadVal(in, mc16)) {
						ok = false;
						break;
					}
					morphCount = mc16;
				}

				auto& morphMap = tri->shapes[shapeKey];
				for (std::uint32_t j = 0; j < morphCount && ok; ++j) {
					std::uint8_t mLen = 0;
					if (!ReadVal(in, mLen)) {
						ok = false;
						break;
					}
					std::string morphName(mLen, '\0');
					if (mLen > 0 && !ReadExact(in, morphName.data(), mLen)) {
						ok = false;
						break;
					}
					const auto morphKey = Lower(morphName);

					if (!packed) {
						std::uint32_t mBlock = 0;
						if (!ReadVal(in, mBlock)) {
							ok = false;
							break;
						}
					}

					float         mult = 1.0f;
					std::uint32_t vCount = 0;
					if (!packed) {
						if (!ReadVal(in, vCount)) {
							ok = false;
							break;
						}
					} else {
						std::uint16_t vc16 = 0;
						if (!ReadVal(in, mult) || !ReadVal(in, vc16)) {
							ok = false;
							break;
						}
						vCount = vc16;
					}

					if (vCount > 65535u) {
						ok = false;
						break;
					}

					std::vector<VertexDelta> deltas;
					deltas.reserve(vCount);
					if (!packed) {
						for (std::uint32_t k = 0; k < vCount; ++k) {
							std::uint32_t idx = 0;
							float         x = 0.0f, y = 0.0f, z = 0.0f;
							if (!ReadVal(in, idx) || !ReadVal(in, x) || !ReadVal(in, y) || !ReadVal(in, z)) {
								ok = false;
								break;
							}
							deltas.push_back({ idx, x, y, z });
						}
					} else {
						for (std::uint32_t k = 0; k < vCount; ++k) {
							std::uint16_t idx = 0;
							std::int16_t  x = 0, y = 0, z = 0;
							if (!ReadVal(in, idx) || !ReadVal(in, x) || !ReadVal(in, y) || !ReadVal(in, z)) {
								ok = false;
								break;
							}
							deltas.push_back({ idx, static_cast<float>(x) * mult, static_cast<float>(y) * mult, static_cast<float>(z) * mult });
						}
					}
					if (!ok) {
						break;
					}
					morphMap.emplace(morphKey, std::move(deltas));
					++totalMorphs;
				}
			}

			if (!ok) {
				logger::warn("  BodyMorph: .tri の読み込みに失敗: {}", fullPath);
				tri.reset();
			} else {
				logger::info("  BodyMorph: .tri を読み込みました: {}（shape {} 個、モーフ {} 個）", fullPath, tri->shapes.size(), totalMorphs);
			}

			std::lock_guard l(g_triMutex);
			g_triCache[key] = tri;
			return tri;
		}

		std::string ExtractBodyTriFromObj(RE::NiAVObject* a_obj)
		{
			if (!a_obj) {
				return {};
			}
			if (auto ed = a_obj->GetExtraData<RE::NiStringExtraData>("BODYTRI")) {
				if (ed->value && *ed->value) {
					return NormalizeTriPath(ed->value);
				}
			}
			return {};
		}

		std::string FindBodyTriRecursive(RE::NiAVObject* a_obj)
		{
			if (!a_obj) {
				return {};
			}
			if (auto found = ExtractBodyTriFromObj(a_obj); !found.empty()) {
				return found;
			}
			if (auto node = a_obj->AsNode()) {
				for (auto& c : node->GetChildren()) {
					if (auto found = FindBodyTriRecursive(c.get()); !found.empty()) {
						return found;
					}
				}
			}
			return {};
		}
	}

	void Init()
	{
		if (g_inited) {
			return;
		}
		g_inited = true;

		auto messaging = SKSE::GetMessagingInterface();
		if (!messaging) {
			return;
		}

		InterfaceExchangeMessage msg{};
		messaging->Dispatch(InterfaceExchangeMessage::kMessage_ExchangeInterface, &msg, sizeof(msg), "skee");
		if (!msg.interfaceMap) {
			messaging->Dispatch(InterfaceExchangeMessage::kMessage_ExchangeInterface, &msg, sizeof(msg), "SKEE");
		}
		if (!msg.interfaceMap) {
			logger::info("BodyMorph: RaceMenu / SKEE は未導入です（体型モーフ連携なし）");
			return;
		}

		auto* raw = msg.interfaceMap->QueryInterface("BodyMorph");
		if (!raw) {
			logger::info("BodyMorph: SKEE の BodyMorph インターフェースが見つかりません");
			return;
		}

		g_iface = static_cast<IBodyMorphInterface*>(raw);
		logger::info("BodyMorph: RaceMenu / SKEE の BodyMorph インターフェースと接続しました (v{})", g_iface->GetVersion());
	}

	bool Available()
	{
		return g_iface != nullptr;
	}

	Snapshot Query(RE::Actor* a_actor)
	{
		Snapshot out;
		if (!g_iface || !a_actor || !g_iface->HasMorphs(a_actor)) {
			return out;
		}

		struct NameCollector : IBodyMorphInterface::MorphVisitor
		{
			std::set<std::string> names;
			void                  Visit(RE::TESObjectREFR*, const char* a_morphName) override
			{
				if (a_morphName && *a_morphName) {
					names.insert(a_morphName);
				}
			}
		};

		NameCollector collector;
		g_iface->VisitMorphs(a_actor, collector);
		if (collector.names.empty()) {
			return out;
		}

		// 大文字小文字違いの同一モーフ名があれば合算して小文字キーへ正規化
		std::map<std::string, float> merged;
		for (const auto& origName : collector.names) {
			const float v = g_iface->GetBodyMorphs(a_actor, origName.c_str());
			if (std::isfinite(v) && std::fabs(v) > 1e-4f) {
				merged[Lower(origName)] += v;
			}
		}

		std::uint64_t h = 0xcbf29ce484222325ull;
		for (const auto& [name, val] : merged) {
			if (std::fabs(val) <= 1e-4f) {
				continue;
			}
			out.morphs.push_back({ name, val });
			for (unsigned char c : name) {
				h = (h ^ c) * 0x100000001b3ull;
			}
			h = (h ^ 0xFFu) * 0x100000001b3ull;
			// 0.001 刻みで丸めてハッシュ（微小な浮動小数点揺れでの不要な再構築を防ぐ）
			const auto q = static_cast<std::int32_t>(std::lround(val * 1000.0f));
			h = (h ^ static_cast<std::uint32_t>(q)) * 0x100000001b3ull;
		}
		out.hash = out.morphs.empty() ? 0ull : (h ? h : 1ull);
		return out;
	}

	std::uint64_t Hash(RE::Actor* a_actor)
	{
		return Query(a_actor).hash;
	}

	std::string FindBodyTri(RE::NiAVObject* a_root, RE::BSGeometry* a_geo)
	{
		if (auto found = ExtractBodyTriFromObj(a_geo); !found.empty()) {
			return found;
		}
		return FindBodyTriRecursive(a_root);
	}

	bool ApplyToPositions(const std::string& a_triRelPath, const std::string& a_shapeName, const Snapshot& a_morphs,
		std::vector<std::array<float, 3>>& a_pos, std::vector<char>& a_hit, int& a_moved, float& a_maxMove)
	{
		a_moved = 0;
		a_maxMove = 0.0f;
		if (a_triRelPath.empty() || a_morphs.empty() || a_pos.empty() || a_pos.size() != a_hit.size()) {
			return false;
		}
		auto tri = LoadTri(a_triRelPath);
		if (!tri) {
			return false;
		}
		const auto shapeKey = Lower(a_shapeName);
		auto       sit = tri->shapes.find(shapeKey);
		if (sit == tri->shapes.end()) {
			return false;
		}

		const auto nv = a_pos.size();
		std::vector<std::array<float, 3>> acc(nv, { 0.0f, 0.0f, 0.0f });
		std::vector<char>                 touched(nv, 0);
		int                               matchedMorphs = 0;

		for (const auto& m : a_morphs.morphs) {
			auto mit = sit->second.find(m.name);
			if (mit == sit->second.end()) {
				continue;
			}
			++matchedMorphs;
			const float factor = m.value;
			for (const auto& d : mit->second) {
				if (d.index >= nv) {
					continue;
				}
				acc[d.index][0] += d.dx * factor;
				acc[d.index][1] += d.dy * factor;
				acc[d.index][2] += d.dz * factor;
				touched[d.index] = 1;
			}
		}
		if (!matchedMorphs) {
			return false;
		}

		for (std::size_t v = 0; v < nv; ++v) {
			if (!touched[v]) {
				continue;
			}
			const float l2 = acc[v][0] * acc[v][0] + acc[v][1] * acc[v][1] + acc[v][2] * acc[v][2];
			if (l2 < 1e-8f) {
				continue;
			}
			a_pos[v][0] += acc[v][0];
			a_pos[v][1] += acc[v][1];
			a_pos[v][2] += acc[v][2];
			a_hit[v] = 1;
			++a_moved;
			a_maxMove = (std::max)(a_maxMove, std::sqrt(l2));
		}
		return a_moved > 0;
	}
}
