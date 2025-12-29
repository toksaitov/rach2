//
// Copyright(C) 2005-2014 Simon Howard
//
// Ported to Rach2 freestanding environment
// Modified to use MEMFILE instead of FILE*
//
// DESCRIPTION:
//    Reading of MIDI files from memory.

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "memio.h"
#include "midifile.h"

#define HEADER_CHUNK_ID "MThd"
#define TRACK_CHUNK_ID  "MTrk"

// Byte swap macros for big-endian MIDI data
static inline unsigned short SwapBE16(unsigned short x)
{
    return ((x >> 8) & 0xFF) | ((x & 0xFF) << 8);
}

static inline unsigned int SwapBE32(unsigned int x)
{
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |
           ((x & 0xFF00) << 8) | ((x & 0xFF) << 24);
}

typedef struct {
    byte chunk_id[4];
    unsigned int chunk_size;
} __attribute__((packed)) chunk_header_t;

typedef struct {
    chunk_header_t chunk_header;
    unsigned short format_type;
    unsigned short num_tracks;
    unsigned short time_division;
} __attribute__((packed)) midi_header_t;

typedef struct
{
    // Length in bytes:
    unsigned int data_len;

    // Events in this track:
    midi_event_t *events;
    int num_events;
} midi_track_t;

struct midi_track_iter_s
{
    midi_track_t *track;
    unsigned int position;
};

struct midi_file_s
{
    midi_header_t header;

    // All tracks in this file:
    midi_track_t *tracks;
    unsigned int num_tracks;
};

// Check the header of a chunk:
static boolean CheckChunkHeader(chunk_header_t *chunk, const char *expected_id)
{
    return (memcmp((char *) chunk->chunk_id, expected_id, 4) == 0);
}

// Read a single byte.  Returns false on error.
static boolean ReadByte(byte *result, MEMFILE *stream)
{
    return mem_fread(result, 1, 1, stream) == 1;
}

// Read a variable-length value.
static boolean ReadVariableLength(unsigned int *result, MEMFILE *stream)
{
    int i;
    byte b = 0;

    *result = 0;

    for (i = 0; i < 4; ++i)
    {
        if (!ReadByte(&b, stream))
        {
            return false;
        }

        // Insert the bottom seven bits from this byte.
        *result <<= 7;
        *result |= b & 0x7f;

        // If the top bit is not set, this is the end.
        if ((b & 0x80) == 0)
        {
            return true;
        }
    }

    return false;
}

// Read a byte sequence into the data buffer.
static void *ReadByteSequence(unsigned int num_bytes, MEMFILE *stream)
{
    unsigned int i;
    byte *result;

    // Allocate a buffer. Allocate one extra byte, as malloc(0) is
    // non-portable.
    result = malloc(num_bytes + 1);

    if (result == NULL)
    {
        return NULL;
    }

    // Read the data:
    for (i = 0; i < num_bytes; ++i)
    {
        if (!ReadByte(&result[i], stream))
        {
            free(result);
            return NULL;
        }
    }

    return result;
}

// Read a MIDI channel event.
// two_param indicates that the event type takes two parameters
// (three byte) otherwise it is single parameter (two byte)
static boolean ReadChannelEvent(midi_event_t *event,
                                byte event_type, boolean two_param,
                                MEMFILE *stream)
{
    byte b = 0;

    // Set basics:
    event->event_type = event_type & 0xf0;
    event->data.channel.channel = event_type & 0x0f;

    // Read parameters:
    if (!ReadByte(&b, stream))
    {
        return false;
    }

    event->data.channel.param1 = b;

    // Second parameter:
    if (two_param)
    {
        if (!ReadByte(&b, stream))
        {
            return false;
        }

        event->data.channel.param2 = b;
    }

    return true;
}

// Read sysex event:
static boolean ReadSysExEvent(midi_event_t *event, int event_type,
                              MEMFILE *stream)
{
    event->event_type = event_type;

    if (!ReadVariableLength(&event->data.sysex.length, stream))
    {
        return false;
    }

    // Read the byte sequence:
    event->data.sysex.data = ReadByteSequence(event->data.sysex.length, stream);

    if (event->data.sysex.data == NULL)
    {
        return false;
    }

    return true;
}

// Read meta event:
static boolean ReadMetaEvent(midi_event_t *event, MEMFILE *stream)
{
    byte b = 0;

    event->event_type = MIDI_EVENT_META;

    // Read meta event type:
    if (!ReadByte(&b, stream))
    {
        return false;
    }

    event->data.meta.type = b;

    // Read length of meta event data:
    if (!ReadVariableLength(&event->data.meta.length, stream))
    {
        return false;
    }

    // Read the byte sequence:
    event->data.meta.data = ReadByteSequence(event->data.meta.length, stream);

    if (event->data.meta.data == NULL)
    {
        return false;
    }

    return true;
}

static boolean ReadEvent(midi_event_t *event, unsigned int *last_event_type,
                         MEMFILE *stream)
{
    byte event_type = 0;

    if (!ReadVariableLength(&event->delta_time, stream))
    {
        return false;
    }

    if (!ReadByte(&event_type, stream))
    {
        return false;
    }

    // All event types have their top bit set.  Therefore, if
    // the top bit is not set, it is because we are using the "same
    // as previous event type" shortcut to save a byte.  Skip back
    // a byte so that we read this byte again.
    if ((event_type & 0x80) == 0)
    {
        event_type = *last_event_type;

        if (mem_fseek(stream, -1, MEM_SEEK_CUR) != 0)
        {
            return false;
        }
    }
    else
    {
        *last_event_type = event_type;
    }

    // Check event type:
    switch (event_type & 0xf0)
    {
        // Two parameter channel events:
        case MIDI_EVENT_NOTE_OFF:
        case MIDI_EVENT_NOTE_ON:
        case MIDI_EVENT_AFTERTOUCH:
        case MIDI_EVENT_CONTROLLER:
        case MIDI_EVENT_PITCH_BEND:
            return ReadChannelEvent(event, event_type, true, stream);

        // Single parameter channel events:
        case MIDI_EVENT_PROGRAM_CHANGE:
        case MIDI_EVENT_CHAN_AFTERTOUCH:
            return ReadChannelEvent(event, event_type, false, stream);

        default:
            break;
    }

    // Specific value?
    switch (event_type)
    {
        case MIDI_EVENT_SYSEX:
        case MIDI_EVENT_SYSEX_SPLIT:
            return ReadSysExEvent(event, event_type, stream);

        case MIDI_EVENT_META:
            return ReadMetaEvent(event, stream);

        default:
            break;
    }

    return false;
}

// Free an event:
static void FreeEvent(midi_event_t *event)
{
    // Some event types have dynamically allocated buffers assigned
    // to them that must be freed.
    switch (event->event_type)
    {
        case MIDI_EVENT_SYSEX:
        case MIDI_EVENT_SYSEX_SPLIT:
            free(event->data.sysex.data);
            break;

        case MIDI_EVENT_META:
            free(event->data.meta.data);
            break;

        default:
            // Nothing to do.
            break;
    }
}

// Read and check the track chunk header
static boolean ReadTrackHeader(midi_track_t *track, MEMFILE *stream)
{
    chunk_header_t chunk_header;

    if (mem_fread(&chunk_header, sizeof(chunk_header_t), 1, stream) < 1)
    {
        return false;
    }

    if (!CheckChunkHeader(&chunk_header, TRACK_CHUNK_ID))
    {
        return false;
    }

    track->data_len = SwapBE32(chunk_header.chunk_size);

    return true;
}

static boolean ReadTrack(midi_track_t *track, MEMFILE *stream)
{
    midi_event_t *new_events;
    midi_event_t *event;
    unsigned int last_event_type;
    unsigned int capacity;

    track->num_events = 0;
    track->events = NULL;

    // Read the header:
    if (!ReadTrackHeader(track, stream))
    {
        printf("ReadTrack: header failed\n");
        return false;
    }

    // Pre-allocate events array with initial capacity
    // Using capacity-based growth to avoid bump allocator waste
    capacity = 256;
    track->events = malloc(sizeof(midi_event_t) * capacity);
    if (track->events == NULL)
    {
        printf("ReadTrack: initial malloc failed\n");
        return false;
    }

    // Then the events:
    last_event_type = 0;

    for (;;)
    {
        // Grow array if needed (double capacity)
        if (track->num_events >= capacity)
        {
            capacity *= 2;
            new_events = malloc(sizeof(midi_event_t) * capacity);
            if (new_events == NULL)
            {
                printf("ReadTrack: realloc failed at event %d\n", track->num_events);
                return false;
            }
            // Copy existing events to new array
            memcpy(new_events, track->events, sizeof(midi_event_t) * track->num_events);
            // Note: old memory is leaked (bump allocator doesn't free)
            track->events = new_events;
        }

        // Read the next event:
        event = &track->events[track->num_events];
        if (!ReadEvent(event, &last_event_type, stream))
        {
            printf("ReadTrack: ReadEvent failed at event %d\n", track->num_events);
            return false;
        }

        ++track->num_events;

        // End of track?
        if (event->event_type == MIDI_EVENT_META
         && event->data.meta.type == MIDI_META_END_OF_TRACK)
        {
            break;
        }
    }

    return true;
}

// Free a track:
static void FreeTrack(midi_track_t *track)
{
    unsigned int i;

    for (i = 0; i < track->num_events; ++i)
    {
        FreeEvent(&track->events[i]);
    }

    free(track->events);
}

static boolean ReadAllTracks(midi_file_t *file, MEMFILE *stream)
{
    unsigned int i;

    // Allocate list of tracks and read each track:
    file->tracks = malloc(sizeof(midi_track_t) * file->num_tracks);

    if (file->tracks == NULL)
    {
        return false;
    }

    memset(file->tracks, 0, sizeof(midi_track_t) * file->num_tracks);

    // Read each track:
    for (i = 0; i < file->num_tracks; ++i)
    {
        if (!ReadTrack(&file->tracks[i], stream))
        {
            return false;
        }
    }

    return true;
}

// Read and check the header chunk.
static boolean ReadFileHeader(midi_file_t *file, MEMFILE *stream)
{
    unsigned int format_type;

    if (mem_fread(&file->header, sizeof(midi_header_t), 1, stream) < 1)
    {
        return false;
    }

    if (!CheckChunkHeader(&file->header.chunk_header, HEADER_CHUNK_ID)
     || SwapBE32(file->header.chunk_header.chunk_size) != 6)
    {
        return false;
    }

    format_type = SwapBE16(file->header.format_type);
    file->num_tracks = SwapBE16(file->header.num_tracks);

    if ((format_type != 0 && format_type != 1)
     || file->num_tracks < 1)
    {
        return false;
    }

    return true;
}

void MIDI_FreeFile(midi_file_t *file)
{
    unsigned int i;

    if (file->tracks != NULL)
    {
        for (i = 0; i < file->num_tracks; ++i)
        {
            FreeTrack(&file->tracks[i]);
        }

        free(file->tracks);
    }

    free(file);
}

midi_file_t *MIDI_LoadFileBuffer(void *data, size_t len)
{
    midi_file_t *file;
    MEMFILE *stream;

    file = malloc(sizeof(midi_file_t));

    if (file == NULL)
    {
        printf("MIDI_Load: malloc failed\n");
        return NULL;
    }

    file->tracks = NULL;
    file->num_tracks = 0;

    // Open memory stream
    stream = mem_fopen_read(data, len);

    if (stream == NULL)
    {
        printf("MIDI_Load: mem_fopen_read failed\n");
        free(file);
        return NULL;
    }

    // Read MIDI file header
    if (!ReadFileHeader(file, stream))
    {
        printf("MIDI_Load: ReadFileHeader failed\n");
        mem_fclose(stream);
        MIDI_FreeFile(file);
        return NULL;
    }

    // Read all tracks:
    if (!ReadAllTracks(file, stream))
    {
        printf("MIDI_Load: ReadAllTracks failed\n");
        mem_fclose(stream);
        MIDI_FreeFile(file);
        return NULL;
    }

    mem_fclose(stream);

    return file;
}

// Get the number of tracks in a MIDI file.
unsigned int MIDI_NumTracks(midi_file_t *file)
{
    return file->num_tracks;
}

// Start iterating over the events in a track.
midi_track_iter_t *MIDI_IterateTrack(midi_file_t *file, unsigned int track)
{
    midi_track_iter_t *iter;

    if (track >= file->num_tracks)
    {
        return NULL;
    }

    iter = malloc(sizeof(*iter));
    if (iter == NULL)
    {
        return NULL;
    }

    iter->track = &file->tracks[track];
    iter->position = 0;

    return iter;
}

void MIDI_FreeIterator(midi_track_iter_t *iter)
{
    free(iter);
}

// Get the time until the next MIDI event in a track.
unsigned int MIDI_GetDeltaTime(midi_track_iter_t *iter)
{
    if (iter->position < iter->track->num_events)
    {
        midi_event_t *next_event;

        next_event = &iter->track->events[iter->position];

        return next_event->delta_time;
    }
    else
    {
        return 0;
    }
}

// Get a pointer to the next MIDI event.
int MIDI_GetNextEvent(midi_track_iter_t *iter, midi_event_t **event)
{
    if (iter->position < iter->track->num_events)
    {
        *event = &iter->track->events[iter->position];
        ++iter->position;

        return 1;
    }
    else
    {
        return 0;
    }
}

unsigned int MIDI_GetFileTimeDivision(midi_file_t *file)
{
    short result = SwapBE16(file->header.time_division);

    // Negative time division indicates SMPTE time and must be handled
    // differently.
    if (result < 0)
    {
        return (signed int)(-(result/256))
             * (signed int)(result & 0xFF);
    }
    else
    {
        return result;
    }
}

void MIDI_RestartIterator(midi_track_iter_t *iter)
{
    iter->position = 0;
}
