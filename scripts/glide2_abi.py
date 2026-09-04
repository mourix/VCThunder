# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""glide2_abi.py -- a tripwire: Glide 2 argument sizes, asserted and not consulted.

This table covers the entries whose call-site ``add esp, N`` cleanup is folded
or absent. It SUPPLIES no value: ``scripts/glide2_pushes.py`` derives those
sizes from the games' own push sequences, and ``gen-gamedefs.py`` asserts the
result against every row below, reporting a conflict rather than preferring
either. The two agree on all 16 rows in Hydro and all 13 present in Offroad.

Keep it. A silent disagreement between two derivations is exactly the class of
error that looks like a result, and a table that is only ever compared against
costs nothing to carry.
"""

FALLBACK_ARG_BYTES = {
    "grAADrawLine": 8,
    "grDisableAllEffects": 0,
    "grGlideInit": 0,
    "grGlideShutdown": 0,
    "grLfbConstantDepth": 4,
    "grLfbWriteColorFormat": 4,
    "grSstIdle": 0,
    "grSstStatus": 0,
    "grSstWinClose": 0,
    "grTexDownloadMipMapLevelPartial": 40,
    "grTexMaxAddress": 4,
    "grTexMinAddress": 4,
    "grTexNCCTable": 8,
    "grTexSource": 16,
    "grTexTextureMemRequired": 8,
    "guTexMemReset": 0,
}
