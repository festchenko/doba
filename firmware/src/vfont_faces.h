#pragma once
// The named faces the UI draws with. Default: the epdiy bitmap fonts. Built
// with -DVFONT, every name resolves to a vector face (see vfont.h) at the
// same digit / cap height as the bitmap it replaces, so layouts are
// unchanged and only the outlines differ. Values: Anton (Impact re-cut);
// labels: Arimo 700 (Arial-metric). Heights are epdc_digit_height() of the
// bitmap faces as measured on the host.
#ifdef VFONT
#include "vfont.h"
// DOBA: one monospaced family everywhere — JetBrains Mono (OFL), 800 for
// values, 700 for labels — the tactical look: digits keep their columns as they
// tick, and every screen reads like the map's coordinate strip.
#include "fonts/vf/jbmono_800.h"
#include "fonts/vf/jbmono_700.h"
// DOBA: words in Inter (OFL, Latin + Cyrillic), numbers stay JetBrains Mono —
// the calm look from the design canvas: proportional text, columns for digits.
#include "fonts/vf/inter_500.h"
#include "fonts/vf/inter_700.h"
#define VF_VALUES (&JBMono_800)
#define VF_MONO   (&JBMono_700)
#define VF_LABELS (&Inter_500)
#define VF_STRONG (&Inter_700)
#define Impact_S     (*vf_face_digit(VF_VALUES, 20))
#define Impact_T     (*vf_face_digit(VF_VALUES, 30))
#define Impact_V     (*vf_face_digit(VF_VALUES, 46))
#define Impact_A     (*vf_face_digit(VF_VALUES, 58))
#define Impact_M     (*vf_face_digit(VF_VALUES, 69))
#define Impact_40    (*vf_face_digit(VF_VALUES, 68))
#define Impact_B     (*vf_face_digit(VF_VALUES, 81))
#define Impact_H     (*vf_face_digit(VF_VALUES, 95))
#define Impact_C     (*vf_face_digit(VF_VALUES, 120))
#define Impact_XL    (*vf_face_digit(VF_VALUES, 158))
#define Impact_128   (*vf_face_digit(VF_VALUES, 219))
#define Arial_L      (*vf_face_digit(VF_LABELS, 11))
#define Arial_B      (*vf_face_digit(VF_LABELS, 13))
#define ArialBold_14 (*vf_face_digit(VF_STRONG, 16))
#define ArialBold_20 (*vf_face_digit(VF_STRONG, 22))
#else
#include "fonts/arial_l.h"
#include "fonts/arial_b.h"
#include "fonts/impact_s.h"
#include "fonts/impact_t.h"
#include "fonts/impact_m.h"
#include "fonts/impact_v.h"
#include "fonts/impact_h.h"
#include "fonts/impact_a.h"
#include "fonts/impact_b.h"
#include "fonts/impact_c.h"
#include "fonts/impact_xl.h"
#include "fonts/arialbold_14.h"
#include "fonts/arialbold_20.h"
#include "fonts/impact_40.h"
#include "fonts/impact_128.h"
#endif
