#include "UI.h"

#include "SKSEMenuFramework.h"

#include "Compass.h"
#include "Settings.h"

#include "utils/Logger.h"

namespace UI
{
	namespace
	{
		std::string statusMessage;

		constexpr const char* kLogLevelNames[] = { "Trace", "Debug", "Info", "Warning", "Error", "Critical", "Off" };
		constexpr int kLogLevelCount = 7;

		// The framework renders from the renderer's present hook, which is not the thread
		// Scaleform and the rest of the game expect to be talked to. Anything that reaches into
		// the compass or the rest of the HUD has to be handed to the main thread first.
		void OnMainThread(std::function<void()> a_task)
		{
			if (auto* taskInterface = SKSE::GetTaskInterface())
			{
				taskInterface->AddTask(std::move(a_task));
			}
		}

		// The bundled header reaches ImGui through the framework's exported cimgui entry points.
		// Older builds of SKSE Menu Framework do not export them, and every widget call in
		// Render() would then call through a null function pointer, so refuse to register
		// unless the ones this panel needs are all there. Varargs widgets resolve to a
		// "...V"-suffixed export (TextDisabled resolves igTextDisabledV, not igTextDisabled) -
		// probing the wrong name lets Register() succeed and then crashes on the first draw.
		bool HasRequiredExports()
		{
			constexpr const char* required[] = {
				"AddSectionItem",
				"igTextV",
				"igTextDisabledV",
				"igTextWrappedV",
				"igSetTooltipV",
				"igSeparatorText",
				"igCheckbox",
				"igCombo_Str_arr",
				"igSliderFloat",
				"igIsItemHovered",
				"igButton",
				"igSameLine",
				"igSpacing",
				"igPushItemWidth",
				"igPopItemWidth"
			};

			for (const char* name : required)
			{
				if (!GetMenuFrameworkFunction<void*>(name))
				{
					logger::warn("SKSE Menu Framework does not export \"{}\"", name);

					return false;
				}
			}

			return true;
		}

		void HelpMarker(const char* a_description)
		{
			ImGuiMCP::SameLine();
			ImGuiMCP::TextDisabled("(?)");

			if (ImGuiMCP::IsItemHovered())
			{
				ImGuiMCP::SetTooltip("%s", a_description);
			}
		}

		void ApplyUseMetricUnits()
		{
			OnMainThread([]() {
				if (auto* compass = CNO::Compass::GetSingleton())
				{
					compass->SetUnits(settings::display::useMetricUnits);
				}
			});
		}

		void RenderDisplaySection()
		{
			using namespace settings::display;

			ImGuiMCP::SeparatorText("Compass and markers");

			if (ImGuiMCP::Checkbox("Use metric units", &useMetricUnits))
			{
				ApplyUseMetricUnits();
			}
			HelpMarker("Shows distances to markers in meters instead of the vanilla feet.");

			ImGuiMCP::Checkbox("Show undiscovered location markers", &showUndiscoveredLocationMarkers);
			ImGuiMCP::Checkbox("Undiscovered means unknown marker", &undiscoveredMeansUnknownMarkers);
			HelpMarker("Shows a generic marker instead of the location's real icon until you discover it.");
			ImGuiMCP::Checkbox("Undiscovered means unknown info", &undiscoveredMeansUnknownInfo);
			HelpMarker("Hides the location's name and distance on the compass until you discover it.");

			ImGuiMCP::Checkbox("Show enemy markers", &showEnemyMarkers);
			ImGuiMCP::Checkbox("Show enemy name under marker", &showEnemyNameUnderMarker);
			ImGuiMCP::Checkbox("Show interior markers", &showInteriorMarkers);

			ImGuiMCP::Checkbox("Show objective as target", &showObjectiveAsTarget);
			ImGuiMCP::Checkbox("Show other objectives count", &showOtherObjectivesCount);

			ImGuiMCP::SliderFloat("Angle to show marker details", &angleToShowMarkerDetails, 0.0F, 90.0F, "%.0f");
			HelpMarker("How close to the center of the compass a marker has to be before its name and distance appear.");
			ImGuiMCP::SliderFloat("Angle to keep marker details shown", &angleToKeepMarkerDetailsShown, 0.0F, 90.0F, "%.0f");
			HelpMarker("Once shown, a marker's details stay visible until it drifts past this wider angle - keeps the text from flickering right at the threshold.");
			ImGuiMCP::SliderFloat("Focusing delay to show", &focusingDelayToShow, 0.0F, 2.0F, "%.2f");
			HelpMarker("How long a marker has to stay within the angle above before its details appear.");
		}

		void RenderQuestListSection()
		{
			using namespace settings::questlist;

			ImGuiMCP::SeparatorText("Quest list");

			ImGuiMCP::SliderFloat("Position X", &positionX, 0.0F, 1.0F, "%.3f");
			ImGuiMCP::SliderFloat("Position Y", &positionY, 0.0F, 1.0F, "%.3f");
			ImGuiMCP::SliderFloat("Max height", &maxHeight, 0.0F, 1.0F, "%.2f");

			ImGuiMCP::Checkbox("Show in exteriors", &showInExteriors);
			ImGuiMCP::Checkbox("Show in interiors", &showInInteriors);
			ImGuiMCP::Checkbox("Hide in combat", &hideInCombat);
			HelpMarker("Hides the quest list entirely while a weapon or spell is drawn.");

			ImGuiMCP::SliderFloat("Walking delay to show", &walkingDelayToShow, 0.0F, 3.0F, "%.2f");
			ImGuiMCP::SliderFloat("Jogging delay to show", &joggingDelayToShow, 0.0F, 3.0F, "%.2f");
			ImGuiMCP::SliderFloat("Sprinting delay to show", &sprintingDelayToShow, 0.0F, 3.0F, "%.2f");
			HelpMarker("How long you have to move at each pace before the quest list fades in.");
		}

		void RenderDebugSection()
		{
			using namespace settings;

			ImGuiMCP::SeparatorText("Debug");

			int level = static_cast<int>(debug::logLevel);
			if (ImGuiMCP::Combo("Log level", &level, kLogLevelNames, kLogLevelCount))
			{
				debug::logLevel = static_cast<logger::level>(level);

				OnMainThread([]() { logger::set_level(settings::debug::logLevel, settings::debug::logLevel); });
			}
			HelpMarker("Applies to the log immediately.");
		}

		void RenderButtons()
		{
			if (ImGuiMCP::Button("Save"))
			{
				OnMainThread([]() {
					statusMessage = settings::Save() ? "Settings saved." : "Could not save the INI. See the log for why.";
				});
			}
			HelpMarker("Writes every setting above back to the INI. Comments and unrelated keys are left alone.");

			ImGuiMCP::SameLine();

			if (ImGuiMCP::Button("Reload from INI"))
			{
				OnMainThread([]() {
					if (settings::Reload())
					{
						ApplyLiveSettings();

						statusMessage = "Settings reloaded from the INI.";
					}
					else
					{
						statusMessage = "Could not read the INI. See the log for why.";
					}
				});
			}
			HelpMarker("Throws away any change made here since the last save and re-reads the INI from disk. Also picks up edits made to the file by hand.");

			ImGuiMCP::SameLine();

			if (ImGuiMCP::Button("Restore defaults"))
			{
				OnMainThread([]() {
					settings::RestoreDefaults();
					ApplyLiveSettings();
				});

				statusMessage = "Defaults restored. Press Save to keep them.";
			}
			HelpMarker("Puts every setting back to the value it has on a fresh install. Nothing is written until you press Save.");

			if (!statusMessage.empty())
			{
				ImGuiMCP::TextWrapped("%s", statusMessage.c_str());
			}

			ImGuiMCP::Spacing();
			ImGuiMCP::TextDisabled("%s", settings::GetIniPath().c_str());
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled())
		{
			logger::info("SKSE Menu Framework is not installed; settings will be read from the INI only");

			return;
		}

		if (!HasRequiredExports())
		{
			logger::warn("The installed SKSE Menu Framework is older than this plugin's settings "
						 "menu needs. Update it to version 3 or newer to configure the compass and "
						 "quest list in game.");

			return;
		}

		SKSEMenuFramework::SetSection("Compass Navigation Overhaul");
		SKSEMenuFramework::AddSectionItem("Settings", SettingsPanel::Render);

		logger::info("Registered the settings page with SKSE Menu Framework");
	}

	void ApplyLiveSettings()
	{
		logger::set_level(settings::debug::logLevel, settings::debug::logLevel);

		ApplyUseMetricUnits();
	}

	void __stdcall SettingsPanel::Render()
	{
		ImGuiMCP::TextWrapped("Most settings apply as soon as you make them. Press Save to keep them for the next time you play.");
		ImGuiMCP::Spacing();

		ImGuiMCP::PushItemWidth(260.0F);

		RenderDisplaySection();
		ImGuiMCP::Spacing();

		RenderQuestListSection();
		ImGuiMCP::Spacing();

		RenderDebugSection();
		ImGuiMCP::Spacing();

		ImGuiMCP::PopItemWidth();

		RenderButtons();
	}
}
