#pragma once

namespace UI
{
	// Adds this mod's page to the SKSE Menu Framework's Mod Control Panel. Safe to call when
	// the framework is missing or too old to drive: it logs why and does nothing else.
	void Register();

	// Pushes the current value of settings::display::useMetricUnits into the running compass,
	// and the log level into spdlog. Everything else under settings::display and
	// settings::questlist is read live wherever it's used, so it needs nothing doing here.
	void ApplyLiveSettings();

	namespace SettingsPanel
	{
		void __stdcall Render();
	}
}
