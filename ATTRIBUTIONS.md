# Attributions

Rebate is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is PROVISIONAL: hand-written in the shape the `stoatworks-backend` sync
(`scripts/sync-attributions.py`) generates. Rebate is not yet registered in that
script's lists, so the sync cannot produce this file yet. Once the registration is
finished the sync overwrites this file; edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives through the vcpkg manifest on Windows only. Not fetched on macOS.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls — listed because it is present in the checkout. The harness writes PNGs through the system zlib and not through this.

## Within the fleet

Not third-party, but owed a line. The 5x7 bitmap font the edge print is drawn
from is **graticule**'s (`github.com/stoatworks-labs/graticule`, MIT, Stoatworks
Labs), copied unchanged. The harness shape, the `--pipe` contract, the verify
script and the negative-control pattern are **pitch**'s; the host clock-unit
voting is **readout**'s by way of pitch; `PassBuffer` is **tinsel**'s. The idea of
a subtractive model done in densities comes from **rosette**. The PCG output mix
used for the grain is the well-known `pcg_hash` construction, written out here
rather than copied from anyone's source.

## Science, not code

The model is built from textbook photographic science rather than from anyone's
implementation: the characteristic (H&D) curve and its toe, straight line and
shoulder; the colour negative's masking couplers and the orange base they make;
the Nutting/Poisson picture of a developed layer as randomly placed clouds, whose
covered fraction fluctuates binomially; C-41, E-6 and cross-processing; the
common 35 mm negative perforation dimensions. No stock's published curves or data
were used: the five stocks are invented and described by their parameters, and
no real manufacturer's name or mark appears in the plugin or its edge print.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
