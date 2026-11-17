#pragma once
#include <vector>

/** Note generator for the MIDI Studio's Keys part (piano, synth, bass): chords, arpeggios, bass lines and riffs in a key
    and scale, over a chord progression. Pure functions, no JUCE. Times are in quarter notes from the start. */
namespace ff::keys
{
enum Scale { major, minor, dorian, mixolydian, pentMajor, pentMinor, blues, numScales };
enum Style { blockChords, arpUp, arpUpDown, bassRoots, bassWalking, riff, numStyles };

const char* scaleName (int scale);
const char* styleName (int style);
const std::vector<int>& scaleSteps (int scale);   // semitones above the key note, ascending, within one octave
int numProgressions();
const char* progressionName (int index);
const std::vector<int>& progression (int index);  // a scale degree (0-based) for each bar, repeated to fill the length

struct Options
{
    int key = 0;                 // 0 = C ... 11 = B
    int scale = major;
    int progression = 0;
    int style = blockChords;
    int bars = 4;
    int octave = 4;              // C4 = MIDI 60; the bass styles play two octaves lower
    int tsNum = 4, tsDen = 4;
    int seed = 1;
};

struct Note { double startQ = 0.0, lengthQ = 0.0; int pitch = 60, velocity = 100; };

double barQuarters (const Options&);
std::vector<Note> generate (const Options&);
} // namespace ff::keys
