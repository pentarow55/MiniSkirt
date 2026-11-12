// 開発用: MiniSkirt.log と同じフォルダの MiniSkirt_cmd.txt に書いたコマンドを、ゲーム内で実行する（実行したらファイルを消す）。
// コンソールのキー入力を使わずに、NPC を出す・動かす・消すのを自動化するため。公開前に外す。
//   @spawn <ベースの FormID(16進)>      プレイヤーの足元に出す。出した参照の FormID を結果ファイルに書く
//   @on <参照の FormID(16進)> <コマンド>  その参照を対象にコンソールコマンドを実行する（例: @on FF000D95 moveto player 0 400 0）
//   @anim <NPC の参照> <イベント名>        アニメーションのグラフにイベントを送る（受け付けたら 1）
//   @enabled <0|1>                       MiniSkirt の有効／無効を切り替え、読み込み済みの NPC を作り直す
//   @hem <%>                             スカート丈を上書きする（MCM と同じ値。0 で上書きをやめる）
//   @flag <番号> <0|1>                   開発用の切り替え（DevFlags.h）。不具合の切り分けで処理の一部を変える
//   @cam <x> <y> <z> <pitch> <yaw>       自由カメラにして、位置（ワールド座標）と向き（度。pitch は正で下向き、yaw は 0 で北・時計回り）を決める（SE/AE だけ）
//   @camoff                              自由カメラをやめる
//   @fov <度>                            視野角
//   @freeze <0|1>                        自由カメラに入り直して、時間を止める／動かす（tfc 1 と同じ）
//   @where <参照の FormID(16進)|player|cam>  位置と向きを結果ファイルに書く（構図を決める用）
//   @actors                              近くの NPC の参照・位置・性別・名前を結果ファイルに書く
//   @place <参照> <x> <y> <z> <向き(度)>  参照を同じセルの中の位置に置き、向きを決める
//   @npc <名前の一部>                     表示名で NPC のベースを探し、ベースの FormID・性別・名前を書く
//   @furn <参照|player> [半径]            まわりの家具の参照・位置・名前を結果ファイルに書く
//   @ray <x1> <y1> <z1> <x2> <y2> <z2>  2 点の間を遮る物があるか、当たるまでの割合を書く（1 で見通せる）
//   @done <目印>                         結果ファイルに目印を書く（呼び出し側が終わりを待つ用）
//   上記以外の行                          プレイヤーを対象にコンソールコマンドを実行する
// 結果は MiniSkirt_cmd_result.txt に追記する。

#include "PCH.h"

#include "DevFlags.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace Probe
{
	void DebugSetEnabled(bool a_on);
	void DebugSetLength(float a_cm);
}

namespace DebugCmd
{
	namespace
	{
		std::filesystem::path Dir()
		{
			auto p = logger::log_directory();
			return p ? *p : std::filesystem::path{};
		}

		void Result(const std::string& a_line)
		{
			std::ofstream f(Dir() / "MiniSkirt_cmd_result.txt", std::ios::app);
			f << a_line << "\n";
		}

		void RunScript(const std::string& a_cmd, RE::TESObjectREFR* a_target)
		{
			auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto script = factory ? factory->Create() : nullptr;
			if (!script) {
				logger::info("デバッグコマンド: Script を作れない");
				return;
			}
			script->SetCommand(a_cmd);
			script->CompileAndRun(a_target);
			delete script;
		}

		void Exec(const std::string& a_line)
		{
			auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			if (a_line.rfind("@spawn ", 0) == 0) {
				const auto id = static_cast<RE::FormID>(std::stoul(a_line.substr(7), nullptr, 16));
				auto base = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
				if (!base) {
					logger::info("デバッグコマンド: ベース {:08X} が無い", id);
					Result(std::format("spawn-failed {:08X}", id));
					return;
				}
				auto ref = player->PlaceObjectAtMe(base, false);
				logger::info("デバッグコマンド: {:08X} を出した → 参照 {:08X}", id, ref ? ref->GetFormID() : 0u);
				Result(std::format("spawn {:08X} {:08X}", id, ref ? ref->GetFormID() : 0u));
			} else if (a_line.rfind("@on ", 0) == 0) {
				std::istringstream ss(a_line.substr(4));
				std::string        hex;
				ss >> hex;
				std::string rest;
				std::getline(ss, rest);
				const auto id = static_cast<RE::FormID>(std::stoul(hex, nullptr, 16));
				auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!ref) {
					logger::info("デバッグコマンド: 参照 {:08X} が無い", id);
					return;
				}
				RunScript(rest, ref);
			} else if (a_line.rfind("@anim ", 0) == 0) {
				// NPC のアニメーションのグラフにイベントを送る（座る・しゃがむなどの姿勢を作る）。受け付けられたかを結果に書く
				std::istringstream ss(a_line.substr(6));
				std::string        a, ev;
				ss >> a >> ev;
				auto       actor = RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(std::stoul(a, nullptr, 16)));
				const bool ok = actor && actor->NotifyAnimationGraph(ev);
				logger::info("デバッグコマンド: {} にアニメーションのイベント {} → {}", a, ev, ok);
				Result(std::format("anim {} {} {}", a, ev, ok ? 1 : 0));
			} else if (a_line.rfind("@enabled ", 0) == 0) {
				Probe::DebugSetEnabled(a_line.substr(9) != "0");
			} else if (a_line.rfind("@flag ", 0) == 0) {
				// 開発用の切り替え（DevFlags.h）。変えたあとで @enabled 0 → 1 などで作り直すと効く
				std::istringstream ss(a_line.substr(6));
				int                i = -1, v = 0;
				ss >> i >> v;
				if (i >= 0 && i < DevFlags::kCount) {
					DevFlags::g[i] = v;
				}
				logger::info("デバッグコマンド: フラグ {} = {}", i, v);
				Result(std::format("flag {} {}", i, v));
			} else if (a_line.rfind("@hem ", 0) == 0) {
				// スカート丈（%、MCM と同じ値。16〜100）。「@hem 0」で上書きをやめる
				Probe::DebugSetLength(std::stof(a_line.substr(5)));
				Result("hem " + a_line.substr(5));
			} else if (a_line.rfind("@cam ", 0) == 0 || a_line == "@camoff") {
				auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera || REL::Module::IsVR()) {
					Result("cam-failed");
					return;
				}
				if (a_line == "@camoff") {
					if (camera->IsInFreeCameraMode()) {
						camera->ToggleFreeCameraMode(false);
					}
					Result("camoff");
					return;
				}
				std::istringstream ss(a_line.substr(5));
				float              x, y, z, pitch, yaw;
				if (!(ss >> x >> y >> z >> pitch >> yaw)) {
					Result("cam-failed args");
					return;
				}
				if (!camera->IsInFreeCameraMode()) {
					camera->ToggleFreeCameraMode(false);
				}
				auto state = static_cast<RE::FreeCameraState*>(camera->GetRuntimeData().cameraStates[RE::CameraState::kFree].get());
				if (!state) {
					Result("cam-failed state");
					return;
				}
				constexpr float kRad = 3.14159265f / 180.0f;
				state->translation = { x, y, z };
				state->rotation.x = pitch * kRad;
				state->rotation.y = yaw * kRad;
				Result(std::format("cam {} {} {} {} {}", x, y, z, pitch, yaw));
			} else if (a_line.rfind("@fov ", 0) == 0) {
				if (auto camera = RE::PlayerCamera::GetSingleton()) {
					camera->GetRuntimeData2().worldFOV = std::stof(a_line.substr(5));
				}
			} else if (a_line.rfind("@freeze ", 0) == 0) {
				// 自由カメラに入り直して時間停止を切り替える（コンソールの tfc 1 と同じ。Main の値を直接書くと 1.7.104 では描画まで止まった）
				auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera || REL::Module::IsVR()) {
					return;
				}
				if (camera->IsInFreeCameraMode()) {
					camera->ToggleFreeCameraMode(false);
				}
				camera->ToggleFreeCameraMode(a_line.substr(8) != "0");
			} else if (a_line.rfind("@where ", 0) == 0) {
				const auto arg = a_line.substr(7);
				if (arg == "cam") {
					auto camera = RE::PlayerCamera::GetSingleton();
					if (camera && camera->cameraRoot) {
						const auto& t = camera->cameraRoot->world.translate;
						int         idx = -1;
						for (int i = 0; i < RE::CameraState::kTotal; ++i) {
							if (camera->currentState && camera->currentState == camera->GetRuntimeData().cameraStates[i]) {
								idx = i;
							}
						}
						Result(std::format("where cam {:.1f} {:.1f} {:.1f} state={} free={}", t.x, t.y, t.z, idx, camera->IsInFreeCameraMode()));
					}
					return;
				}
				RE::TESObjectREFR* ref = arg == "player" ? player : RE::TESForm::LookupByID<RE::TESObjectREFR>(static_cast<RE::FormID>(std::stoul(arg, nullptr, 16)));
				if (!ref) {
					Result("where-failed " + arg);
					return;
				}
				const auto p = ref->GetPosition();
				Result(std::format("where {} {:.1f} {:.1f} {:.1f} {:.1f}", arg, p.x, p.y, p.z, ref->GetAngleZ() * 180.0f / 3.14159265f));
			} else if (a_line == "@actors") {
				// 読み込み済みの NPC（プレイヤー以外）を「actor 参照 x y z 女性 名前」で書く（下見で人の集まる所を探す用）
				if (auto pl = RE::ProcessLists::GetSingleton()) {
					for (auto& h : pl->highActorHandles) {
						auto a = h.get();
						if (!a || a->IsDead()) {
							continue;
						}
						const auto p = a->GetPosition();
						auto       base = a->GetActorBase();
						Result(std::format("actor {:08X} {:.0f} {:.0f} {:.0f} {} {}", a->GetFormID(), p.x, p.y, p.z, base && base->IsFemale() ? 1 : 0, a->GetName()));
					}
				}
			} else if (a_line.rfind("@place ", 0) == 0) {
				// 参照を同じセルの中の位置に置き、向き（度。0 で北・時計回り）を決める
				std::istringstream ss(a_line.substr(7));
				std::string        hex;
				float              x, y, z, deg;
				if (!(ss >> hex >> x >> y >> z >> deg)) {
					Result("place-failed args");
					return;
				}
				auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(static_cast<RE::FormID>(std::stoul(hex, nullptr, 16)));
				if (!ref) {
					Result("place-failed " + hex);
					return;
				}
				const float rad = deg * 3.14159265f / 180.0f;
				ref->SetPosition(RE::NiPoint3{ x, y, z });
				if (auto actor = ref->As<RE::Actor>()) {
					actor->SetHeading(rad);
				} else {
					ref->SetAngle(RE::NiPoint3{ 0.0f, 0.0f, rad });
				}
				Result(std::format("place {} {} {} {} {}", hex, x, y, z, deg));
			} else if (a_line.rfind("@npc ", 0) == 0) {
				// 名前（ゲームの表示名の一部）で NPC のベースを探し、「npc ベース 女性 名前」で書く（@spawn で出す用）
				const auto key = a_line.substr(5);
				if (auto dh = RE::TESDataHandler::GetSingleton()) {
					for (auto npc : dh->GetFormArray<RE::TESNPC>()) {
						const char* n = npc ? npc->GetName() : nullptr;
						if (n && std::string_view(n).find(key) != std::string_view::npos) {
							Result(std::format("npc {:08X} {} {}", npc->GetFormID(), npc->IsFemale() ? 1 : 0, n));
						}
					}
				}
			} else if (a_line.rfind("@furn ", 0) == 0) {
				// 参照のまわり（半径）にある家具を「furn 参照 x y z 名前」で書く（NPC に使わせる金床・椅子などを探す用）
				std::istringstream ss(a_line.substr(6));
				std::string        hex;
				float              radius = 500.0f;
				ss >> hex >> radius;
				auto origin = hex == "player" ? player : RE::TESForm::LookupByID<RE::TESObjectREFR>(static_cast<RE::FormID>(std::stoul(hex, nullptr, 16)));
				auto tes = RE::TES::GetSingleton();
				if (!origin || !tes) {
					Result("furn-failed " + hex);
					return;
				}
				tes->ForEachReferenceInRange(origin, radius, [](RE::TESObjectREFR* a_ref) {
					auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
					if (base && base->Is(RE::FormType::Furniture)) {
						const auto p = a_ref->GetPosition();
						const char* n = base->GetName();
						Result(std::format("furn {:08X} {:.0f} {:.0f} {:.0f} {}", a_ref->GetFormID(), p.x, p.y, p.z, n && *n ? n : base->GetFormEditorID()));
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			} else if (a_line.rfind("@ray ", 0) == 0) {
				// 2 点の間に壁や物があるか（視線の当たり判定）。「ray <当たるまでの割合>」を書く（1 で何も無い）
				std::istringstream ss(a_line.substr(5));
				RE::NiPoint3       a, b;
				if (!(ss >> a.x >> a.y >> a.z >> b.x >> b.y >> b.z)) {
					Result("ray-failed args");
					return;
				}
				auto cell = player->GetParentCell();
				auto world = cell ? cell->GetbhkWorld() : nullptr;
				if (!world) {
					Result("ray-failed world");
					return;
				}
				const float    s = RE::bhkWorld::GetWorldScale();
				RE::bhkPickData pick;
				pick.rayInput.from = RE::hkVector4(a.x * s, a.y * s, a.z * s, 0.0f);
				pick.rayInput.to = RE::hkVector4(b.x * s, b.y * s, b.z * s, 0.0f);
				pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
				world->PickObject(pick);
				Result(std::format("ray {:.3f}", pick.rayOutput.HasHit() ? pick.rayOutput.hitFraction : 1.0f));
			} else if (a_line.rfind("@done ", 0) == 0) {
				Result("done " + a_line.substr(6));
			} else if (!a_line.empty()) {
				RunScript(a_line, player);
			}
		}

		void Poll()
		{
			const auto path = Dir() / "MiniSkirt_cmd.txt";
			std::error_code ec;
			while (true) {
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
				if (!std::filesystem::exists(path, ec)) {
					continue;
				}
				std::vector<std::string> lines;
				{
					std::ifstream f(path);
					std::string   l;
					while (std::getline(f, l)) {
						while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) {
							l.pop_back();
						}
						if (!l.empty()) {
							lines.push_back(l);
						}
					}
				}
				std::filesystem::remove(path, ec);
				SKSE::GetTaskInterface()->AddTask([lines] {
					for (auto& l : lines) {
						try {
							Exec(l);
						} catch (...) {
							logger::info("デバッグコマンド: 実行に失敗: {}", l);
						}
					}
				});
			}
		}
	}

	void Install()
	{
		std::thread(Poll).detach();
		logger::info("デバッグコマンド: MiniSkirt_cmd.txt を監視する（開発用。公開前に外す）");
	}
}
