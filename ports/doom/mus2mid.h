//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2006 Ben Ryves 2006
//
// Ported to Rach2 freestanding environment
//
// mus2mid.h - Convert MUS to MIDI format

#ifndef MUS2MID_H
#define MUS2MID_H

#include "doomtype.h"
#include "memio.h"

// Convert MUS data to MIDI format
// Returns false on success, true on failure
boolean mus2mid(MEMFILE *musinput, MEMFILE *midioutput);

#endif /* MUS2MID_H */
