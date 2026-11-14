/**
 * sa2-nx (2026-10-20): a function the input system calls at every controller state read (InputSystem::GetState),
 * before the read: Renut's late input delays the frame's first read there and its latency probe notes the read
 * (rex::latency::BeforeInputRead / NoteInputRead, app/src/renut_engine/sa2_compat/frame_latency.cpp; banjo-nx calls
 * them from its SDK directly). None by default.
 */
#pragma once

#include <cstdint>

namespace rex::input {

using ReadHook = void (*)();
void SetReadHook(ReadHook hook);

// Dunwall (2026-11-09, input_hold_until_dlc_ready): the content code reports the game's DLC scan (the items of a
// XamContentCreateEnumerator of downloadable content) and each DLC package it opens (XamContentCreate); the input
// system holds the buttons back until the game has opened everything its scan found.
void NoteGameDlcEnumerated(uint32_t count);
void NoteGameDlcOpened();

}  // namespace rex::input
