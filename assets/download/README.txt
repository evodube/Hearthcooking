Hearthcooking - cook by hand in Skyrim SE, AE and GOG
=====================================================

Needs: SKSE64, Address Library for SKSE Plugins. Optional: SKSE Menu Framework (in-game settings).

Sit down at a cooking pot or a Hearthfire oven. A recipe book opens:
  - Quick Cook: the game's own crafting menu; its dishes are Plain.
  - Cook by Hand: make it yourself for Burnt, Plain, Hearty or Masterwork.

Controls in the kitchen
  Left button      hold to pick up, let go to put down; click with the knife over the board to chop
  Right button     put back what you're holding
  H                how to cook (this recipe)
  M                sound on/off
  Tab / Esc        put the dish aside (nothing is used up)

Settings: Data\SKSE\Plugins\Hearthcooking.ini (or SKSE Menu Framework > Hearthcooking).
Set Enabled = 0 to switch it off and get the game's own menus back.

Source and licence: GPL-3.0 (LICENSE.txt in this folder).

Translations
  The kitchen's words are read from Data\SKSE\Plugins\Hearthcooking\Translations\Hearthcooking_<LANGUAGE>.txt, the
  language being the game's own (Skyrim.ini sLanguage: KOREAN, FRENCH...). Hearthcooking_ENGLISH.txt there lists every
  line: copy it, rename it for your language and translate the text after each TAB. The notes at its top explain the
  {} gaps. A line left out stays English. To force a language: Hearthcooking.ini [General] Language = KOREAN.
  A translation patch is just that one file: no plugin, no DLL.
