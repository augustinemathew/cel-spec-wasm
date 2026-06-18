## Eval benchmark results — 2026-06-16, Mac (dynamic)

Eval steady-state, median real time ns/call (lower is better); `×cel-cpp` > 1.0 means that comparator is faster than cel-cpp.  `n/a` = cell does not run on that comparator (see skip tags in the corpus YAML).

### arithmetic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| abcAbcShapeLit | `1 + 2 + 3 + 1 + 2 + 3` | 156 | 523 | 0.30× |
| abcAbcShapeVars | `a + b + c + a + b + c` | 208 | 564 | 0.37× |
| doubleAdd2 | `a + b` | 71 | 223 | 0.32× |
| doubleAdd2Const | `1.0 + 1.0` | 52 | 192 | 0.27× |
| doubleAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 318 | 954 | 0.33× |
| doubleAdd10TermsConst | `1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0` | 226 | 814 | 0.28× |
| doubleAdd50Terms | `a + b + … + i + j (50 terms)` | 1,585 | 4,037 | 0.39× |
| doubleAdd50TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (50 terms)` | 1,047 | 3,915 | 0.27× |
| doubleAdd250Terms | `a + b + … + i + j (250 terms)` | 7,466 | 19.8 µs | 0.38× |
| doubleAdd250TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (250 terms)` | 5,191 | 19.8 µs | 0.26× |
| doubleAdd1000Terms | `a + b + … + i + j (1000 terms)` | 30.8 µs | 77.9 µs | 0.40× |
| doubleAdd1000TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (1000 terms)` | 21.7 µs | n/a | n/a |
| doubleDiv_simple | `a / b` | 73 | 225 | 0.32× |
| doubleDiv_simpleConst | `3.0 / 2.0` | 53 | 196 | 0.27× |
| doubleMul_simple | `a * b` | 73 | 225 | 0.33× |
| doubleMul_simpleConst | `3.14 * 2.0` | 54 | 189 | 0.28× |
| doubleNeg | `-a` | 55 | 208 | 0.26× |
| doubleNegConst | `-3.14` | 32 | 108 | 0.29× |
| doubleSub_simple | `a - b` | 74 | 225 | 0.33× |
| doubleSub_simpleConst | `3.5 - 1.25` | 55 | 198 | 0.28× |
| intAdd2 | `a + b` | 71 | 224 | 0.32× |
| intAdd2Const | `1 + 1` | 55 | 196 | 0.28× |
| intAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 327 | 1,003 | 0.33× |
| intAdd10TermsConst | `1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1` | 237 | 863 | 0.27× |
| intAdd50Terms | `a + b + … + i + j (50 terms)` | 1,540 | 4,318 | 0.36× |
| intAdd50TermsConst | `1 + 1 + … + 1 + 1 (50 terms)` | 1,104 | 4,183 | 0.26× |
| intAdd250Terms | `a + b + … + i + j (250 terms)` | 7,757 | 21.2 µs | 0.37× |
| intAdd250TermsConst | `1 + 1 + … + 1 + 1 (250 terms)` | 5,436 | 20.7 µs | 0.26× |
| intAdd1000Terms | `a + b + … + i + j (1000 terms)` | 34.0 µs | 82.9 µs | 0.41× |
| intAdd1000TermsConst | `1 + 1 + … + 1 + 1 (1000 terms)` | 22.9 µs | n/a | n/a |
| intAddDeepTree | `((a + b) + (c + d)) + ((e + f) + (g + h))` | 298 | 789 | 0.38× |
| intDiv_simple | `a / b` | 77 | 223 | 0.34× |
| intDiv_simpleConst | `84 / 2` | 57 | 192 | 0.30× |
| intMixedOps3 | `(a + b) * c - d` | 146 | 413 | 0.35× |
| intMod_simple | `a % b` | 76 | 225 | 0.34× |
| intMod_simpleConst | `100 % 7` | 56 | 196 | 0.29× |
| intMul2 | `a * b` | 76 | 225 | 0.34× |
| intMul2Const | `1 * 1` | 55 | 197 | 0.28× |
| intMul10Terms | `a * b * c * d * e * f * g * h * i * j` | 333 | 988 | 0.34× |
| intMul10TermsConst | `1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1` | 214 | 865 | 0.25× |
| intMul50Terms | `a * b * … * i * j (50 terms)` | 1,627 | 4,308 | 0.38× |
| intMul50TermsConst | `1 * 1 * … * 1 * 1 (50 terms)` | 983 | 4,183 | 0.24× |
| intMul250Terms | `a * b * … * i * j (250 terms)` | 7,667 | 21.1 µs | 0.36× |
| intMul250TermsConst | `1 * 1 * … * 1 * 1 (250 terms)` | 4,802 | 21.2 µs | 0.23× |
| intMul1000Terms | `a * b * … * i * j (1000 terms)` | 32.9 µs | 82.4 µs | 0.40× |
| intMul1000TermsConst | `1 * 1 * … * 1 * 1 (1000 terms)` | 20.8 µs | n/a | n/a |
| intNeg | `-a` | 60 | 207 | 0.29× |
| intNegConst | `-42` | 33 | 108 | 0.31× |
| intSub2 | `a - b` | 72 | 222 | 0.33× |
| intSub2Const | `1 - 1` | 55 | 191 | 0.29× |
| intSub10Terms | `a - b - c - d - e - f - g - h - i - j` | 323 | 955 | 0.34× |
| intSub10TermsConst | `1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1` | 225 | 873 | 0.26× |
| intSub50Terms | `a - b - … - i - j (50 terms)` | 1,528 | 4,053 | 0.38× |
| intSub50TermsConst | `1 - 1 - … - 1 - 1 (50 terms)` | 1,056 | 3,914 | 0.27× |
| intSub250Terms | `a - b - … - i - j (250 terms)` | 7,550 | 19.5 µs | 0.39× |
| intSub250TermsConst | `1 - 1 - … - 1 - 1 (250 terms)` | 5,177 | 19.9 µs | 0.26× |
| intSub1000Terms | `a - b - … - i - j (1000 terms)` | 33.1 µs | 79.1 µs | 0.42× |
| intSub1000TermsConst | `1 - 1 - … - 1 - 1 (1000 terms)` | 21.7 µs | n/a | n/a |
| polyMix1000Terms | `a*d + b*a + … + i*j + j*g (1000 terms)` | 64.7 µs | 165.9 µs | 0.39× |
| uintAdd_simple | `a + b` | 72 | 223 | 0.32× |
| uintAdd_simpleConst | `1u + 2u` | 54 | 191 | 0.28× |
| uintDiv_simple | `a / b` | 77 | 223 | 0.34× |
| uintDiv_simpleConst | `6u / 2u` | 56 | 193 | 0.29× |
| uintMod_simple | `a % b` | 76 | 224 | 0.34× |
| uintMod_simpleConst | `7u % 3u` | 55 | 193 | 0.29× |
| uintMul_simple | `a * b` | 76 | 226 | 0.34× |
| uintMul_simpleConst | `6u * 7u` | 55 | 198 | 0.28× |
| uintSub_simple | `a - b` | 76 | 224 | 0.34× |
| uintSub_simpleConst | `3u - 1u` | 56 | 193 | 0.29× |

### comparisons

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| boolEq | `a == b` | 63 | 240 | 0.26× |
| boolEqConst | `true == true` | 43 | 212 | 0.20× |
| boolNe | `a != b` | 63 | 240 | 0.26× |
| boolNeConst | `true != false` | 44 | 213 | 0.21× |
| bytesEq | `a == b` | 65 | 261 | 0.25× |
| bytesEqConst | `b"a" == b"a"` | 47 | 214 | 0.22× |
| bytesGe | `a >= b` | 79 | 244 | 0.33× |
| bytesGeConst | `b"b" >= b"b"` | 61 | 197 | 0.31× |
| bytesGt | `a > b` | 78 | 244 | 0.32× |
| bytesGtConst | `b"b" > b"a"` | 60 | 197 | 0.30× |
| bytesLe | `a <= b` | 79 | 244 | 0.32× |
| bytesLeConst | `b"a" <= b"a"` | 60 | 197 | 0.30× |
| bytesLt | `a < b` | 78 | 245 | 0.32× |
| bytesLtConst | `b"a" < b"b"` | 60 | 197 | 0.30× |
| bytesNe | `a != b` | 64 | 260 | 0.25× |
| bytesNeConst | `b"a" != b"b"` | 47 | 214 | 0.22× |
| doubleEq | `a == b` | 66 | 242 | 0.27× |
| doubleEqConst | `1.5 == 1.5` | 44 | 215 | 0.20× |
| doubleGe | `a >= b` | 72 | 225 | 0.32× |
| doubleGeConst | `3.5 >= 3.5` | 55 | 198 | 0.28× |
| doubleGt | `a > b` | 73 | 225 | 0.32× |
| doubleGtConst | `3.5 > 2.5` | 55 | 196 | 0.28× |
| doubleLe | `a <= b` | 73 | 225 | 0.33× |
| doubleLeConst | `2.5 <= 2.5` | 54 | 197 | 0.28× |
| doubleLt | `a < b` | 73 | 224 | 0.33× |
| doubleLtConst | `1.5 < 2.5` | 55 | 195 | 0.28× |
| doubleNe | `a != b` | 63 | 241 | 0.26× |
| doubleNeConst | `1.5 != 2.5` | 44 | 213 | 0.21× |
| durEq | `duration("60s") == duration("1m")` | 116 | 512 | 0.23× |
| durGe | `duration("1m") >= duration("60s")` | 126 | 492 | 0.26× |
| durGt | `duration("2m") > duration("1m")` | 134 | 485 | 0.28× |
| durLe | `duration("1m") <= duration("60s")` | 125 | 492 | 0.26× |
| durLt | `duration("1m") < duration("2m")` | 136 | 485 | 0.28× |
| durNe | `duration("60s") != duration("2m")` | 115 | 506 | 0.23× |
| intEq | `a == b` | 63 | 239 | 0.26× |
| intEqConst | `42 == 42` | 44 | 214 | 0.21× |
| intGe | `a >= b` | 73 | 224 | 0.32× |
| intGeConst | `3 >= 3` | 55 | 197 | 0.28× |
| intGt | `a > b` | 72 | 223 | 0.32× |
| intGtConst | `3 > 2` | 55 | 195 | 0.28× |
| intLe | `a <= b` | 73 | 225 | 0.32× |
| intLeConst | `2 <= 2` | 55 | 198 | 0.28× |
| intLt | `a < b` | 72 | 223 | 0.32× |
| intLtChain20 | `a<b && b<c && … && s<t && t<u (20 terms)` | 1,180 | 3,482 | 0.34× |
| intLtChain20Const | `1<2 && 2<3 && … && 19<20 && 20<21 (20 terms)` | 807 | 3,245 | 0.25× |
| intLtConst | `1 < 2` | 56 | 196 | 0.28× |
| intLtDouble | `a < b` | 74 | n/a | n/a |
| intNe | `a != b` | 63 | 240 | 0.26× |
| intNeConst | `42 != 43` | 45 | 214 | 0.21× |
| listEq | `[1,2,3] == [1,2,3]` | 218 | 989 | 0.22× |
| listNe | `[1,2,3] != [1,2,4]` | 206 | 988 | 0.21× |
| mapEq | `{"a":1,"b":2} == {"b":2,"a":1}` | 407 | 892 | 0.46× |
| mapNe | `{"a":1} != {"a":2}` | 217 | 633 | 0.34× |
| nullEq | `null == null` | 44 | 213 | 0.21× |
| stringEq | `a == b` | 65 | 263 | 0.25× |
| stringEqConst | `"a" == "a"` | 48 | 213 | 0.22× |
| stringGe | `a >= b` | 79 | 248 | 0.32× |
| stringGeConst | `"b" >= "b"` | 61 | 198 | 0.31× |
| stringGt | `a > b` | 79 | 247 | 0.32× |
| stringGtConst | `"b" > "a"` | 60 | 197 | 0.31× |
| stringLe | `a <= b` | 79 | 248 | 0.32× |
| stringLeConst | `"a" <= "a"` | 60 | 197 | 0.31× |
| stringLt | `a < b` | 79 | 246 | 0.32× |
| stringLtConst | `"a" < "b"` | 61 | 197 | 0.31× |
| stringNe | `a != b` | 65 | 262 | 0.25× |
| stringNeConst | `"a" != "b"` | 48 | 214 | 0.22× |
| tsEq | `timestamp("2024-01-01T00:00:00Z") == timestamp("2024-01-01T…` | 296 | 890 | 0.33× |
| tsGe | `timestamp("2024-01-01T00:00:00Z") >= timestamp("2024-01-01T…` | 308 | 901 | 0.34× |
| tsGt | `timestamp("2024-01-02T00:00:00Z") > timestamp("2024-01-01T0…` | 314 | 908 | 0.35× |
| tsLe | `timestamp("2024-01-01T00:00:00Z") <= timestamp("2024-01-01T…` | 306 | 893 | 0.34× |
| tsLt | `timestamp("2024-01-01T00:00:00Z") < timestamp("2024-01-02T0…` | 315 | 899 | 0.35× |
| tsNe | `timestamp("2024-01-01T00:00:00Z") != timestamp("2024-01-02T…` | 299 | 903 | 0.33× |
| uintEq | `a == b` | 63 | 241 | 0.26× |
| uintEqConst | `42u == 42u` | 44 | 214 | 0.21× |
| uintGe | `a >= b` | 72 | 225 | 0.32× |
| uintGeConst | `3u >= 3u` | 55 | 197 | 0.28× |
| uintGt | `a > b` | 72 | 224 | 0.32× |
| uintGtConst | `3u > 2u` | 55 | 196 | 0.28× |
| uintLe | `a <= b` | 73 | 225 | 0.32× |
| uintLeConst | `2u <= 2u` | 56 | 197 | 0.28× |
| uintLt | `a < b` | 74 | 224 | 0.33× |
| uintLtConst | `1u < 2u` | 56 | 196 | 0.29× |
| uintNe | `a != b` | 63 | 240 | 0.26× |
| uintNeConst | `42u != 43u` | 44 | 213 | 0.21× |

### comprehensions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| all10 | `[1,2,3,4,5,6,7,8,9,10].all(x, x > 0)` | 827 | 3,546 | 0.23× |
| all20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].all(x,…` | 1,469 | 6,673 | 0.22× |
| all100 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 6,697 | 32.7 µs | 0.20× |
| all1000 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 66.6 µs | n/a | n/a |
| exists20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,242 | 7,027 | 0.18× |
| existsMapKey | `{1:"a",2:"b",3:"c"}.exists(k, k == 2)` | 334 | 1,733 | 0.19× |
| existsOne20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,172 | 7,371 | 0.16× |
| filter20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].f…` | 3,328 | 7,896 | 0.42× |
| map20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].m…` | 5,352 | 5,330 | 1.00× |

### conversions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesFromString | `bytes(s)` | 70 | 219 | 0.32× |
| bytesFromStringConst | `bytes("abc")` | 61 | 200 | 0.31× |
| doubleFromInt | `double(i)` | 56 | 207 | 0.27× |
| doubleFromIntConst | `double(42)` | 48 | 192 | 0.25× |
| doubleFromString | `double(s)` | 79 | 235 | 0.34× |
| doubleFromStringConst | `double("42.5")` | 70 | 213 | 0.33× |
| doubleFromUint | `double(u)` | 56 | 208 | 0.27× |
| doubleFromUintConst | `double(42u)` | 49 | 192 | 0.25× |
| durationRoundTrip | `string(duration(s))` | 123 | 392 | 0.31× |
| intFromDouble | `int(d)` | 62 | 208 | 0.30× |
| intFromDoubleConst | `int(42.9)` | 52 | 191 | 0.27× |
| intFromString | `int(s)` | 65 | 209 | 0.31× |
| intFromStringConst | `int("42")` | 55 | 193 | 0.28× |
| intFromStringNested | `int(string(123))` | 85 | 297 | 0.29× |
| intFromTimestamp | `int(timestamp("2024-01-01T00:00:00Z"))` | 168 | 546 | 0.31× |
| intFromUint | `int(u)` | 60 | 207 | 0.29× |
| intFromUintConst | `int(42u)` | 52 | 191 | 0.27× |
| stringFromBool | `string(x)` | 63 | 218 | 0.29× |
| stringFromBoolConst | `string(true)` | 53 | 208 | 0.26× |
| stringFromBytes | `string(x)` | 70 | 222 | 0.32× |
| stringFromBytesConst | `string(b"abc")` | 61 | 199 | 0.31× |
| stringFromDouble | `string(d)` | 105 | 367 | 0.29× |
| stringFromDoubleConst | `string(42.5)` | 93 | 355 | 0.26× |
| stringFromDuration | `string(duration("90s"))` | 113 | 373 | 0.30× |
| stringFromInt | `string(i)` | 68 | 225 | 0.30× |
| stringFromIntConst | `string(42)` | 59 | 215 | 0.27× |
| stringFromTimestamp | `string(timestamp("2024-01-01T00:00:00Z"))` | 337 | 888 | 0.38× |
| stringFromUint | `string(u)` | 69 | 226 | 0.30× |
| stringFromUintConst | `string(42u)` | 59 | 218 | 0.27× |
| timestampRoundTrip | `string(timestamp(s))` | 365 | 923 | 0.40× |
| typeOfInt | `type(i) == int` | 82 | 318 | 0.26× |
| typeOfString | `type(s) == string` | 83 | 327 | 0.25× |
| uintFromDouble | `uint(d)` | 61 | 208 | 0.29× |
| uintFromDoubleConst | `uint(42.9)` | 52 | 195 | 0.27× |
| uintFromInt | `uint(i)` | 61 | 208 | 0.29× |
| uintFromIntConst | `uint(42)` | 51 | 192 | 0.27× |
| uintFromString | `uint(s)` | 63 | 219 | 0.29× |
| uintFromStringConst | `uint("42")` | 54 | 194 | 0.28× |

### index

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| listInt | `[10,20,30,40,50][i]` | 134 | 723 | 0.18× |
| listIntConst | `[10,20,30,40,50][4]` | 122 | 709 | 0.17× |
| mapBool | `{true:1,false:0}[k]` | 147 | 511 | 0.29× |
| mapBoolConst | `{true:1,false:0}[true]` | 136 | 500 | 0.27× |
| mapInt | `{1:10,2:20,3:30}[k]` | 169 | 723 | 0.23× |
| mapIntConst | `{1:10,2:20,3:30}[3]` | 158 | 714 | 0.22× |
| mapString | `{"a":1,"b":2,"c":3}[k]` | 199 | 664 | 0.30× |
| mapStringConst | `{"a":1,"b":2,"c":3}["c"]` | 187 | 650 | 0.29× |
| mapUint | `{1u:10,2u:20,3u:30}[k]` | 168 | 725 | 0.23× |
| mapUintConst | `{1u:10,2u:20,3u:30}[3u]` | 160 | 711 | 0.23× |

### lists

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| 5 | `a in ["123","augustine","jess","bob","alice"]` | 162 | 759 | 0.21× |
| 5_lit | `"alice" in ["123","augustine","jess","bob","alice"]` | 151 | 731 | 0.21× |
| 20 | `a in ["alice","bob","carol","dave","eve","frank","grace","h…` | 375 | 1,871 | 0.20× |
| 20_lit | `"tom" in ["alice","bob","carol","dave","eve","frank","grace…` | 353 | 1,864 | 0.19× |
| 100 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 1,411 | 8,175 | 0.17× |
| 100_lit | `"tom4" in ["alice0","bob0","carol0","dave0","eve0","frank0"…` | 1,351 | 8,155 | 0.17× |
| 100_lit_first | `"alice0" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 994 | 8,040 | 0.12× |
| 100_lit_miss | `"nobody" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 1,576 | 8,040 | 0.20× |
| 1000 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 13.2 µs | n/a | n/a |
| 1000_lit | `"tom49" in ["alice0","bob0","carol0","dave0","eve0","frank0…` | 13.1 µs | n/a | n/a |
| bool20 | `x in [false,false,false,false,false,false,false,false,false…` | 336 | 1,879 | 0.18× |
| bound100 | `x in xs` | 411 | 586 | 0.70× |
| bound1000 | `x in xs` | 3,385 | 3,476 | 0.97× |
| bound10000 | `x in xs` | 33.0 µs | 32.5 µs | 1.02× |
| bound100000 | `x in xs` | 328.6 µs | 322.0 µs | 1.02× |
| bound1000000 | `x in xs` | 3.33 ms | 3.22 ms | 1.03× |
| bound1000000_first | `x in xs` | 77 | 988.1 µs | 0.00× |
| bound1000000_miss | `x in xs` | 3.45 ms | 3.22 ms | 1.07× |
| concat | `size([1,2,3] + [4,5])` | 278 | 921 | 0.30× |
| double20 | `x in [1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,10.0,11.0,12.0,13…` | 350 | 1,890 | 0.19× |
| iam100 | `perm in perms` | 622 | 876 | 0.71× |
| iam1000 | `perm in perms` | 5,791 | 6,366 | 0.91× |
| iam1000_first | `perm in perms` | 75 | 1,266 | 0.06× |
| iam1000_miss | `perm in perms` | 5,450 | 6,223 | 0.88× |
| int20 | `x in [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]` | 342 | 1,881 | 0.18× |
| uint20 | `x in [1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,16…` | 344 | 1,876 | 0.18× |

### literals

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bool | `true` | 33 | 109 | 0.30× |
| double | `3.14` | 34 | 108 | 0.31× |
| int | `42` | 33 | 107 | 0.31× |
| null | `null` | 32 | 107 | 0.30× |
| string | `"hello"` | 32 | 115 | 0.28× |

### logic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| and2 | `a && b` | 62 | 224 | 0.28× |
| and2Const | `true && true` | 43 | 197 | 0.22× |
| and10Terms | `a && b && c && d && e && f && g && h && i && j` | 270 | 990 | 0.27× |
| andNoShortCircuit | `a && s.contains("yyy")` | 93 | 328 | 0.28× |
| andShortCircuit | `a && s.contains("yyy")` | 48 | 328 | 0.15× |
| not1 | `!a` | 42 | 221 | 0.19× |
| not1Const | `!false` | 35 | 208 | 0.17× |
| not3 | `!!!a` | 42 | 220 | 0.19× |
| not3Const | `!!!false` | 34 | 208 | 0.16× |
| or2 | `a \|\| b` | 62 | 223 | 0.28× |
| or2Const | `false \|\| true` | 43 | 196 | 0.22× |
| or10Terms | `a \|\| b \|\| c \|\| d \|\| e \|\| f \|\| g \|\| h \|\| i \|\| j` | 256 | 985 | 0.26× |
| orNoShortCircuit | `a \|\| s.contains("yyy")` | 93 | 329 | 0.28× |
| orShortCircuit | `a \|\| s.contains("yyy")` | 48 | 327 | 0.15× |

### long_strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| containsLong_N10 | `a.contains("yyy")` | 66 | 222 | 0.30× |
| containsLong_N100 | `a.contains("yyy")` | 66 | 236 | 0.28× |
| containsLong_N1000 | `a.contains("yyy")` | 81 | 361 | 0.22× |
| containsLong_N10000 | `a.contains("yyy")` | 242 | n/a | n/a |
| eqLong_N10_match | `a == "xxxxxxxxxx"` | 56 | 236 | 0.24× |
| eqLong_N10_mismatch | `a == "xxxxxxxxxx"` | 56 | 235 | 0.24× |
| eqLong_N100_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 59 | 240 | 0.25× |
| eqLong_N100_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 59 | 240 | 0.25× |
| eqLong_N100_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 58 | 234 | 0.25× |
| eqLong_N1000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 71 | 309 | 0.23× |
| eqLong_N1000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 73 | 309 | 0.24× |
| eqLong_N1000_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 58 | 245 | 0.24× |
| eqLong_N1000_mismatch_len | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 57 | 244 | 0.24× |
| eqLong_N10000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 240 | n/a | n/a |
| eqLong_N10000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 239 | n/a | n/a |

### maps

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| dotField | `{"k":1}.k` | 103 | n/a | n/a |
| hasKey | `has({"a":1}.a)` | 99 | n/a | n/a |
| inBool | `k in {true:1,false:0}` | 149 | 523 | 0.28× |
| inBoolConst | `true in {true:1,false:0}` | 138 | 514 | 0.27× |
| inInt | `k in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 408 | 2,446 | 0.17× |
| inIntConst | `10 in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 354 | 2,404 | 0.15× |
| inString | `k in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i":9…` | 427 | 1,924 | 0.22× |
| inStringConst | `"j" in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i"…` | 416 | 1,911 | 0.22× |
| inUint | `k in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 362 | 2,420 | 0.15× |
| inUintConst | `10u in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 350 | 2,413 | 0.15× |

### policies

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| arena_map_gate | `c.age >= {"us": 21, "de": 18}["us"]` | 219 | 633 | 0.35× |
| authz_basic | `(c.is_premium && c.age >= 18 && c.name in ["Ada", "Grace", …` | 280 | 1,104 | 0.25× |
| authz_deep | `(m.inner.b && m.inner.i64 >= 18 && m.inner.inner.s in ["Ada…` | 432 | 1,162 | 0.37× |
| authz_deep8 | `(m.inner.inner.inner.inner.inner.inner.inner.b && m.inner.i…` | 972 | 1,624 | 0.60× |
| mega100 | `(m.i64 + m.str_to_i32["q1"] + m.rep_i32[1]) == -1 ? "deny" …` | 9,175 | 20.0 µs | 0.46× |
| premium_gate | `c.is_premium ? c.age : 0` | 112 | 259 | 0.43× |
| quota_check | `m.str_to_i32["used"] + m.str_to_i32["pending"] < m.str_to_i…` | 322 | 919 | 0.35× |
| risk_score | `c.credit_score >= 700.0 && c.balance_cents > 1000u` | 164 | 430 | 0.38× |
| str_in_list | `m.s in ["alpha", "beta", "gamma", "delta"]` | 179 | 805 | 0.22× |
| ternary2 | `c.age > 30 ? (c.is_premium ? "gold" : "silver") : "basic"` | 151 | 431 | 0.35× |
| ternary5 | `c.age > 60 ? "a" : c.age > 50 ? "b" : c.age > 40 ? "c" : c.…` | 307 | 878 | 0.35× |
| tier_route | `c.balance_cents >= 100000u ? "platinum" : (c.is_premium ? "…` | 251 | 906 | 0.28× |

### proto

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| construct_name | `celwasm.testdata.Customer{name: "Ada"}.name` | 159 | 347 | 0.46× |
| cust_age | `c.age` | 63 | 152 | 0.42× |
| cust_is_premium | `c.is_premium` | 63 | 151 | 0.42× |
| cust_name | `c.name` | 73 | 262 | 0.28× |
| map_i64_str | `m.i64_to_str[2]` | 104 | 450 | 0.23× |
| map_str_i32 | `m.str_to_i32["b"]` | 104 | 345 | 0.30× |
| map_str_msg_i64 | `m.str_to_msg["k"].i64` | 148 | 377 | 0.39× |
| metadata_b | `c.metadata["b"]` | 115 | 453 | 0.25× |
| pair_list_arena | `[10, 20, 30, 40, 50][2]` | 120 | 712 | 0.17× |
| pair_map_arena | `{"a": 1, "b": 2, "c": 3}["b"]` | 191 | 634 | 0.30× |
| read_b | `m.b` | 64 | 151 | 0.42× |
| read_f64 | `m.f64` | 65 | 151 | 0.43× |
| read_s | `m.s` | 76 | 269 | 0.28× |
| read_u64 | `m.u64` | 65 | 151 | 0.43× |
| reads5 | `m.i32 + m.i64 + m.si32 + m.si64 + m.sfx32` | 330 | 574 | 0.57× |
| reads10 | `m.i32 + m.i64 + … + m.si32 + m.si64 (10 terms)` | 676 | 1,096 | 0.62× |
| reads100 | `m.i32 + m.i64 + … + m.si32 + m.si64 (100 terms)` | 6,437 | 10.6 µs | 0.61× |
| rep_i32_at0 | `m.rep_i32[0]` | 97 | 290 | 0.33× |
| rep_i32_at9 | `m.rep_i32[9]` | 95 | 290 | 0.33× |
| rep_msg_at1_s | `m.rep_msg[1].s` | 150 | 469 | 0.32× |
| select_depth1 | `m.i64` | 65 | 152 | 0.43× |
| select_depth2 | `m.inner.i64` | 100 | 198 | 0.51× |
| select_depth4 | `m.inner.inner.inner.i64` | 175 | 275 | 0.64× |
| select_depth8 | `m.inner.inner.inner.inner.inner.inner.inner.i64` | 302 | 433 | 0.70× |
| select_depth16 | `m.inner.inner.inner.inner.inner.inner.inner.inner.inner.inn…` | 566 | 746 | 0.76× |
| tags_at2 | `c.tags[2]` | 104 | 407 | 0.26× |

### size

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytes | `size(x)` | 58 | 217 | 0.27× |
| bytesConst | `size(b"0123456789abcdef")` | 53 | 193 | 0.28× |
| list10 | `size([1,2,3,4,5,6,7,8,9,10])` | 193 | 1,076 | 0.18× |
| list100 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 999 | 8,027 | 0.12× |
| list1000 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 8,585 | n/a | n/a |
| map10 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10})` | 371 | 2,229 | 0.17× |
| map100 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10,11:11,12:12…` | 2,854 | 120.9 µs | 0.02× |
| string | `size(s)` | 63 | 225 | 0.28× |
| stringConst | `size("abcdefghijklmnopqrstuvwxyz")` | 60 | 201 | 0.30× |

### strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesConcat2 | `a + b` | 90 | 265 | 0.34× |
| bytesConcat2Const | `b"ab" + b"cd"` | 73 | 222 | 0.33× |
| concat2 | `a + b` | 91 | 271 | 0.34× |
| concat2Const | `"hello " + "world"` | 74 | 225 | 0.33× |
| concatChain10Terms | `a + b + c + d + e + f + g + h + i + j` | 534 | 1,329 | 0.40× |
| concatChain100Terms | `a + b + … + i + j (100 terms)` | 6,283 | 11.4 µs | 0.55× |
| concatChain1000Terms | `a + b + … + i + j (1000 terms)` | 121.9 µs | n/a | n/a |
| contains | `a.contains("aug")` | 67 | 223 | 0.30× |
| containsConst | `"augustine".contains("aug")` | 57 | 201 | 0.28× |
| endsWith | `a.endsWith("ine")` | 65 | 220 | 0.29× |
| endsWithConst | `"augustine".endsWith("ine")` | 55 | 198 | 0.28× |
| eqConst | `"hello" == "world"` | 45 | 215 | 0.21× |
| eqVar | `a == "augustine"` | 57 | 236 | 0.24× |
| matchesCheap | `a.matches("^aug")` | 1,518 | 246 | 6.18× |
| matchesCheapConst | `"augustine".matches("^aug")` | 1,475 | 226 | 6.53× |
| matchesComplex | `a.matches("^[a-z]+-[0-9]{2,4}@[a-z]+\\.(com\|org)$")` | 9,039 | 318 | 28.40× |
| matchesComplexConst | `"user-1234@example.com".matches("^[a-z]+-[0-9]{2,4}@[a-z]+\…` | 8,958 | 296 | 30.24× |
| startsWith | `a.startsWith("aug")` | 65 | 221 | 0.30× |
| startsWithConst | `"augustine".startsWith("aug")` | 55 | 195 | 0.28× |

### ternary

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| intComputedCond | `a > b ? x : y` | 97 | 325 | 0.30× |
| intConst | `1 > 2 ? 10 : 20` | 69 | 268 | 0.26× |
| intVarCond | `c ? x : y` | 64 | n/a | n/a |
| nested3 | `a > b ? "gt" : (a == b ? "eq" : "lt")` | 127 | 486 | 0.26× |
| stringComputedCond | `a > b ? s : t` | 96 | 346 | 0.28× |
| stringConst | `3 > 2 ? "yes" : "no"` | 71 | 279 | 0.25× |

### time

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| durAddDur | `(duration("90s") + duration("30s")).getSeconds()` | 151 | 579 | 0.26× |
| durAddTs | `int(duration("1h") + timestamp("2024-01-01T00:00:00Z"))` | 237 | 786 | 0.30× |
| durGetHours | `duration("3723s").getHours()` | 85 | 331 | 0.26× |
| durGetSeconds | `duration("3723s").getSeconds()` | 84 | 331 | 0.25× |
| durSubDur | `(duration("90s") - duration("30s")).getSeconds()` | 150 | 581 | 0.26× |
| tsAddDur | `int(timestamp("2024-01-01T00:00:00Z") + duration("1h"))` | 234 | 790 | 0.30× |
| tsGetDayOfWeekUtc | `timestamp("2024-06-15T10:30:45Z").getDayOfWeek()` | 196 | 563 | 0.35× |
| tsGetFullYearTz | `timestamp("2024-06-15T10:30:45Z").getFullYear("America/New_…` | 220 | 615 | 0.36× |
| tsGetFullYearUtc | `timestamp("2024-06-15T10:30:45Z").getFullYear()` | 193 | 562 | 0.34× |
| tsGetFullYearUtcMax | `timestamp("9999-12-31T23:59:59Z").getFullYear()` | 222 | 574 | 0.39× |
| tsGetHoursTz | `timestamp("2024-06-15T10:30:45Z").getHours("America/New_Yor…` | 220 | 606 | 0.36× |
| tsGetHoursUtc | `timestamp("2024-06-15T10:30:45Z").getHours()` | 194 | 558 | 0.35× |
| tsGetSecondsTz | `timestamp("2024-06-15T10:30:45Z").getSeconds("America/New_Y…` | 219 | 610 | 0.36× |
| tsGetSecondsUtc | `timestamp("2024-06-15T10:30:45Z").getSeconds()` | 192 | 559 | 0.34× |
| tsSubDur | `int(timestamp("2024-01-01T01:00:00Z") - duration("1h"))` | 232 | 797 | 0.29× |
| tsSubTs | `(timestamp("2024-01-01T01:00:00Z") - timestamp("2024-01-01T…` | 330 | 994 | 0.33× |

### Per-operator headline — T(N) = setup + N·per_op

Linear regression over each length-sweep family; slope is the steady-state cost of one more operation, crossover is the expression length where the comparator overtakes cel-cpp.

| surface | operator family | points | cel-cpp slope | cel-cpp setup | celwasm-dynamic slope | celwasm-dynamic setup | celwasm-dynamic crossover vs cel-cpp |
|---|---|---|---|---|---|---|---|
| arithmetic | doubleAdd | 5 | 30.8 | -30 | 77.7 | 174 | never wins |
| arithmetic | intAdd | 5 | 34.0 | -197 | 82.7 | 214 | never wins |
| arithmetic | intMul | 5 | 32.9 | -115 | 82.3 | 214 | never wins |
| arithmetic | intSub | 5 | 33.1 | -184 | 78.9 | 42 | never wins |
| comprehensions | all | 4 | 66.5 | 117 | 324.1 | 251 | never wins |
| lists | bound | 5 | 3.3 | -1,062 | 3.2 | 224 | N ≈ 11,562 |
| long_strings | containsLong_N | 4 | 0.0 | 64 | 0.1 | 221 | never wins |
| proto | reads | 3 | 64.2 | 21 | 105.5 | 44 | never wins |
| proto | select_depth | 5 | 33.3 | 35 | 39.4 | 116 | never wins |
| size | list | 3 | 8.5 | 130 | n/a | n/a | n/a |
| strings | concatChain | 3 | 125.0 | -3,336 | n/a | n/a | n/a |

