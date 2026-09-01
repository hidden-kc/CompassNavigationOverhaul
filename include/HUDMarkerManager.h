#pragma once

#include "RE/H/HUDMarkerManager.h"

#include "Settings.h"

#include "Compass.h"
#include "QuestItemList.h"

namespace CNO
{
	// The marker array holds 48 position slots (RE::HUDMarkerManager::position). The game's own
	// quest limit is 16 and Quest Marker Limit Fix raises it to exactly 48, so nothing ever
	// commits past this.
	inline constexpr std::uint32_t kMarkerSlotCount = 48;

	// A quest target whose AddMarker call was declined (returned false) at the UpdateQuests call
	// site. Quest Marker Limit Fix makes exactly that happen for every quest target: it swallows
	// the calls, picks the closest markers itself, then replays the winners straight into the
	// original AddMarker - calls this mod's call-site hook never sees. The target is stashed here
	// with the position the game handed us; SetMarkersExtraInfo() then looks for that position in
	// the marker array and, if someone else committed it, runs the ordinary quest bookkeeping
	// against the slot it actually landed in. Without such a mod a declined marker was genuinely
	// not added, matches nothing, and is dropped.
	struct PendingQuestMarker
	{
		RE::TESObjectREFR* marker;
		RE::NiPoint3 position;
		RE::TESQuest* quest;
		RE::BGSInstancedQuestObjective* questObjective;
		int questAgeIndex;
		std::uint32_t markerIcon;
	};

	class HUDMarkerManager
	{
	public:
		static HUDMarkerManager* GetSingleton()
		{
			static HUDMarkerManager singleton;

			return &singleton;
		}

		void ProcessQuestMarker(RE::TESQuest* a_quest, RE::BGSInstancedQuestObjective* a_questObjective,
								int a_questAgeIndex, RE::TESObjectREFR* a_marker, std::uint32_t a_markerIcon,
								std::uint32_t a_slotIndex);

		void DeferQuestMarker(RE::TESObjectREFR* a_marker, const RE::NiPoint3& a_position,
							  RE::TESQuest* a_quest, RE::BGSInstancedQuestObjective* a_questObjective,
							  int a_questAgeIndex, std::uint32_t a_markerIcon);

		void ProcessLocationMarker(RE::ExtraMapMarker* a_mapMarker, RE::TESObjectREFR* a_marker,
								   std::uint32_t a_markerIcon);

		void ProcessEnemyMarker(RE::Character* a_enemy, std::uint32_t a_markerIcon);

		void ProcessPlayerSetMarker(RE::TESObjectREFR* a_marker, std::uint32_t a_markerIcon);

		void SetMarkersExtraInfo();

	private:

		void ReconcilePendingQuestMarkers();

		void ClaimSlot(std::uint32_t a_slot)
		{
			if (a_slot < kMarkerSlotCount)
			{
				claimedSlots[a_slot] = true;
			}
		}

		bool IsTheFocusedMarker(const RE::TESObjectREFR* a_marker) const
		{
			return focusedMarker && a_marker == focusedMarker->ref;
		}

		std::unique_ptr<Compass::Marker> GetMostCenteredMarker() const;

		bool UpdateFocusedMarker();

		float GetAngleBetween(const RE::PlayerCamera* a_playerCamera, const RE::TESObjectREFR* a_marker) const;

		bool IsPlayerAllyOfFaction(const RE::TESFaction* a_faction) const;

		bool IsPlayerOpponentOfFaction(const RE::TESFaction* a_faction) const;

		std::string GetSideInQuest(RE::QUEST_DATA::Type a_questType) const;

		// TESForm::LookupByID returns null for a form ID that does not resolve (missing
		// master, corrupted install), and TESForm::As<T>() reads the form's type off `this`
		// without a null check of its own - calling it on a null lookup result crashes the
		// same way the sibling mod's unchecked GetSetting() did. A missing faction just means
		// the player is treated as not belonging to/opposing it, which GetSideInQuest already
		// falls back to correctly when IsPlayerAllyOfFaction/IsPlayerOpponentOfFaction are
		// given a null faction.
		static const RE::TESFaction* LookupFaction(std::uint32_t a_formID, const char* a_name)
		{
			if (RE::TESForm* form = RE::TESForm::LookupByID(a_formID))
			{
				if (const RE::TESFaction* faction = form->As<RE::TESFaction>())
				{
					return faction;
				}

				logger::error("Form {:08X} ({}) is not a faction", a_formID, a_name);

				return nullptr;
			}

			logger::error("Could not find faction {} (form {:08X}); side-in-quest checks against it will be skipped", a_name, a_formID);

			return nullptr;
		}

		// Not cached as members: Compass/QuestItemList::InitSingleton() run off Infinity UI
		// messages, and this manager's own singleton is constructed lazily off the first
		// compass-update hook call - there is no guarantee the two happen in the order we'd
		// like. Caching a null result here at construction would mean every frame for the
		// rest of the session dereferences it. SetMarkersExtraInfo() fetches and null-checks
		// these fresh each call instead.

		float facingAngle = settings::display::angleToShowMarkerDetails;
		float keepFocusedAngle = settings::display::angleToKeepMarkerDetailsShown;

		float timePreFocusingMarker = 0.0F;
		float timeFocusingMarker = 0.0F;

		std::vector<Compass::Marker> facedMarkers;
		std::unique_ptr<Compass::Marker> preFocusedMarker;
		std::unique_ptr<Compass::Marker> focusedMarker;

		std::unordered_map<RE::TESObjectREFR*, std::unordered_map<RE::TESQuest*, QuestItem>> questItems;
		std::unordered_map<RE::TESObjectREFR*, QuestItem> miscQuestItem;

		// Per-frame state for the deferred quest-marker path (see PendingQuestMarker). The claim
		// marks are written by the immediate Process*Marker paths, so reconciliation only ever
		// matches slots some other mod committed; both are consumed and reset together at the top
		// of the next SetMarkersExtraInfo().
		std::vector<PendingQuestMarker> pendingQuestMarkers;
		bool claimedSlots[kMarkerSlotCount]{};

		RE::HUDMarkerManager* const hudMarkerManager = RE::HUDMarkerManager::GetSingleton();
		RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
		RE::PlayerCamera* playerCamera = RE::PlayerCamera::GetSingleton();
		RE::BSTimer* timeManager = RE::BSTimer::GetTimeManager();

		// Factions to lookup
		// Reference: Creation Kit -> Skyrim.esm, Dawnguard.esm
		const RE::TESFaction* const imperialLegionFaction = LookupFaction(0x0002BF9A, "ImperialLegionFaction");
		const RE::TESFaction* const stormCloaksFaction = LookupFaction(0x00028849, "StormCloaksFaction");
		const RE::TESFaction* const sonsOfSkyrimFaction = LookupFaction(0x0002BF9B, "SonsOfSkyrimFaction");
		const RE::TESFaction* const dawnGuardFaction = LookupFaction(0x02014217, "DawnGuardFaction");
		const RE::TESFaction* const vampireFaction = LookupFaction(0x02003376, "VampireFaction");
	};
}