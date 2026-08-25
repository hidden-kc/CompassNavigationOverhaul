#include "Settings.h"

#include "utils/INISettingCollection.h"
#include "utils/Logger.h"

#include <windows.h>

namespace settings
{
	using namespace utils;

	namespace
	{
		constexpr const char* kDebugSection = "Debug";
		constexpr const char* kDisplaySection = "Display";
		constexpr const char* kQuestListSection = "QuestList";

		std::string iniPath;
		std::string iniFileName;

		// The values the plugin compiles in, captured before the INI is read so that
		// "Restore defaults" means "what you would get with no INI at all".
		struct Defaults
		{
			logger::level logLevel;

			bool useMetricUnits;
			bool showUndiscoveredLocationMarkers;
			bool undiscoveredMeansUnknownMarkers;
			bool undiscoveredMeansUnknownInfo;
			bool showEnemyMarkers;
			bool showEnemyNameUnderMarker;
			bool showObjectiveAsTarget;
			bool showOtherObjectivesCount;
			bool showInteriorMarkers;
			float angleToShowMarkerDetails;
			float angleToKeepMarkerDetailsShown;
			float focusingDelayToShow;

			float positionX;
			float positionY;
			float maxHeight;
			bool showInExteriors;
			bool showInInteriors;
			float walkingDelayToShow;
			float joggingDelayToShow;
			float sprintingDelayToShow;
			bool hideInCombat;
		};

		Defaults defaults;

		void CaptureDefaults()
		{
			defaults.logLevel = debug::logLevel;

			defaults.useMetricUnits = display::useMetricUnits;
			defaults.showUndiscoveredLocationMarkers = display::showUndiscoveredLocationMarkers;
			defaults.undiscoveredMeansUnknownMarkers = display::undiscoveredMeansUnknownMarkers;
			defaults.undiscoveredMeansUnknownInfo = display::undiscoveredMeansUnknownInfo;
			defaults.showEnemyMarkers = display::showEnemyMarkers;
			defaults.showEnemyNameUnderMarker = display::showEnemyNameUnderMarker;
			defaults.showObjectiveAsTarget = display::showObjectiveAsTarget;
			defaults.showOtherObjectivesCount = display::showOtherObjectivesCount;
			defaults.showInteriorMarkers = display::showInteriorMarkers;
			defaults.angleToShowMarkerDetails = display::angleToShowMarkerDetails;
			defaults.angleToKeepMarkerDetailsShown = display::angleToKeepMarkerDetailsShown;
			defaults.focusingDelayToShow = display::focusingDelayToShow;

			defaults.positionX = questlist::positionX;
			defaults.positionY = questlist::positionY;
			defaults.maxHeight = questlist::maxHeight;
			defaults.showInExteriors = questlist::showInExteriors;
			defaults.showInInteriors = questlist::showInInteriors;
			defaults.walkingDelayToShow = questlist::walkingDelayToShow;
			defaults.joggingDelayToShow = questlist::joggingDelayToShow;
			defaults.sprintingDelayToShow = questlist::sprintingDelayToShow;
			defaults.hideInCombat = questlist::hideInCombat;
		}

		// One key a Save() is about to write. Queued rather than written on the spot so the
		// whole file is rewritten once at the end instead of once per key.
		struct PendingWrite
		{
			std::string section;
			std::string key;
			std::string value;
		};

		std::vector<PendingWrite> pendingWrites;

		bool EqualsIgnoreCase(std::string_view a_lhs, std::string_view a_rhs)
		{
			return std::ranges::equal(a_lhs, a_rhs, [](char a_l, char a_r) {
				return std::tolower(static_cast<unsigned char>(a_l)) == std::tolower(static_cast<unsigned char>(a_r));
			});
		}

		std::string_view Trim(std::string_view a_text)
		{
			constexpr std::string_view kSpace = " \t\r\n";

			const std::size_t first = a_text.find_first_not_of(kSpace);

			if (first == std::string_view::npos)
			{
				return {};
			}

			return a_text.substr(first, a_text.find_last_not_of(kSpace) - first + 1);
		}

		// Queues a key for the next FlushPendingWrites(). Cannot fail on its own - the file is
		// only touched at flush time, so that is where a write error can surface.
		bool WriteRaw(const char* a_section, const char* a_key, const std::string& a_value)
		{
			pendingWrites.emplace_back(a_section, a_key, a_value);

			return true;
		}

		// Rewrites the INI with every queued change applied in place, leaving comments and any
		// keys this plugin does not know about untouched.
		//
		// Deliberately plain file I/O rather than WritePrivateProfileString. Mod Organizer 2's
		// usvfs does not reliably redirect the Win32 profile APIs: those calls returned success
		// and the plugin logged a successful save, while the file on disk was never written -
		// not in the mod folder, not in Overwrite - so every saved setting was silently lost on
		// the next load. Ordinary file reads and writes go through the VFS correctly.
		bool FlushPendingWrites()
		{
			if (pendingWrites.empty())
			{
				return true;
			}

			std::string text;

			{
				std::ifstream in(iniPath, std::ios::binary);

				if (in)
				{
					text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
				}
				else
				{
					logger::warn("Could not read {} before saving; writing it from scratch", iniPath);
				}
			}

			// Keep whatever line ending the file already uses, so a save does not rewrite every
			// line of a CRLF file as LF (or the other way round) and bury the real change.
			const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";

			std::vector<std::string> lines;

			for (std::size_t start = 0; start <= text.size();)
			{
				const std::size_t end = text.find('\n', start);

				if (end == std::string::npos)
				{
					if (start < text.size())
					{
						lines.emplace_back(text.substr(start));
					}

					break;
				}

				std::string line = text.substr(start, end - start);

				if (!line.empty() && line.back() == '\r')
				{
					line.pop_back();
				}

				lines.push_back(std::move(line));
				start = end + 1;
			}

			std::vector<bool> applied(pendingWrites.size(), false);

			// Pass one: replace any key that is already present under its own section.
			std::string currentSection;

			for (std::string& line : lines)
			{
				const std::string_view trimmed = Trim(line);

				if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']')
				{
					currentSection = std::string{ trimmed.substr(1, trimmed.size() - 2) };

					continue;
				}

				if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#')
				{
					continue;
				}

				const std::size_t separator = line.find('=');

				if (separator == std::string::npos)
				{
					continue;
				}

				const std::string_view key = Trim(std::string_view{ line }.substr(0, separator));

				for (std::size_t i = 0; i < pendingWrites.size(); ++i)
				{
					if (applied[i] || !EqualsIgnoreCase(currentSection, pendingWrites[i].section) ||
						!EqualsIgnoreCase(key, pendingWrites[i].key))
					{
						continue;
					}

					line = std::format("{}={}", key, pendingWrites[i].value);
					applied[i] = true;

					break;
				}
			}

			// Pass two: anything still unapplied is a key (or a whole section) the file does not
			// have yet, so append it at the end of its section, creating the section if needed.
			for (std::size_t i = 0; i < pendingWrites.size(); ++i)
			{
				if (applied[i])
				{
					continue;
				}

				const PendingWrite& pending = pendingWrites[i];

				std::size_t insertAt = lines.size();
				bool sectionFound = false;

				for (std::size_t l = 0; l < lines.size(); ++l)
				{
					const std::string_view trimmed = Trim(lines[l]);

					if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']')
					{
						continue;
					}

					if (sectionFound)
					{
						// The next section header - this key belongs just before it.
						insertAt = l;

						break;
					}

					if (EqualsIgnoreCase(trimmed.substr(1, trimmed.size() - 2), pending.section))
					{
						sectionFound = true;
						insertAt = lines.size();
					}
				}

				if (!sectionFound)
				{
					if (!lines.empty() && !Trim(lines.back()).empty())
					{
						lines.emplace_back();
					}

					lines.push_back(std::format("[{}]", pending.section));
					insertAt = lines.size();
				}

				// Step back over trailing blank lines so the key lands with its own section
				// rather than in the gap before the next one.
				while (insertAt > 0 && Trim(lines[insertAt - 1]).empty())
				{
					--insertAt;
				}

				lines.insert(lines.begin() + insertAt, std::format("{}={}", pending.key, pending.value));
				applied[i] = true;
			}

			std::string output;

			for (const std::string& line : lines)
			{
				output += line;
				output += newline;
			}

			std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);

			if (!out)
			{
				logger::error("Could not open {} for writing; settings were not saved", iniPath);

				return false;
			}

			out.write(output.data(), static_cast<std::streamsize>(output.size()));
			out.close();

			if (!out)
			{
				logger::error("Could not write {}; settings were not saved", iniPath);

				return false;
			}

			logger::debug("FlushPendingWrites: wrote {} key(s) to {}", pendingWrites.size(), iniPath);

			return true;
		}

		bool WriteFloat(const char* a_section, const char* a_key, float a_value)
		{
			return WriteRaw(a_section, a_key, std::format("{:g}", a_value));
		}

		bool WriteUInt(const char* a_section, const char* a_key, std::uint32_t a_value)
		{
			return WriteRaw(a_section, a_key, std::format("{}", a_value));
		}

		bool WriteBool(const char* a_section, const char* a_key, bool a_value)
		{
			return WriteRaw(a_section, a_key, a_value ? "1" : "0");
		}

		// RE::INISettingCollection::GetSetting returns null for a name that is not in the
		// collection, and the templated GetSetting<T> helpers dereference that without
		// checking. AddChecked below deliberately skips a malformed setting, so a skipped one
		// would then be read back as null and crash during SKSEPluginLoad - trading one fatal
		// bug for another. Read through here instead: the value keeps whatever default it
		// already had, and the log says which setting went missing.
		template <typename T>
		T Read(INISettingCollection* a_collection, const char* a_name, T a_fallback)
		{
			if (!a_collection->GetSetting(a_name))
			{
				logger::error("Setting \"{}\" is missing from the collection; keeping the current value", a_name);

				return a_fallback;
			}

			return a_collection->GetSetting<T>(a_name);
		}

		// MakeSetting takes the setting's type from the first letter of its name - i signed,
		// u unsigned, f float, b bool, s string - and quietly hands back a setting with a null
		// name when the value passed does not match. The game's collection dereferences that
		// name, so inserting one crashes on startup with nothing useful in the log. Refuse it
		// here instead, where the message can say which setting is at fault.
		void AddChecked(INISettingCollection* a_collection, RE::Setting* a_setting, const char* a_name)
		{
			if (a_setting && a_setting->name)
			{
				a_collection->AddSettings(a_setting);

				return;
			}

			logger::critical("Setting \"{}\" was built with a value that does not match the type its "
							 "name prefix promises, so it has been skipped", a_name);
		}

		// Copies whatever the collection currently holds into the variables above. Shared by
		// Init() and Reload() so the two cannot read the INI differently.
		void ReadFromCollection()
		{
			INISettingCollection* c = INISettingCollection::GetSingleton();

			{
				using namespace debug;
				const auto raw = Read<std::uint32_t>(c, "uLogLevel:Debug", static_cast<std::uint32_t>(logLevel));

				// spdlog indexes its level table by this value, so a hand-edited uLogLevel=99
				// would read off the end of it the next time anything logged.
				logLevel = raw <= static_cast<std::uint32_t>(logger::level::off)
							   ? static_cast<logger::level>(raw)
							   : logger::level::info;
			}

			{
				using namespace display;
				useMetricUnits = Read<bool>(c, "bUseMetricUnits:Display", useMetricUnits);
				showUndiscoveredLocationMarkers = Read<bool>(c, "bShowUndiscoveredLocationMarkers:Display", showUndiscoveredLocationMarkers);
				undiscoveredMeansUnknownMarkers = Read<bool>(c, "bUndiscoveredMeansUnknownMarkers:Display", undiscoveredMeansUnknownMarkers);
				undiscoveredMeansUnknownInfo = Read<bool>(c, "bUndiscoveredMeansUnknownInfo:Display", undiscoveredMeansUnknownInfo);
				showEnemyMarkers = Read<bool>(c, "bShowEnemyMarkers:Display", showEnemyMarkers);
				showEnemyNameUnderMarker = Read<bool>(c, "bShowEnemyNameUnderMarker:Display", showEnemyNameUnderMarker);
				showObjectiveAsTarget = Read<bool>(c, "bShowObjectiveAsTarget:Display", showObjectiveAsTarget);
				showOtherObjectivesCount = Read<bool>(c, "bShowOtherObjectivesCount:Display", showOtherObjectivesCount);
				showInteriorMarkers = Read<bool>(c, "bShowInteriorMarkers:Display", showInteriorMarkers);
				angleToShowMarkerDetails = Read<float>(c, "fAngleToShowMarkerDetails:Display", angleToShowMarkerDetails);
				angleToKeepMarkerDetailsShown = Read<float>(c, "fAngleToKeepMarkerDetailsShown:Display", angleToKeepMarkerDetailsShown);
				focusingDelayToShow = Read<float>(c, "fFocusingDelayToShow:Display", focusingDelayToShow);
			}

			{
				using namespace questlist;
				positionX = Read<float>(c, "fPositionX:QuestList", positionX);
				positionY = Read<float>(c, "fPositionY:QuestList", positionY);
				maxHeight = Read<float>(c, "fMaxHeight:QuestList", maxHeight);
				showInExteriors = Read<bool>(c, "bShowInExteriors:QuestList", showInExteriors);
				showInInteriors = Read<bool>(c, "bShowInInteriors:QuestList", showInInteriors);
				walkingDelayToShow = Read<float>(c, "fWalkingDelayToShow:QuestList", walkingDelayToShow);
				joggingDelayToShow = Read<float>(c, "fJoggingDelayToShow:QuestList", joggingDelayToShow);
				sprintingDelayToShow = Read<float>(c, "fSprintingDelayToShow:QuestList", sprintingDelayToShow);
				hideInCombat = Read<bool>(c, "bHideInCombat:QuestList", hideInCombat);
			}
		}
	}

	void Init(const std::string& a_iniFileName)
	{
		CaptureDefaults();

		iniFileName = a_iniFileName;
		iniPath = std::filesystem::current_path().append("Data\\SKSE\\Plugins").append(a_iniFileName).string();

		INISettingCollection* iniSettingCollection = INISettingCollection::GetSingleton();

		// Registered one at a time through AddChecked, so a type that does not match its name
		// prefix is reported rather than crashing the game as it loads.
		const auto add = [iniSettingCollection](const char* a_name, auto a_value) {
			AddChecked(iniSettingCollection, MakeSetting(a_name, a_value), a_name);
		};

		{
			using namespace debug;
			add("uLogLevel:Debug", static_cast<std::uint32_t>(logLevel));
		}

		{
			using namespace display;
			add("bUseMetricUnits:Display", useMetricUnits);
			add("bShowUndiscoveredLocationMarkers:Display", showUndiscoveredLocationMarkers);
			add("bUndiscoveredMeansUnknownMarkers:Display", undiscoveredMeansUnknownMarkers);
			add("bUndiscoveredMeansUnknownInfo:Display", undiscoveredMeansUnknownInfo);
			add("bShowEnemyMarkers:Display", showEnemyMarkers);
			add("bShowEnemyNameUnderMarker:Display", showEnemyNameUnderMarker);
			add("bShowObjectiveAsTarget:Display", showObjectiveAsTarget);
			add("bShowOtherObjectivesCount:Display", showOtherObjectivesCount);
			add("bShowInteriorMarkers:Display", showInteriorMarkers);
			add("fAngleToShowMarkerDetails:Display", angleToShowMarkerDetails);
			add("fAngleToKeepMarkerDetailsShown:Display", angleToKeepMarkerDetailsShown);
			add("fFocusingDelayToShow:Display", focusingDelayToShow);
		}

		{
			using namespace questlist;
			add("fPositionX:QuestList", positionX);
			add("fPositionY:QuestList", positionY);
			add("fMaxHeight:QuestList", maxHeight);
			add("bShowInExteriors:QuestList", showInExteriors);
			add("bShowInInteriors:QuestList", showInInteriors);
			add("fWalkingDelayToShow:QuestList", walkingDelayToShow);
			add("fJoggingDelayToShow:QuestList", joggingDelayToShow);
			add("fSprintingDelayToShow:QuestList", sprintingDelayToShow);
			add("bHideInCombat:QuestList", hideInCombat);
		}

		if (!iniSettingCollection->ReadFromFile(a_iniFileName))
		{
			logger::warn("Could not read {}, falling back to default options", a_iniFileName);
		}

		ReadFromCollection();
	}

	bool Reload()
	{
		if (iniFileName.empty())
		{
			logger::error("Cannot reload settings before Init() has run");

			return false;
		}

		if (!INISettingCollection::GetSingleton()->ReadFromFile(iniFileName))
		{
			logger::error("Could not re-read {}; keeping the settings already loaded", iniPath);

			return false;
		}

		ReadFromCollection();

		logger::info("Reloaded settings from {}", iniPath);

		return true;
	}

	bool Save()
	{
		if (iniPath.empty())
		{
			logger::error("Cannot save settings before Init() has run");

			return false;
		}

		bool ok = true;

		// Anything left over from an earlier Save() has already been flushed; starting clean
		// keeps a failed flush from writing a stale value on the next attempt.
		pendingWrites.clear();

		ok &= WriteUInt(kDebugSection, "uLogLevel", static_cast<std::uint32_t>(debug::logLevel));

		ok &= WriteBool(kDisplaySection, "bUseMetricUnits", display::useMetricUnits);
		ok &= WriteBool(kDisplaySection, "bShowUndiscoveredLocationMarkers", display::showUndiscoveredLocationMarkers);
		ok &= WriteBool(kDisplaySection, "bUndiscoveredMeansUnknownMarkers", display::undiscoveredMeansUnknownMarkers);
		ok &= WriteBool(kDisplaySection, "bUndiscoveredMeansUnknownInfo", display::undiscoveredMeansUnknownInfo);
		ok &= WriteBool(kDisplaySection, "bShowEnemyMarkers", display::showEnemyMarkers);
		ok &= WriteBool(kDisplaySection, "bShowEnemyNameUnderMarker", display::showEnemyNameUnderMarker);
		ok &= WriteBool(kDisplaySection, "bShowObjectiveAsTarget", display::showObjectiveAsTarget);
		ok &= WriteBool(kDisplaySection, "bShowOtherObjectivesCount", display::showOtherObjectivesCount);
		ok &= WriteBool(kDisplaySection, "bShowInteriorMarkers", display::showInteriorMarkers);
		ok &= WriteFloat(kDisplaySection, "fAngleToShowMarkerDetails", display::angleToShowMarkerDetails);
		ok &= WriteFloat(kDisplaySection, "fAngleToKeepMarkerDetailsShown", display::angleToKeepMarkerDetailsShown);
		ok &= WriteFloat(kDisplaySection, "fFocusingDelayToShow", display::focusingDelayToShow);

		ok &= WriteFloat(kQuestListSection, "fPositionX", questlist::positionX);
		ok &= WriteFloat(kQuestListSection, "fPositionY", questlist::positionY);
		ok &= WriteFloat(kQuestListSection, "fMaxHeight", questlist::maxHeight);
		ok &= WriteBool(kQuestListSection, "bShowInExteriors", questlist::showInExteriors);
		ok &= WriteBool(kQuestListSection, "bShowInInteriors", questlist::showInInteriors);
		ok &= WriteFloat(kQuestListSection, "fWalkingDelayToShow", questlist::walkingDelayToShow);
		ok &= WriteFloat(kQuestListSection, "fJoggingDelayToShow", questlist::joggingDelayToShow);
		ok &= WriteFloat(kQuestListSection, "fSprintingDelayToShow", questlist::sprintingDelayToShow);
		ok &= WriteBool(kQuestListSection, "bHideInCombat", questlist::hideInCombat);

		// Write the file once, with every queued key applied. Until this succeeds nothing has
		// reached disk, so its result - not the queueing above - decides whether Save() worked.
		ok &= FlushPendingWrites();

		pendingWrites.clear();

		if (ok)
		{
			logger::info("Saved settings to {}", iniPath);
		}
		else
		{
			logger::error("Failed to save settings to {}", iniPath);
		}

		return ok;
	}

	void RestoreDefaults()
	{
		debug::logLevel = defaults.logLevel;

		display::useMetricUnits = defaults.useMetricUnits;
		display::showUndiscoveredLocationMarkers = defaults.showUndiscoveredLocationMarkers;
		display::undiscoveredMeansUnknownMarkers = defaults.undiscoveredMeansUnknownMarkers;
		display::undiscoveredMeansUnknownInfo = defaults.undiscoveredMeansUnknownInfo;
		display::showEnemyMarkers = defaults.showEnemyMarkers;
		display::showEnemyNameUnderMarker = defaults.showEnemyNameUnderMarker;
		display::showObjectiveAsTarget = defaults.showObjectiveAsTarget;
		display::showOtherObjectivesCount = defaults.showOtherObjectivesCount;
		display::showInteriorMarkers = defaults.showInteriorMarkers;
		display::angleToShowMarkerDetails = defaults.angleToShowMarkerDetails;
		display::angleToKeepMarkerDetailsShown = defaults.angleToKeepMarkerDetailsShown;
		display::focusingDelayToShow = defaults.focusingDelayToShow;

		questlist::positionX = defaults.positionX;
		questlist::positionY = defaults.positionY;
		questlist::maxHeight = defaults.maxHeight;
		questlist::showInExteriors = defaults.showInExteriors;
		questlist::showInInteriors = defaults.showInInteriors;
		questlist::walkingDelayToShow = defaults.walkingDelayToShow;
		questlist::joggingDelayToShow = defaults.joggingDelayToShow;
		questlist::sprintingDelayToShow = defaults.sprintingDelayToShow;
		questlist::hideInCombat = defaults.hideInCombat;
	}

	const std::string& GetIniPath() { return iniPath; }
}
