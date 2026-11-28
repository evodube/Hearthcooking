// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Lang.h"

namespace Lang
{
	namespace
	{
		std::mutex                                   g_mtx;
		std::unordered_map<std::string, std::string> g_lines;
		std::set<std::string>                        g_missing;  // (each told once)
		std::string                                  g_lang = "ENGLISH", g_file;

		std::string Upper(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			return a_s;
		}
		std::string Trim(std::string a_s)
		{
			while (!a_s.empty() && (a_s.back() == ' ' || a_s.back() == '\t' || a_s.back() == '\r')) {
				a_s.pop_back();
			}
			std::size_t i = 0;
			while (i < a_s.size() && (a_s[i] == ' ' || a_s[i] == '\t')) {
				++i;
			}
			return a_s.substr(i);
		}

		// The game's language: Hearthcooking.ini's [General] Language if set, else the game's own sLanguage
		std::string GameLanguage()
		{
			char buf[64]{};
			::GetPrivateProfileStringA("General", "Language", "", buf, sizeof(buf), "Data\\SKSE\\Plugins\\Hearthcooking.ini");
			std::string s = buf;
			if (const auto c = s.find_first_of(";#"); c != std::string::npos) {
				s.resize(c);
			}
			s = Trim(s);
			if (!s.empty()) {
				return Upper(s);
			}
			if (auto* ini = RE::INISettingCollection::GetSingleton()) {
				if (auto* set = ini->GetSetting("sLanguage:General"); set && set->GetString() && *set->GetString()) {
					return Upper(set->GetString());
				}
			}
			return "ENGLISH";
		}

		// The file's text as UTF-8, whether it was saved as UTF-16 (as the game's translation files are) or UTF-8
		std::string ReadText(const std::filesystem::path& a_path)
		{
			std::ifstream f(a_path, std::ios::binary);
			if (!f) {
				return {};
			}
			std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE) {
				const auto* w = reinterpret_cast<const wchar_t*>(raw.data() + 2);
				const int   wn = static_cast<int>((raw.size() - 2) / 2);
				const int   n = ::WideCharToMultiByte(CP_UTF8, 0, w, wn, nullptr, 0, nullptr, nullptr);
				std::string out(static_cast<std::size_t>((std::max)(0, n)), '\0');
				if (n > 0) {
					::WideCharToMultiByte(CP_UTF8, 0, w, wn, out.data(), n, nullptr, nullptr);
				}
				return out;
			}
			if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB &&
				static_cast<unsigned char>(raw[2]) == 0xBF) {
				return raw.substr(3);
			}
			return raw;
		}

		std::string Unescape(const std::string& a_s)
		{
			std::string o;
			for (std::size_t i = 0; i < a_s.size(); ++i) {
				if (a_s[i] == '\\' && i + 1 < a_s.size()) {
					const char c = a_s[i + 1];
					if (c == 'n') {
						o += '\n', ++i;
						continue;
					}
					if (c == 't') {
						o += '\t', ++i;
						continue;
					}
					if (c == '\\') {
						o += '\\', ++i;
						continue;
					}
				}
				o += a_s[i];
			}
			return o;
		}
	}

	void Load()
	{
		const std::string lang = GameLanguage();
		const std::filesystem::path dir = "Data\\SKSE\\Plugins\\Hearthcooking\\Translations";
		const std::filesystem::path path = dir / fmt::format("Hearthcooking_{}.txt", lang);
		std::unordered_map<std::string, std::string> lines;
		std::error_code ec;
		std::string     file;
		if (std::filesystem::exists(path, ec)) {
			file = path.string();
			const std::string text = ReadText(path);
			std::size_t       at = 0;
			int               bad = 0;
			while (at < text.size()) {
				auto end = text.find('\n', at);
				if (end == std::string::npos) {
					end = text.size();
				}
				std::string line = text.substr(at, end - at);
				at = end + 1;
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				const std::string t = Trim(line);
				if (t.empty() || t.rfind("//", 0) == 0 || t[0] == '#' || t[0] == ';') {
					continue;
				}
				const auto tab = line.find('\t');
				if (tab == std::string::npos) {
					++bad;
					continue;
				}
				std::string key = Unescape(line.substr(0, tab));
				std::string val = line.substr(tab + 1);
				while (!val.empty() && val.front() == '\t') {
					val.erase(val.begin());  // (more tabs to line the columns up)
				}
				if (!key.empty() && key[0] == '$') {
					key.erase(key.begin());
				}
				if (!key.empty() && !val.empty()) {
					lines[key] = Unescape(val);
				}
			}
			spdlog::info("Language {}: {} lines from {}{}", lang, lines.size(), file, bad ? fmt::format(" ({} lines without a tab skipped)", bad) : "");
		} else if (lang != "ENGLISH") {
			spdlog::info("Language {}: no {} - the kitchen's words stay English (Hearthcooking_ENGLISH.txt there is the template)", lang, path.string());
		}
		std::lock_guard lk(g_mtx);
		g_lines = std::move(lines);
		g_missing.clear();
		g_lang = lang;
		g_file = file;
	}

	std::string Language()
	{
		std::lock_guard lk(g_mtx);
		return g_lang;
	}
	int Count()
	{
		std::lock_guard lk(g_mtx);
		return static_cast<int>(g_lines.size());
	}
	std::string File()
	{
		std::lock_guard lk(g_mtx);
		return g_file;
	}

	std::string Tr(const char* a_english)
	{
		if (!a_english || !*a_english) {
			return a_english ? a_english : "";
		}
		std::lock_guard lk(g_mtx);
		if (g_lines.empty()) {
			return a_english;
		}
		if (const auto it = g_lines.find(a_english); it != g_lines.end()) {
			return it->second;
		}
		if (g_missing.insert(a_english).second) {
			spdlog::info("Language {}: not translated yet: {}", g_lang, a_english);
		}
		return a_english;
	}

	std::vector<std::string> AllText()
	{
		std::lock_guard          lk(g_mtx);
		std::vector<std::string> out;
		out.reserve(g_lines.size());
		for (const auto& [k, v] : g_lines) {
			out.push_back(v);
		}
		return out;
	}
}
