#pragma once

// スカートの判定と丈の短縮。viewer/skirt.py の C++ 版（基準はそちら）。
// 座標は Skyrim の骨の初めの姿勢のまま（z 上、足元 0、頭のてっぺん約 128）。

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Windows.h の min / max マクロを無効にする
#undef min
#undef max

namespace Skirt
{
	struct Vec3
	{
		float x, y, z;
	};

	struct Result
	{
		bool        isSkirt{ false };
		float       hemZ{ 0.0f };
		float       flare{ 0.0f };
		bool        wrapsLegs{ false };  // 「脚が分かれている」とき: 左右の脚をそれぞれ筒のように包んでいるか（ズボン。viewer/skirt.py の wraps_legs）
		std::string reason;
	};

	// shape ひとつ分の判定（viewer/skirt.py の analyze と同じ答え）
	// a_minFlare: 裾の広がり（裾の幅 ÷ 上の幅）がこれ未満ならスカートとしない。鎧のスカート状の部品は、まっすぐ垂れていて広がらないことが多いので 0 を渡す
	// 速さ: 頂点を 3 回なめるだけ（高さの範囲／輪切りごとの幅・奥行き・左右の大きさ／真ん中の頂点の数）。輪切り 1 本ごとに頂点を全部なめ直さない
	inline Result Analyze(const std::vector<Vec3>& a_pos, float a_waist, float a_minFlare = 1.05f)
	{
		Result r;
		if (a_pos.empty()) {
			r.reason = "頂点なし";
			return r;
		}
		float       minZ = a_pos[0].z, maxZ = a_pos[0].z;
		std::size_t nLow = 0;
		for (auto& p : a_pos) {
			minZ = std::min(minZ, p.z);
			maxZ = std::max(maxZ, p.z);
			nLow += p.z < a_waist;
		}
		r.hemZ = minZ;
		if (a_pos.size() < 50 || nLow < a_pos.size() / 10) {
			r.reason = "腰より下の頂点が少ない";
			return r;
		}
		const float hem = minZ;
		if (a_waist - hem < 12.0f) {
			r.reason = "腰から裾までが短い";
			return r;
		}
		if (maxZ < a_waist - 8.0f) {
			r.reason = "腰まで届かない（ブーツなど）";
			return r;
		}
		constexpr int kSlices = 8;
		float         zc[kSlices];
		for (int i = 0; i < kSlices; ++i) {
			zc[i] = (a_waist - 6.0f) + ((hem + 4.0f) - (a_waist - 6.0f)) * static_cast<float>(i) / (kSlices - 1);
		}
		// 頂点の高さから、入りうる輪切りの番号の範囲（輪切りは上から下へ等間隔。幅 ±3 で重なることがある）。確かめは zc との差で行う
		const float step = (zc[0] - zc[kSlices - 1]) / (kSlices - 1);
		auto        range = [&](float z, int& lo, int& hi) {
			const float t = (zc[0] - z) / step, h = 3.0f / step;
			lo = std::max(0, static_cast<int>(t - h) - 1);  // floor / ceil は遅いので、切り捨て ±1 の幅で取る
			hi = std::min(kSlices - 1, static_cast<int>(t + h) + 1);
		};
		int   n[kSlices]{}, ns[kSlices][2]{};
		float x0[kSlices], x1[kSlices], y0[kSlices], y1[kSlices];
		float sx0[kSlices][2], sx1[kSlices][2], sy0[kSlices][2], sy1[kSlices][2];  // 左右（x<0 / x>=0）別の範囲（wrapsLegs 用）
		for (int i = 0; i < kSlices; ++i) {
			x0[i] = y0[i] = 1e9f;
			x1[i] = y1[i] = -1e9f;
			for (int s = 0; s < 2; ++s) {
				sx0[i][s] = sy0[i][s] = 1e9f;
				sx1[i][s] = sy1[i][s] = -1e9f;
			}
		}
		for (auto& p : a_pos) {
			if (!(p.z < a_waist)) {
				continue;
			}
			int lo, hi;
			range(p.z, lo, hi);
			for (int i = lo; i <= hi; ++i) {
				if (std::fabs(p.z - zc[i]) < 3.0f) {
					x0[i] = std::min(x0[i], p.x);
					x1[i] = std::max(x1[i], p.x);
					y0[i] = std::min(y0[i], p.y);
					y1[i] = std::max(y1[i], p.y);
					++n[i];
					const int s = p.x < 0.0f ? 0 : 1;
					sx0[i][s] = std::min(sx0[i][s], p.x);
					sx1[i][s] = std::max(sx1[i][s], p.x);
					sy0[i][s] = std::min(sy0[i][s], p.y);
					sy1[i][s] = std::max(sy1[i][s], p.y);
					++ns[i][s];
				}
			}
		}
		int   empty = 0;
		bool  seen[kSlices]{};  // 頂点が足りて輪切りを測れたか
		float widths[kSlices]{};
		for (int i = 0; i < kSlices; ++i) {
			if (n[i] < 4) {
				++empty;
				continue;
			}
			widths[i] = x1[i] - x0[i];
			seen[i] = true;
		}
		// 真ん中に頂点があれば「つながっている」。切れ込みのあるローブは奥行き（y）が幅（x）と同じくらいあるのでつながっているとみなす
		int center[kSlices]{};
		for (auto& p : a_pos) {
			if (!(p.z < a_waist)) {
				continue;
			}
			int lo, hi;
			range(p.z, lo, hi);
			for (int i = lo; i <= hi; ++i) {
				if (seen[i] && std::fabs(p.z - zc[i]) < 3.0f && std::fabs(p.x) < 0.1f * widths[i]) {
					++center[i];
				}
			}
		}
		bool bridged[kSlices]{};
		for (int i = 0; i < kSlices; ++i) {
			bridged[i] = seen[i] && (center[i] >= 3 || (y1[i] - y0[i]) >= 0.55f * widths[i]);
		}
		if (empty > 2) {
			r.reason = "輪切りに頂点が無い";
			return r;
		}
		// 広がり: 裾の幅 ÷ 上のほうの輪切りでいちばん細い幅（腰の高さは垂れた袖で広く測れることがあるので使わない）
		float narrow = 1e9f;
		for (int i = 0; i < kSlices - 2; ++i) {
			if (widths[i] > 0.0f) {
				narrow = std::min(narrow, widths[i]);
			}
		}
		narrow = std::max(narrow == 1e9f ? 1e-3f : narrow, 1e-3f);
		r.flare = std::max({ widths[kSlices - 1], widths[kSlices - 2], widths[kSlices - 3] }) / narrow;
		// 裾の輪（いちばん下）がつながっていなければ、足首で分かれた脚。途中は頂点が少ない服があるので見ない
		// ただし、いちばん下の輪が上の輪よりずっと細い（幅が半分未満）のは、裾の端に頂点がわずかに残っているだけで、脚の分かれ目ではない。
		// そういう輪は飛ばして、その上の輪で見る（viewer/skirt.py の analyze と同じ。腰の高さの輪は垂れた袖で広く測れるので入れない）
		float wmax = 0.0f;
		for (int i = 1; i < kSlices - 1; ++i) {
			wmax = std::max(wmax, widths[i]);
		}
		int bottom = -1;
		for (int i = kSlices - 1; i >= 0; --i) {
			if (seen[i] && widths[i] >= 0.5f * wmax) {
				bottom = i;
				break;
			}
		}
		const bool bottomBridged = bottom >= 0 ? bridged[bottom] : (seen[kSlices - 1] ? bridged[kSlices - 1] : bridged[kSlices - 2]);
		if (!bottomBridged) {
			r.reason = "脚が分かれている（ズボン／素の脚）";
			// 左右の脚をそれぞれ筒のように包んでいるか: 左右の両方に頂点がある輪のうちいちばん下で、左右どちらも幅・奥行きが 5 以上。
			// 左右に垂れた薄いマントの布（首長の服 02: 幅 2〜5、奥行き 1.5）や骨のエフェクトは包んでいない。ズボンは片脚で幅 7〜16、奥行き 8〜23
			for (int i = kSlices - 1; i >= 0; --i) {
				if (ns[i][0] < 3 || ns[i][1] < 3) {
					continue;
				}
				r.wrapsLegs = true;
				for (int s = 0; s < 2; ++s) {
					if (sx1[i][s] - sx0[i][s] < 5.0f || sy1[i][s] - sy0[i][s] < 5.0f) {
						r.wrapsLegs = false;
					}
				}
				break;
			}
			return r;
		}
		if (r.flare < a_minFlare) {
			r.reason = "裾が広がらない";
			return r;
		}
		r.isSkirt = true;
		r.reason = "スカート";
		return r;
	}

	// 下にズボンをはいているか（viewer/skirt.py の leg_tubes と同じ）。股のすぐ下（腰 -10）から 10 ずつ下げた 4 本の水平面で三角形を切り、
	// 切り口をつないだ輪のうち、片脚ずつを囲む閉じた輪（面積 a_minArea 以上）が左右にある面が a_need 本以上ならズボン。
	// スカートは両脚をまとめて 1 つの輪で囲む。裾の切れ込みやマントの布は輪が閉じない。ローブの中に作り込まれたズボン（首長の服 02・サルモールのローブ）も見つかる。
	// 腰まで届かない shape（ブーツなど）は数えない。a_tris には袖の頂点を含まない三角形だけを渡す（垂れた袖の筒を脚と数えない）。
	// 速さ: 三角形を 1 回なめ、輪切りごとに切り口の端点（数百個）を並べ替えてつなぐだけ
	inline bool LegTubes(const std::vector<Vec3>& a_pos, const std::vector<std::array<std::uint32_t, 3>>& a_tris, float a_waist,
		float a_minArea = 40.0f, int a_need = 3)
	{
		if (a_tris.empty()) {
			return false;
		}
		// 1 回の走査で、高さの範囲を測りながら、三角形をそれぞれがまたぐ面に振り分けておく（面ごとに全部を見直さない）
		constexpr int kPlanes = 4;
		float         zp[kPlanes];
		for (int k = 0; k < kPlanes; ++k) {
			zp[k] = a_waist - 10.0f - 10.0f * static_cast<float>(k);
		}
		// 作業用の配列は呼ぶたびに確保し直さず、使い回す（スレッドごと）
		struct Seg
		{
			float x[2], y[2];
		};
		struct Work
		{
			std::vector<std::uint32_t> bucket[kPlanes];
			std::vector<Seg>           sg;
			std::vector<std::int64_t>  ek;     // 端点ごとの位置の鍵（線分 i の端 e は 2i+e）
			std::vector<std::uint32_t> enext;  // 同じ鍵の次の端点（入れた順 = 線分の番号・端の順。python の辞書の入れた順と同じ）
			std::vector<std::int64_t>  hkey;   // 開番地法の表: 鍵
			std::vector<std::uint32_t> hhead, htail;
			std::vector<char>          used;
		};
		thread_local Work w;
		auto& bucket = w.bucket;
		auto& sg = w.sg;
		auto& ek = w.ek;
		auto& enext = w.enext;
		auto& hkey = w.hkey;
		auto& hhead = w.hhead;
		auto& htail = w.htail;
		auto& used = w.used;
		for (auto& b : bucket) {
			b.clear();
		}
		float                      zmin = 1e9f, zmax = -1e9f;
		for (std::uint32_t i = 0; i < a_tris.size(); ++i) {
			const auto& t = a_tris[i];
			const float z0 = a_pos[t[0]].z, z1 = a_pos[t[1]].z, z2 = a_pos[t[2]].z;
			const float lo = std::min(z0, std::min(z1, z2)), hi = std::max(z0, std::max(z1, z2));
			zmin = std::min(zmin, lo);
			zmax = std::max(zmax, hi);
			if (hi < zp[kPlanes - 1] || lo > zp[0]) {
				continue;
			}
			for (int k = 0; k < kPlanes; ++k) {
				if (!(hi < zp[k] || lo > zp[k])) {
					bucket[k].push_back(i);
				}
			}
		}
		if (zmax < a_waist - 8.0f) {
			return false;
		}
		int nPlanes = 0;
		while (nPlanes < kPlanes && zp[nPlanes] > zmin + 1.0f) {
			++nPlanes;
		}
		if (nPlanes < a_need) {
			return false;
		}
		auto less = [&](std::uint32_t a, std::uint32_t b) {  // 位置の順（z, x, y）。辺の内分点を、向きによらず同じ値にする
			const auto &pa = a_pos[a], &pb = a_pos[b];
			return pa.z != pb.z ? pa.z < pb.z : (pa.x != pb.x ? pa.x < pb.x : pa.y < pb.y);
		};
		// 0.01 刻みに丸めた位置。python の round と同じく、ちょうど半分は偶数へ（2^52+2^51 を足して引くと、既定の丸めで最も近い偶数になる。関数を呼ぶより速い）
		auto key = [](float x, float y) {
			constexpr double kMagic = 6755399441055744.0;
			const double     qx = (static_cast<double>(x / 0.01f) + kMagic) - kMagic;
			const double     qy = (static_cast<double>(y / 0.01f) + kMagic) - kMagic;
			return (static_cast<std::int64_t>(qx) << 32) ^ (static_cast<std::int64_t>(qy) & 0xFFFFFFFF);
		};
		constexpr std::uint32_t kNone = 0xFFFFFFFF;
		// 面を 1 本ずつ見て、答えが決まったら打ち切る（a_need 本見つかればズボン、残りで a_need 本に届かなければズボンでない）。
		// スカートは上の 2 本で外れるので、たいてい 2 本で終わる
		int hits = 0;
		for (int k = 0; k < nPlanes; ++k) {
			const float zk = zp[k];
			auto        dz = [&](std::uint32_t v) {  // python と同じく float で計算する（numpy の float32）
				const float d = a_pos[v].z - zk;
				return d == 0.0f ? 1e-4f : d;
			};
			sg.clear();
			for (const auto ti : bucket[k]) {
				const auto& t = a_tris[ti];
				Seg s{};
				int m = 0;
				for (int e = 0; e < 3; ++e) {
					std::uint32_t a = t[e], b = t[(e + 1) % 3];
					if ((dz(a) > 0) == (dz(b) > 0)) {
						continue;
					}
					if (less(b, a)) {
						std::swap(a, b);
					}
					const float u = dz(a) / (dz(a) - dz(b));
					if (m < 2) {
						s.x[m] = a_pos[a].x + (a_pos[b].x - a_pos[a].x) * u;
						s.y[m] = a_pos[a].y + (a_pos[b].y - a_pos[a].y) * u;
					}
					++m;
				}
				if (m == 2) {
					sg.push_back(s);
				}
			}
			const std::uint32_t ns = static_cast<std::uint32_t>(sg.size());
			const std::uint32_t ne = ns * 2;
			std::uint32_t       cap = 16;
			while (cap < ne * 2) {
				cap <<= 1;
			}
			ek.resize(ne);
			enext.assign(ne, kNone);
			hkey.resize(cap);
			hhead.assign(cap, kNone);
			htail.resize(cap);
			auto slot = [&](std::int64_t kk) {
				std::uint32_t h = static_cast<std::uint32_t>((static_cast<std::uint64_t>(kk) * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
				while (hhead[h] != kNone && hkey[h] != kk) {
					h = (h + 1) & (cap - 1);
				}
				return h;
			};
			for (std::uint32_t e = 0; e < ne; ++e) {
				const auto& s = sg[e >> 1];
				ek[e] = key(s.x[e & 1], s.y[e & 1]);
				const auto h = slot(ek[e]);
				if (hhead[h] == kNone) {
					hkey[h] = ek[e];
					hhead[h] = htail[h] = e;
				} else {
					enext[htail[h]] = e;
					htail[h] = e;
				}
			}
			used.assign(ns, 0);
			bool L = false, R = false;
			for (std::uint32_t i = 0; i < ns && !(L && R); ++i) {
				if (used[i]) {
					continue;
				}
				used[i] = 1;
				// 輪をたどりながら、面積（靴ひもの式）と x の範囲を足し込む。始点と終点を結ぶ分は最後に足す
				float  px = sg[i].x[1], py = sg[i].y[1];
				double a2 = static_cast<double>(sg[i].x[0]) * py - static_cast<double>(sg[i].y[0]) * px;
				float  xmin = std::min(sg[i].x[0], px), xmax = std::max(sg[i].x[0], px);
				std::size_t len = 2;
				std::uint32_t cur = i * 2 + 1;
				bool          closed = false;
				const auto    k0 = ek[i * 2];
				for (;;) {
					const auto kk = ek[cur];
					std::uint32_t e = hhead[slot(kk)];
					while (e != kNone && used[e >> 1]) {
						e = enext[e];
					}
					if (e == kNone) {
						closed = kk == k0 && len > 3;
						break;
					}
					used[e >> 1] = 1;
					cur = e ^ 1u;  // その線分のもう一方の端
					const float nx = sg[cur >> 1].x[cur & 1], ny = sg[cur >> 1].y[cur & 1];
					a2 += static_cast<double>(px) * ny - static_cast<double>(py) * nx;
					xmin = std::min(xmin, nx);
					xmax = std::max(xmax, nx);
					px = nx;
					py = ny;
					++len;
				}
				if (!closed) {
					continue;
				}
				a2 += static_cast<double>(px) * sg[i].y[0] - static_cast<double>(py) * sg[i].x[0];
				if (0.5 * std::fabs(a2) < a_minArea) {
					continue;
				}
				if (xmax < 0.0f) {
					L = true;
				} else if (xmin > 0.0f) {
					R = true;
				}
			}
			hits += L && R;
			if (hits >= a_need) {
				return true;
			}
			if (hits + (nPlanes - 1 - k) < a_need) {
				return false;
			}
		}
		return false;
	}

	// 片側（a_side: -1 = x<0、+1 = x>0）の脚の頂点が、足元から途切れずに届く高さ。
	// 2 刻みの帯で頂点が 3 個未満になったらそこまで。腰より下に頂点が無ければ 0
	inline float LegTopSide(const std::vector<Vec3>& a_pos, float a_waist, int a_side)
	{
		std::vector<float> zs;
		for (auto& p : a_pos) {
			if (p.z < a_waist && p.x * static_cast<float>(a_side) > 0.0f) {
				zs.push_back(p.z);
			}
		}
		if (zs.empty()) {
			return 0.0f;
		}
		const float z0 = *std::min_element(zs.begin(), zs.end());
		float       top = z0;
		const int   steps = static_cast<int>((a_waist - z0) / 2.0f) + 1;
		for (int k = 0; k < steps; ++k) {
			const float lo = z0 + 2.0f * k;
			int         n = 0;
			for (float z : zs) {
				if (z >= lo && z < lo + 2.0f) {
					++n;
				}
			}
			if (n < 3) {
				break;
			}
			top = lo + 2.0f;
		}
		return top;
	}

	// 脚（スカート以外の shape）が足元から途切れずに届く高さ（viewer/skirt.py の leg_top）。
	// 左右で別に測って低いほう（片脚だけ・片方が途中で切れている形でも、もう片方に穴をあけない）
	inline float LegTop(const std::vector<Vec3>& a_pos, float a_waist)
	{
		return std::min(LegTopSide(a_pos, a_waist, -1), LegTopSide(a_pos, a_waist, +1));
	}

	// 腰より下を縦に縮め、裾が a_hem の高さに来るようにする。もう短いものは変えない。戻り値: 縮めたあとの裾の高さ（縮めなければ元の裾）
	// a_sleeve（頂点ごとの袖の度合い。省略可）: 0.5 以上の頂点は裾の高さの基準に入れず、度合いの分だけ動かさない
	inline float Shorten(std::vector<Vec3>& a_pos, float a_waist, float a_hem, const std::vector<float>* a_sleeve = nullptr)
	{
		auto sl = [&](std::size_t i) { return a_sleeve && a_sleeve->size() == a_pos.size() ? (*a_sleeve)[i] : 0.0f; };
		float cur = 1e9f;
		for (std::size_t i = 0; i < a_pos.size(); ++i) {
			if (a_pos[i].z < a_waist && sl(i) < 0.5f) {
				cur = std::min(cur, a_pos[i].z);
			}
		}
		if (cur >= a_waist) {
			return cur;
		}
		const float k = (a_waist - a_hem) / (a_waist - cur);
		if (k >= 1.0f || k <= 0.0f) {
			return cur;
		}
		for (std::size_t i = 0; i < a_pos.size(); ++i) {
			auto& p = a_pos[i];
			if (p.z < a_waist) {
				const float z1 = a_waist - (a_waist - p.z) * k;
				p.z += (z1 - p.z) * std::clamp(1.0f - sl(i), 0.0f, 1.0f);
			}
		}
		return a_waist - (a_waist - cur) * k;
	}

	// ---- A ライン（お尻で膨らみ、その下は斜めにまっすぐ広がる形）。viewer/skirt.py の a_line と同じ（基準はそちら） ----
	namespace ALineDetail
	{
		constexpr int   kNth = 36;   // 角度の区切り（10 度ごと）
		constexpr float kDz = 2.0f;  // 高さの区切り
		constexpr float kPi = 3.14159265358979f;

		inline float Nan() { return std::numeric_limits<float>::quiet_NaN(); }

		inline int Bin(float a_dx, float a_dy)
		{
			const float th = std::atan2(a_dy, a_dx);
			const int   k = static_cast<int>((th + kPi) / (2.0f * kPi) * kNth);
			return ((k % kNth) + kNth) % kNth;
		}

		// 輪の配列の nan を両隣の値で直線に埋める
		inline void FillCirc(float* a_v)
		{
			std::vector<int> ok;
			for (int i = 0; i < kNth; ++i) {
				if (!std::isnan(a_v[i])) {
					ok.push_back(i);
				}
			}
			if (ok.empty()) {
				return;
			}
			std::array<float, kNth> src{};
			std::copy(a_v, a_v + kNth, src.begin());
			for (int i = 0; i < kNth; ++i) {
				if (!std::isnan(src[i])) {
					continue;
				}
				int a = -1, b = -1;
				for (int o : ok) {
					if (o < i) {
						a = o;
					}
					if (o > i && b < 0) {
						b = o;
					}
				}
				if (a < 0) {
					a = ok.back() - kNth;
				}
				if (b < 0) {
					b = ok.front() + kNth;
				}
				const float va = src[((a % kNth) + kNth) % kNth], vb = src[b % kNth];
				a_v[i] = va + (vb - va) * static_cast<float>(i - a) / static_cast<float>(b - a);
			}
		}

		// 角度の向きに ±1 の移動平均（nan は無視）
		inline void SmoothCirc(float* a_v)
		{
			std::array<float, kNth> src{};
			std::copy(a_v, a_v + kNth, src.begin());
			for (int i = 0; i < kNth; ++i) {
				float s = 0.0f;
				int   n = 0;
				for (int d = -1; d <= 1; ++d) {
					const float x = src[(i + d + kNth) % kNth];
					if (!std::isnan(x)) {
						s += x;
						++n;
					}
				}
				a_v[i] = n ? s / static_cast<float>(n) : Nan();
			}
		}

		struct Grid
		{
			int                nz{ 0 };
			std::vector<float> v;  // [nz][kNth]
			float*             Row(int a_z) { return v.data() + static_cast<std::size_t>(a_z) * kNth; }
			float              At(int a_z, int a_t) const { return v[static_cast<std::size_t>(a_z) * kNth + a_t]; }
			// 高さ zf・角度 tf（区切りの番号、小数）で読む
			float Lerp(float a_zf, float a_tf) const
			{
				const int   z0i = (std::clamp)(static_cast<int>(std::floor(a_zf)), 0, nz - 1);
				const int   z1i = (std::clamp)(z0i + 1, 0, nz - 1);
				const float fz = (std::clamp)(a_zf - static_cast<float>(z0i), 0.0f, 1.0f);
				const float fl = std::floor(a_tf);
				const int   t0 = ((static_cast<int>(fl) % kNth) + kNth) % kNth, t1 = (t0 + 1) % kNth;
				const float ft = a_tf - fl;
				const float a = At(z0i, t0) * (1 - ft) + At(z0i, t1) * ft;
				const float b = At(z1i, t0) * (1 - ft) + At(z1i, t1) * ft;
				return a * (1 - fz) + b * fz;
			}
		};

		// 角度・高さごとのいちばん外の半径。a_peak=false: ならしたもの（スカート）、true: となりとの最大（体。小さく見積もらない）。
		// a_raw=true: ならさず広げない（体。お尻の線より上で使う）
		inline Grid Envelope(const std::vector<Vec3>& a_p, float a_cx, float a_cy, float a_z0, int a_nz, bool a_peak, bool a_raw = false)
		{
			Grid g;
			g.nz = a_nz;
			g.v.assign(static_cast<std::size_t>(a_nz) * kNth, Nan());
			for (auto& p : a_p) {
				const float dx = p.x - a_cx, dy = p.y - a_cy;
				const float r = std::sqrt(dx * dx + dy * dy);
				const int   zi = (std::clamp)(static_cast<int>(std::floor((p.z - a_z0) / kDz)), 0, a_nz - 1);
				float&      cell = g.Row(zi)[Bin(dx, dy)];
				cell = std::isnan(cell) ? r : (std::max)(cell, r);
			}
			// 高さの向きに埋める（端より外は端の値）
			for (int t = 0; t < kNth; ++t) {
				std::vector<int> ok;
				for (int z = 0; z < a_nz; ++z) {
					if (!std::isnan(g.Row(z)[t])) {
						ok.push_back(z);
					}
				}
				if (ok.empty()) {
					continue;
				}
				for (int z = 0; z < a_nz; ++z) {
					if (!std::isnan(g.Row(z)[t])) {
						continue;
					}
					if (z < ok.front()) {
						g.Row(z)[t] = g.Row(ok.front())[t];
					} else if (z > ok.back()) {
						g.Row(z)[t] = g.Row(ok.back())[t];
					} else {
						int a = ok.front(), b = ok.back();
						for (int o : ok) {
							if (o < z) {
								a = o;
							}
							if (o > z) {
								b = o;
								break;
							}
						}
						const float va = g.Row(a)[t], vb = g.Row(b)[t];
						g.Row(z)[t] = va + (vb - va) * static_cast<float>(z - a) / static_cast<float>(b - a);
					}
				}
			}
			for (int z = 0; z < a_nz; ++z) {
				FillCirc(g.Row(z));
			}
			if (a_raw) {
				return g;
			}
			if (a_peak) {
				for (int z = 0; z < a_nz; ++z) {
					std::array<float, kNth> src{};
					std::copy(g.Row(z), g.Row(z) + kNth, src.begin());
					for (int t = 0; t < kNth; ++t) {
						g.Row(z)[t] = (std::max)({ src[t], src[(t + 1) % kNth], src[(t + kNth - 1) % kNth] });
					}
				}
				auto src = g.v;
				for (int z = 1; z + 1 < a_nz; ++z) {
					for (int t = 0; t < kNth; ++t) {
						const auto i = static_cast<std::size_t>(z) * kNth + t;
						g.v[i] = (std::max)({ src[i - kNth], src[i], src[i + kNth] });
					}
				}
				return g;
			}
			for (int z = 0; z < a_nz; ++z) {
				SmoothCirc(g.Row(z));
			}
			auto src = g.v;
			for (int z = 1; z + 1 < a_nz; ++z) {
				for (int t = 0; t < kNth; ++t) {
					const auto i = static_cast<std::size_t>(z) * kNth + t;
					g.v[i] = (src[i - kNth] + src[i] + src[i + kNth]) / 3.0f;
				}
			}
			return g;
		}
	}

	namespace NearDetail
	{
		// 最も近い点までの距離の 2 乗を求める k-d 木（点を、広がりがいちばん大きい軸の中央値で 2 つに分けていく）。
		// 総当たりと同じ答え（同じ式で測った距離の最小。調べる順が違うだけ）
		struct CellGrid
		{
			std::vector<Vec3> pts;  // 木の順に並べた点。区間 [lo, hi) の中央 m が節、左 [lo, m)、右 (m, hi)
			std::vector<char> axis;
			explicit CellGrid(const std::vector<Vec3>& a_p) :
				pts(a_p), axis(a_p.size(), 0)
			{
				Build(0, pts.size());
			}
			static float Get(const Vec3& p, int a) { return a == 0 ? p.x : (a == 1 ? p.y : p.z); }
			void         Build(std::size_t lo, std::size_t hi)
			{
				if (hi - lo <= 1) {
					return;
				}
				float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
				for (auto i = lo; i < hi; ++i) {
					for (int k = 0; k < 3; ++k) {
						mn[k] = (std::min)(mn[k], Get(pts[i], k));
						mx[k] = (std::max)(mx[k], Get(pts[i], k));
					}
				}
				int a = 0;
				for (int k = 1; k < 3; ++k) {
					if (mx[k] - mn[k] > mx[a] - mn[a]) {
						a = k;
					}
				}
				const auto m = lo + (hi - lo) / 2;
				std::nth_element(pts.begin() + lo, pts.begin() + m, pts.begin() + hi, [a](const Vec3& p, const Vec3& q) { return Get(p, a) < Get(q, a); });
				axis[m] = static_cast<char>(a);
				Build(lo, m);
				Build(m + 1, hi);
			}
			// a_bound より近い点が無ければ a_bound を返す（それより遠い所は探さない）
			float Nearest(const Vec3& q, float a_bound = 1e30f) const
			{
				float best = a_bound;
				Search(q, 0, pts.size(), best);
				return best;
			}
			void Search(const Vec3& q, std::size_t lo, std::size_t hi, float& best) const
			{
				if (hi <= lo) {
					return;
				}
				const auto  m = lo + (hi - lo) / 2;
				const auto& p = pts[m];
				best = (std::min)(best, (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z));
				if (hi - lo == 1) {
					return;
				}
				const int   a = axis[m];
				const float d = Get(q, a) - Get(p, a);
				// 近い側を先に、遠い側は分け目までの距離が今の最短より近いときだけ
				if (d < 0.0f) {
					Search(q, lo, m, best);
					if (d * d <= best) {
						Search(q, m + 1, hi, best);
					}
				} else {
					Search(q, m + 1, hi, best);
					if (d * d <= best) {
						Search(q, lo, m, best);
					}
				}
			}
		};

		// 同じ点の集まりの木は作り直さない（帯の体は同じ体の NPC で同じ）。中身の全バイトで見分け、最近の 8 個を覚える
		inline const CellGrid& Cached(const std::vector<Vec3>& a_p)
		{
			std::uint64_t h = 1469598103934665603ull;
			const auto*   b = reinterpret_cast<const unsigned char*>(a_p.data());
			for (std::size_t i = 0, n = a_p.size() * sizeof(Vec3); i < n; ++i) {
				h = (h ^ b[i]) * 1099511628211ull;
			}
			struct Slot
			{
				std::uint64_t             h;
				std::size_t               n;
				std::shared_ptr<CellGrid> g;
			};
			thread_local std::vector<Slot> cache;
			for (std::size_t i = 0; i < cache.size(); ++i) {
				if (cache[i].h == h && cache[i].n == a_p.size()) {
					std::rotate(cache.begin(), cache.begin() + i, cache.begin() + i + 1);
					return *cache.front().g;
				}
			}
			cache.insert(cache.begin(), Slot{ h, a_p.size(), std::make_shared<CellGrid>(a_p) });
			if (cache.size() > 8) {
				cache.pop_back();
			}
			return *cache.front().g;
		}
	}

	// 袖の度合い（0〜1）を足す: 体の腕・手の頂点のほうが帯の体の頂点より近い頂点は 1
	// （a_w には腕の骨の重みの和を入れておく。袖の骨の重みが腰に付いている服もあるので位置でも見る）
	// 複数の shape をまとめて見る（探すためのマス分けを 1 回で済ませる）
	inline void Sleeve(const std::vector<std::vector<Vec3>>& a_pos, std::vector<std::vector<float>>& a_w, const std::vector<Vec3>& a_band, const std::vector<Vec3>& a_arm)
	{
		if (a_arm.empty()) {
			return;
		}
		float armZ = 1e9f;
		for (auto& p : a_arm) {
			armZ = (std::min)(armZ, p.z);
		}
		const float       zmin = armZ - 10.0f;
		std::vector<Vec3> bp;
		for (auto& p : a_band) {
			if (p.z >= zmin - 6.0f) {
				bp.push_back(p);
			}
		}
		const auto& armS = NearDetail::Cached(a_arm);
		const auto& bandS = NearDetail::Cached(bp);
		for (std::size_t s = 0; s < a_pos.size(); ++s) {
			for (std::size_t i = 0; i < a_pos[s].size(); ++i) {
				// 腕のほうが体より近いか。体までの距離を先に測り、腕はそれより近い点だけを探す（答えは両方を測って比べるのと同じ）
				if (a_pos[s][i].z < zmin) {
					continue;
				}
				const float db = bandS.Nearest(a_pos[s][i]);
				if (armS.Nearest(a_pos[s][i], db) < db) {
					a_w[s][i] = 1.0f;
				}
			}
		}
	}

	inline void Sleeve(const std::vector<Vec3>& a_pos, std::vector<float>& a_w, const std::vector<Vec3>& a_band, const std::vector<Vec3>& a_arm)
	{
		if (a_pos.empty()) {
			return;
		}
		std::vector<std::vector<Vec3>>  p{ a_pos };
		std::vector<std::vector<float>> w{ std::move(a_w) };
		Sleeve(p, w, a_band, a_arm);
		a_w = std::move(w[0]);
	}

	// 縮めたあとのスカート（全部の shape）を A ラインにする。体（帯）の骨盤まわりの中心の縦の軸のまわりの半径に、
	// 目標の半径 ÷ 元のなめらかな半径（スカート全部の、角度・高さごとのいちばん外）を掛ける（プリーツ・重ねの前後は保たれる）。
	//  お尻の線: 角度ごとに、裾〜上端−2 で帯の体がいちばん外に出る半径 Rb と高さ zb（zb は上端−8 まで）
	//  お尻の線より下: max(Rb + ease, 元の zb の半径) + tan(angle)·(zb − z)（直線に広がる）
	//  上端〜お尻の線: 元の上端の半径から上の値へ smoothstep（お尻の山は waist より上にあるので、帯の上端から）
	//  どの高さでも体のその高さの半径 + ease 以上（上端から 8 下までで 0 から ease へ）。内側の布は体 + ease/2 より内へ入れない
	//  上端より上は変えない。a_sleeve（頂点ごとの袖の度合い）が 0.5 以上の頂点は元の半径に入れず、度合いの分だけ動かさない
	// a_band: 帯の体の頂点（腕・手首は除く）。戻り値: 動かした頂点の数
	// a_expandOnly（切らなかったスカート。丈 100% など）: 形は元のまま、帯の体に当たる所だけ外へ出す。お尻の線より下は「体 + ゆとり」、
	//  上は上端〜お尻の線の山（上と同じ smoothstep）を、元の半径より大きいときだけ使う（縮めない・広がり角度を付けない）。
	//  差し替えた脚（帯の体）の腰が、服に付いていた体より張っていると、元の布から出るため（フルダの服で腰の横に素肌が出た）
	inline int ALine(std::vector<std::vector<Vec3>>& a_shapes, const std::vector<std::vector<float>>& a_sleeve, const std::vector<Vec3>& a_band, float a_waist,
		float a_hem, float a_ease, float a_angle, float a_top, std::string& a_log, bool a_keepLayers = false, bool a_expandOnly = false)
	{
		using namespace ALineDetail;
		constexpr float   kRise = 8.0f;
		std::vector<Vec3> allp;
		for (std::size_t s = 0; s < a_shapes.size(); ++s) {
			for (std::size_t i = 0; i < a_shapes[s].size(); ++i) {
				if (a_sleeve[s][i] < 0.5f) {
					allp.push_back(a_shapes[s][i]);
				}
			}
		}
		std::vector<Vec3> band;
		for (auto& p : a_band) {
			if (p.z <= a_top) {
				band.push_back(p);
			}
		}
		if (allp.empty() || band.empty()) {
			a_log = "スカートか帯の頂点が無い";
			return 0;
		}
		// 中心: 帯の体の waist−15〜waist の範囲の真ん中
		float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
		for (auto& p : band) {
			if (p.z >= a_waist - 15.0f && p.z <= a_waist) {
				x0 = (std::min)(x0, p.x), x1 = (std::max)(x1, p.x), y0 = (std::min)(y0, p.y), y1 = (std::max)(y1, p.y);
			}
		}
		if (x0 > x1) {
			for (auto& p : band) {
				x0 = (std::min)(x0, p.x), x1 = (std::max)(x1, p.x), y0 = (std::min)(y0, p.y), y1 = (std::max)(y1, p.y);
			}
		}
		const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
		// お尻の線
		float rb[kNth], zb[kNth];
		std::fill(rb, rb + kNth, Nan());
		std::fill(zb, zb + kNth, Nan());
		for (auto& p : band) {
			if (p.z < a_hem || p.z > a_top - 2.0f) {
				continue;
			}
			const float dx = p.x - cx, dy = p.y - cy;
			const float r = std::sqrt(dx * dx + dy * dy);
			const int   k = Bin(dx, dy);
			if (std::isnan(rb[k]) || r > rb[k]) {
				rb[k] = r;
				zb[k] = p.z;
			}
		}
		FillCirc(rb);
		FillCirc(zb);
		{
			float src[kNth];
			std::copy(rb, rb + kNth, src);
			for (int i = 0; i < kNth; ++i) {
				rb[i] = (std::max)({ src[i], src[(i + 1) % kNth], src[(i + kNth - 1) % kNth] });
			}
		}
		SmoothCirc(rb);
		SmoothCirc(zb);
		if (std::isnan(rb[0]) || std::isnan(zb[0])) {
			a_log = "お尻の線を測れない（裾〜上端に帯の頂点が無い）";
			return 0;
		}
		float rbMin = 1e9f, rbMax = -1e9f, zbMin = 1e9f, zbMax = -1e9f;
		for (int i = 0; i < kNth; ++i) {
			zb[i] = (std::min)(zb[i], a_top - kRise);
			rbMin = (std::min)(rbMin, rb[i]), rbMax = (std::max)(rbMax, rb[i]), zbMin = (std::min)(zbMin, zb[i]), zbMax = (std::max)(zbMax, zb[i]);
		}
		float zmin = a_hem;
		for (auto& p : allp) {
			zmin = (std::min)(zmin, p.z);
		}
		const float z0 = zmin - kDz;
		const int   nz = static_cast<int>((a_top + 4.0f - z0) / kDz) + 2;
		const Grid  env = Envelope(allp, cx, cy, z0, nz, false);
		const Grid  benv = Envelope(band, cx, cy, z0, nz, true);
		// お尻の線より上は、となりとの最大を取らない体の外形で見る。取ると、お尻の上のくびれ（頂点がまばら）で横・下のお尻の山を拾い、
		// 体を 2 近く大きく見積もって、スカートがお尻から腰まで出っ張った（イソルダの農民の服 04。お尻が大きく見えた）。足りない分は押し出しが補う
		const Grid  benvUp = Envelope(band, cx, cy, z0, nz, false, true);
		const float tanA = std::tan(a_angle * kPi / 180.0f);
		int         moved = 0;
		for (std::size_t s = 0; s < a_shapes.size(); ++s) {
			for (std::size_t i = 0; i < a_shapes[s].size(); ++i) {
				auto&       p = a_shapes[s][i];
				const float aw = a_sleeve[s][i];
				if (p.z >= a_top || aw >= 1.0f) {
					continue;
				}
				const float dx = p.x - cx, dy = p.y - cy;
				const float r = std::sqrt(dx * dx + dy * dy);
				if (r < 1e-3f) {
					continue;
				}
				const float tf = (std::atan2(dy, dx) + kPi) / (2.0f * kPi) * kNth - 0.5f;
				const float zf = (p.z - z0) / kDz - 0.5f;
				const float fl = std::floor(tf);
				const int   t0 = ((static_cast<int>(fl) % kNth) + kNth) % kNth, t1 = (t0 + 1) % kNth;
				const float ft = tf - fl;
				const float RB = rb[t0] * (1 - ft) + rb[t1] * ft;
				const float ZB = zb[t0] * (1 - ft) + zb[t1] * ft;
				const float r0 = (std::max)(env.Lerp(zf, tf), 1e-3f);
				const float r0t = env.Lerp((a_top - z0) / kDz - 0.5f, tf);
				const float r0b = env.Lerp((ZB - z0) / kDz - 0.5f, tf);
				const float rhip = (std::max)(RB + a_ease, r0b);
				float       rt;
				if (a_expandOnly && p.z < ZB) {
					rt = 0.0f;  // お尻の線より下は体の外形（下の bz）だけ
				} else if (p.z < ZB) {
					rt = rhip + tanA * (ZB - p.z);
				} else {
					const float t = (std::clamp)((a_top - p.z) / (std::max)(a_top - ZB, 1e-3f), 0.0f, 1.0f);
					rt = r0t + (rhip - r0t) * (t * t * (3.0f - 2.0f * t));
				}
				const float fade = (std::clamp)((a_top - p.z) / kRise, 0.0f, 1.0f);
				const float bz = p.z < ZB ? benv.Lerp(zf, tf) : benvUp.Lerp(zf, tf);
				rt = (std::max)(rt, bz + a_ease * fade);
				// a_keepLayers（鎧）: 重なった板の間隔を保つ。外側（r0）の移動量 rt−r0 を、同じ方位・高さの全頂点へ同じだけ足す。
				// 比で縮尺して体の外形でそろえると、内側の板が全部同じ半径に潰れて、前後が入れ替わる（背中に穴が空く）
				// 縮めない（広げるほうだけ）: 鎧は目標（お尻の線＋ゆとり）より元から緩い。縮めると丸いお尻が板を突き破る
				float rn = a_keepLayers ? (std::max)(r + (std::max)(rt - r0, 0.0f), 1.0f) : (std::max)(r * rt / r0, bz + 0.5f * a_ease * fade);
				if (a_expandOnly) {
					rn = (std::max)(r * (std::max)(rt / r0, 1.0f), r);  // 広げるだけ（外側が足りない分の比で、重なった布もそろって出る）
				}
				float       f = rn / r;
				f = 1.0f + (f - 1.0f) * (std::clamp)(1.0f - aw, 0.0f, 1.0f);
				if (std::fabs(f - 1.0f) > 1e-4f) {
					p.x = cx + dx * f;
					p.y = cy + dy * f;
					++moved;
				}
			}
		}
		a_log = std::format("中心 ({:.1f}, {:.1f})、お尻の線 半径 {:.1f}〜{:.1f}・高さ {:.1f}〜{:.1f}、ゆとり {}、{}", cx, cy, rbMin, rbMax, zbMin, zbMax, a_ease,
			a_expandOnly ? std::string("切っていないので広げるだけ") : std::format("角度 {}°", a_angle));
		return moved;
	}
}
