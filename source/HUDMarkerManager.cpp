#include "HUDMarkerManager.h"

#include "Diagnostics.h"

#include "RE/B/BSTimer.h"

#include "RE/I/IMenu.h"

#include "NND/NPCNameProvider.h"

namespace CNO
{
	void HUDMarkerManager::ProcessQuestMarker(RE::TESQuest* a_quest, RE::BGSInstancedQuestObjective* a_questObjective,
											  int a_questAgeIndex, RE::TESObjectREFR* a_marker, std::uint32_t a_markerIcon,
											  std::uint32_t a_slotIndex)
	{
		// A bare counter increment, deliberately: the game's own marker loop calls this once per
		// quest marker per frame, so anything that allocated here would be on the HUD path.
		diagnostics::RecordQuestMarkerProcessed();

		float angleToPlayerCamera = GetAngleBetween(playerCamera, a_marker);

		if ((IsTheFocusedMarker(a_marker) && angleToPlayerCamera < keepFocusedAngle) ||
			angleToPlayerCamera < facingAngle)
		{
			std::string description;

			if (settings::display::showObjectiveAsTarget)
			{
				description = a_questObjective->GetDisplayTextWithReplacedTags().c_str();
			}
			else
			{
				// A quest marker can reference to a character or a location
				switch (a_marker->GetFormType())
				{
				case RE::FormType::Reference:
					if (auto teleportDoor = a_marker->As<RE::TESObjectREFR>())
					{
						// If it is a teleport door, we can get the door at the other side
						if (auto teleportLinkedDoor = teleportDoor->extraList.GetTeleportLinkedDoor().get())
						{
							// First, try interior cell
							if (RE::TESObjectCELL* cell = teleportLinkedDoor->GetParentCell())
							{
								description = cell->GetName();
							}
							// Exterior cell
							else if (RE::TESWorldSpace* worldSpace = teleportLinkedDoor->GetWorldspace())
							{
								description = worldSpace->GetName();
							}
						}
					}
					break;
				case RE::FormType::ActorCharacter:
					if (auto character = a_marker->As<RE::Character>())
					{
						description = NND::NPCNameProvider::GetSingleton()->GetName(character);
					}
					break;
				}
			}

			facedMarkers.emplace_back(a_marker, angleToPlayerCamera,
									  a_slotIndex,
									  a_markerIcon, description);

			RE::QUEST_DATA::Type questType = a_quest->GetType();

			bool isInSameLocation = a_markerIcon == RE::HUDMarker::FrameOffsets::GetSingleton()->quest;

			QuestItem* questItem;

			if (questType == RE::QUEST_DATA::Type::kMiscellaneous)
			{
				if (!miscQuestItem.contains(a_marker))
				{
					miscQuestItem[a_marker] = QuestItem{ a_marker, questType, "$MISCELLANEOUS", isInSameLocation, a_questAgeIndex };
				}

				questItem = &miscQuestItem[a_marker];
			}
			else
			{
				RE::BSString questFullName = a_quest->GetFullName();
				ReplaceTagsInQuestText(&questFullName, a_quest, a_quest->currentInstanceID);

				questItems[a_marker][a_quest] = QuestItem{ a_marker, questType, questFullName.c_str(), isInSameLocation, a_questAgeIndex };

				questItem = &questItems[a_marker][a_quest];
			}

			if (std::ranges::find(questItem->objectives, a_questObjective) == questItem->objectives.end())
			{
				questItem->objectives.push_back(a_questObjective);
			}
		}
	}

	void HUDMarkerManager::DeferQuestMarker(RE::TESObjectREFR* a_marker, const RE::NiPoint3& a_position,
											RE::TESQuest* a_quest, RE::BGSInstancedQuestObjective* a_questObjective,
											int a_questAgeIndex, std::uint32_t a_markerIcon)
	{
		// Bounded by the array size: no deferred target can ever be committed beyond
		// kMarkerSlotCount slots, so remembering more than that would only be work the
		// reconciliation pass can never match.
		if (pendingQuestMarkers.size() >= kMarkerSlotCount)
		{
			return;
		}

		pendingQuestMarkers.push_back(
			PendingQuestMarker{ a_marker, a_position, a_quest, a_questObjective, a_questAgeIndex, a_markerIcon });
	}

	// Runs at the top of SetMarkersExtraInfo(), i.e. after every AddMarker this frame - including
	// ones made directly against the original function by a marker-limit mod that bypassed this
	// mod's call-site hooks, and after any slot shuffling such a mod did to evict markers. The
	// marker array carries no handles, so a deferred target is matched by the exact position the
	// game handed its AddMarker call: whatever committed it copied that same position into the
	// slot, and eviction moves entries without changing their values, so matching against the
	// final array state is the more correct view even in frames where a slot was evicted. Slots
	// this mod committed itself are claimed and never matched.
	void HUDMarkerManager::ReconcilePendingQuestMarkers()
	{
		const std::uint32_t markerCount = std::min(hudMarkerManager->currentMarkerIndex, kMarkerSlotCount);

		std::size_t matched = 0;
		std::size_t unmatched = 0;

		for (const PendingQuestMarker& pending : pendingQuestMarkers)
		{
			std::uint32_t slot = kMarkerSlotCount;

			for (std::uint32_t i = 0; i < markerCount; i++)
			{
				if (!claimedSlots[i] && hudMarkerManager->position[i] == pending.position)
				{
					slot = i;

					break;
				}
			}

			if (slot == kMarkerSlotCount)
			{
				// Genuinely not displayed this frame - the declining side chose not to commit it,
				// so drop it. A mismatch here is also the failure mode if the exact-position
				// assumption ever stops holding, and it is deliberately cosmetic: the marker's
				// own display is that other mod's business, only this mod's hover details for it
				// go missing until the next frame.
				++unmatched;

				continue;
			}

			ProcessQuestMarker(pending.quest, pending.questObjective, pending.questAgeIndex, pending.marker,
							   pending.markerIcon, slot);

			++matched;
		}

		if (matched != 0 || unmatched != 0)
		{
			diagnostics::RecordQuestMarkersReconciled(matched, unmatched);
		}

		pendingQuestMarkers.clear();

		std::ranges::fill(claimedSlots, false);
	}

	void HUDMarkerManager::ProcessLocationMarker(RE::ExtraMapMarker* a_mapMarker, RE::TESObjectREFR* a_marker,
												 std::uint32_t a_markerIcon)
	{
		diagnostics::RecordLocationMarkerProcessed();

		// Claimed unconditionally, before the angle gating: deferred quest targets may only ever
		// match slots this mod did not commit itself, whatever the facing angle did with them.
		ClaimSlot(hudMarkerManager->currentMarkerIndex - 1);

		float angleToPlayerCamera = GetAngleBetween(playerCamera, a_marker);

		bool isDiscoveredLocation = a_mapMarker->mapData->flags.all(RE::MapMarkerData::Flag::kVisible);

		if (isDiscoveredLocation || !settings::display::undiscoveredMeansUnknownInfo)
		{
			if ((IsTheFocusedMarker(a_marker) && angleToPlayerCamera < keepFocusedAngle) ||
				angleToPlayerCamera < facingAngle)
			{
				std::string_view locationFullName = a_mapMarker->mapData->locationName.GetFullName();

				facedMarkers.emplace_back(a_marker, angleToPlayerCamera,
										  hudMarkerManager->currentMarkerIndex - 1,
										  a_markerIcon, locationFullName);
			}
		}

		if (!isDiscoveredLocation && settings::display::undiscoveredMeansUnknownMarkers)
		{
			hudMarkerManager->scaleformMarkerData[hudMarkerManager->currentMarkerIndex - 1].icon.SetNumber(0);
		}
	}

	void HUDMarkerManager::ProcessEnemyMarker(RE::Character* a_enemy, std::uint32_t a_markerIcon)
	{
		diagnostics::RecordEnemyMarkerProcessed();

		ClaimSlot(hudMarkerManager->currentMarkerIndex - 1);

		float angleToPlayerCamera = GetAngleBetween(playerCamera, a_enemy);

		if ((IsTheFocusedMarker(a_enemy) && angleToPlayerCamera < keepFocusedAngle) ||
			angleToPlayerCamera < facingAngle)
		{
			std::string enemyName = NND::NPCNameProvider::GetSingleton()->GetName(a_enemy);

			facedMarkers.emplace_back(a_enemy, angleToPlayerCamera,
									hudMarkerManager->currentMarkerIndex - 1,
									a_markerIcon, enemyName);
		}
	}

	void HUDMarkerManager::ProcessPlayerSetMarker(RE::TESObjectREFR* a_marker, std::uint32_t a_markerIcon)
	{
		diagnostics::RecordPlayerSetMarkerProcessed();

		ClaimSlot(hudMarkerManager->currentMarkerIndex - 1);

		float angleToPlayerCamera = GetAngleBetween(playerCamera, a_marker);

		if ((IsTheFocusedMarker(a_marker) && angleToPlayerCamera < keepFocusedAngle) ||
			angleToPlayerCamera < facingAngle)
		{
			facedMarkers.emplace_back(a_marker, angleToPlayerCamera,
										hudMarkerManager->currentMarkerIndex - 1,
										a_markerIcon, "");
		}
	}

	void HUDMarkerManager::SetMarkersExtraInfo()
	{
		// Fetched fresh rather than cached as members: Compass/QuestItemList::InitSingleton()
		// run off Infinity UI messages that are not guaranteed to have landed before this
		// manager's own singleton was first constructed off a compass-update hook. A stale
		// null cached at construction would dereference every frame for the rest of the
		// session; bailing out here just skips this frame's compass/quest-list update.
		Compass* compass = Compass::GetSingleton();

		if (!compass)
		{
			logger::error("Compass singleton not ready; skipping this frame's compass update");

			diagnostics::RecordCompassUpdateSkipped("the Compass singleton was not ready (the Infinity UI "
													"HUD patch has not delivered the replaced instance yet)");

			return;
		}

		QuestItemList* questItemList = QuestItemList::GetSingleton();

		if (!questItemList)
		{
			logger::error("QuestItemList singleton not ready; skipping this frame's compass update");

			diagnostics::RecordCompassUpdateSkipped("the QuestItemList singleton was not ready (the Infinity UI "
													"HUD patch has not delivered the replaced instance yet)");

			return;
		}

		// Before the focus logic, so markers a limit mod committed on its own are part of this
		// frame's faced markers and quest bookkeeping like any immediately-processed one.
		ReconcilePendingQuestMarkers();

		bool focusChanged = UpdateFocusedMarker();

		if (focusChanged)
		{
			// Recorded here rather than per frame on purpose: this is the one place a marker
			// description is worth copying, because focus changes are user-paced. UpdateFocusedMarker()
			// has already swapped focusedMarker over, so this is the marker now being focused.
			diagnostics::RecordFocusChanged(focusedMarker ? std::string_view{ focusedMarker->description } :
														    std::string_view{});

			compass->UnfocusMarker();
			timeFocusingMarker = 0.0F;
		}
		else if (focusedMarker)
		{
			timeFocusingMarker += timeManager->realTimeDelta;
		}

		bool isFocusedQuestMarker = false;

		if (focusedMarker)
		{
			std::string focusedMarkerDescription = focusedMarker->description;

			if (settings::display::showObjectiveAsTarget && settings::display::showOtherObjectivesCount)
			{
				int objectivesCount = 0;

				if (questItems.contains(focusedMarker->ref))
				{
					isFocusedQuestMarker = true;

					std::unordered_map<RE::TESQuest*, QuestItem>& questItemMap = questItems[focusedMarker->ref];

					for (auto& [quest, questItem] : questItemMap)
					{
						objectivesCount += questItem.objectives.size();
					}
				}

				if (miscQuestItem.contains(focusedMarker->ref))
				{
					isFocusedQuestMarker = true;

					QuestItem& questItem = miscQuestItem[focusedMarker->ref];

					objectivesCount += questItem.objectives.size();
				}

				if (objectivesCount > 1)
				{
					focusedMarkerDescription += " (+" + std::to_string(objectivesCount - 1) + ")";
				}
			}

			compass->SetFocusedMarkerInfo(focusedMarkerDescription, focusedMarker->distanceToPlayer,
										  focusedMarker->heightDifference, focusedMarker->index);

			if (focusChanged)
			{
				compass->FocusMarker();
			}

			compass->UpdateFocusedMarker();
		}

		RE::ActorState* playerState = player->AsActorState();

		bool canQuestItemListBeDisplayed = questItemList->CanBeDisplayed(player->GetParentCell(), playerState->IsWeaponDrawn());

		if (!canQuestItemListBeDisplayed || focusChanged)
		{
			questItemList->RemoveAllQuests();
		}

		if (canQuestItemListBeDisplayed && isFocusedQuestMarker)
		{
			if (focusChanged)
			{
				if (questItems.contains(focusedMarker->ref))
				{
					std::unordered_map<RE::TESQuest*, QuestItem>& questItemMap = questItems[focusedMarker->ref];

					for (auto& [quest, questItem] : questItemMap)
					{
						questItemList->AddQuest(questItem);
						questItemList->SetQuestSide(GetSideInQuest(questItem.type));

						// If we call a function more than once per frame (like in this for-loop)
						// we need to update the stage with `GFxMovieView::Advance`, otherwise graphical
						// glitches occur to the element when showing up
						questItemList->GetMovieView()->Advance(0.0F);
					}
				}
				
				if (miscQuestItem.contains(focusedMarker->ref))
				{
					QuestItem& questItem = miscQuestItem[focusedMarker->ref];

					questItemList->AddQuest(questItem);

					// If we call a function more than once per frame (like in this for-loop)
					// we need to update the stage with `GFxMovieView::Advance`, otherwise graphical
					// glitches occur to the element when showing up
					questItemList->GetMovieView()->Advance(0.0F);
				}
			}

			questItemList->SetHiddenByForce(false);

			float playerSpeed = playerState->DoGetMovementSpeed();

			float delayToShow = (playerSpeed < player->GetWalkSpeed()) ? settings::questlist::walkingDelayToShow :
								(playerSpeed < player->GetJogSpeed())  ? settings::questlist::joggingDelayToShow :
																		 settings::questlist::sprintingDelayToShow;

			if (timeFocusingMarker > delayToShow)
			{
				questItemList->ShowAllQuests();
				questItemList->Update();
			}
		}

		// Snapshot before the clears below, so these are the counts this frame actually worked
		// with. All three are sizes the update already maintains, so this costs a lock and three
		// loads - nothing is walked or copied.
		diagnostics::RecordCompassUpdate(facedMarkers.size(), questItems.size(), miscQuestItem.size(),
			focusedMarker != nullptr);

		facedMarkers.clear();
		questItems.clear();
		miscQuestItem.clear();
	}

	std::unique_ptr<Compass::Marker> HUDMarkerManager::GetMostCenteredMarker() const
	{
		std::unique_ptr<Compass::Marker> mostCenteredMarker = nullptr;

		float closestAngleToPlayerCamera = std::numeric_limits<float>::max();

		int mostCenteredMarkerIndex = -1;

		for (int i = 0; i < facedMarkers.size(); i++)
		{
			const Compass::Marker& facedMarker = facedMarkers[i];

			if (facedMarker.angleToPlayerCamera < closestAngleToPlayerCamera)
			{
				mostCenteredMarkerIndex = i;
				closestAngleToPlayerCamera = facedMarker.angleToPlayerCamera;
			}
		}

		if (mostCenteredMarkerIndex >= 0)
		{
			mostCenteredMarker = std::make_unique<Compass::Marker>(facedMarkers[mostCenteredMarkerIndex]);
		}

		return mostCenteredMarker;
	}

	bool HUDMarkerManager::UpdateFocusedMarker()
	{
		std::unique_ptr<Compass::Marker> mostCenteredMarker = GetMostCenteredMarker();

		static auto IsMarkerDifferent = [](const std::unique_ptr<Compass::Marker>& a_lhs, const std::unique_ptr<Compass::Marker>& a_rhs) -> bool
		{
			if (a_lhs && a_rhs)
			{
				return a_lhs->ref != a_rhs->ref;
			}
			else if (!a_lhs && !a_rhs)
			{
				return false;
			}

			return true;
		};

		if (IsMarkerDifferent(mostCenteredMarker, preFocusedMarker))
		{
			timePreFocusingMarker = 0.0F;
		}

		if (preFocusedMarker || mostCenteredMarker)
		{
			preFocusedMarker = std::move(mostCenteredMarker);
		}

		if (IsMarkerDifferent(preFocusedMarker, focusedMarker))
		{
			if (preFocusedMarker)
			{
				if (timePreFocusingMarker > settings::display::focusingDelayToShow)
				{	
					focusedMarker = std::move(preFocusedMarker);
					return true;
				}
				else
				{
					timePreFocusingMarker += timeManager->realTimeDelta;
				}
			}
			else
			{
				focusedMarker = nullptr;
				return true;
			}
		}
		else if (preFocusedMarker && focusedMarker)
		{
			focusedMarker = std::move(preFocusedMarker);
		}

		return false;
	}

	float HUDMarkerManager::GetAngleBetween(const RE::PlayerCamera* a_playerCamera,
											const RE::TESObjectREFR* a_marker) const
	{
		float angleToPlayerCameraInRadians = util::GetAngleBetween(a_playerCamera, a_marker);
		float angleToPlayerCamera = util::RadiansToDegrees(angleToPlayerCameraInRadians);

		if (angleToPlayerCamera > 180.0F)
			angleToPlayerCamera = 360.0F - angleToPlayerCamera;

		return angleToPlayerCamera;
	}

	bool HUDMarkerManager::IsPlayerAllyOfFaction(const RE::TESFaction* a_faction) const
	{
		if (player->IsInFaction(a_faction)) 
		{
			return true;
		}

		return player->VisitFactions([a_faction](RE::TESFaction* a_visitedFaction, std::int8_t a_rank) -> bool
		{
			if (a_visitedFaction == a_faction && a_rank > 0)
			{
				return true;
			}

			for (RE::GROUP_REACTION* reactionToFaction : a_visitedFaction->reactions)
			{
				auto relatedFaction = reactionToFaction->form->As<RE::TESFaction>();
				if (relatedFaction == a_faction && reactionToFaction->fightReaction >= RE::FIGHT_REACTION::kAlly)
				{
					return true;
				}
			}

			return false;
		});
	}

	bool HUDMarkerManager::IsPlayerOpponentOfFaction(const RE::TESFaction* a_faction) const
	{
		return player->VisitFactions([a_faction](RE::TESFaction* a_visitedFaction, std::int8_t a_rank) -> bool
		{
			if (a_visitedFaction == a_faction && a_rank < 0)
			{
				return true;
			}

			for (RE::GROUP_REACTION* reactionToFaction : a_visitedFaction->reactions)
			{
				auto relatedFaction = reactionToFaction->form->As<RE::TESFaction>();
				if (relatedFaction == a_faction && reactionToFaction->fightReaction == RE::FIGHT_REACTION::kEnemy)
				{
					return true;
				}
			}

			return false;
		});
	}

	std::string HUDMarkerManager::GetSideInQuest(RE::QUEST_DATA::Type a_questType) const
	{
		switch (a_questType)
		{
		case RE::QUEST_DATA::Type::kCivilWar:
			if (IsPlayerAllyOfFaction(sonsOfSkyrimFaction) || IsPlayerAllyOfFaction(stormCloaksFaction) ||
				IsPlayerOpponentOfFaction(imperialLegionFaction))
			{
				return "StormCloaks";
			}
			else
			{
				return "ImperialLegion";
			}
		case RE::QUEST_DATA::Type::kDLC01_Vampire:
			if (player->HasKeywordString("Vampire") || IsPlayerAllyOfFaction(vampireFaction) ||
				IsPlayerOpponentOfFaction(dawnGuardFaction))
			{
				return "Vampires";
			}
			else
			{
				return "Dawnguard"; 
			}
		}

		return { };
	}
}
