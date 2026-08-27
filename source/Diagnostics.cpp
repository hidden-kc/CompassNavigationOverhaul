#include "Diagnostics.h"

#include "DevBench/DevBenchAPI.h"
#include "Settings.h"
#include "utils/Logger.h"

#include <array>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>

namespace diagnostics
{
	namespace
	{
		using clock = std::chrono::steady_clock;

		std::mutex mtx;

		enum class ComapState
		{
			kNotAttempted,
			kApplied,
			kSkipped
		};

		constexpr std::size_t hookGroupCount = static_cast<std::size_t>(HookGroup::kTotal);

		constexpr std::array<const char*, hookGroupCount> hookGroupNames{
			"updateQuests",
			"allowedToShowMapMarker",
			"updateLocations",
			"updateEnemies",
			"updatePlayerSetMarker",
			"compassUpdateVFunc"
		};

		struct State
		{
			// Hook installation (main.cpp -> hooks::Install(), once, before kPostLoad)
			bool hookInstallStarted = false;
			bool hookInstallCompleted = false;
			bool trampolineAllocated = false;
			bool trampolineValid = false;
			std::size_t trampolineBytes = 0;
			std::array<bool, hookGroupCount> hookGroupInstalled{};
			std::optional<clock::time_point> lastHookInstall;

			// CoMAP compatibility patch
			ComapState comapState = ComapState::kNotAttempted;
			std::string comapReason = "the patch decision has not been made yet (kPostLoad not reached)";
			bool comapDetected = false;
			bool comapDetectionRun = false;
			std::uint32_t comapVersion = 0;
			std::optional<clock::time_point> lastComapDecision;

			// Infinity UI / HUD patch lifecycle
			bool infinityUiListenerAttempted = false;
			bool infinityUiListenerRegistered = false;
			std::uint64_t hudPatchesStarted = 0;
			std::uint64_t hudPatchesFinished = 0;
			bool compassInstanceReady = false;
			bool questItemListReady = false;
			std::optional<clock::time_point> lastHudPatch;

			// Per-frame marker bookkeeping
			std::uint64_t questMarkersProcessed = 0;
			std::optional<clock::time_point> lastQuestMarker;

			std::uint64_t locationMarkersProcessed = 0;
			std::optional<clock::time_point> lastLocationMarker;

			std::uint64_t enemyMarkersProcessed = 0;
			std::optional<clock::time_point> lastEnemyMarker;

			std::uint64_t playerSetMarkersProcessed = 0;
			std::optional<clock::time_point> lastPlayerSetMarker;

			std::uint64_t compassUpdates = 0;
			std::optional<clock::time_point> lastCompassUpdate;
			std::size_t lastFacedMarkers = 0;
			std::size_t lastQuestMarkerRefs = 0;
			std::size_t lastMiscQuestMarkerRefs = 0;
			bool lastHadFocusedMarker = false;

			std::uint64_t compassUpdatesSkipped = 0;
			std::optional<clock::time_point> lastCompassUpdateSkipped;
			std::string lastCompassUpdateSkipReason;

			std::uint64_t focusChanges = 0;
			std::optional<clock::time_point> lastFocusChange;
			std::string lastFocusedDescription;
		};

		State state;

		// Escapes the handful of characters JSON requires; every string this module puts into a
		// value is either a fixed literal from our own code (a skip reason) or a marker
		// description that ultimately came from a plugin's own text, so this is defensive rather
		// than load-bearing for our own literals - but a quest objective or location name really
		// can contain a quote, so it is load-bearing for those.
		std::string EscapeJson(std::string_view a_text)
		{
			std::string out;
			out.reserve(a_text.size());

			for (char c : a_text)
			{
				switch (c)
				{
				case '"':
					out += "\\\"";
					break;
				case '\\':
					out += "\\\\";
					break;
				case '\n':
					out += "\\n";
					break;
				default:
					out += c;
					break;
				}
			}

			return out;
		}

		// Renders "field": null or "field": <seconds ago>, so a query can tell "never happened"
		// apart from "happened a long time ago" instead of both looking like a missing/zero
		// field.
		std::string SecondsAgoField(const char* a_name, const std::optional<clock::time_point>& a_when)
		{
			if (!a_when)
			{
				return std::format("\"{}SecondsAgo\": null", a_name);
			}

			const double seconds = std::chrono::duration<double>(clock::now() - *a_when).count();

			return std::format("\"{}SecondsAgo\": {:.1f}", a_name, seconds);
		}

		const char* Bool(bool a_value)
		{
			return a_value ? "true" : "false";
		}

		const char* ComapStateName(ComapState a_state)
		{
			switch (a_state)
			{
			case ComapState::kApplied:
				return "applied";
			case ComapState::kSkipped:
				return "skipped";
			default:
				return "notAttempted";
			}
		}

		// SKSE packs a plugin version as major<<24 | minor<<16 | patch<<8 | build, which is what
		// the CoMAP version comparison in MessageListeners.cpp is written against - decode it so
		// a query does not have to.
		std::string DecodeVersion(std::uint32_t a_version)
		{
			return std::format("{}.{}.{}.{}", (a_version >> 24) & 0xFF, (a_version >> 16) & 0xFF,
				(a_version >> 8) & 0xFF, a_version & 0xFF);
		}

		std::string BuildSettingsJson()
		{
			// Deliberately a selection, not a dump: these are the settings that change what a
			// marker on the compass does, which is what a "why is it behaving like this"
			// question is actually about. The purely cosmetic quest-list geometry is included
			// only because an off-screen list is indistinguishable from a broken one.
			return std::format(
				"\"settings\":{{"
				"\"logLevel\":{},"
				"\"display\":{{"
				"\"useMetricUnits\":{},"
				"\"showUndiscoveredLocationMarkers\":{},"
				"\"undiscoveredMeansUnknownMarkers\":{},"
				"\"undiscoveredMeansUnknownInfo\":{},"
				"\"showEnemyMarkers\":{},"
				"\"showEnemyNameUnderMarker\":{},"
				"\"showObjectiveAsTarget\":{},"
				"\"showOtherObjectivesCount\":{},"
				"\"showInteriorMarkers\":{},"
				"\"angleToShowMarkerDetails\":{:.2f},"
				"\"angleToKeepMarkerDetailsShown\":{:.2f},"
				"\"focusingDelayToShow\":{:.2f}"
				"}},"
				"\"questList\":{{"
				"\"showInExteriors\":{},"
				"\"showInInteriors\":{},"
				"\"hideInCombat\":{},"
				"\"positionX\":{:.3f},"
				"\"positionY\":{:.3f},"
				"\"maxHeight\":{:.3f},"
				"\"walkingDelayToShow\":{:.2f},"
				"\"joggingDelayToShow\":{:.2f},"
				"\"sprintingDelayToShow\":{:.2f}"
				"}}"
				"}}",
				static_cast<int>(settings::debug::logLevel),
				Bool(settings::display::useMetricUnits),
				Bool(settings::display::showUndiscoveredLocationMarkers),
				Bool(settings::display::undiscoveredMeansUnknownMarkers),
				Bool(settings::display::undiscoveredMeansUnknownInfo),
				Bool(settings::display::showEnemyMarkers),
				Bool(settings::display::showEnemyNameUnderMarker),
				Bool(settings::display::showObjectiveAsTarget),
				Bool(settings::display::showOtherObjectivesCount),
				Bool(settings::display::showInteriorMarkers),
				settings::display::angleToShowMarkerDetails,
				settings::display::angleToKeepMarkerDetailsShown,
				settings::display::focusingDelayToShow,
				Bool(settings::questlist::showInExteriors),
				Bool(settings::questlist::showInInteriors),
				Bool(settings::questlist::hideInCombat),
				settings::questlist::positionX,
				settings::questlist::positionY,
				settings::questlist::maxHeight,
				settings::questlist::walkingDelayToShow,
				settings::questlist::joggingDelayToShow,
				settings::questlist::sprintingDelayToShow);
		}

		// Caller holds mtx.
		std::string BuildHooksJson()
		{
			std::string groups;

			for (std::size_t i = 0; i < hookGroupCount; ++i)
			{
				if (i != 0)
				{
					groups += ',';
				}

				groups += std::format("\"{}\":{}", hookGroupNames[i], Bool(state.hookGroupInstalled[i]));
			}

			return std::format(
				"\"hooks\":{{"
				"\"installStarted\":{},"
				"\"installCompleted\":{},"
				"\"trampolineAllocated\":{},"
				"\"trampolineValid\":{},"
				"\"trampolineBytes\":{},"
				"\"groups\":{{{}}},"
				"{}"
				"}}",
				Bool(state.hookInstallStarted),
				Bool(state.hookInstallCompleted),
				Bool(state.trampolineAllocated),
				Bool(state.trampolineValid),
				state.trampolineBytes,
				groups,
				SecondsAgoField("lastInstall", state.lastHookInstall));
		}

		// Caller holds mtx.
		std::string BuildComapJson()
		{
			return std::format(
				"\"comapPatch\":{{"
				"\"state\":\"{}\","
				"\"reason\":\"{}\","
				"\"detectionRun\":{},"
				"\"detected\":{},"
				"\"detectedVersion\":\"{}\","
				"\"detectedVersionRaw\":{},"
				"{}"
				"}}",
				ComapStateName(state.comapState),
				EscapeJson(state.comapReason),
				Bool(state.comapDetectionRun),
				Bool(state.comapDetected),
				DecodeVersion(state.comapVersion),
				state.comapVersion,
				SecondsAgoField("lastDecision", state.lastComapDecision));
		}

		// Caller holds mtx.
		std::string BuildInfinityUiJson()
		{
			return std::format(
				"\"infinityUi\":{{"
				"\"listenerRegistrationAttempted\":{},"
				"\"listenerRegistered\":{},"
				"\"hudPatchesStarted\":{},"
				"\"hudPatchesFinished\":{},"
				"\"compassInstanceReady\":{},"
				"\"questItemListReady\":{},"
				"{}"
				"}}",
				Bool(state.infinityUiListenerAttempted),
				Bool(state.infinityUiListenerRegistered),
				state.hudPatchesStarted,
				state.hudPatchesFinished,
				Bool(state.compassInstanceReady),
				Bool(state.questItemListReady),
				SecondsAgoField("lastHudPatch", state.lastHudPatch));
		}

		// Caller holds mtx.
		std::string BuildMarkersJson()
		{
			return std::format(
				"\"markers\":{{"
				"\"quest\":{{\"processed\":{},{}}},"
				"\"location\":{{\"processed\":{},{}}},"
				"\"enemy\":{{\"processed\":{},{}}},"
				"\"playerSet\":{{\"processed\":{},{}}}"
				"}}",
				state.questMarkersProcessed,
				SecondsAgoField("last", state.lastQuestMarker),
				state.locationMarkersProcessed,
				SecondsAgoField("last", state.lastLocationMarker),
				state.enemyMarkersProcessed,
				SecondsAgoField("last", state.lastEnemyMarker),
				state.playerSetMarkersProcessed,
				SecondsAgoField("last", state.lastPlayerSetMarker));
		}

		// Caller holds mtx.
		std::string BuildCompassUpdateJson()
		{
			return std::format(
				"\"compassUpdates\":{{"
				"\"count\":{},"
				"\"lastFacedMarkers\":{},"
				"\"lastQuestMarkerRefs\":{},"
				"\"lastMiscQuestMarkerRefs\":{},"
				"\"lastHadFocusedMarker\":{},"
				"{},"
				"\"skippedCount\":{},"
				"\"lastSkipReason\":\"{}\","
				"{},"
				"\"focusChanges\":{},"
				"\"lastFocusedDescription\":\"{}\","
				"{}"
				"}}",
				state.compassUpdates,
				state.lastFacedMarkers,
				state.lastQuestMarkerRefs,
				state.lastMiscQuestMarkerRefs,
				Bool(state.lastHadFocusedMarker),
				SecondsAgoField("last", state.lastCompassUpdate),
				state.compassUpdatesSkipped,
				EscapeJson(state.lastCompassUpdateSkipReason),
				SecondsAgoField("lastSkip", state.lastCompassUpdateSkipped),
				state.focusChanges,
				EscapeJson(state.lastFocusedDescription),
				SecondsAgoField("lastFocusChange", state.lastFocusChange));
		}

		void StatusTool(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			std::string json;

			{
				std::scoped_lock lock(mtx);

				json = "{";
				json += BuildSettingsJson();
				json += ',';
				json += BuildHooksJson();
				json += ',';
				json += BuildComapJson();
				json += ',';
				json += BuildInfinityUiJson();
				json += ',';
				json += BuildMarkersJson();
				json += ',';
				json += BuildCompassUpdateJson();
				json += '}';
			}

			a_write(a_sink, json.c_str());
		}
	}

	void Init(bool a_lastAttempt)
	{
		static bool registered = false;

		if (registered)
		{
			return;
		}

		DevBenchAPI::IDevBenchInterface001* devBench = DevBenchAPI::GetDevBenchInterface001();

		if (!devBench)
		{
			if (a_lastAttempt)
			{
				logger::info("DevBench not detected; skipping the \"compassnavigationoverhaul.status\" "
							 "live-diagnostics tool (logging alone still covers this session - see "
							 "CLAUDE.md rule 31)");
			}
			else
			{
				// Not terminal - devbench's own server can still be finishing startup this soon
				// after kPostLoad (confirmed from a real launch's timestamps: devbench finished
				// ~100ms after kPostLoad fired, which was enough to lose this race). Retried
				// again at the next message point per rule 17.
				logger::debug("DevBench not detected yet; will retry at the next message");
			}

			return;
		}

		constexpr const char* descriptor =
			"{"
			"\"description\":\"Live Compass Navigation Overhaul state: current settings, which "
			"hook groups installed, whether the CoMAP compatibility patch applied or was skipped "
			"and why, the Infinity UI HUD patch lifecycle, and per-frame marker/compass-update "
			"counters.\","
			"\"inputSchema\":{\"type\":\"object\",\"properties\":{}},"
			"\"readOnly\":true"
			"}";

		if (devBench->RegisterTool("compassnavigationoverhaul.status", descriptor, &StatusTool, nullptr))
		{
			logger::info("Registered \"compassnavigationoverhaul.status\" with DevBench (build {})",
				devBench->GetBuildNumber());
		}
		else
		{
			logger::warn("DevBench reported \"compassnavigationoverhaul.status\" replaced an existing "
						 "tool of the same name");
		}

		registered = true;
	}

	void RecordHookInstallStarted()
	{
		std::scoped_lock lock(mtx);

		state.hookInstallStarted = true;
		state.lastHookInstall = clock::now();
	}

	void RecordTrampolineAllocated(std::size_t a_bytes, bool a_valid)
	{
		std::scoped_lock lock(mtx);

		state.trampolineAllocated = true;
		state.trampolineValid = a_valid;
		state.trampolineBytes = a_bytes;
		state.lastHookInstall = clock::now();
	}

	void RecordHookGroupInstalled(HookGroup a_group)
	{
		const auto index = static_cast<std::size_t>(a_group);

		if (index >= hookGroupCount)
		{
			return;
		}

		std::scoped_lock lock(mtx);

		state.hookGroupInstalled[index] = true;
		state.lastHookInstall = clock::now();
	}

	void RecordHookInstallCompleted()
	{
		std::scoped_lock lock(mtx);

		state.hookInstallCompleted = true;
		state.lastHookInstall = clock::now();
	}

	void RecordComapDetection(bool a_present, std::uint32_t a_version)
	{
		std::scoped_lock lock(mtx);

		state.comapDetectionRun = true;
		state.comapDetected = a_present;
		state.comapVersion = a_version;
	}

	void RecordComapPatchNotAttempted(std::string_view a_reason)
	{
		std::scoped_lock lock(mtx);

		state.comapState = ComapState::kNotAttempted;
		state.comapReason = a_reason;
		state.lastComapDecision = clock::now();
	}

	void RecordComapPatchApplied()
	{
		std::scoped_lock lock(mtx);

		state.comapState = ComapState::kApplied;
		state.comapReason = "the CoMAP compass movie-def hook was written successfully";
		state.lastComapDecision = clock::now();
	}

	void RecordComapPatchSkipped(std::string_view a_reason)
	{
		std::scoped_lock lock(mtx);

		state.comapState = ComapState::kSkipped;
		state.comapReason = a_reason;
		state.lastComapDecision = clock::now();
	}

	void RecordInfinityUiListener(bool a_registered)
	{
		std::scoped_lock lock(mtx);

		state.infinityUiListenerAttempted = true;
		state.infinityUiListenerRegistered = a_registered;
	}

	void RecordHudPatchStarted()
	{
		std::scoped_lock lock(mtx);

		++state.hudPatchesStarted;
		state.lastHudPatch = clock::now();
	}

	void RecordHudPatchFinished()
	{
		std::scoped_lock lock(mtx);

		++state.hudPatchesFinished;
		state.lastHudPatch = clock::now();
	}

	void RecordCompassInstanceReady(bool a_ready)
	{
		std::scoped_lock lock(mtx);

		state.compassInstanceReady = a_ready;
		state.lastHudPatch = clock::now();
	}

	void RecordQuestItemListReady(bool a_ready)
	{
		std::scoped_lock lock(mtx);

		state.questItemListReady = a_ready;
		state.lastHudPatch = clock::now();
	}

	void RecordQuestMarkerProcessed()
	{
		std::scoped_lock lock(mtx);

		++state.questMarkersProcessed;
		state.lastQuestMarker = clock::now();
	}

	void RecordLocationMarkerProcessed()
	{
		std::scoped_lock lock(mtx);

		++state.locationMarkersProcessed;
		state.lastLocationMarker = clock::now();
	}

	void RecordEnemyMarkerProcessed()
	{
		std::scoped_lock lock(mtx);

		++state.enemyMarkersProcessed;
		state.lastEnemyMarker = clock::now();
	}

	void RecordPlayerSetMarkerProcessed()
	{
		std::scoped_lock lock(mtx);

		++state.playerSetMarkersProcessed;
		state.lastPlayerSetMarker = clock::now();
	}

	void RecordCompassUpdate(std::size_t a_facedMarkers, std::size_t a_questMarkerRefs,
							 std::size_t a_miscQuestMarkerRefs, bool a_hasFocusedMarker)
	{
		std::scoped_lock lock(mtx);

		++state.compassUpdates;
		state.lastFacedMarkers = a_facedMarkers;
		state.lastQuestMarkerRefs = a_questMarkerRefs;
		state.lastMiscQuestMarkerRefs = a_miscQuestMarkerRefs;
		state.lastHadFocusedMarker = a_hasFocusedMarker;
		state.lastCompassUpdate = clock::now();
	}

	void RecordCompassUpdateSkipped(std::string_view a_reason)
	{
		std::scoped_lock lock(mtx);

		++state.compassUpdatesSkipped;
		state.lastCompassUpdateSkipReason = a_reason;
		state.lastCompassUpdateSkipped = clock::now();
	}

	void RecordFocusChanged(std::string_view a_focusedDescription)
	{
		std::scoped_lock lock(mtx);

		++state.focusChanges;
		state.lastFocusedDescription = a_focusedDescription;
		state.lastFocusChange = clock::now();
	}
}
