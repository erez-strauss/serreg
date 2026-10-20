# Third-party notices

es::serreg has no third-party code dependencies. This file credits the one algorithm it implements from another
project's description and constants.

## xxHash (XXH3-64)

`include/es/serreg/hash.hpp` contains an independent `constexpr` implementation of the XXH3-64 hash (seed 0), used
since library 2.1.0 / wire format 1.3 for every schema, layout, call, literal and program hash. It follows the
xxHash algorithm specification and uses its 192-byte default secret and prime constants. No xxHash source file is
vendored. Its known-answer tests (`tests/unit/test_hash.cpp`) are values produced by the xxHash 0.8.3 reference.

xxHash — Extremely Fast Hash algorithm, https://github.com/Cyan4973/xxHash

```
xxHash Library
Copyright (c) 2012-2021 Yann Collet
All rights reserved.

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Test-only

Catch2 v3 (Boost Software License 1.0) is fetched by CMake when the tests are built (`tests/CMakeLists.txt`, or a
local checkout through `ES_SERREG_CATCH2_DIR`). It is not part of the package.
