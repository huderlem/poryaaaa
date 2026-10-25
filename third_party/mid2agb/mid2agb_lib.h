/*
 * mid2agb as a library (poryaaaa).  The converter is pokeemerald's
 * tools/mid2agb, vendored unchanged but for the marked lines (RaiseError
 * throws, the file-scope state resets); this replaces its main.cpp.
 */
#ifndef MID2AGB_LIB_H
#define MID2AGB_LIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert the MIDI file at midiPath to mid2agb's assembler text, as
 * `mid2agb <midi> <label>.s <options>` writes it.  options: mid2agb's own
 * (-E, -X, -N, -V<n>, -P<n>, -R<n>, -G<name>, -L<label>; the value joined to
 * the letter); label: the song label (-L overrides it).  Returns a malloc'd
 * NUL-terminated text (free() it), or NULL with a one-line message in err. */
char *mid2agb_convert(const char *midiPath, const char *label,
                      int optionCount, const char *const *options,
                      char *err, size_t errLen);

#ifdef __cplusplus
}
#endif

#endif /* MID2AGB_LIB_H */
