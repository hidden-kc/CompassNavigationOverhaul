#include "Settings.h"

#include "IUI/API.h"
#include "NND/NPCNameProvider.h"

#include "Compass.h"
#include "QuestItemList.h"
#include "Test.h"

#include "IUI/GFxLoggers.h"

#include "Hooks.h"
#include "UI.h"

#undef GetModuleHandle

const SKSE::LoadInterface* skse;

void InfinityUIMessageListener(SKSE::MessagingInterface::Message* a_msg);

void SKSEMessageListener(SKSE::MessagingInterface::Message* a_msg)
{
	// If all plugins have been loaded
	if (a_msg->type == SKSE::MessagingInterface::kPostLoad) 
	{
		if (SKSE::GetMessagingInterface()->RegisterListener("InfinityUI", InfinityUIMessageListener)) 
		{
			logger::info("Successfully registered for Infinity UI messages!");
		}
		else 
		{
			logger::error("Infinity UI installation not detected. Please, download it from https://www.nexusmods.com/skyrimspecialedition/mods/74483");
		}

		NND::NPCNameProvider::GetSingleton()->RequestAPI();

		const SKSE::PluginInfo* mapMarkerFrameworkPluginInfo = skse->GetPluginInfo("MapMarkerFramework");

		if (mapMarkerFrameworkPluginInfo && mapMarkerFrameworkPluginInfo->version < 0x02020000)
		{
			logger::info("CoMAP detected. Loading compatibility patch...");
			hooks::compat::MapMarkerFramework::Install(SKSE::WinAPI::GetModuleHandle("MapMarkerFramework.dll"));
			hooks::compat::MapMarkerFramework::pluginInfo = mapMarkerFrameworkPluginInfo;
			logger::info("Successfully loaded compatibility patch for CoMAP!");
		}
	}
	else if (a_msg->type == SKSE::MessagingInterface::kPostPostLoad)
	{
		UI::Register();
	}
}

void InfinityUIMessageListener(SKSE::MessagingInterface::Message* a_msg)
{
	if (!a_msg || std::string_view(a_msg->sender) != "InfinityUI") 
	{
		return;
	}

	if (auto message = IUI::API::TranslateAs<IUI::API::Message>(a_msg))
	{
		// Infinity UI is expected to always populate movie/GetMovieDef() for every message it
		// sends, but a plugin combination we have not tested could plausibly deliver one that
		// does not - dereferencing that unconditionally is exactly how the sibling mod's
		// crash happened, so bail out rather than assume the contract always holds.
		if (!message->movie)
		{
			logger::error("Infinity UI message (type {}) has no movie; ignoring it", static_cast<std::uint32_t>(a_msg->type));

			return;
		}

		RE::GFxMovieDef* movieDef = message->movie->GetMovieDef();

		if (!movieDef)
		{
			logger::error("Infinity UI message (type {}) has no movie definition; ignoring it", static_cast<std::uint32_t>(a_msg->type));

			return;
		}

		std::string_view movieUrl = movieDef->GetFileURL();

		if (movieUrl.find("HUDMenu") == std::string::npos)
		{
			return;
		}

		GFxMemberLogger<logger::level::debug> memberLogger;

		switch (a_msg->type)
		{
		case IUI::API::Message::Type::kStartLoadInstances:
			logger::info("Started loading HUD patches");
			break;
		case IUI::API::Message::Type::kPreReplaceInstance:
			if (auto preReplaceMessage = IUI::API::TranslateAs<IUI::API::PreReplaceInstanceMessage>(a_msg))
			{
				std::string pathToOriginal = preReplaceMessage->originalInstance.ToString().c_str();

				if (pathToOriginal == CNO::Compass::path)
				{
					CNO::Compass::InitSingleton(preReplaceMessage->originalInstance);

					// InitSingleton only sets the singleton the first time it is called, so a
					// re-entrant or unexpectedly-ordered kPreReplaceInstance message could
					// plausibly leave it unset here - the same "it always worked before"
					// assumption that caused the sibling mod's crash. GetSingleton() is
					// checked a few lines further down in the kPostPatchInstance case; do the
					// same here instead of trusting InitSingleton unconditionally.
					if (auto compass = CNO::Compass::GetSingleton())
					{
						logger::debug("Before replacing:");
						memberLogger.LogMembersOf(*compass);

						RE::GPointF coord = compass->LocalToGlobal();
						logger::debug("{} is on ({}, {})", compass->ToString().c_str(), coord.x, coord.y);
					}
					else
					{
						logger::error("Compass singleton could not be initialized for {}", CNO::Compass::path);
					}
				}
			}
			break;
		case IUI::API::Message::Type::kPostPatchInstance:
			if (auto postPatchMessage = IUI::API::TranslateAs<IUI::API::PostPatchInstanceMessage>(a_msg))
			{
				std::string pathToNew = postPatchMessage->newInstance.ToString().c_str();

				if (pathToNew == CNO::Compass::path)
				{
					// We initialised the CompassShoutMeterHolder singleton in the pre-replace step,
					// if not, there has been an error
					if (auto compass = CNO::Compass::GetSingleton())
					{
						compass->SetupMod(postPatchMessage->newInstance);
						compass->SetUnits(settings::display::useMetricUnits);
							
						logger::debug("After replacing:");
						memberLogger.LogMembersOf(*compass);

						RE::GPointF coord = compass->LocalToGlobal();
						logger::debug("{} is on ({}, {})", compass->ToString().c_str(), coord.x, coord.y);

						if (hooks::compat::MapMarkerFramework::pluginInfo)
						{
							hooks::compat::MapMarkerFramework::compassMovieDef = postPatchMessage->newInstanceMovieDef;
						}
					}
					else
					{
						logger::error("Compass instance counterpart not ready for {}", CNO::Compass::path);
					}
				}
				else if (pathToNew == QuestItemList::path)
				{
					QuestItemList::InitSingleton(postPatchMessage->newInstance);

					// Same reasoning as the Compass singleton above: InitSingleton is not
					// guaranteed to have set the singleton by the time we read it back here.
					if (auto questItemList = QuestItemList::GetSingleton())
					{
						memberLogger.LogMembersOf(*questItemList);

						RE::GPointF coord = questItemList->LocalToGlobal();
						logger::debug("{} is on ({}, {})", questItemList->ToString().c_str(), coord.x, coord.y);
					}
					else
					{
						logger::error("QuestItemList singleton could not be initialized for {}", QuestItemList::path);
					}
				}
			}
			break;
		case IUI::API::Message::Type::kAbortPatchInstance:
			if (auto abortPatchMessage = IUI::API::TranslateAs<IUI::API::AbortPatchInstanceMessage>(a_msg))
			{
				std::string pathToOriginal = abortPatchMessage->originalValue.ToString().c_str();

				if (pathToOriginal == CNO::Compass::path)
				{
					logger::error("Aborted replacement of {}", CNO::Compass::path);
				}
			}
			break;
		case IUI::API::Message::Type::kFinishLoadInstances:
			if (auto finishLoadMessage = IUI::API::TranslateAs<IUI::API::FinishLoadInstancesMessage>(a_msg))
			{
				RE::GFxValue test;
				if (finishLoadMessage->movie->GetVariable(&test, Test::path.data()))
				{
					Test::InitSingleton(test);
				}
			}
			logger::info("Finished loading HUD patches");
			break;
		case IUI::API::Message::Type::kPostInitExtensions:
			if (auto postInitExtMessage = IUI::API::TranslateAs<IUI::API::PostInitExtensionsMessage>(a_msg))
			{
				if (auto questItemList = QuestItemList::GetSingleton())
				{
					questItemList->AddToHudElements();

					logger::debug("QuestItemList added to HUD elements");
				}

				logger::debug("Extensions initialization finished");
			}
			break;
		default:
			break;
		}
	}
}
