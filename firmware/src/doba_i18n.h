#pragma once
// DOBA: the interface in English or Ukrainian.
//
// Strings stay in English in the code; TR("...") gives the Ukrainian when
// that is the language and the table (tools/i18n/uk.tsv -> doba_i18n_uk.inc)
// has it, else the English. Formats keep their % specifiers in the same
// order. TRN(n, one, many) picks the plural: English one/many, Ukrainian
// one/few/many from the table entry "one|few|many".
#include <cstddef>

namespace i18n {
enum Lang : unsigned char { EN = 0, UK = 1 };
Lang lang();
void setLang(Lang l);   // kept in NVS
void load();            // at boot
const char* tr(const char* en);
const char* trn(long n, const char* one, const char* many);
}  // namespace i18n

#define TR(s) ::i18n::tr(s)
#define TRN(n, one, many) ::i18n::trn((long)(n), one, many)
