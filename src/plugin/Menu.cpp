// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
//
// The settings page in SKSE Menu Framework (if it's installed): every INI setting, changed live, saved to the INI.
#include "Menu.h"
#include "Config.h"
#include "Kit.h"
#include "Lang.h"

#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include "SKSEMenuFramework.h"

namespace Menu
{
	namespace
	{
		namespace UI = ImGuiMCP;

		bool g_dirty = false;

		void Hint(const char* a_text)
		{
			UI::TextDisabled("%s", a_text);
			UI::Spacing();
		}
		void Changed(bool a_changed) { g_dirty |= a_changed; }
		// a label in the player's language, the English kept as its id (so a translation doesn't move its setting)
		std::string L(const char* a_english) { return Tr(a_english) + "##" + a_english; }

		void __stdcall Settings()
		{
			auto& v = Config::Edit();
			UI::SeparatorText("Hearthcooking");
			Changed(UI::Checkbox(L("Cook by hand at pots and ovens").c_str(), &v.enabled));
			Hint(Tr("Off: the cooking pot and the oven open the game's own menu.").c_str());
			UI::SeparatorText(L("The kitchen").c_str());
			Changed(UI::SliderFloat(L("Tilt").c_str(), &v.tiltDeg, 30.0f, 80.0f, Tr("%.0f degrees").c_str()));
			Hint(Tr("How far you look down on it.").c_str());
			Changed(UI::SliderFloat(L("Zoom").c_str(), &v.zoom, 0.6f, 1.6f, "%.2f"));
			{
				// the knife, from every one the game has (for trying them)
				const auto&  knives = Kit::Knives();
				const auto&  now = Kit::Get(Kit::Role::Knife);
				const std::string shown = now.vanilla ? now.path : Tr("Hearthcooking's own");
				if (UI::BeginCombo(L("Knife").c_str(), shown.c_str())) {
					if (UI::Selectable(L("The one it finds by itself").c_str(), false)) {
						Kit::SetMesh(Kit::Role::Knife, "");
					}
					if (UI::Selectable(L("Hearthcooking's own").c_str(), !now.vanilla)) {
						Kit::SetMesh(Kit::Role::Knife, "ours");
					}
					for (const auto& k : knives) {
						if (UI::Selectable(k.c_str(), now.vanilla && _stricmp(now.path.c_str(), k.c_str()) == 0)) {
							Kit::SetMesh(Kit::Role::Knife, k);
						}
					}
					UI::EndCombo();
				}
				Hint(Tr("Takes effect the next time the kitchen opens; kept in Hearthcooking.ini.").c_str());
			}
			UI::SeparatorText(L("Graphics").c_str());
			{
				static const char* kQ[] = { N_("Low (fastest)"), N_("Medium"), N_("High") };
				if (UI::BeginCombo(L("Kitchen quality").c_str(), Tr(kQ[std::clamp(v.quality, 0, 2)]).c_str())) {
					for (int q = 2; q >= 0; --q) {
						if (UI::Selectable(L(kQ[q]).c_str(), v.quality == q)) {
							v.quality = q;
							Changed(true);
						}
					}
					UI::EndCombo();
				}
				Hint(Tr("High: 4x anti-aliasing and sharp shadows. Medium: 2x. Low: none, for older cards. Shadows change at the next game start.").c_str());
			}
			UI::SeparatorText(L("Controls and sound").c_str());
			Changed(UI::SliderFloat(L("Mouse speed").c_str(), &v.mouseSpeed, 0.2f, 3.0f, "%.2f"));
			Changed(UI::SliderFloat(L("Interface size").c_str(), &v.uiScale, 0.6f, 1.6f, "%.2f"));
			Changed(UI::Checkbox(L("Sound").c_str(), &v.sound));
			Changed(UI::SliderFloat(L("Kitchen sounds").c_str(), &v.volume, 0.0f, 2.0f, "%.2f"));
			Changed(UI::SliderFloat(L("Fire and simmer").c_str(), &v.ambience, 0.0f, 2.0f, "%.2f"));
			Changed(UI::SliderFloat(L("Menu clicks").c_str(), &v.effectsVolume, 0.0f, 2.0f, "%.2f"));
			if (UI::CollapsingHeader(L("Each sound on its own").c_str())) {
				Hint(Tr("1 is as it comes, 0 is off. On top of the sliders above.").c_str());
				for (std::size_t k = 0; k < std::size(Config::kSoundNames); ++k) {
					const std::string name = Config::kSoundNames[k];
					float lv = v.soundLevels.count(name) ? v.soundLevels[name] : 1.0f;
					if (UI::SliderFloat(L(Config::kSoundLabels[k]).c_str(), &lv, 0.0f, 2.0f, "%.2f")) {
						v.soundLevels[name] = lv;
						g_dirty = true;
					}
				}
			}
			Changed(UI::Checkbox(L("Show what to do next").c_str(), &v.hints));
			{
				const char* styles[] = { "Xbox", "PlayStation" };
				if (UI::BeginCombo(L("Controller buttons").c_str(), styles[v.padStyle == 1 ? 1 : 0])) {
					for (int k = 0; k < 2; ++k) {
						if (UI::Selectable(styles[k], v.padStyle == k)) {
							v.padStyle = k;
							g_dirty = true;
						}
					}
					UI::EndCombo();
				}
			}
			UI::SeparatorText(L("Language").c_str());
			{
				const auto file = Lang::File();
				UI::TextDisabled("%s", (file.empty() ? TrF("{}: no translation file, the kitchen speaks English", Lang::Language())
				                                     : TrF("{}: {} lines from {}", Lang::Language(), Lang::Count(), file)).c_str());
				if (UI::Button(L("Reload the translation").c_str())) {
					Lang::Load();
				}
				Hint(Tr("For translators: edit the file and reload to see it in the kitchen. Letters the kitchen's fonts haven't drawn yet show after a restart.").c_str());
			}
			Changed(UI::Checkbox(L("Debug: mark what the mouse can grab").c_str(), &v.debugOverlay));
			UI::Spacing();
			UI::Separator();
			if (UI::Button(L("Save to Hearthcooking.ini").c_str())) {
				Config::Save();
				g_dirty = false;
			}
			UI::SameLine();
			if (UI::Button(L("Undo changes").c_str())) {
				Config::Load();
				g_dirty = false;
			}
			if (g_dirty) {
				UI::SameLine();
				UI::TextDisabled("%s", Tr("(not saved yet: changes last until you quit)").c_str());
			}
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			spdlog::info("SKSE Menu Framework not installed: settings are in Hearthcooking.ini");
			return;
		}
		SKSEMenuFramework::SetSection("Hearthcooking");
		SKSEMenuFramework::AddSectionItem("Settings", Settings);
		spdlog::info("Settings page added to SKSE Menu Framework");
	}
}
