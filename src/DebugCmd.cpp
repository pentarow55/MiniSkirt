// 開発用: MiniSkirt.log と同じフォルダの MiniSkirt_cmd.txt に書いたコマンドを、ゲーム内で実行する（実行したらファイルを消す）。
// コンソールのキー入力を使わずに、NPC を出す・動かす・消すのを自動化するため。公開前に外す。
//   @spawn <ベースの FormID(16進)>      プレイヤーの足元に出す。出した参照の FormID を結果ファイルに書く
//   @on <参照の FormID(16進)> <コマンド>  その参照を対象にコンソールコマンドを実行する（例: @on FF000D95 moveto player 0 400 0）
//   @enabled <0|1>                       MiniSkirt の有効／無効を切り替え、読み込み済みの NPC を作り直す
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
