// 開発用: MiniSkirt.log と同じフォルダの MiniSkirt_cmd.txt に書いたコマンドを、ゲーム内で実行する（実行したらファイルを消す）。
// コンソールのキー入力を使わずに、NPC を出す・動かす・消すのを自動化するため。公開前に外す。
//   @spawn <ベースの FormID(16進)>      プレイヤーの足元に出す。出した参照の FormID を結果ファイルに書く
//   @on <参照の FormID(16進)> <コマンド>  その参照を対象にコンソールコマンドを実行する（例: @on FF000D95 moveto player 0 400 0）
//   @enabled <0|1>                       MiniSkirt の有効／無効を切り替え、読み込み済みの NPC を作り直す
//   @hem <cm>                            スカート丈を上書きする（MCM と同じ値。0 で上書きをやめる）
//   @cam <x> <y> <z> <pitch> <yaw>       自由カメラにして、位置（ワールド座標）と向き（度。pitch は正で下向き、yaw は 0 で北・時計回り）を決める（SE/AE だけ）
//   @camoff                              自由カメラをやめる
//   @fov <度>                            視野角
//   @freeze <0|1>                        時間を止める／動かす（描画は続く）
//   @where <参照の FormID(16進)|player|cam>  位置と向きを結果ファイルに書く（構図を決める用）
//   @done <目印>                         結果ファイルに目印を書く（呼び出し側が終わりを待つ用）
//   上記以外の行                          プレイヤーを対象にコンソールコマンドを実行する
// 結果は MiniSkirt_cmd_result.txt に追記する。

#include "PCH.h"

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
			} else if (a_line.rfind("@enabled ", 0) == 0) {
				Probe::DebugSetEnabled(a_line.substr(9) != "0");
			} else if (a_line.rfind("@hem ", 0) == 0) {
				// スカート丈（cm、MCM と同じ値。16〜55）。「@hem 0」で上書きをやめる
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
				if (auto main = RE::Main::GetSingleton()) {
					main->GetRuntimeData().freezeTime = a_line.substr(8) != "0";
				}
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
						auto main = RE::Main::GetSingleton();
						Result(std::format("where cam {:.1f} {:.1f} {:.1f} state={} free={} freeze={}", t.x, t.y, t.z, idx, camera->IsInFreeCameraMode(),
							main ? main->GetRuntimeData().freezeTime : false));
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
