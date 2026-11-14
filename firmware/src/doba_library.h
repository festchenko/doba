#pragma once

// DOBA: the offline library — Wikipedia and reference ZIMs converted on a
// computer (library/zim2doba.py) into a format the ESP32 reads with a few KB:
//
//   /doba/lib/<book>/info.txt     name=..., lang=...
//   /doba/lib/<book>/index.dti    sorted fixed-width title index (128 B records)
//   /doba/lib/<book>/aNNNN.dta    articles, each its own zlib stream
//
// Index header (32 B): "DOBAIX1\0", u32 count, u32 record size (128),
// u32 chunk count, 12 B reserved. Record: key[56] (folded title, see
// doba_fold), title[56] (display, UTF-8), u16 chunk, u16 flags, u32 offset,
// u32 length, u32 reserved. Article text, inflated, is UTF-8 lines whose first
// byte is the kind: T title, H section, S subsection, P paragraph, B bullet.
// P and B lines may carry links: \x01 visible text \x02 target title \x03.
// info.txt may name the source: source=Wikipedia EN · CC BY-SA.

#include <cstddef>
#include <cstdint>

#include "doba_keyboard.h"

namespace doba_lib {

int count();            // books found on the card (scanned once, then cached)
void rescan();

void renderSearch(kbd::Keyboard& k, uint8_t* fb);
bool tapSearch(kbd::Keyboard& k, int x, int y);   // true: an article was opened
void renderArticle(uint8_t* fb);
bool tapArticle(int x, int y);       // a link opens its article; else pages. true: CLOSE tapped
bool articleBack();                  // back along followed links; false at the first
void leaveArticle();
bool libraryBack();
bool typing();                       // the search keyboard is up (taps are keys: no "busy" mark)
// Open an article by its title, in the first book that has it (a console command for tests).
bool openTitle(const char* title);                  // section/search view -> library home; false at home
const char* libraryTitle();          // status-bar title for the current library view                 // forget the link history

// Title folding, identical to library/zim2doba.py fold(): lower case, accents
// and ё dropped, punctuation to single spaces. out gets at most cap-1 bytes.
void fold(const char* in, char* out, size_t cap);

}  // namespace doba_lib
