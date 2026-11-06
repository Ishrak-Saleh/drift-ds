// Small rules for reading settings.ini that need no Windows, so tests can run them on their own.
//
// Where it fits: read_settings in main.c reads each KEY=value line and turns it into a DRIFT_KEY
// variable. This file cleans the value part first.
// Why: the settings file is easier to read with a short note after a value, for example
//   FULLSCREEN_SIZE=1280x720 # fullscreen only
// so the note must not become part of the value.
#pragma once

// Cuts a comment and the blanks at the end off a settings value, in place. A '#' that comes after
// a space or a tab starts a comment. A '#' with no blank before it stays in the value (a device
// name can contain one). Blanks at the start of the value are kept.
void ini_clean_value(char* value);
