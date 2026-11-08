#include "PCH.h"

#include "Layout.h"

namespace Probe
{
	void Early();
	void Install();
}

#ifdef MINISKIRT_DEVTOOLS
namespace DebugCmd
{
	void Install();
}
#endif

namespace
{
	void InitializeLog()
	{
		auto path = logger::log_directory();
		if (!path) {
			return;
		}
		*path /= "MiniSkirt.log"sv;

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), false);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			logger::info("データの読み込み完了 (kDataLoaded)");
			Probe::Install();
#ifdef MINISKIRT_DEVTOOLS
			DebugCmd::Install();
#endif
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	SKSE::Init(a_skse);

	logger::info("################ 起動 ################");
	logger::info("MiniSkirt v{} を読み込みました", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string());
	logger::info("ゲームのバージョン: {}", a_skse->RuntimeVersion().string());
	Layout::LogStartup();

	// 帯ファイルはゲームの初期化より前に書き出しておく（その起動から読めるように）
	Probe::Early();

	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
