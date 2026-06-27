## Eval benchmark results — 2026-06-27, Mac (dynamic)

Eval steady-state, median real time ns/call (lower is better); `×cel-cpp` > 1.0 means that comparator is faster than cel-cpp.  `n/a` = cell does not run on that comparator (see skip tags in the corpus YAML).

### arithmetic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| abcAbcShapeLit | `1 + 2 + 3 + 1 + 2 + 3` | 343 | 475 | 0.72× |
| abcAbcShapeVars | `a + b + c + a + b + c` | 352 | 520 | 0.68× |
| doubleAdd2 | `a + b` | 79 | 224 | 0.35× |
| doubleAdd2Const | `1.0 + 1.0` | 59 | 193 | 0.31× |
| doubleAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 381 | 938 | 0.41× |
| doubleAdd10TermsConst | `1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0` | 267 | 806 | 0.33× |
| doubleAdd50Terms | `a + b + … + i + j (50 terms)` | 1,644 | 4,011 | 0.41× |
| doubleAdd50TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (50 terms)` | 1,129 | 3,859 | 0.29× |
| doubleAdd250Terms | `a + b + … + i + j (250 terms)` | 8,618 | 19.6 µs | 0.44× |
| doubleAdd250TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (250 terms)` | 5,616 | 19.7 µs | 0.28× |
| doubleAdd1000Terms | `a + b + … + i + j (1000 terms)` | 47.2 µs | 76.5 µs | 0.62× |
| doubleAdd1000TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (1000 terms)` | 28.2 µs | 74.0 µs | 0.38× |
| doubleDiv_simple | `a / b` | 78 | 223 | 0.35× |
| doubleDiv_simpleConst | `3.0 / 2.0` | 57 | 193 | 0.30× |
| doubleMul_simple | `a * b` | 78 | 223 | 0.35× |
| doubleMul_simpleConst | `3.14 * 2.0` | 57 | 193 | 0.30× |
| doubleNeg | `-a` | 58 | 210 | 0.27× |
| doubleNegConst | `-3.14` | 35 | 110 | 0.32× |
| doubleSub_simple | `a - b` | 78 | 225 | 0.35× |
| doubleSub_simpleConst | `3.5 - 1.25` | 57 | 192 | 0.30× |
| intAdd2 | `a + b` | 74 | 222 | 0.33× |
| intAdd2Const | `1 + 1` | 57 | 192 | 0.29× |
| intAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 346 | 922 | 0.37× |
| intAdd10TermsConst | `1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1` | 243 | 806 | 0.30× |
| intAdd50Terms | `a + b + … + i + j (50 terms)` | 1,688 | 3,990 | 0.42× |
| intAdd50TermsConst | `1 + 1 + … + 1 + 1 (50 terms)` | 1,737 | 3,871 | 0.45× |
| intAdd250Terms | `a + b + … + i + j (250 terms)` | 8,162 | 19.5 µs | 0.42× |
| intAdd250TermsConst | `1 + 1 + … + 1 + 1 (250 terms)` | 5,680 | 19.6 µs | 0.29× |
| intAdd1000Terms | `a + b + … + i + j (1000 terms)` | 33.8 µs | 78.1 µs | 0.43× |
| intAdd1000TermsConst | `1 + 1 + … + 1 + 1 (1000 terms)` | 23.7 µs | 77.7 µs | 0.31× |
| intAddDeepTree | `((a + b) + (c + d)) + ((e + f) + (g + h))` | 568 | 752 | 0.75× |
| intDiv_simple | `a / b` | 161 | 223 | 0.72× |
| intDiv_simpleConst | `84 / 2` | 116 | 194 | 0.60× |
| intMixedOps3 | `(a + b) * c - d` | 322 | 406 | 0.79× |
| intMod_simple | `a % b` | 154 | 221 | 0.70× |
| intMod_simpleConst | `100 % 7` | 116 | 193 | 0.60× |
| intMul2 | `a * b` | 159 | 223 | 0.71× |
| intMul2Const | `1 * 1` | 126 | 196 | 0.65× |
| intMul10Terms | `a * b * c * d * e * f * g * h * i * j` | 730 | 940 | 0.78× |
| intMul10TermsConst | `1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1` | 475 | 820 | 0.58× |
| intMul50Terms | `a * b * … * i * j (50 terms)` | 3,356 | 4,027 | 0.83× |
| intMul50TermsConst | `1 * 1 * … * 1 * 1 (50 terms)` | 2,363 | 3,921 | 0.60× |
| intMul250Terms | `a * b * … * i * j (250 terms)` | 16.5 µs | 19.9 µs | 0.83× |
| intMul250TermsConst | `1 * 1 * … * 1 * 1 (250 terms)` | 10.6 µs | 19.9 µs | 0.53× |
| intMul1000Terms | `a * b * … * i * j (1000 terms)` | 65.1 µs | 78.0 µs | 0.83× |
| intMul1000TermsConst | `1 * 1 * … * 1 * 1 (1000 terms)` | 45.3 µs | 80.2 µs | 0.57× |
| intNeg | `-a` | 119 | 209 | 0.57× |
| intNegConst | `-42` | 73 | 110 | 0.67× |
| intSub2 | `a - b` | 172 | 222 | 0.78× |
| intSub2Const | `1 - 1` | 120 | 194 | 0.62× |
| intSub10Terms | `a - b - c - d - e - f - g - h - i - j` | 719 | 964 | 0.75× |
| intSub10TermsConst | `1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1` | 522 | 820 | 0.64× |
| intSub50Terms | `a - b - … - i - j (50 terms)` | 3,718 | 4,050 | 0.92× |
| intSub50TermsConst | `1 - 1 - … - 1 - 1 (50 terms)` | 2,417 | 3,956 | 0.61× |
| intSub250Terms | `a - b - … - i - j (250 terms)` | 18.6 µs | 19.9 µs | 0.93× |
| intSub250TermsConst | `1 - 1 - … - 1 - 1 (250 terms)` | 12.6 µs | 20.0 µs | 0.63× |
| intSub1000Terms | `a - b - … - i - j (1000 terms)` | 77.3 µs | 79.6 µs | 0.97× |
| intSub1000TermsConst | `1 - 1 - … - 1 - 1 (1000 terms)` | 53.2 µs | 80.5 µs | 0.66× |
| polyMix1000Terms | `a*d + b*a + … + i*j + j*g (1000 terms)` | 144.4 µs | 159.7 µs | 0.90× |
| uintAdd_simple | `a + b` | 170 | 224 | 0.76× |
| uintAdd_simpleConst | `1u + 2u` | 126 | 194 | 0.65× |
| uintDiv_simple | `a / b` | 176 | 223 | 0.79× |
| uintDiv_simpleConst | `6u / 2u` | 122 | 194 | 0.63× |
| uintMod_simple | `a % b` | 166 | 218 | 0.76× |
| uintMod_simpleConst | `7u % 3u` | 126 | 193 | 0.65× |
| uintMul_simple | `a * b` | 163 | 223 | 0.73× |
| uintMul_simpleConst | `6u * 7u` | 129 | 194 | 0.67× |
| uintSub_simple | `a - b` | 167 | 222 | 0.75× |
| uintSub_simpleConst | `3u - 1u` | 116 | 197 | 0.59× |

### comparisons

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| boolEq | `a == b` | 146 | 243 | 0.60× |
| boolEqConst | `true == true` | 99 | 213 | 0.46× |
| boolNe | `a != b` | 136 | 240 | 0.57× |
| boolNeConst | `true != false` | 97 | 213 | 0.46× |
| bytesEq | `a == b` | 154 | 363 | 0.43× |
| bytesEqConst | `b"a" == b"a"` | 104 | 215 | 0.48× |
| bytesGe | `a >= b` | 159 | 256 | 0.62× |
| bytesGeConst | `b"b" >= b"b"` | 119 | 196 | 0.61× |
| bytesGt | `a > b` | 150 | 246 | 0.61× |
| bytesGtConst | `b"b" > b"a"` | 112 | 193 | 0.58× |
| bytesLe | `a <= b` | 138 | 245 | 0.57× |
| bytesLeConst | `b"a" <= b"a"` | 102 | 198 | 0.52× |
| bytesLt | `a < b` | 127 | 280 | 0.45× |
| bytesLtConst | `b"a" < b"b"` | 93 | 195 | 0.48× |
| bytesNe | `a != b` | 98 | 263 | 0.37× |
| bytesNeConst | `b"a" != b"b"` | 66 | 215 | 0.31× |
| doubleEq | `a == b` | 88 | 240 | 0.37× |
| doubleEqConst | `1.5 == 1.5` | 63 | 215 | 0.29× |
| doubleGe | `a >= b` | 104 | 225 | 0.46× |
| doubleGeConst | `3.5 >= 3.5` | 76 | 192 | 0.40× |
| doubleGt | `a > b` | 99 | 225 | 0.44× |
| doubleGtConst | `3.5 > 2.5` | 71 | 193 | 0.37× |
| doubleLe | `a <= b` | 98 | 223 | 0.44× |
| doubleLeConst | `2.5 <= 2.5` | 74 | 199 | 0.37× |
| doubleLt | `a < b` | 98 | 226 | 0.43× |
| doubleLtConst | `1.5 < 2.5` | 73 | 195 | 0.37× |
| doubleNe | `a != b` | 83 | 244 | 0.34× |
| doubleNeConst | `1.5 != 2.5` | 60 | 214 | 0.28× |
| durEq | `duration("60s") == duration("1m")` | 158 | 505 | 0.31× |
| durGe | `duration("1m") >= duration("60s")` | 164 | 494 | 0.33× |
| durGt | `duration("2m") > duration("1m")` | 173 | 482 | 0.36× |
| durLe | `duration("1m") <= duration("60s")` | 168 | 490 | 0.34× |
| durLt | `duration("1m") < duration("2m")` | 162 | 491 | 0.33× |
| durNe | `duration("60s") != duration("2m")` | 145 | 515 | 0.28× |
| intEq | `a == b` | 76 | 251 | 0.30× |
| intEqConst | `42 == 42` | 54 | 226 | 0.24× |
| intGe | `a >= b` | 85 | 240 | 0.35× |
| intGeConst | `3 >= 3` | 65 | 190 | 0.34× |
| intGt | `a > b` | 87 | 241 | 0.36× |
| intGtConst | `3 > 2` | 63 | 199 | 0.31× |
| intLe | `a <= b` | 84 | 228 | 0.37× |
| intLeConst | `2 <= 2` | 59 | 201 | 0.29× |
| intLt | `a < b` | 80 | 231 | 0.35× |
| intLtChain20 | `a<b && b<c && … && s<t && t<u (20 terms)` | 1,219 | 3,413 | 0.36× |
| intLtChain20Const | `1<2 && 2<3 && … && 19<20 && 20<21 (20 terms)` | 828 | 3,132 | 0.26× |
| intLtConst | `1 < 2` | 59 | 201 | 0.29× |
| intLtDouble | `a < b` | 79 | n/a | n/a |
| intNe | `a != b` | 67 | 246 | 0.27× |
| intNeConst | `42 != 43` | 48 | 221 | 0.22× |
| listEq | `[1,2,3] == [1,2,3]` | 220 | 303 | 0.73× |
| listNe | `[1,2,3] != [1,2,4]` | 217 | 314 | 0.69× |
| mapEq | `{"a":1,"b":2} == {"b":2,"a":1}` | 395 | 1,273 | 0.31× |
| mapNe | `{"a":1} != {"a":2}` | 222 | 1,067 | 0.21× |
| nullEq | `null == null` | 48 | 219 | 0.22× |
| stringEq | `a == b` | 68 | 275 | 0.25× |
| stringEqConst | `"a" == "a"` | 48 | 223 | 0.22× |
| stringGe | `a >= b` | 83 | 252 | 0.33× |
| stringGeConst | `"b" >= "b"` | 61 | 206 | 0.30× |
| stringGt | `a > b` | 82 | 252 | 0.33× |
| stringGtConst | `"b" > "a"` | 61 | 198 | 0.31× |
| stringLe | `a <= b` | 82 | 255 | 0.32× |
| stringLeConst | `"a" <= "a"` | 62 | 199 | 0.31× |
| stringLt | `a < b` | 80 | 255 | 0.31× |
| stringLtConst | `"a" < "b"` | 62 | 198 | 0.31× |
| stringNe | `a != b` | 66 | 267 | 0.25× |
| stringNeConst | `"a" != "b"` | 49 | 216 | 0.23× |
| tsEq | `timestamp("2024-01-01T00:00:00Z") == timestamp("2024-01-01T…` | 299 | 922 | 0.32× |
| tsGe | `timestamp("2024-01-01T00:00:00Z") >= timestamp("2024-01-01T…` | 321 | 911 | 0.35× |
| tsGt | `timestamp("2024-01-02T00:00:00Z") > timestamp("2024-01-01T0…` | 322 | 949 | 0.34× |
| tsLe | `timestamp("2024-01-01T00:00:00Z") <= timestamp("2024-01-01T…` | 312 | 926 | 0.34× |
| tsLt | `timestamp("2024-01-01T00:00:00Z") < timestamp("2024-01-02T0…` | 323 | 913 | 0.35× |
| tsNe | `timestamp("2024-01-01T00:00:00Z") != timestamp("2024-01-02T…` | 305 | 938 | 0.33× |
| uintEq | `a == b` | 65 | 244 | 0.27× |
| uintEqConst | `42u == 42u` | 47 | 217 | 0.22× |
| uintGe | `a >= b` | 76 | 232 | 0.33× |
| uintGeConst | `3u >= 3u` | 54 | 196 | 0.28× |
| uintGt | `a > b` | 74 | 232 | 0.32× |
| uintGtConst | `3u > 2u` | 56 | 197 | 0.28× |
| uintLe | `a <= b` | 73 | 228 | 0.32× |
| uintLeConst | `2u <= 2u` | 57 | 202 | 0.28× |
| uintLt | `a < b` | 73 | 229 | 0.32× |
| uintLtConst | `1u < 2u` | 57 | 199 | 0.28× |
| uintNe | `a != b` | 64 | 246 | 0.26× |
| uintNeConst | `42u != 43u` | 46 | 215 | 0.21× |

### comprehensions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| all10 | `[1,2,3,4,5,6,7,8,9,10].all(x, x > 0)` | 830 | 2,641 | 0.31× |
| all20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].all(x,…` | 1,487 | 5,146 | 0.29× |
| all100 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 6,693 | 24.6 µs | 0.27× |
| all1000 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 66.3 µs | 247.3 µs | 0.27× |
| exists20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,237 | 5,354 | 0.23× |
| existsMapKey | `{1:"a",2:"b",3:"c"}.exists(k, k == 2)` | 322 | 1,844 | 0.17× |
| existsOne20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,161 | 5,904 | 0.20× |
| filter20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].f…` | 3,308 | 6,338 | 0.52× |
| map20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].m…` | 5,412 | 3,601 | 1.50× |
| mapLookupLoop64 | `[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22…` | 178.2 µs | 3.69 ms | 0.05× |
| mapLookupLoop256 | `[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22…` | 2.69 ms | 209.29 ms | 0.01× |

### conversions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesFromString | `bytes(s)` | 71 | 222 | 0.32× |
| bytesFromStringConst | `bytes("abc")` | 63 | 199 | 0.31× |
| doubleFromInt | `double(i)` | 56 | 209 | 0.27× |
| doubleFromIntConst | `double(42)` | 49 | 193 | 0.25× |
| doubleFromString | `double(s)` | 83 | 237 | 0.35× |
| doubleFromStringConst | `double("42.5")` | 73 | 212 | 0.35× |
| doubleFromUint | `double(u)` | 60 | 209 | 0.29× |
| doubleFromUintConst | `double(42u)` | 52 | 193 | 0.27× |
| durationRoundTrip | `string(duration(s))` | 129 | 396 | 0.32× |
| intFromDouble | `int(d)` | 67 | 210 | 0.32× |
| intFromDoubleConst | `int(42.9)` | 57 | 194 | 0.29× |
| intFromString | `int(s)` | 69 | 221 | 0.31× |
| intFromStringConst | `int("42")` | 61 | 194 | 0.31× |
| intFromStringNested | `int(string(123))` | 90 | 294 | 0.31× |
| intFromTimestamp | `int(timestamp("2024-01-01T00:00:00Z"))` | 178 | 541 | 0.33× |
| intFromUint | `int(u)` | 68 | 209 | 0.32× |
| intFromUintConst | `int(42u)` | 57 | 192 | 0.29× |
| stringFromBool | `string(x)` | 69 | 221 | 0.31× |
| stringFromBoolConst | `string(true)` | 60 | 208 | 0.29× |
| stringFromBytes | `string(x)` | 76 | 223 | 0.34× |
| stringFromBytesConst | `string(b"abc")` | 64 | 199 | 0.32× |
| stringFromDouble | `string(d)` | 111 | 375 | 0.30× |
| stringFromDoubleConst | `string(42.5)` | 101 | 362 | 0.28× |
| stringFromDuration | `string(duration("90s"))` | 124 | 373 | 0.33× |
| stringFromInt | `string(i)` | 74 | 228 | 0.33× |
| stringFromIntConst | `string(42)` | 64 | 216 | 0.29× |
| stringFromTimestamp | `string(timestamp("2024-01-01T00:00:00Z"))` | 344 | 877 | 0.39× |
| stringFromUint | `string(u)` | 73 | 228 | 0.32× |
| stringFromUintConst | `string(42u)` | 65 | 216 | 0.30× |
| timestampRoundTrip | `string(timestamp(s))` | 376 | 919 | 0.41× |
| typeOfInt | `type(i) == int` | 90 | 317 | 0.28× |
| typeOfString | `type(s) == string` | 88 | 329 | 0.27× |
| uintFromDouble | `uint(d)` | 67 | 210 | 0.32× |
| uintFromDoubleConst | `uint(42.9)` | 58 | 193 | 0.30× |
| uintFromInt | `uint(i)` | 66 | 215 | 0.31× |
| uintFromIntConst | `uint(42)` | 57 | 205 | 0.28× |
| uintFromString | `uint(s)` | 70 | 224 | 0.31× |
| uintFromStringConst | `uint("42")` | 60 | 192 | 0.31× |

### index

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| listInt | `[10,20,30,40,50][i]` | 149 | 226 | 0.66× |
| listIntConst | `[10,20,30,40,50][4]` | 129 | 213 | 0.61× |
| mapBool | `{true:1,false:0}[k]` | 154 | 616 | 0.25× |
| mapBoolConst | `{true:1,false:0}[true]` | 140 | 596 | 0.24× |
| mapInt | `{1:10,2:20,3:30}[k]` | 190 | 826 | 0.23× |
| mapIntConst | `{1:10,2:20,3:30}[3]` | 169 | 802 | 0.21× |
| mapIntN8 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70}[k]` | 342 | 2,082 | 0.16× |
| mapIntN64 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 2,929 | 59.5 µs | 0.05× |
| mapIntN256 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 11.8 µs | 868.8 µs | 0.01× |
| mapStrN8 | `{"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40,"k00…` | 438 | 1,860 | 0.24× |
| mapStrN64 | `{"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40,"k00…` | 3,253 | 36.6 µs | 0.09× |
| mapStrN256 | `{"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40,"k00…` | 19.5 µs | 480.0 µs | 0.04× |
| mapString | `{"a":1,"b":2,"c":3}[k]` | 201 | 783 | 0.26× |
| mapStringConst | `{"a":1,"b":2,"c":3}["c"]` | 193 | 736 | 0.26× |
| mapUint | `{1u:10,2u:20,3u:30}[k]` | 171 | 827 | 0.21× |
| mapUintConst | `{1u:10,2u:20,3u:30}[3u]` | 162 | 807 | 0.20× |

### lists

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| 5 | `a in ["123","augustine","jess","bob","alice"]` | 161 | 253 | 0.64× |
| 5_lit | `"alice" in ["123","augustine","jess","bob","alice"]` | 151 | 227 | 0.67× |
| 20 | `a in ["alice","bob","carol","dave","eve","frank","grace","h…` | 361 | 265 | 1.36× |
| 20_lit | `"tom" in ["alice","bob","carol","dave","eve","frank","grace…` | 353 | 241 | 1.47× |
| 100 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 1,404 | 353 | 3.98× |
| 100_lit | `"tom4" in ["alice0","bob0","carol0","dave0","eve0","frank0"…` | 1,344 | 319 | 4.21× |
| 100_lit_first | `"alice0" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 935 | 227 | 4.12× |
| 100_lit_miss | `"nobody" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 1,364 | 311 | 4.39× |
| 1000 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 13.4 µs | 1,265 | 10.57× |
| 1000_lit | `"tom49" in ["alice0","bob0","carol0","dave0","eve0","frank0…` | 13.3 µs | 1,101 | 12.12× |
| bool20 | `x in [false,false,false,false,false,false,false,false,false…` | 339 | 246 | 1.38× |
| bound100 | `x in xs` | 421 | 517 | 0.81× |
| bound1000 | `x in xs` | 3,456 | 2,749 | 1.26× |
| bound10000 | `x in xs` | 35.6 µs | 26.0 µs | 1.37× |
| bound100000 | `x in xs` | 351.6 µs | 262.0 µs | 1.34× |
| bound1000000 | `x in xs` | 3.45 ms | 2.71 ms | 1.27× |
| bound1000000_first | `x in xs` | 77 | 993.0 µs | 0.00× |
| bound1000000_miss | `x in xs` | 3.55 ms | 2.65 ms | 1.34× |
| concat | `size([1,2,3] + [4,5])` | 288 | 315 | 0.92× |
| double20 | `x in [1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,10.0,11.0,12.0,13…` | 351 | 244 | 1.44× |
| iam100 | `perm in perms` | 633 | 799 | 0.79× |
| iam1000 | `perm in perms` | 5,893 | 5,674 | 1.04× |
| iam1000_first | `perm in perms` | 77 | 1,274 | 0.06× |
| iam1000_miss | `perm in perms` | 5,645 | 5,542 | 1.02× |
| int20 | `x in [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]` | 345 | 244 | 1.42× |
| uint20 | `x in [1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,16…` | 340 | 244 | 1.39× |

### literals

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bool | `true` | 33 | 110 | 0.30× |
| double | `3.14` | 34 | 110 | 0.31× |
| int | `42` | 35 | 112 | 0.31× |
| null | `null` | 33 | 110 | 0.30× |
| string | `"hello"` | 34 | 116 | 0.29× |

### logic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| and2 | `a && b` | 63 | 224 | 0.28× |
| and2Const | `true && true` | 43 | 196 | 0.22× |
| and10Terms | `a && b && c && d && e && f && g && h && i && j` | 263 | 962 | 0.27× |
| andNoShortCircuit | `a && s.contains("yyy")` | 93 | 325 | 0.29× |
| andShortCircuit | `a && s.contains("yyy")` | 49 | 325 | 0.15× |
| not1 | `!a` | 44 | 221 | 0.20× |
| not1Const | `!false` | 35 | 206 | 0.17× |
| not3 | `!!!a` | 44 | 221 | 0.20× |
| not3Const | `!!!false` | 36 | 205 | 0.18× |
| or2 | `a \|\| b` | 61 | 224 | 0.27× |
| or2Const | `false \|\| true` | 45 | 192 | 0.23× |
| or10Terms | `a \|\| b \|\| c \|\| d \|\| e \|\| f \|\| g \|\| h \|\| i \|\| j` | 256 | 955 | 0.27× |
| orNoShortCircuit | `a \|\| s.contains("yyy")` | 94 | 326 | 0.29× |
| orShortCircuit | `a \|\| s.contains("yyy")` | 49 | 326 | 0.15× |

### long_strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| containsLong_N10 | `a.contains("yyy")` | 66 | 225 | 0.29× |
| containsLong_N100 | `a.contains("yyy")` | 66 | 236 | 0.28× |
| containsLong_N1000 | `a.contains("yyy")` | 82 | 361 | 0.23× |
| containsLong_N10000 | `a.contains("yyy")` | 244 | 1,553 | 0.16× |
| eqLong_N10_match | `a == "xxxxxxxxxx"` | 58 | 234 | 0.25× |
| eqLong_N10_mismatch | `a == "xxxxxxxxxx"` | 57 | 236 | 0.24× |
| eqLong_N100_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 60 | 242 | 0.25× |
| eqLong_N100_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 60 | 241 | 0.25× |
| eqLong_N100_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 58 | 239 | 0.24× |
| eqLong_N1000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 74 | 310 | 0.24× |
| eqLong_N1000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 73 | 312 | 0.24× |
| eqLong_N1000_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 59 | 245 | 0.24× |
| eqLong_N1000_mismatch_len | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 59 | 244 | 0.24× |
| eqLong_N10000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 242 | 945 | 0.26× |
| eqLong_N10000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 246 | 945 | 0.26× |

### maps

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| dotField | `{"k":1}.k` | 113 | n/a | n/a |
| eqIntN64 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 7,110 | 112.2 µs | 0.06× |
| eqIntN256 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 32.1 µs | 1.58 ms | 0.02× |
| hasKey | `has({"a":1}.a)` | 100 | n/a | n/a |
| inBool | `k in {true:1,false:0}` | 156 | 624 | 0.25× |
| inBoolConst | `true in {true:1,false:0}` | 145 | 600 | 0.24× |
| inInt | `k in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 415 | 2,741 | 0.15× |
| inIntConst | `10 in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 362 | 2,723 | 0.13× |
| inIntN8 | `k in {0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70}` | 338 | 2,067 | 0.16× |
| inIntN64 | `k in {0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:1…` | 2,649 | 54.9 µs | 0.05× |
| inIntN256 | `k in {0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:1…` | 13.6 µs | 789.0 µs | 0.02× |
| inStrN8 | `k in {"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40…` | 397 | 1,847 | 0.21× |
| inStrN64 | `k in {"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40…` | 3,061 | 35.3 µs | 0.09× |
| inStrN256 | `k in {"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40…` | 21.4 µs | 451.8 µs | 0.05× |
| inString | `k in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i":9…` | 446 | 2,193 | 0.20× |
| inStringConst | `"j" in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i"…` | 413 | 2,170 | 0.19× |
| inUint | `k in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 372 | 2,759 | 0.13× |
| inUintConst | `10u in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 357 | 2,741 | 0.13× |

### policies

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| arena_map_gate | `c.age >= {"us": 21, "de": 18}["us"]` | 222 | 721 | 0.31× |
| authz_basic | `(c.is_premium && c.age >= 18 && c.name in ["Ada", "Grace", …` | 285 | 752 | 0.38× |
| authz_deep | `(m.inner.b && m.inner.i64 >= 18 && m.inner.inner.s in ["Ada…` | 447 | 887 | 0.50× |
| authz_deep8 | `(m.inner.inner.inner.inner.inner.inner.inner.b && m.inner.i…` | 1,080 | 1,355 | 0.80× |
| mega100 | `(m.i64 + m.str_to_i32["q1"] + m.rep_i32[1]) == -1 ? "deny" …` | 9,417 | 19.9 µs | 0.47× |
| premium_gate | `c.is_premium ? c.age : 0` | 115 | 262 | 0.44× |
| quota_check | `m.str_to_i32["used"] + m.str_to_i32["pending"] < m.str_to_i…` | 326 | 928 | 0.35× |
| risk_score | `c.credit_score >= 700.0 && c.balance_cents > 1000u` | 166 | 430 | 0.39× |
| str_in_list | `m.s in ["alpha", "beta", "gamma", "delta"]` | 187 | 373 | 0.50× |
| ternary2 | `c.age > 30 ? (c.is_premium ? "gold" : "silver") : "basic"` | 155 | 433 | 0.36× |
| ternary5 | `c.age > 60 ? "a" : c.age > 50 ? "b" : c.age > 40 ? "c" : c.…` | 312 | 872 | 0.36× |
| tier_route | `c.balance_cents >= 100000u ? "platinum" : (c.is_premium ? "…` | 251 | 915 | 0.27× |

### proto

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| construct_name | `celwasm.testdata.Customer{name: "Ada"}.name` | 162 | 350 | 0.46× |
| cust_age | `c.age` | 65 | 152 | 0.43× |
| cust_is_premium | `c.is_premium` | 65 | 152 | 0.43× |
| cust_name | `c.name` | 76 | 264 | 0.29× |
| map_i64_str | `m.i64_to_str[2]` | 105 | 450 | 0.23× |
| map_str_i32 | `m.str_to_i32["b"]` | 107 | 347 | 0.31× |
| map_str_msg_i64 | `m.str_to_msg["k"].i64` | 150 | 380 | 0.39× |
| metadata_b | `c.metadata["b"]` | 108 | 455 | 0.24× |
| pair_list_arena | `[10, 20, 30, 40, 50][2]` | 130 | 208 | 0.62× |
| pair_map_arena | `{"a": 1, "b": 2, "c": 3}["b"]` | 187 | 715 | 0.26× |
| read_b | `m.b` | 67 | 151 | 0.44× |
| read_f64 | `m.f64` | 67 | 152 | 0.44× |
| read_s | `m.s` | 78 | 267 | 0.29× |
| read_u64 | `m.u64` | 66 | 151 | 0.44× |
| reads5 | `m.i32 + m.i64 + m.si32 + m.si64 + m.sfx32` | 338 | 569 | 0.59× |
| reads10 | `m.i32 + m.i64 + … + m.si32 + m.si64 (10 terms)` | 671 | 1,078 | 0.62× |
| reads100 | `m.i32 + m.i64 + … + m.si32 + m.si64 (100 terms)` | 6,526 | 10.5 µs | 0.62× |
| rep_i32_at0 | `m.rep_i32[0]` | 100 | 290 | 0.34× |
| rep_i32_at9 | `m.rep_i32[9]` | 101 | 291 | 0.35× |
| rep_msg_at1_s | `m.rep_msg[1].s` | 156 | 469 | 0.33× |
| select_depth1 | `m.i64` | 68 | 152 | 0.45× |
| select_depth2 | `m.inner.i64` | 102 | 198 | 0.52× |
| select_depth4 | `m.inner.inner.inner.i64` | 183 | 277 | 0.66× |
| select_depth8 | `m.inner.inner.inner.inner.inner.inner.inner.i64` | 320 | 435 | 0.74× |
| select_depth16 | `m.inner.inner.inner.inner.inner.inner.inner.inner.inner.inn…` | 593 | 747 | 0.79× |
| tags_at2 | `c.tags[2]` | 111 | 403 | 0.28× |

### size

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytes | `size(x)` | 58 | 217 | 0.27× |
| bytesConst | `size(b"0123456789abcdef")` | 54 | 191 | 0.28× |
| list10 | `size([1,2,3,4,5,6,7,8,9,10])` | 213 | 207 | 1.03× |
| list100 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 1,027 | 207 | 4.97× |
| list1000 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 8,968 | 208 | 43.21× |
| map10 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10})` | 382 | 2,670 | 0.14× |
| map100 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10,11:11,12:12…` | 3,183 | 124.4 µs | 0.03× |
| string | `size(s)` | 68 | 229 | 0.30× |
| stringConst | `size("abcdefghijklmnopqrstuvwxyz")` | 66 | 200 | 0.33× |

### strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesConcat2 | `a + b` | 106 | 270 | 0.39× |
| bytesConcat2Const | `b"ab" + b"cd"` | 87 | 221 | 0.40× |
| concat2 | `a + b` | 109 | 274 | 0.40× |
| concat2Const | `"hello " + "world"` | 86 | 224 | 0.38× |
| concatChain10Terms | `a + b + c + d + e + f + g + h + i + j` | 542 | 1,334 | 0.41× |
| concatChain100Terms | `a + b + … + i + j (100 terms)` | 6,804 | 11.6 µs | 0.59× |
| concatChain1000Terms | `a + b + … + i + j (1000 terms)` | 140.8 µs | 130.1 µs | 1.08× |
| contains | `a.contains("aug")` | 66 | 223 | 0.30× |
| containsConst | `"augustine".contains("aug")` | 56 | 198 | 0.29× |
| endsWith | `a.endsWith("ine")` | 67 | 219 | 0.31× |
| endsWithConst | `"augustine".endsWith("ine")` | 56 | 194 | 0.29× |
| eqConst | `"hello" == "world"` | 48 | 213 | 0.23× |
| eqVar | `a == "augustine"` | 58 | 236 | 0.25× |
| matchesCheap | `a.matches("^aug")` | 1,462 | 248 | 5.90× |
| matchesCheapConst | `"augustine".matches("^aug")` | 1,487 | 226 | 6.59× |
| matchesComplex | `a.matches("^[a-z]+-[0-9]{2,4}@[a-z]+\\.(com\|org)$")` | 9,085 | 324 | 28.01× |
| matchesComplexConst | `"user-1234@example.com".matches("^[a-z]+-[0-9]{2,4}@[a-z]+\…` | 9,473 | 298 | 31.76× |
| startsWith | `a.startsWith("aug")` | 65 | 222 | 0.29× |
| startsWithConst | `"augustine".startsWith("aug")` | 55 | 196 | 0.28× |

### ternary

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| intComputedCond | `a > b ? x : y` | 100 | 326 | 0.31× |
| intConst | `1 > 2 ? 10 : 20` | 71 | 271 | 0.26× |
| intVarCond | `c ? x : y` | 64 | n/a | n/a |
| nested3 | `a > b ? "gt" : (a == b ? "eq" : "lt")` | 129 | 485 | 0.27× |
| stringComputedCond | `a > b ? s : t` | 98 | 347 | 0.28× |
| stringConst | `3 > 2 ? "yes" : "no"` | 71 | 291 | 0.24× |

### time

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| durAddDur | `(duration("90s") + duration("30s")).getSeconds()` | 150 | 585 | 0.26× |
| durAddTs | `int(duration("1h") + timestamp("2024-01-01T00:00:00Z"))` | 234 | 805 | 0.29× |
| durGetHours | `duration("3723s").getHours()` | 85 | 330 | 0.26× |
| durGetSeconds | `duration("3723s").getSeconds()` | 85 | 325 | 0.26× |
| durSubDur | `(duration("90s") - duration("30s")).getSeconds()` | 149 | 586 | 0.25× |
| tsAddDur | `int(timestamp("2024-01-01T00:00:00Z") + duration("1h"))` | 233 | 798 | 0.29× |
| tsGetDayOfWeekUtc | `timestamp("2024-06-15T10:30:45Z").getDayOfWeek()` | 204 | 566 | 0.36× |
| tsGetFullYearTz | `timestamp("2024-06-15T10:30:45Z").getFullYear("America/New_…` | 222 | 624 | 0.36× |
| tsGetFullYearUtc | `timestamp("2024-06-15T10:30:45Z").getFullYear()` | 198 | 571 | 0.35× |
| tsGetFullYearUtcMax | `timestamp("9999-12-31T23:59:59Z").getFullYear()` | 223 | 576 | 0.39× |
| tsGetHoursTz | `timestamp("2024-06-15T10:30:45Z").getHours("America/New_Yor…` | 221 | 619 | 0.36× |
| tsGetHoursUtc | `timestamp("2024-06-15T10:30:45Z").getHours()` | 196 | 566 | 0.35× |
| tsGetSecondsTz | `timestamp("2024-06-15T10:30:45Z").getSeconds("America/New_Y…` | 224 | 633 | 0.35× |
| tsGetSecondsUtc | `timestamp("2024-06-15T10:30:45Z").getSeconds()` | 197 | 574 | 0.34× |
| tsSubDur | `int(timestamp("2024-01-01T01:00:00Z") - duration("1h"))` | 234 | 801 | 0.29× |
| tsSubTs | `(timestamp("2024-01-01T01:00:00Z") - timestamp("2024-01-01T…` | 335 | 2,723 | 0.12× |

### Per-operator headline — T(N) = setup + N·per_op

Linear regression over each length-sweep family; slope is the steady-state cost of one more operation, crossover is the expression length where the comparator overtakes cel-cpp.

| surface | operator family | points | cel-cpp slope | cel-cpp setup | celwasm-dynamic slope | celwasm-dynamic setup | celwasm-dynamic crossover vs cel-cpp |
|---|---|---|---|---|---|---|---|
| arithmetic | doubleAdd | 5 | 47.5 | -879 | 76.4 | 220 | never wins |
| arithmetic | intAdd | 5 | 33.8 | -54 | 78.0 | 72 | never wins |
| arithmetic | intMul | 5 | 65.0 | 104 | 77.9 | 187 | never wins |
| arithmetic | intSub | 5 | 77.4 | -195 | 79.5 | 93 | never wins |
| comprehensions | all | 4 | 66.1 | 140 | 247.2 | 100 | never wins |
| index | mapIntN | 3 | 46.3 | -30 | 3,680.0 | -92,249 | never wins |
| index | mapStrN | 3 | 78.8 | -891 | 2,026.2 | -48,697 | never wins |
| lists | bound | 5 | 3.4 | 1,861 | 2.7 | -2,408 | always wins |
| long_strings | containsLong_N | 4 | 0.0 | 64 | 0.1 | 225 | never wins |
| maps | inIntN | 3 | 54.5 | -422 | 3,340.6 | -83,242 | never wins |
| maps | inStrN | 3 | 87.4 | -1,275 | 1,905.6 | -45,349 | never wins |
| proto | reads | 3 | 65.1 | 16 | 104.6 | 39 | never wins |
| proto | select_depth | 5 | 35.0 | 36 | 39.5 | 117 | never wins |
| size | list | 3 | 8.8 | 134 | 0.0 | 207 | N ≈ 8 |
| strings | concatChain | 3 | 144.6 | -4,117 | 130.8 | -705 | N ≈ 247 |

