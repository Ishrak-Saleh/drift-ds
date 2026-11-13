//Small rules for reading settings.ini that need no Windows, so tests can run them on their own.
//
//where it fits: read_settings in main.c reads each KEY=value line and turns it into a DRIFT_KEY
//variable. This file cleans the value part first.
//why: the settings file is easier to read with a short note after a value, for example
//  FULLSCREEN_SIZE=1280x720 # fullscreen only
//so the note must not become part of the value.
#pragma once

//cuts a comment and the blanks at the end off a settings value, in place. A '#' that comes after
//a space or a tab starts a comment. A '#' with no blank before it stays in the value (a device
//name can contain one). Blanks at the start of the value are kept.
void ini_clean_value(char* value);

//splits one settings line in place. Returns 1 and sets *key and *value for a KEY=value line, and 0
//for anything else (blank line, a line that starts with #, ; or [, a line without a =). Blanks
//around the key are cut and the key is made upper case, so "scale = 3" works like "SCALE=3". The
//value gets the same cleaning as ini_clean_value, and its leading blanks are cut too.
int ini_split(char* line, char** key, char** value);

//1 when the key is one the game or the launcher reads, 0 for a typo or an old name
int ini_key_known(const char* key);
