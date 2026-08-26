#include "UI.h"

#include "SKSEMenuFramework.h"

#include "Compass.h"
#include "Settings.h"

#include "utils/Logger.h"
#include "utils/Toggle.h"

#include <algorithm>

namespace UI
{
	namespace
	{
		std::string statusMessage;

		// The slider the arrow keys currently drive. Set by clicking one.
		std::string selectedSlider;

		// Mirrors !selectedSlider.empty(), but readable from OnInputEvent below, which the
		// framework runs on its own input thread - selectedSlider itself is a plain
		// std::string with no such guarantee, so this is the thread-safe version of the same
		// fact. Written by NudgeableSlider on the render thread whenever it selects a slider.
		std::atomic<bool> sliderSelected{ false };

		SKSEMenuFramework::Model::InputEvent* inputHook = nullptr;

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
				// Needed by NudgeableSlider's arrow-key nudge (ported from Dragon's Eye
				// Minimap's UI.cpp - CLAUDE.md rule 24).
				"igIsKeyPressed_Bool",
				"igIsItemClicked",
				"igIsItemActive",
				"igIsItemHovered",
				"igButton",
				"igSameLine",
				"igSpacing",
				"igPushItemWidth",
				"igPopItemWidth",
				// Toggle() - the on/off switch every boolean setting now renders as
				// instead of a tick-box (utils/Toggle.h, CLAUDE.md rule 32).
				"igGetCursorScreenPos",
				"igGetWindowDrawList",
				"igGetFrameHeight",
				"igInvisibleButton",
				"igPushID_Str",
				"igPopID",
				"ImDrawList_AddRectFilled",
				"ImDrawList_AddCircleFilled"
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

		// Runs on the framework's input thread (see the note on sliderSelected above for why
		// that matters here). Swallows a left/right arrow press while a NudgeableSlider is
		// selected, so it does not also reach whatever else on-screen treats an arrow key as
		// gamepad-equivalent menu navigation. This only touches the RE::InputEvent the game
		// itself sees; ImGui reads its own key state through the framework's separate hook, so
		// NudgeableSlider's own nudge still happens exactly as before regardless of what this
		// returns. Ported from Dragon's Eye Minimap's UI.cpp - CLAUDE.md rule 24.
		bool __stdcall OnInputEvent(RE::InputEvent* a_event)
		{
			if (!sliderSelected.load())
			{
				return false;
			}

			auto* buttonEvent = a_event ? a_event->AsButtonEvent() : nullptr;

			if (!buttonEvent || !buttonEvent->IsPressed())
			{
				return false;
			}

			if (buttonEvent->GetDevice() != RE::INPUT_DEVICE::kKeyboard)
			{
				return false;
			}

			const auto code = static_cast<std::int32_t>(buttonEvent->GetIDCode());

			if (code != RE::BSKeyboardDevice::Keys::kLeft && code != RE::BSKeyboardDevice::Keys::kRight)
			{
				return false;
			}

			logger::trace("OnInputEvent: swallowing arrow key {} - a slider is selected", code);

			return true;
		}

		// A slider that the arrow keys can also nudge, once it has been clicked. Dragging is
		// hopeless for the last decimal place, and the framework does not turn on ImGui's own
		// keyboard navigation, so this tracks the selection itself rather than changing a
		// setting shared with every other mod's page. Ported verbatim from Dragon's Eye
		// Minimap's UI.cpp, which already had this working - see CLAUDE.md rule 24.
		bool NudgeableSlider(const char* a_label, float* a_value, float a_min, float a_max,
							 const char* a_format, float a_step)
		{
			bool changed = ImGuiMCP::SliderFloat(a_label, a_value, a_min, a_max, a_format);

			if (ImGuiMCP::IsItemClicked() || ImGuiMCP::IsItemActive())
			{
				selectedSlider = a_label;
				sliderSelected.store(true);
			}

			if (selectedSlider == a_label)
			{
				float nudge = 0.0F;

				if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_LeftArrow))
				{
					nudge -= a_step;
				}
				if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_RightArrow))
				{
					nudge += a_step;
				}

				if (nudge != 0.0F)
				{
					*a_value = std::clamp(*a_value + nudge, a_min, a_max);
					changed = true;
				}

				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled("<-->");
			}

			return changed;
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

			if (ImGuiMCP::Toggle("Use metric units", &useMetricUnits))
			{
				ApplyUseMetricUnits();
			}
			HelpMarker("Shows distances to markers in meters instead of the vanilla feet.");

			ImGuiMCP::Toggle("Show undiscovered location markers", &showUndiscoveredLocationMarkers);
			ImGuiMCP::Toggle("Undiscovered means unknown marker", &undiscoveredMeansUnknownMarkers);
			HelpMarker("Shows a generic marker instead of the location's real icon until you discover it.");
			ImGuiMCP::Toggle("Undiscovered means unknown info", &undiscoveredMeansUnknownInfo);
			HelpMarker("Hides the location's name and distance on the compass until you discover it.");

			ImGuiMCP::Toggle("Show enemy markers", &showEnemyMarkers);
			ImGuiMCP::Toggle("Show enemy name under marker", &showEnemyNameUnderMarker);
			ImGuiMCP::Toggle("Show interior markers", &showInteriorMarkers);

			ImGuiMCP::Toggle("Show objective as target", &showObjectiveAsTarget);
			ImGuiMCP::Toggle("Show other objectives count", &showOtherObjectivesCount);

			NudgeableSlider("Angle to show marker details", &angleToShowMarkerDetails, 0.0F, 90.0F, "%.0f", 1.0F);
			HelpMarker("How close to the center of the compass a marker has to be before its name and distance appear.");
			NudgeableSlider("Angle to keep marker details shown", &angleToKeepMarkerDetailsShown, 0.0F, 90.0F, "%.0f", 1.0F);
			HelpMarker("Once shown, a marker's details stay visible until it drifts past this wider angle - keeps the text from flickering right at the threshold.");
			NudgeableSlider("Focusing delay to show", &focusingDelayToShow, 0.0F, 2.0F, "%.2f", 0.01F);
			HelpMarker("How long a marker has to stay within the angle above before its details appear.");
		}

		void RenderQuestListSection()
		{
			using namespace settings::questlist;

			ImGuiMCP::SeparatorText("Quest list");

			NudgeableSlider("Position X", &positionX, 0.0F, 1.0F, "%.3f", 0.01F);
			NudgeableSlider("Position Y", &positionY, 0.0F, 1.0F, "%.3f", 0.01F);
			NudgeableSlider("Max height", &maxHeight, 0.0F, 1.0F, "%.2f", 0.01F);

			ImGuiMCP::Toggle("Show in exteriors", &showInExteriors);
			ImGuiMCP::Toggle("Show in interiors", &showInInteriors);
			ImGuiMCP::Toggle("Hide in combat", &hideInCombat);
			HelpMarker("Hides the quest list entirely while a weapon or spell is drawn.");

			NudgeableSlider("Walking delay to show", &walkingDelayToShow, 0.0F, 3.0F, "%.2f", 0.01F);
			NudgeableSlider("Jogging delay to show", &joggingDelayToShow, 0.0F, 3.0F, "%.2f", 0.01F);
			NudgeableSlider("Sprinting delay to show", &sprintingDelayToShow, 0.0F, 3.0F, "%.2f", 0.01F);
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

		// Keeps an arrow key nudging a selected slider from also reaching the game's own
		// gamepad-equivalent menu navigation underneath - see OnInputEvent's own comment.
		if (GetMenuFrameworkFunction<void*>("RegisterInpoutEvent"))
		{
			inputHook = SKSEMenuFramework::AddInputEvent(OnInputEvent);
		}
		else
		{
			logger::info("SKSE Menu Framework does not export \"RegisterInpoutEvent\"; an arrow "
						 "key nudging a slider may also move menu navigation underneath");
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
