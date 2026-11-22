// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Config.h"

namespace Config
{
	namespace
	{
		Values      g_values;
		std::set<std::string> g_tipsSeen;
		const char* kPath = "Data\\SKSE\\Plugins\\Hearthcooking.ini";

		std::string Str(const char* a_section, const char* a_key, const char* a_def)
		{
			char buf[256]{};
			::GetPrivateProfileStringA(a_section, a_key, a_def, buf, sizeof(buf), kPath);
			std::string s = buf;
			if (const auto c = s.find_first_of(";#"); c != std::string::npos) {
				s.resize(c);
			}
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
				s.pop_back();
			}
			return s;
		}
		float Float(const char* a_section, const char* a_key, float a_def)
		{
			const auto s = Str(a_section, a_key, "");
			if (s.empty()) {
				return a_def;
			}
			try {
				return std::stof(s);
			} catch (...) {
				return a_def;
			}
		}
		bool Bool(const char* a_section, const char* a_key, bool a_def)
		{
			const auto s = Str(a_section, a_key, "");
			if (s.empty()) {
				return a_def;
			}
			return s == "1" || _stricmp(s.c_str(), "true") == 0 || _stricmp(s.c_str(), "on") == 0;
		}
	}

	const Values& Get() { return g_values; }

	bool TipSeen(const char* a_name) { return g_tipsSeen.count(a_name) > 0; }
	void MarkTipSeen(const char* a_name)
	{
		if (g_tipsSeen.insert(a_name).second) {
			::WritePrivateProfileStringA("Tips", a_name, "1", kPath);
		}
	}
	void ResetTips()
	{
		g_tipsSeen.clear();
		::WritePrivateProfileStringA("Tips", nullptr, nullptr, kPath);  // (the whole section)
	}
	Values&       Edit() { return g_values; }

	void Save()
	{
		const auto& v = g_values;
		auto put = [](const char* a_section, const char* a_key, const std::string& a_value) {
			::WritePrivateProfileStringA(a_section, a_key, a_value.c_str(), kPath);
		};
		auto f = [](float a_v) { return fmt::format("{:.2f}", a_v); };
		put("General", "Enabled", v.enabled ? "1" : "0");
		put("Kitchen", "TiltDegrees", f(v.tiltDeg));
		put("Kitchen", "Zoom", f(v.zoom));
		put("Graphics", "Quality", std::to_string(v.quality));
		put("Play", "MouseSpeed", f(v.mouseSpeed));
		put("Play", "UiScale", f(v.uiScale));
		put("Play", "Sound", v.sound ? "1" : "0");
		put("Play", "Volume", f(v.volume));
		put("Play", "Ambience", f(v.ambience));
		put("Play", "EffectsVolume", f(v.effectsVolume));
		for (const auto& [name, lv] : v.soundLevels) {
			put("Sounds", name.c_str(), f(lv));
		}
		put("Play", "Hints", v.hints ? "1" : "0");
		put("Play", "Tips", v.tips ? "1" : "0");
		put("Play", "ControllerButtons", v.padStyle == 1 ? "PlayStation" : "Xbox");
		put("Debug", "Overlay", v.debugOverlay ? "1" : "0");
		spdlog::info("Settings saved");
	}

	void Load()
	{
		Values v;
		v.enabled = Bool("General", "Enabled", v.enabled);
		v.tiltDeg = std::clamp(Float("Kitchen", "TiltDegrees", v.tiltDeg), 25.0f, 85.0f);
		v.zoom = std::clamp(Float("Kitchen", "Zoom", v.zoom), 0.5f, 2.0f);
		v.quality = std::clamp(static_cast<int>(Float("Graphics", "Quality", static_cast<float>(v.quality))), 0, 2);
		v.mouseSpeed = std::clamp(Float("Play", "MouseSpeed", v.mouseSpeed), 0.1f, 5.0f);
		v.uiScale = std::clamp(Float("Play", "UiScale", v.uiScale), 0.5f, 2.0f);
		v.sound = Bool("Play", "Sound", v.sound);
		v.volume = std::clamp(Float("Play", "Volume", v.volume), 0.0f, 2.0f);
		v.ambience = std::clamp(Float("Play", "Ambience", v.ambience), 0.0f, 2.0f);
		v.effectsVolume = std::clamp(Float("Play", "EffectsVolume", v.effectsVolume), 0.0f, 2.0f);
		v.soundLevels.clear();
		for (const char* name : kSoundNames) {
			const float lv = Float("Sounds", name, -1.0f);
			if (lv >= 0.0f) {
				v.soundLevels[name] = std::clamp(lv, 0.0f, 2.0f);
			}
		}
		v.hints = Bool("Play", "Hints", v.hints);
		v.tips = Bool("Play", "Tips", v.tips);
		{
			const auto pad = Str("Play", "ControllerButtons", "Xbox");
			v.padStyle = _stricmp(pad.c_str(), "PlayStation") == 0 || _stricmp(pad.c_str(), "PS") == 0 ? 1 : 0;
		}
		{
			// the tips already shown
			g_tipsSeen.clear();
			char buf[4096]{};
			::GetPrivateProfileSectionA("Tips", buf, sizeof(buf), kPath);
			for (const char* q = buf; *q; q += std::strlen(q) + 1) {
				std::string kv = q;
				if (const auto eq = kv.find('='); eq != std::string::npos && kv.substr(eq + 1) != "0") {
					g_tipsSeen.insert(kv.substr(0, eq));
				}
			}
		}
		v.debugOverlay = Bool("Debug", "Overlay", v.debugOverlay);
		v.logInput = Bool("Debug", "LogInput", v.logInput);
		g_values = v;
		spdlog::info("Settings: {}, tilt {} zoom {} ui {}", v.enabled ? "on" : "off (the game's own menus)", v.tiltDeg, v.zoom, v.uiScale);
	}
}
