#include "PCH.h"

#include "Layout.h"

#include <atomic>

namespace Layout
{
	namespace
	{
		std::atomic<int>  g_ok{ 0 };
		std::atomic<int>  g_bad{ 0 };
		std::atomic<bool> g_off{ false };
		constexpr int     kMaxLogged = 10;
		constexpr int     kStopAfter = 3;

		const char* RuntimeName()
		{
			switch (REL::Module::GetRuntime()) {
			case REL::Module::Runtime::SE:
				return "SE";
			case REL::Module::Runtime::AE:
				return "AE";
			case REL::Module::Runtime::VR:
				return "VR";
			default:
				return "不明";
			}
		}

		bool Fail(std::string& a_why, std::string a_reason)
		{
			a_why = std::move(a_reason);
			const int bad = ++g_bad;
			if (bad <= kMaxLogged) {
				logger::warn("構造体の確認: 合わない（{}）。この shape は処理しない", a_why);
			}
			if (bad >= kStopAfter && g_ok.load() == 0 && !g_off.exchange(true)) {
				logger::error("構造体の確認: {} 回続けて合わず、一度も合っていないので、スカートの処理を止める（ゲームの版: {}）", bad, RuntimeName());
			}
			return false;
		}
	}

	void LogStartup()
	{
		using Part = RE::NiSkinPartition::Partition;
		logger::info("ゲームの種類: {}（実行時の判別）", RuntimeName());
		logger::info("構造体（このビルドの前提）: NiSkinPartition={:#x} Partition={:#x} TriShape={:#x} NiSkinInstance={:#x}",
			sizeof(RE::NiSkinPartition), sizeof(Part), sizeof(RE::BSGraphics::TriShape), sizeof(RE::NiSkinInstance));
		logger::info("オフセット: Partition.buffData={:#x} TriShape.rawVertexData={:#x} TriShape.rawIndexData={:#x}",
			offsetof(Part, buffData), offsetof(RE::BSGraphics::TriShape, rawVertexData), offsetof(RE::BSGraphics::TriShape, rawIndexData));
	}

	bool Disabled()
	{
		return g_off.load();
	}

	bool Check(RE::NiSkinInstance* a_skin, std::string& a_why)
	{
		if (g_off.load()) {
			a_why = "構造体が合わないので止めてある";
			return false;
		}
		if (!a_skin || !a_skin->skinData || !a_skin->skinPartition || !a_skin->bones) {
			return true;  // 皮が無いのは構造の問題ではない
		}
		auto       part = a_skin->skinPartition.get();
		const auto boneCount = a_skin->skinData->GetBoneCount();
		if (boneCount == 0 || boneCount > 255) {
			return Fail(a_why, std::format("骨の数 {} が範囲外", boneCount));
		}
		const auto np = part->numPartitions;
		if (np == 0 || np > 64) {
			return Fail(a_why, std::format("区画の数 {} が範囲外", np));
		}
		if (part->partitions.data() == nullptr || part->partitions.size() != np) {
			return Fail(a_why, std::format("区画の配列の要素数 {} が区画の数 {} と合わない", part->partitions.size(), np));
		}
		if (part->vertexCount == 0 || part->vertexCount > 200000) {
			return Fail(a_why, std::format("頂点の数 {} が範囲外", part->vertexCount));
		}
		for (std::uint32_t i = 0; i < np; ++i) {
			const auto& p = part->partitions[i];
			if (!p.buffData) {
				return Fail(a_why, std::format("区画 {} に頂点の写し（buffData）が無い", i));
			}
			const std::uint32_t stride = p.buffData->vertexDesc.GetSize();
			if (stride < 12 || stride > 128 || (stride & 3) != 0) {
				return Fail(a_why, std::format("区画 {} の頂点の大きさ {} が範囲外", i, stride));
			}
			if (p.vertices > part->vertexCount) {
				return Fail(a_why, std::format("区画 {} の頂点数 {} が全体 {} を超える", i, p.vertices, part->vertexCount));
			}
			// 区画の骨の数が皮全体の骨の数を超える nif がある（Outfit Studio で作った服など。頂点の骨番号は皮の骨の一覧の番号なので、動作には影響しない）。
			// 構造体のずれの目印にはならないので、上限（骨は 255 本まで）だけ見る
			if (p.numBones > 255) {
				return Fail(a_why, std::format("区画 {} の骨の数 {} が範囲外", i, p.numBones));
			}
			if (p.bonesPerVertex > 4) {
				return Fail(a_why, std::format("区画 {} の頂点あたりの骨の数 {} が範囲外", i, p.bonesPerVertex));
			}
			if (const std::uint32_t rc = p.buffData->refCount; rc > 1000000) {
				return Fail(a_why, std::format("区画 {} の参照数 {} が範囲外", i, rc));
			}
		}
		if (++g_ok == 1) {
			logger::info("構造体の確認: 実物が前提と合っていた（ゲームの種類: {}、区画 {} 個、頂点 {}、骨 {}）", RuntimeName(), np, part->vertexCount, boneCount);
		}
		return true;
	}
}
