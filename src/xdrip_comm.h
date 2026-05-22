#pragma once

#include <pebble.h>
#include "app_messages.h"

// File type constants matching ConstantsPebble.swift
#define XDRIP_FILE_TYPE_SGV        0
#define XDRIP_FILE_TYPE_TREATMENTS 1

// Called from comm.c whenever a MSG_TYPE_XDRIP_CHUNK message is received.
// Accumulates chunks from both file types. When both the SGV file and the
// treatments file have been fully received, assembles a DataMessage and
// invokes data_callback.
// Returns true when a DataMessage was assembled and data_callback was called.
bool xdrip_handle_chunk(
    uint8_t file_type,
    uint16_t total_chunks,
    uint16_t chunk_index,
    const uint8_t *chunk_data,
    uint16_t chunk_len,
    void (*data_callback)(DataMessage *data)
);
