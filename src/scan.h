// Pattern scanning over the loaded client image.
//
// Everything here works on memory, never on the file on disk. That matters: some
// Ragnarok clients ship packed, and their own stub unpacks the real code before
// any of it runs. A packed client has no readable strings and no real code in the
// file, but by the time this DLL loads, memory holds the finished image.
#pragma once

#include <cstdint>
#include <cstddef>

namespace scan {

// Whole mapped image, including data sections.
extern uint8_t* image_begin;
extern uint8_t* image_end;

// Executable section only. Used when searching for code.
extern uint8_t* text_begin;
extern uint8_t* text_end;

// Fills the four pointers above from the client's PE headers.
void Init();

// Classic signature scan. '?' in the mask skips that byte.
uintptr_t FindPattern(const uint8_t* pattern, const char* mask,
                      uintptr_t base, uintptr_t size);

// Raw byte search, used mostly to find string literals.
uint8_t* FindBytes(uint8_t* begin, uint8_t* end, const void* needle, size_t len);

// Finds a C string including its terminator, so "foo" does not match "foobar".
uint8_t* FindString(const char* text);

// Destination of a 5-byte relative call or jump at p (E8/E9 rel32).
uintptr_t CallTarget(const uint8_t* p);

// Overwrites the rel32 of an existing 5-byte call so it lands on `dest`.
// Only the operand changes: one whole instruction becomes another of the same
// length. Never pad a larger block with nops instead - if any jump elsewhere in
// the function targets an address inside that block, execution slides through
// the padding and the client crashes somewhere unrelated.
bool RepointCall(uint8_t* call_site, void* dest);

} // namespace scan
