#include "Settings.h"

#include "Diagnostics.h"

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
		// DevBenchAPI's own contract: the interface can only be requested once SKSE has sent
		// kPostLoad, since that's the earliest point every plugin (DevBench included) has had
		// its own SKSEPluginLoad run.
		logger::debug("kPostLoad received; registering live diagnostics with DevBench if present");
		diagnostics::Init();

		const bool infinityUiRegistered =
			SKSE::GetMessagingInterface()->RegisterListener("InfinityUI", InfinityUIMessageListener);

		diagnostics::RecordInfinityUiListener(infinityUiRegistered);

		if (infinityUiRegistered)
		{
			logger::info("Successfully registered for Infinity UI messages!");
		}
		else
		{
			logger::error("Infinity UI installation not detected. Please, download it from https://www.nexusmods.com/skyrimspecialedition/mods/74483");
		}

		// First attempt. RequestAPI() is idempotent and no-ops once it has succeeded, so it is
		// retried at kDataLoaded below - CLAUDE.md rule 17. A single early miss used to leave
		// NPC names silently degraded to vanilla for the rest of the session.
		NND::NPCNameProvider::GetSingleton()->RequestAPI();

		const SKSE::PluginInfo* mapMarkerFrameworkPluginInfo = skse->GetPluginInfo("MapMarkerFramework");

		diagnostics::RecordComapDetection(mapMarkerFrameworkPluginInfo != nullptr,
			mapMarkerFrameworkPluginInfo ? mapMarkerFrameworkPluginInfo->version : 0);

		if (mapMarkerFrameworkPluginInfo && mapMarkerFrameworkPluginInfo->version < 0x02020000)
		{
			logger::info("CoMAP detected. Loading compatibility patch...");
			if (hooks::compat::MapMarkerFramework::Install(SKSE::WinAPI::GetModuleHandle("MapMarkerFramework.dll")))
			{
				hooks::compat::MapMarkerFramework::pluginInfo = mapMarkerFrameworkPluginInfo;
				logger::info("Successfully loaded compatibility patch for CoMAP!");

				diagnostics::RecordComapPatchApplied();
			}
			else
			{
				logger::warn("CoMAP compatibility patch was not applied; CoMAP and this mod will both run, unpatched");

				// No RecordComapPatchSkipped() here on purpose: both of Install()'s own
				// `return false` paths record their specific reason at the point the decision
				// was actually made. Restating it from out here could only make it vaguer, or
				// worse, overwrite the real reason with a guess.
			}
		}
		else if (mapMarkerFrameworkPluginInfo)
		{
			// CoMAP 2.2.0.0 and later handles the compass movie def itself, so there is nothing
			// to patch. Explicitly "not attempted" rather than "skipped" - nothing went wrong.
			logger::info("CoMAP is installed but needs no compatibility patch at this version; not attempting it");

			diagnostics::RecordComapPatchNotAttempted(
				"MapMarkerFramework (CoMAP) is installed but its version is 2.2.0.0 or newer, "
				"which handles the compass itself and needs no patch");
		}
		else
		{
			logger::debug("MapMarkerFramework (CoMAP) is not installed; no compatibility patch needed");

			diagnostics::RecordComapPatchNotAttempted("MapMarkerFramework (CoMAP) is not installed");
		}

		// Detection only - the deferred quest-marker path in CNO::HUDMarkerManager is passive and
		// needs no coordination with the other plugin. This exists so the log and the DevBench
		// status tool can tell the known Quest Marker Limit Fix interplay (its replayed markers
		// needing end-of-frame reconciliation) apart from some other cause when quest markers show
		// but carry no details.
		const SKSE::PluginInfo* questMarkerLimitFixPluginInfo = skse->GetPluginInfo("Quest Marker Limit Fix");

		diagnostics::RecordQmlfDetection(questMarkerLimitFixPluginInfo != nullptr,
			questMarkerLimitFixPluginInfo ? questMarkerLimitFixPluginInfo->version : 0);

		if (questMarkerLimitFixPluginInfo)
		{
			logger::info("Quest Marker Limit Fix detected; quest markers it replays itself will be "
						 "reconciled against the marker array at the end of each frame");
		}
		else
		{
			logger::debug("Quest Marker Limit Fix is not installed; quest markers are processed at "
						  "their AddMarker calls as usual");
		}
	}
	else if (a_msg->type == SKSE::MessagingInterface::kPostPostLoad)
	{
		UI::Register();

		// Rule-17 retry: a real launch showed devbench's own server can still be finishing
		// startup a moment after kPostLoad fires, which is early enough to lose the race even
		// though kPostLoad is DevBenchAPI's own documented earliest-safe point. Cheap no-op if
		// the kPostLoad attempt already succeeded.
		diagnostics::Init();
	}
	else if (a_msg->type == SKSE::MessagingInterface::kDataLoaded)
	{
		// Second and last attempt at the NND API (CLAUDE.md rule 17). By kDataLoaded every
		// plugin has finished loading, so if it is not available now it is not installed.
		NND::NPCNameProvider::GetSingleton()->RequestAPI();

		// Last retry point - if DevBench still isn't found here, conclude it isn't installed
		// and say so, rather than staying silent about it forever.
		diagnostics::Init(/* a_lastAttempt = */ true);
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
			diagnostics::RecordHudPatchStarted();
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
						diagnostics::RecordCompassInstanceReady(true);

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

						diagnostics::RecordCompassInstanceReady(false);
					}
				}
				else if (pathToNew == QuestItemList::path)
				{
					QuestItemList::InitSingleton(postPatchMessage->newInstance);

					// Same reasoning as the Compass singleton above: InitSingleton is not
					// guaranteed to have set the singleton by the time we read it back here.
					if (auto questItemList = QuestItemList::GetSingleton())
					{
						diagnostics::RecordQuestItemListReady(true);

						memberLogger.LogMembersOf(*questItemList);

						RE::GPointF coord = questItemList->LocalToGlobal();
						logger::debug("{} is on ({}, {})", questItemList->ToString().c_str(), coord.x, coord.y);
					}
					else
					{
						logger::error("QuestItemList singleton could not be initialized for {}", QuestItemList::path);

						diagnostics::RecordQuestItemListReady(false);
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
			diagnostics::RecordHudPatchFinished();
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
