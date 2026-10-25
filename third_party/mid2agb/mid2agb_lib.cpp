// mid2agb as a library (poryaaaa): main.cpp's globals and option handling,
// writing to a temporary file instead of <output>.s.  See mid2agb_lib.h.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <stdexcept>
#include <string>
#include "main.h"
#include "midi.h"
#include "agb.h"
#include "error.h"
#include "mid2agb_lib.h"

FILE* g_inputFile = nullptr;
FILE* g_outputFile = nullptr;

std::string g_asmLabel;
int g_masterVolume = 127;
std::string g_voiceGroup = "_dummy";
int g_priority = 0;
int g_reverb = -1;
int g_clocksPerBeat = 1;
bool g_exactGateTime = false;
bool g_compressionEnabled = true;

static void SetError(char *err, size_t errLen, const char *msg)
{
    if (err && errLen)
        std::snprintf(err, errLen, "mid2agb: %s", msg);
}

static void CloseFiles()
{
    if (g_inputFile)
        std::fclose(g_inputFile);
    if (g_outputFile)
        std::fclose(g_outputFile);
    g_inputFile = nullptr;
    g_outputFile = nullptr;
}

extern "C" char *mid2agb_convert(const char *midiPath, const char *label,
                                 int optionCount, const char *const *options,
                                 char *err, size_t errLen)
{
    // main.cpp's defaults
    g_asmLabel = label ? label : "song";
    g_masterVolume = 127;
    g_voiceGroup = "_dummy";
    g_priority = 0;
    g_reverb = -1;
    g_clocksPerBeat = 1;
    g_exactGateTime = false;
    g_compressionEnabled = true;
    ResetMidiState();
    ResetAgbState();

    char *text = nullptr;
    try
    {
        // main.cpp's switch, with the value joined to the letter
        for (int i = 0; i < optionCount; i++)
        {
            const char *option = options[i];
            if (!option || option[0] != '-' || option[1] == '\0')
                RaiseError("bad option \"%s\"", option ? option : "");
            const char *arg = option + 2;
            switch (std::toupper((unsigned char)option[1]))
            {
            case 'E': g_exactGateTime = true; break;
            case 'G': g_voiceGroup = arg; break;
            case 'L': g_asmLabel = arg; break;
            case 'N': g_compressionEnabled = false; break;
            case 'P': g_priority = std::atoi(arg); break;
            case 'R': g_reverb = std::atoi(arg); break;
            case 'V': g_masterVolume = std::atoi(arg); break;
            case 'X': g_clocksPerBeat = 2; break;
            default: RaiseError("unknown option \"%s\"", option);
            }
        }

        g_inputFile = std::fopen(midiPath, "rb");
        if (g_inputFile == nullptr)
            RaiseError("failed to open \"%s\" for reading", midiPath);
        g_outputFile = std::tmpfile();
        if (g_outputFile == nullptr)
            RaiseError("failed to open a temporary file for writing");

        ReadMidiFileHeader();
        PrintAgbHeader();
        ReadMidiTracks();
        PrintAgbFooter();

        long size = std::ftell(g_outputFile);
        if (size < 0 || std::fseek(g_outputFile, 0, SEEK_SET) != 0)
            RaiseError("failed to read the output back");
        text = static_cast<char *>(std::malloc((size_t)size + 1));
        if (!text)
            RaiseError("out of memory");
        if (std::fread(text, 1, (size_t)size, g_outputFile) != (size_t)size)
        {
            std::free(text);
            text = nullptr;
            RaiseError("failed to read the output back");
        }
        text[size] = '\0';
    }
    catch (const std::exception& e)
    {
        SetError(err, errLen, e.what());
        CloseFiles();
        return nullptr;
    }
    CloseFiles();
    return text;
}
