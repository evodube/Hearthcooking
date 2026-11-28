// Hearthcooking - cook by hand in Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#pragma once

#include <string>
#include <vector>

// The kitchen's words in the player's language. Every line the player reads is written in English in the code and
// looked up by that English in Data\SKSE\Plugins\Hearthcooking\Translations\Hearthcooking_<LANGUAGE>.txt (the game's
// own language: ENGLISH, KOREAN, FRENCH...), one "English<TAB>translation" per line. A line that isn't there stays
// English. Hearthcooking_ENGLISH.txt lists every line and is the template to translate.
namespace Lang
{
	void        Load();      // at data loaded, and again from the settings page
	std::string Language();  // the language in use, as the file names have it ("KOREAN")
	int         Count();     // lines loaded from the translation
	std::string File();      // the file read ("" if none)

	std::string Tr(const char* a_english);
	inline std::string Tr(const std::string& a_english) { return Tr(a_english.c_str()); }

	// A line with gaps: "Dice the {}" -> "{}을(를) 깍둑썰기". The translation may number its gaps ({0} {1}) to put
	// them in another order; a translation whose gaps don't work falls back to the English.
	template <class... A>
	std::string TrF(const char* a_english, A&&... a_args)
	{
		const std::string t = Tr(a_english);
		try {
			return fmt::format(fmt::runtime(t), a_args...);
		} catch (...) {
			try {
				return fmt::format(fmt::runtime(a_english), a_args...);
			} catch (...) {
				return a_english;
			}
		}
	}

	std::vector<std::string> AllText();  // every translated line (for the fonts' letters)
}

// marks English that's translated later, where it's used (so the template lists it)
#define N_(a_english) a_english

using Lang::Tr;
using Lang::TrF;
