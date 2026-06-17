## Eval benchmark results — 2026-06-17, Mac (dynamic)

Eval steady-state, median real time ns/call (lower is better); `×cel-cpp` > 1.0 means that comparator is faster than cel-cpp.  `n/a` = cell does not run on that comparator (see skip tags in the corpus YAML).

### arithmetic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| abcAbcShapeLit | `1 + 2 + 3 + 1 + 2 + 3` | 155 | 551 | 0.28× |
| abcAbcShapeVars | `a + b + c + a + b + c` | 209 | 610 | 0.34× |
| doubleAdd2 | `a + b` | 72 | 240 | 0.30× |
| doubleAdd2Const | `1.0 + 1.0` | 54 | 200 | 0.27× |
| doubleAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 320 | 1,061 | 0.30× |
| doubleAdd10TermsConst | `1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0` | 227 | 911 | 0.25× |
| doubleAdd50Terms | `a + b + … + i + j (50 terms)` | 1,506 | 4,057 | 0.37× |
| doubleAdd50TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (50 terms)` | 1,031 | 3,936 | 0.26× |
| doubleAdd250Terms | `a + b + … + i + j (250 terms)` | 7,470 | 21.6 µs | 0.35× |
| doubleAdd250TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (250 terms)` | 5,096 | 21.7 µs | 0.23× |
| doubleAdd1000Terms | `a + b + … + i + j (1000 terms)` | 33.0 µs | 89.1 µs | 0.37× |
| doubleAdd1000TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (1000 terms)` | 21.5 µs | 86.5 µs | 0.25× |
| doubleDiv_simple | `a / b` | 74 | 224 | 0.33× |
| doubleDiv_simpleConst | `3.0 / 2.0` | 54 | 197 | 0.27× |
| doubleMul_simple | `a * b` | 74 | 226 | 0.33× |
| doubleMul_simpleConst | `3.14 * 2.0` | 54 | 197 | 0.27× |
| doubleNeg | `-a` | 55 | 207 | 0.27× |
| doubleNegConst | `-3.14` | 33 | 109 | 0.30× |
| doubleSub_simple | `a - b` | 75 | 224 | 0.33× |
| doubleSub_simpleConst | `3.5 - 1.25` | 55 | 195 | 0.28× |
| intAdd2 | `a + b` | 72 | 226 | 0.32× |
| intAdd2Const | `1 + 1` | 54 | 196 | 0.28× |
| intAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 327 | 946 | 0.35× |
| intAdd10TermsConst | `1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1` | 235 | 816 | 0.29× |
| intAdd50Terms | `a + b + … + i + j (50 terms)` | 1,558 | 4,006 | 0.39× |
| intAdd50TermsConst | `1 + 1 + … + 1 + 1 (50 terms)` | 1,089 | 3,865 | 0.28× |
| intAdd250Terms | `a + b + … + i + j (250 terms)` | 7,781 | 19.7 µs | 0.39× |
| intAdd250TermsConst | `1 + 1 + … + 1 + 1 (250 terms)` | 5,375 | 19.3 µs | 0.28× |
| intAdd1000Terms | `a + b + … + i + j (1000 terms)` | 32.2 µs | 78.0 µs | 0.41× |
| intAdd1000TermsConst | `1 + 1 + … + 1 + 1 (1000 terms)` | 22.5 µs | 75.6 µs | 0.30× |
| intAddDeepTree | `((a + b) + (c + d)) + ((e + f) + (g + h))` | 301 | 786 | 0.38× |
| intDiv_simple | `a / b` | 77 | 225 | 0.34× |
| intDiv_simpleConst | `84 / 2` | 56 | 197 | 0.29× |
| intMixedOps3 | `(a + b) * c - d` | 148 | 412 | 0.36× |
| intMod_simple | `a % b` | 77 | 224 | 0.35× |
| intMod_simpleConst | `100 % 7` | 56 | 196 | 0.29× |
| intMul2 | `a * b` | 77 | 227 | 0.34× |
| intMul2Const | `1 * 1` | 55 | 197 | 0.28× |
| intMul10Terms | `a * b * c * d * e * f * g * h * i * j` | 335 | 954 | 0.35× |
| intMul10TermsConst | `1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1` | 221 | 822 | 0.27× |
| intMul50Terms | `a * b * … * i * j (50 terms)` | 1,536 | 4,080 | 0.38× |
| intMul50TermsConst | `1 * 1 * … * 1 * 1 (50 terms)` | 982 | 3,895 | 0.25× |
| intMul250Terms | `a * b * … * i * j (250 terms)` | 7,499 | 20.2 µs | 0.37× |
| intMul250TermsConst | `1 * 1 * … * 1 * 1 (250 terms)` | 4,765 | 19.8 µs | 0.24× |
| intMul1000Terms | `a * b * … * i * j (1000 terms)` | 30.9 µs | 80.3 µs | 0.38× |
| intMul1000TermsConst | `1 * 1 * … * 1 * 1 (1000 terms)` | 20.3 µs | 78.1 µs | 0.26× |
| intNeg | `-a` | 60 | 208 | 0.29× |
| intNegConst | `-42` | 34 | 110 | 0.31× |
| intSub2 | `a - b` | 73 | 226 | 0.33× |
| intSub2Const | `1 - 1` | 55 | 198 | 0.28× |
| intSub10Terms | `a - b - c - d - e - f - g - h - i - j` | 327 | 968 | 0.34× |
| intSub10TermsConst | `1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1` | 229 | 837 | 0.27× |
| intSub50Terms | `a - b - … - i - j (50 terms)` | 1,537 | 4,103 | 0.37× |
| intSub50TermsConst | `1 - 1 - … - 1 - 1 (50 terms)` | 1,044 | 3,985 | 0.26× |
| intSub250Terms | `a - b - … - i - j (250 terms)` | 7,554 | 20.4 µs | 0.37× |
| intSub250TermsConst | `1 - 1 - … - 1 - 1 (250 terms)` | 5,048 | 20.2 µs | 0.25× |
| intSub1000Terms | `a - b - … - i - j (1000 terms)` | 31.2 µs | 81.4 µs | 0.38× |
| intSub1000TermsConst | `1 - 1 - … - 1 - 1 (1000 terms)` | 21.3 µs | 78.6 µs | 0.27× |
| polyMix1000Terms | `a*d + b*a + … + i*j + j*g (1000 terms)` | 63.5 µs | 157.8 µs | 0.40× |
| uintAdd_simple | `a + b` | 72 | 222 | 0.32× |
| uintAdd_simpleConst | `1u + 2u` | 55 | 197 | 0.28× |
| uintDiv_simple | `a / b` | 77 | 222 | 0.35× |
| uintDiv_simpleConst | `6u / 2u` | 57 | 198 | 0.29× |
| uintMod_simple | `a % b` | 77 | 222 | 0.34× |
| uintMod_simpleConst | `7u % 3u` | 56 | 195 | 0.29× |
| uintMul_simple | `a * b` | 77 | 223 | 0.34× |
| uintMul_simpleConst | `6u * 7u` | 56 | 196 | 0.29× |
| uintSub_simple | `a - b` | 77 | 222 | 0.35× |
| uintSub_simpleConst | `3u - 1u` | 56 | 195 | 0.29× |

### comparisons

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| boolEq | `a == b` | 63 | 240 | 0.26× |
| boolEqConst | `true == true` | 46 | 214 | 0.21× |
| boolNe | `a != b` | 64 | 241 | 0.26× |
| boolNeConst | `true != false` | 46 | 215 | 0.21× |
| bytesEq | `a == b` | 65 | 264 | 0.25× |
| bytesEqConst | `b"a" == b"a"` | 48 | 215 | 0.22× |
| bytesGe | `a >= b` | 80 | 244 | 0.33× |
| bytesGeConst | `b"b" >= b"b"` | 61 | 198 | 0.31× |
| bytesGt | `a > b` | 79 | 244 | 0.32× |
| bytesGtConst | `b"b" > b"a"` | 60 | 197 | 0.30× |
| bytesLe | `a <= b` | 79 | 241 | 0.33× |
| bytesLeConst | `b"a" <= b"a"` | 60 | 197 | 0.30× |
| bytesLt | `a < b` | 79 | 242 | 0.33× |
| bytesLtConst | `b"a" < b"b"` | 59 | 197 | 0.30× |
| bytesNe | `a != b` | 65 | 261 | 0.25× |
| bytesNeConst | `b"a" != b"b"` | 48 | 216 | 0.22× |
| doubleEq | `a == b` | 63 | 243 | 0.26× |
| doubleEqConst | `1.5 == 1.5` | 46 | 217 | 0.21× |
| doubleGe | `a >= b` | 74 | 224 | 0.33× |
| doubleGeConst | `3.5 >= 3.5` | 55 | 195 | 0.28× |
| doubleGt | `a > b` | 74 | 226 | 0.33× |
| doubleGtConst | `3.5 > 2.5` | 55 | 200 | 0.28× |
| doubleLe | `a <= b` | 75 | 229 | 0.33× |
| doubleLeConst | `2.5 <= 2.5` | 56 | 195 | 0.29× |
| doubleLt | `a < b` | 74 | 225 | 0.33× |
| doubleLtConst | `1.5 < 2.5` | 55 | 197 | 0.28× |
| doubleNe | `a != b` | 63 | 241 | 0.26× |
| doubleNeConst | `1.5 != 2.5` | 46 | 216 | 0.21× |
| durEq | `duration("60s") == duration("1m")` | 115 | 511 | 0.23× |
| durGe | `duration("1m") >= duration("60s")` | 124 | 498 | 0.25× |
| durGt | `duration("2m") > duration("1m")` | 133 | 489 | 0.27× |
| durLe | `duration("1m") <= duration("60s")` | 124 | 497 | 0.25× |
| durLt | `duration("1m") < duration("2m")` | 133 | 489 | 0.27× |
| durNe | `duration("60s") != duration("2m")` | 115 | 511 | 0.22× |
| intEq | `a == b` | 64 | 242 | 0.26× |
| intEqConst | `42 == 42` | 46 | 215 | 0.21× |
| intGe | `a >= b` | 73 | 223 | 0.33× |
| intGeConst | `3 >= 3` | 54 | 195 | 0.28× |
| intGt | `a > b` | 73 | 224 | 0.33× |
| intGtConst | `3 > 2` | 54 | 196 | 0.28× |
| intLe | `a <= b` | 72 | 224 | 0.32× |
| intLeConst | `2 <= 2` | 54 | 194 | 0.28× |
| intLt | `a < b` | 72 | 226 | 0.32× |
| intLtChain20 | `a<b && b<c && … && s<t && t<u (20 terms)` | 1,151 | 3,396 | 0.34× |
| intLtChain20Const | `1<2 && 2<3 && … && 19<20 && 20<21 (20 terms)` | 797 | 3,126 | 0.25× |
| intLtConst | `1 < 2` | 55 | 196 | 0.28× |
| intLtDouble | `a < b` | 75 | n/a | n/a |
| intNe | `a != b` | 63 | 241 | 0.26× |
| intNeConst | `42 != 43` | 48 | 215 | 0.23× |
| listEq | `[1,2,3] == [1,2,3]` | 216 | 303 | 0.71× |
| listNe | `[1,2,3] != [1,2,4]` | 219 | 303 | 0.72× |
| mapEq | `{"a":1,"b":2} == {"b":2,"a":1}` | 393 | 891 | 0.44× |
| mapNe | `{"a":1} != {"a":2}` | 220 | 623 | 0.35× |
| nullEq | `null == null` | 46 | 214 | 0.21× |
| stringEq | `a == b` | 65 | 263 | 0.25× |
| stringEqConst | `"a" == "a"` | 48 | 215 | 0.22× |
| stringGe | `a >= b` | 85 | 248 | 0.34× |
| stringGeConst | `"b" >= "b"` | 63 | 199 | 0.31× |
| stringGt | `a > b` | 81 | 248 | 0.32× |
| stringGtConst | `"b" > "a"` | 59 | 197 | 0.30× |
| stringLe | `a <= b` | 79 | 247 | 0.32× |
| stringLeConst | `"a" <= "a"` | 60 | 196 | 0.31× |
| stringLt | `a < b` | 78 | 247 | 0.32× |
| stringLtConst | `"a" < "b"` | 60 | 197 | 0.30× |
| stringNe | `a != b` | 67 | 265 | 0.25× |
| stringNeConst | `"a" != "b"` | 49 | 217 | 0.23× |
| tsEq | `timestamp("2024-01-01T00:00:00Z") == timestamp("2024-01-01T…` | 298 | 891 | 0.33× |
| tsGe | `timestamp("2024-01-01T00:00:00Z") >= timestamp("2024-01-01T…` | 302 | 889 | 0.34× |
| tsGt | `timestamp("2024-01-02T00:00:00Z") > timestamp("2024-01-01T0…` | 310 | 901 | 0.34× |
| tsLe | `timestamp("2024-01-01T00:00:00Z") <= timestamp("2024-01-01T…` | 305 | 878 | 0.35× |
| tsLt | `timestamp("2024-01-01T00:00:00Z") < timestamp("2024-01-02T0…` | 316 | 894 | 0.35× |
| tsNe | `timestamp("2024-01-01T00:00:00Z") != timestamp("2024-01-02T…` | 292 | 918 | 0.32× |
| uintEq | `a == b` | 64 | 243 | 0.26× |
| uintEqConst | `42u == 42u` | 46 | 216 | 0.21× |
| uintGe | `a >= b` | 72 | 222 | 0.33× |
| uintGeConst | `3u >= 3u` | 54 | 194 | 0.28× |
| uintGt | `a > b` | 71 | 224 | 0.32× |
| uintGtConst | `3u > 2u` | 54 | 197 | 0.28× |
| uintLe | `a <= b` | 72 | 222 | 0.32× |
| uintLeConst | `2u <= 2u` | 54 | 195 | 0.28× |
| uintLt | `a < b` | 72 | 224 | 0.32× |
| uintLtConst | `1u < 2u` | 54 | 197 | 0.27× |
| uintNe | `a != b` | 64 | 240 | 0.27× |
| uintNeConst | `42u != 43u` | 46 | 214 | 0.22× |

### comprehensions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| all10 | `[1,2,3,4,5,6,7,8,9,10].all(x, x > 0)` | 807 | 2,537 | 0.32× |
| all20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].all(x,…` | 1,457 | 4,916 | 0.30× |
| all100 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 6,612 | 24.0 µs | 0.28× |
| all1000 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 64.8 µs | 238.1 µs | 0.27× |
| exists20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,242 | 5,263 | 0.24× |
| existsMapKey | `{1:"a",2:"b",3:"c"}.exists(k, k == 2)` | 314 | 1,674 | 0.19× |
| existsOne20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,170 | 5,664 | 0.21× |
| filter20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].f…` | 3,287 | 6,195 | 0.53× |
| map20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].m…` | 5,257 | 3,575 | 1.47× |

### conversions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesFromString | `bytes(s)` | 70 | 218 | 0.32× |
| bytesFromStringConst | `bytes("abc")` | 62 | 199 | 0.31× |
| doubleFromInt | `double(i)` | 56 | 208 | 0.27× |
| doubleFromIntConst | `double(42)` | 48 | 196 | 0.25× |
| doubleFromString | `double(s)` | 79 | 235 | 0.34× |
| doubleFromStringConst | `double("42.5")` | 70 | 216 | 0.32× |
| doubleFromUint | `double(u)` | 56 | 208 | 0.27× |
| doubleFromUintConst | `double(42u)` | 48 | 195 | 0.25× |
| durationRoundTrip | `string(duration(s))` | 121 | 393 | 0.31× |
| intFromDouble | `int(d)` | 61 | 210 | 0.29× |
| intFromDoubleConst | `int(42.9)` | 52 | 196 | 0.27× |
| intFromString | `int(s)` | 65 | 219 | 0.30× |
| intFromStringConst | `int("42")` | 55 | 198 | 0.28× |
| intFromStringNested | `int(string(123))` | 84 | 297 | 0.28× |
| intFromTimestamp | `int(timestamp("2024-01-01T00:00:00Z"))` | 165 | 539 | 0.31× |
| intFromUint | `int(u)` | 61 | 208 | 0.29× |
| intFromUintConst | `int(42u)` | 53 | 196 | 0.27× |
| stringFromBool | `string(x)` | 62 | 218 | 0.29× |
| stringFromBoolConst | `string(true)` | 55 | 208 | 0.26× |
| stringFromBytes | `string(x)` | 69 | 221 | 0.31× |
| stringFromBytesConst | `string(b"abc")` | 62 | 200 | 0.31× |
| stringFromDouble | `string(d)` | 102 | 375 | 0.27× |
| stringFromDoubleConst | `string(42.5)` | 93 | 364 | 0.25× |
| stringFromDuration | `string(duration("90s"))` | 113 | 372 | 0.30× |
| stringFromInt | `string(i)` | 69 | 225 | 0.31× |
| stringFromIntConst | `string(42)` | 58 | 213 | 0.27× |
| stringFromTimestamp | `string(timestamp("2024-01-01T00:00:00Z"))` | 330 | 874 | 0.38× |
| stringFromUint | `string(u)` | 68 | 226 | 0.30× |
| stringFromUintConst | `string(42u)` | 58 | 215 | 0.27× |
| timestampRoundTrip | `string(timestamp(s))` | 348 | 917 | 0.38× |
| typeOfInt | `type(i) == int` | 83 | 319 | 0.26× |
| typeOfString | `type(s) == string` | 81 | 325 | 0.25× |
| uintFromDouble | `uint(d)` | 60 | 209 | 0.29× |
| uintFromDoubleConst | `uint(42.9)` | 52 | 196 | 0.27× |
| uintFromInt | `uint(i)` | 61 | 208 | 0.29× |
| uintFromIntConst | `uint(42)` | 52 | 196 | 0.26× |
| uintFromString | `uint(s)` | 64 | 218 | 0.29× |
| uintFromStringConst | `uint("42")` | 54 | 197 | 0.27× |

### index

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| listInt | `[10,20,30,40,50][i]` | 133 | 225 | 0.59× |
| listIntConst | `[10,20,30,40,50][4]` | 124 | 213 | 0.58× |
| mapBool | `{true:1,false:0}[k]` | 147 | 510 | 0.29× |
| mapBoolConst | `{true:1,false:0}[true]` | 137 | 495 | 0.28× |
| mapInt | `{1:10,2:20,3:30}[k]` | 172 | 730 | 0.23× |
| mapIntConst | `{1:10,2:20,3:30}[3]` | 161 | 718 | 0.22× |
| mapString | `{"a":1,"b":2,"c":3}[k]` | 196 | 667 | 0.29× |
| mapStringConst | `{"a":1,"b":2,"c":3}["c"]` | 184 | 651 | 0.28× |
| mapUint | `{1u:10,2u:20,3u:30}[k]` | 170 | 729 | 0.23× |
| mapUintConst | `{1u:10,2u:20,3u:30}[3u]` | 160 | 720 | 0.22× |

### lists

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| 5 | `a in ["123","augustine","jess","bob","alice"]` | 161 | 252 | 0.64× |
| 5_lit | `"alice" in ["123","augustine","jess","bob","alice"]` | 148 | 231 | 0.64× |
| 20 | `a in ["alice","bob","carol","dave","eve","frank","grace","h…` | 361 | 262 | 1.38× |
| 20_lit | `"tom" in ["alice","bob","carol","dave","eve","frank","grace…` | 348 | 243 | 1.43× |
| 100 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 1,403 | 342 | 4.10× |
| 100_lit | `"tom4" in ["alice0","bob0","carol0","dave0","eve0","frank0"…` | 1,562 | 307 | 5.09× |
| 100_lit_first | `"alice0" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 1,016 | 226 | 4.50× |
| 100_lit_miss | `"nobody" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 1,359 | 334 | 4.07× |
| 1000 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 12.8 µs | 1,054 | 12.12× |
| 1000_lit | `"tom49" in ["alice0","bob0","carol0","dave0","eve0","frank0…` | 12.8 µs | 1,176 | 10.87× |
| bool20 | `x in [false,false,false,false,false,false,false,false,false…` | 335 | 245 | 1.37× |
| bound100 | `x in xs` | 427 | 584 | 0.73× |
| bound1000 | `x in xs` | 3,411 | 3,494 | 0.98× |
| bound10000 | `x in xs` | 33.7 µs | 32.6 µs | 1.03× |
| bound100000 | `x in xs` | 347.4 µs | 323.5 µs | 1.07× |
| bound1000000 | `x in xs` | 3.46 ms | 3.23 ms | 1.07× |
| bound1000000_first | `x in xs` | 76 | 993.7 µs | 0.00× |
| bound1000000_miss | `x in xs` | 3.25 ms | 3.23 ms | 1.00× |
| concat | `size([1,2,3] + [4,5])` | 276 | 316 | 0.87× |
| double20 | `x in [1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,10.0,11.0,12.0,13…` | 353 | 245 | 1.44× |
| iam100 | `perm in perms` | 598 | 876 | 0.68× |
| iam1000 | `perm in perms` | 5,572 | 6,758 | 0.82× |
| iam1000_first | `perm in perms` | 75 | 1,271 | 0.06× |
| iam1000_miss | `perm in perms` | 5,321 | 6,246 | 0.85× |
| int20 | `x in [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]` | 348 | 246 | 1.42× |
| uint20 | `x in [1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,16…` | 347 | 245 | 1.41× |

### literals

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bool | `true` | 33 | 109 | 0.30× |
| double | `3.14` | 33 | 110 | 0.30× |
| int | `42` | 33 | 110 | 0.30× |
| null | `null` | 33 | 108 | 0.31× |
| string | `"hello"` | 33 | 116 | 0.28× |

### logic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| and2 | `a && b` | 62 | 222 | 0.28× |
| and2Const | `true && true` | 44 | 195 | 0.23× |
| and10Terms | `a && b && c && d && e && f && g && h && i && j` | 261 | 936 | 0.28× |
| andNoShortCircuit | `a && s.contains("yyy")` | 93 | 328 | 0.28× |
| andShortCircuit | `a && s.contains("yyy")` | 48 | 325 | 0.15× |
| not1 | `!a` | 43 | 222 | 0.20× |
| not1Const | `!false` | 35 | 210 | 0.17× |
| not3 | `!!!a` | 43 | 222 | 0.20× |
| not3Const | `!!!false` | 36 | 210 | 0.17× |
| or2 | `a \|\| b` | 62 | 222 | 0.28× |
| or2Const | `false \|\| true` | 44 | 194 | 0.23× |
| or10Terms | `a \|\| b \|\| c \|\| d \|\| e \|\| f \|\| g \|\| h \|\| i \|\| j` | 256 | 955 | 0.27× |
| orNoShortCircuit | `a \|\| s.contains("yyy")` | 93 | 326 | 0.28× |
| orShortCircuit | `a \|\| s.contains("yyy")` | 48 | 324 | 0.15× |

### long_strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| containsLong_N10 | `a.contains("yyy")` | 66 | 223 | 0.30× |
| containsLong_N100 | `a.contains("yyy")` | 66 | 233 | 0.28× |
| containsLong_N1000 | `a.contains("yyy")` | 82 | 358 | 0.23× |
| containsLong_N10000 | `a.contains("yyy")` | 240 | 1,557 | 0.15× |
| eqLong_N10_match | `a == "xxxxxxxxxx"` | 57 | 235 | 0.24× |
| eqLong_N10_mismatch | `a == "xxxxxxxxxx"` | 57 | 237 | 0.24× |
| eqLong_N100_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 60 | 243 | 0.25× |
| eqLong_N100_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 59 | 241 | 0.24× |
| eqLong_N100_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 58 | 235 | 0.25× |
| eqLong_N1000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 73 | 309 | 0.24× |
| eqLong_N1000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 73 | 312 | 0.23× |
| eqLong_N1000_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 58 | 243 | 0.24× |
| eqLong_N1000_mismatch_len | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 59 | 244 | 0.24× |
| eqLong_N10000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 241 | 952 | 0.25× |
| eqLong_N10000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 240 | 951 | 0.25× |

### maps

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| dotField | `{"k":1}.k` | 102 | n/a | n/a |
| hasKey | `has({"a":1}.a)` | 100 | n/a | n/a |
| inBool | `k in {true:1,false:0}` | 153 | 522 | 0.29× |
| inBoolConst | `true in {true:1,false:0}` | 142 | 509 | 0.28× |
| inInt | `k in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 382 | 2,601 | 0.15× |
| inIntConst | `10 in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 350 | 2,577 | 0.14× |
| inString | `k in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i":9…` | 423 | 1,965 | 0.22× |
| inStringConst | `"j" in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i"…` | 411 | 1,935 | 0.21× |
| inUint | `k in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 369 | 2,600 | 0.14× |
| inUintConst | `10u in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 354 | 2,581 | 0.14× |

### policies

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| arena_map_gate | `c.age >= {"us": 21, "de": 18}["us"]` | 220 | 628 | 0.35× |
| authz_basic | `(c.is_premium && c.age >= 18 && c.name in ["Ada", "Grace", …` | 281 | 747 | 0.38× |
| authz_deep | `(m.inner.b && m.inner.i64 >= 18 && m.inner.inner.s in ["Ada…` | 438 | 889 | 0.49× |
| authz_deep8 | `(m.inner.inner.inner.inner.inner.inner.inner.b && m.inner.i…` | 1,018 | 1,352 | 0.75× |
| mega100 | `(m.i64 + m.str_to_i32["q1"] + m.rep_i32[1]) == -1 ? "deny" …` | 9,345 | 19.8 µs | 0.47× |
| premium_gate | `c.is_premium ? c.age : 0` | 116 | 260 | 0.44× |
| quota_check | `m.str_to_i32["used"] + m.str_to_i32["pending"] < m.str_to_i…` | 325 | 912 | 0.36× |
| risk_score | `c.credit_score >= 700.0 && c.balance_cents > 1000u` | 166 | 430 | 0.39× |
| str_in_list | `m.s in ["alpha", "beta", "gamma", "delta"]` | 181 | 374 | 0.48× |
| ternary2 | `c.age > 30 ? (c.is_premium ? "gold" : "silver") : "basic"` | 151 | 433 | 0.35× |
| ternary5 | `c.age > 60 ? "a" : c.age > 50 ? "b" : c.age > 40 ? "c" : c.…` | 301 | 850 | 0.35× |
| tier_route | `c.balance_cents >= 100000u ? "platinum" : (c.is_premium ? "…` | 251 | 907 | 0.28× |

### proto

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| construct_name | `celwasm.testdata.Customer{name: "Ada"}.name` | 161 | 350 | 0.46× |
| cust_age | `c.age` | 65 | 151 | 0.43× |
| cust_is_premium | `c.is_premium` | 66 | 152 | 0.43× |
| cust_name | `c.name` | 77 | 264 | 0.29× |
| map_i64_str | `m.i64_to_str[2]` | 107 | 446 | 0.24× |
| map_str_i32 | `m.str_to_i32["b"]` | 103 | 347 | 0.30× |
| map_str_msg_i64 | `m.str_to_msg["k"].i64` | 149 | 378 | 0.39× |
| metadata_b | `c.metadata["b"]` | 109 | 454 | 0.24× |
| pair_list_arena | `[10, 20, 30, 40, 50][2]` | 124 | 211 | 0.59× |
| pair_map_arena | `{"a": 1, "b": 2, "c": 3}["b"]` | 189 | 638 | 0.30× |
| read_b | `m.b` | 64 | 152 | 0.42× |
| read_f64 | `m.f64` | 66 | 151 | 0.44× |
| read_s | `m.s` | 78 | 265 | 0.29× |
| read_u64 | `m.u64` | 66 | 151 | 0.44× |
| reads5 | `m.i32 + m.i64 + m.si32 + m.si64 + m.sfx32` | 332 | 571 | 0.58× |
| reads10 | `m.i32 + m.i64 + … + m.si32 + m.si64 (10 terms)` | 660 | 1,095 | 0.60× |
| reads100 | `m.i32 + m.i64 + … + m.si32 + m.si64 (100 terms)` | 6,397 | 10.7 µs | 0.60× |
| rep_i32_at0 | `m.rep_i32[0]` | 99 | 292 | 0.34× |
| rep_i32_at9 | `m.rep_i32[9]` | 99 | 293 | 0.34× |
| rep_msg_at1_s | `m.rep_msg[1].s` | 154 | 471 | 0.33× |
| select_depth1 | `m.i64` | 67 | 152 | 0.44× |
| select_depth2 | `m.inner.i64` | 103 | 200 | 0.51× |
| select_depth4 | `m.inner.inner.inner.i64` | 181 | 275 | 0.66× |
| select_depth8 | `m.inner.inner.inner.inner.inner.inner.inner.i64` | 318 | 432 | 0.74× |
| select_depth16 | `m.inner.inner.inner.inner.inner.inner.inner.inner.inner.inn…` | 592 | 744 | 0.80× |
| tags_at2 | `c.tags[2]` | 106 | 407 | 0.26× |

### size

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytes | `size(x)` | 59 | 217 | 0.27× |
| bytesConst | `size(b"0123456789abcdef")` | 54 | 195 | 0.28× |
| list10 | `size([1,2,3,4,5,6,7,8,9,10])` | 195 | 212 | 0.92× |
| list100 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 1,016 | 211 | 4.81× |
| list1000 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 8,814 | 212 | 41.48× |
| map10 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10})` | 382 | 2,339 | 0.16× |
| map100 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10,11:11,12:12…` | 2,819 | 123.7 µs | 0.02× |
| string | `size(s)` | 62 | 226 | 0.27× |
| stringConst | `size("abcdefghijklmnopqrstuvwxyz")` | 59 | 204 | 0.29× |

### strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesConcat2 | `a + b` | 90 | 263 | 0.34× |
| bytesConcat2Const | `b"ab" + b"cd"` | 73 | 218 | 0.34× |
| concat2 | `a + b` | 92 | 269 | 0.34× |
| concat2Const | `"hello " + "world"` | 74 | 222 | 0.33× |
| concatChain10Terms | `a + b + c + d + e + f + g + h + i + j` | 535 | 1,274 | 0.42× |
| concatChain100Terms | `a + b + … + i + j (100 terms)` | 6,305 | 10.9 µs | 0.58× |
| concatChain1000Terms | `a + b + … + i + j (1000 terms)` | 126.6 µs | 130.1 µs | 0.97× |
| contains | `a.contains("aug")` | 67 | 220 | 0.30× |
| containsConst | `"augustine".contains("aug")` | 56 | 199 | 0.28× |
| endsWith | `a.endsWith("ine")` | 65 | 218 | 0.30× |
| endsWithConst | `"augustine".endsWith("ine")` | 55 | 197 | 0.28× |
| eqConst | `"hello" == "world"` | 48 | 215 | 0.22× |
| eqVar | `a == "augustine"` | 57 | 235 | 0.24× |
| matchesCheap | `a.matches("^aug")` | 1,484 | 246 | 6.03× |
| matchesCheapConst | `"augustine".matches("^aug")` | 1,472 | 226 | 6.53× |
| matchesComplex | `a.matches("^[a-z]+-[0-9]{2,4}@[a-z]+\\.(com\|org)$")` | 9,055 | 322 | 28.11× |
| matchesComplexConst | `"user-1234@example.com".matches("^[a-z]+-[0-9]{2,4}@[a-z]+\…` | 8,993 | 298 | 30.17× |
| startsWith | `a.startsWith("aug")` | 65 | 219 | 0.30× |
| startsWithConst | `"augustine".startsWith("aug")` | 55 | 198 | 0.28× |

### ternary

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| intComputedCond | `a > b ? x : y` | 103 | 327 | 0.32× |
| intConst | `1 > 2 ? 10 : 20` | 69 | 275 | 0.25× |
| intVarCond | `c ? x : y` | 65 | n/a | n/a |
| nested3 | `a > b ? "gt" : (a == b ? "eq" : "lt")` | 129 | 481 | 0.27× |
| stringComputedCond | `a > b ? s : t` | 98 | 359 | 0.27× |
| stringConst | `3 > 2 ? "yes" : "no"` | 71 | 277 | 0.26× |

### time

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| durAddDur | `(duration("90s") + duration("30s")).getSeconds()` | 150 | 580 | 0.26× |
| durAddTs | `int(duration("1h") + timestamp("2024-01-01T00:00:00Z"))` | 232 | 801 | 0.29× |
| durGetHours | `duration("3723s").getHours()` | 85 | 331 | 0.26× |
| durGetSeconds | `duration("3723s").getSeconds()` | 85 | 331 | 0.26× |
| durSubDur | `(duration("90s") - duration("30s")).getSeconds()` | 148 | 580 | 0.26× |
| tsAddDur | `int(timestamp("2024-01-01T00:00:00Z") + duration("1h"))` | 233 | 786 | 0.30× |
| tsGetDayOfWeekUtc | `timestamp("2024-06-15T10:30:45Z").getDayOfWeek()` | 199 | 556 | 0.36× |
| tsGetFullYearTz | `timestamp("2024-06-15T10:30:45Z").getFullYear("America/New_…` | 221 | 608 | 0.36× |
| tsGetFullYearUtc | `timestamp("2024-06-15T10:30:45Z").getFullYear()` | 192 | 554 | 0.35× |
| tsGetFullYearUtcMax | `timestamp("9999-12-31T23:59:59Z").getFullYear()` | 222 | 572 | 0.39× |
| tsGetHoursTz | `timestamp("2024-06-15T10:30:45Z").getHours("America/New_Yor…` | 219 | 605 | 0.36× |
| tsGetHoursUtc | `timestamp("2024-06-15T10:30:45Z").getHours()` | 193 | 564 | 0.34× |
| tsGetSecondsTz | `timestamp("2024-06-15T10:30:45Z").getSeconds("America/New_Y…` | 218 | 606 | 0.36× |
| tsGetSecondsUtc | `timestamp("2024-06-15T10:30:45Z").getSeconds()` | 192 | 557 | 0.35× |
| tsSubDur | `int(timestamp("2024-01-01T01:00:00Z") - duration("1h"))` | 232 | 768 | 0.30× |
| tsSubTs | `(timestamp("2024-01-01T01:00:00Z") - timestamp("2024-01-01T…` | 324 | 994 | 0.33× |

### Per-operator headline — T(N) = setup + N·per_op

Linear regression over each length-sweep family; slope is the steady-state cost of one more operation, crossover is the expression length where the comparator overtakes cel-cpp.

| surface | operator family | points | cel-cpp slope | cel-cpp setup | celwasm-dynamic slope | celwasm-dynamic setup | celwasm-dynamic crossover vs cel-cpp |
|---|---|---|---|---|---|---|---|
| arithmetic | doubleAdd | 5 | 33.0 | -198 | 89.2 | -184 | never wins |
| arithmetic | intAdd | 5 | 32.2 | -66 | 77.9 | 144 | never wins |
| arithmetic | intMul | 5 | 30.8 | -34 | 80.2 | 112 | never wins |
| arithmetic | intSub | 5 | 31.2 | -48 | 81.3 | 82 | never wins |
| comprehensions | all | 4 | 64.6 | 159 | 237.9 | 178 | never wins |
| lists | bound | 5 | 3.5 | 110 | 3.2 | 200 | N ≈ 396 |
| long_strings | containsLong_N | 4 | 0.0 | 65 | 0.1 | 222 | never wins |
| proto | reads | 3 | 63.8 | 17 | 107.2 | 29 | never wins |
| proto | select_depth | 5 | 34.9 | 36 | 39.2 | 117 | never wins |
| size | list | 3 | 8.7 | 127 | 0.0 | 212 | N ≈ 10 |
| strings | concatChain | 3 | 129.9 | -3,585 | 131.1 | -1,073 | never wins |

