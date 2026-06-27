## Eval benchmark results — 2026-06-27, Mac (dynamic)

Eval steady-state, median real time ns/call (lower is better); `×cel-cpp` > 1.0 means that comparator is faster than cel-cpp.  `n/a` = cell does not run on that comparator (see skip tags in the corpus YAML).

### arithmetic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| abcAbcShapeLit | `1 + 2 + 3 + 1 + 2 + 3` | 156 | 504 | 0.31× |
| abcAbcShapeVars | `a + b + c + a + b + c` | 210 | 545 | 0.39× |
| doubleAdd2 | `a + b` | 72 | 221 | 0.33× |
| doubleAdd2Const | `1.0 + 1.0` | 54 | 191 | 0.28× |
| doubleAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 326 | 915 | 0.36× |
| doubleAdd10TermsConst | `1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0` | 231 | 801 | 0.29× |
| doubleAdd50Terms | `a + b + … + i + j (50 terms)` | 1,516 | 3,932 | 0.39× |
| doubleAdd50TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (50 terms)` | 1,048 | 3,795 | 0.28× |
| doubleAdd250Terms | `a + b + … + i + j (250 terms)` | 7,773 | 19.3 µs | 0.40× |
| doubleAdd250TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (250 terms)` | 5,135 | 19.8 µs | 0.26× |
| doubleAdd1000Terms | `a + b + … + i + j (1000 terms)` | 30.8 µs | 76.2 µs | 0.40× |
| doubleAdd1000TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (1000 terms)` | 21.6 µs | 78.6 µs | 0.27× |
| doubleDiv_simple | `a / b` | 73 | 220 | 0.33× |
| doubleDiv_simpleConst | `3.0 / 2.0` | 53 | 192 | 0.28× |
| doubleMul_simple | `a * b` | 74 | 222 | 0.33× |
| doubleMul_simpleConst | `3.14 * 2.0` | 54 | 191 | 0.28× |
| doubleNeg | `-a` | 56 | 210 | 0.26× |
| doubleNegConst | `-3.14` | 33 | 108 | 0.31× |
| doubleSub_simple | `a - b` | 74 | 220 | 0.34× |
| doubleSub_simpleConst | `3.5 - 1.25` | 55 | 192 | 0.29× |
| intAdd2 | `a + b` | 73 | 224 | 0.33× |
| intAdd2Const | `1 + 1` | 55 | 193 | 0.28× |
| intAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 330 | 936 | 0.35× |
| intAdd10TermsConst | `1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1` | 237 | 808 | 0.29× |
| intAdd50Terms | `a + b + … + i + j (50 terms)` | 1,571 | 4,030 | 0.39× |
| intAdd50TermsConst | `1 + 1 + … + 1 + 1 (50 terms)` | 1,101 | 3,866 | 0.28× |
| intAdd250Terms | `a + b + … + i + j (250 terms)` | 8,006 | 19.2 µs | 0.42× |
| intAdd250TermsConst | `1 + 1 + … + 1 + 1 (250 terms)` | 5,356 | 19.5 µs | 0.28× |
| intAdd1000Terms | `a + b + … + i + j (1000 terms)` | 32.0 µs | 78.0 µs | 0.41× |
| intAdd1000TermsConst | `1 + 1 + … + 1 + 1 (1000 terms)` | 23.0 µs | 77.1 µs | 0.30× |
| intAddDeepTree | `((a + b) + (c + d)) + ((e + f) + (g + h))` | 299 | 752 | 0.40× |
| intDiv_simple | `a / b` | 77 | 221 | 0.35× |
| intDiv_simpleConst | `84 / 2` | 56 | 194 | 0.29× |
| intMixedOps3 | `(a + b) * c - d` | 148 | 403 | 0.37× |
| intMod_simple | `a % b` | 76 | 221 | 0.34× |
| intMod_simpleConst | `100 % 7` | 55 | 200 | 0.28× |
| intMul2 | `a * b` | 77 | 226 | 0.34× |
| intMul2Const | `1 * 1` | 55 | 195 | 0.28× |
| intMul10Terms | `a * b * c * d * e * f * g * h * i * j` | 330 | 953 | 0.35× |
| intMul10TermsConst | `1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1` | 226 | 831 | 0.27× |
| intMul50Terms | `a * b * … * i * j (50 terms)` | 1,534 | 4,083 | 0.38× |
| intMul50TermsConst | `1 * 1 * … * 1 * 1 (50 terms)` | 977 | 4,026 | 0.24× |
| intMul250Terms | `a * b * … * i * j (250 terms)` | 7,495 | 20.1 µs | 0.37× |
| intMul250TermsConst | `1 * 1 * … * 1 * 1 (250 terms)` | 4,769 | 20.1 µs | 0.24× |
| intMul1000Terms | `a * b * … * i * j (1000 terms)` | 31.0 µs | 80.6 µs | 0.38× |
| intMul1000TermsConst | `1 * 1 * … * 1 * 1 (1000 terms)` | 20.5 µs | 79.2 µs | 0.26× |
| intNeg | `-a` | 58 | 211 | 0.27× |
| intNegConst | `-42` | 33 | 109 | 0.30× |
| intSub2 | `a - b` | 73 | 224 | 0.33× |
| intSub2Const | `1 - 1` | 55 | 194 | 0.28× |
| intSub10Terms | `a - b - c - d - e - f - g - h - i - j` | 331 | 936 | 0.35× |
| intSub10TermsConst | `1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1` | 233 | 811 | 0.29× |
| intSub50Terms | `a - b - … - i - j (50 terms)` | 1,529 | 4,009 | 0.38× |
| intSub50TermsConst | `1 - 1 - … - 1 - 1 (50 terms)` | 1,049 | 3,847 | 0.27× |
| intSub250Terms | `a - b - … - i - j (250 terms)` | 7,676 | 19.4 µs | 0.39× |
| intSub250TermsConst | `1 - 1 - … - 1 - 1 (250 terms)` | 5,010 | 19.5 µs | 0.26× |
| intSub1000Terms | `a - b - … - i - j (1000 terms)` | 31.8 µs | 77.5 µs | 0.41× |
| intSub1000TermsConst | `1 - 1 - … - 1 - 1 (1000 terms)` | 21.2 µs | 77.8 µs | 0.27× |
| polyMix1000Terms | `a*d + b*a + … + i*j + j*g (1000 terms)` | 63.9 µs | 155.6 µs | 0.41× |
| uintAdd_simple | `a + b` | 72 | 223 | 0.32× |
| uintAdd_simpleConst | `1u + 2u` | 56 | 193 | 0.29× |
| uintDiv_simple | `a / b` | 76 | 220 | 0.35× |
| uintDiv_simpleConst | `6u / 2u` | 56 | 194 | 0.29× |
| uintMod_simple | `a % b` | 76 | 221 | 0.34× |
| uintMod_simpleConst | `7u % 3u` | 55 | 193 | 0.29× |
| uintMul_simple | `a * b` | 75 | 222 | 0.34× |
| uintMul_simpleConst | `6u * 7u` | 55 | 195 | 0.28× |
| uintSub_simple | `a - b` | 76 | 220 | 0.34× |
| uintSub_simpleConst | `3u - 1u` | 56 | 191 | 0.29× |

### comparisons

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| boolEq | `a == b` | 62 | 241 | 0.26× |
| boolEqConst | `true == true` | 45 | 213 | 0.21× |
| boolNe | `a != b` | 63 | 240 | 0.26× |
| boolNeConst | `true != false` | 45 | 213 | 0.21× |
| bytesEq | `a == b` | 65 | 264 | 0.24× |
| bytesEqConst | `b"a" == b"a"` | 48 | 214 | 0.22× |
| bytesGe | `a >= b` | 79 | 245 | 0.32× |
| bytesGeConst | `b"b" >= b"b"` | 60 | 194 | 0.31× |
| bytesGt | `a > b` | 80 | 245 | 0.32× |
| bytesGtConst | `b"b" > b"a"` | 61 | 194 | 0.32× |
| bytesLe | `a <= b` | 80 | 246 | 0.33× |
| bytesLeConst | `b"a" <= b"a"` | 60 | 194 | 0.31× |
| bytesLt | `a < b` | 79 | 244 | 0.33× |
| bytesLtConst | `b"a" < b"b"` | 60 | 195 | 0.31× |
| bytesNe | `a != b` | 65 | 262 | 0.25× |
| bytesNeConst | `b"a" != b"b"` | 48 | 213 | 0.22× |
| doubleEq | `a == b` | 63 | 237 | 0.27× |
| doubleEqConst | `1.5 == 1.5` | 46 | 212 | 0.22× |
| doubleGe | `a >= b` | 75 | 220 | 0.34× |
| doubleGeConst | `3.5 >= 3.5` | 56 | 193 | 0.29× |
| doubleGt | `a > b` | 73 | 220 | 0.33× |
| doubleGtConst | `3.5 > 2.5` | 57 | 193 | 0.29× |
| doubleLe | `a <= b` | 74 | 220 | 0.34× |
| doubleLeConst | `2.5 <= 2.5` | 56 | 193 | 0.29× |
| doubleLt | `a < b` | 75 | 221 | 0.34× |
| doubleLtConst | `1.5 < 2.5` | 55 | 194 | 0.29× |
| doubleNe | `a != b` | 63 | 238 | 0.26× |
| doubleNeConst | `1.5 != 2.5` | 46 | 212 | 0.22× |
| durEq | `duration("60s") == duration("1m")` | 115 | 507 | 0.23× |
| durGe | `duration("1m") >= duration("60s")` | 127 | 491 | 0.26× |
| durGt | `duration("2m") > duration("1m")` | 134 | 484 | 0.28× |
| durLe | `duration("1m") <= duration("60s")` | 127 | 492 | 0.26× |
| durLt | `duration("1m") < duration("2m")` | 134 | 484 | 0.28× |
| durNe | `duration("60s") != duration("2m")` | 116 | 507 | 0.23× |
| intEq | `a == b` | 63 | 240 | 0.26× |
| intEqConst | `42 == 42` | 46 | 214 | 0.21× |
| intGe | `a >= b` | 72 | 224 | 0.32× |
| intGeConst | `3 >= 3` | 54 | 194 | 0.28× |
| intGt | `a > b` | 72 | 220 | 0.33× |
| intGtConst | `3 > 2` | 55 | 194 | 0.28× |
| intLe | `a <= b` | 73 | 223 | 0.33× |
| intLeConst | `2 <= 2` | 55 | 193 | 0.28× |
| intLt | `a < b` | 72 | 224 | 0.32× |
| intLtChain20 | `a<b && b<c && … && s<t && t<u (20 terms)` | 1,145 | 3,371 | 0.34× |
| intLtChain20Const | `1<2 && 2<3 && … && 19<20 && 20<21 (20 terms)` | 792 | 3,077 | 0.26× |
| intLtConst | `1 < 2` | 54 | 193 | 0.28× |
| intLtDouble | `a < b` | 74 | n/a | n/a |
| intNe | `a != b` | 63 | 238 | 0.26× |
| intNeConst | `42 != 43` | 45 | 212 | 0.21× |
| listEq | `[1,2,3] == [1,2,3]` | 221 | 299 | 0.74× |
| listNe | `[1,2,3] != [1,2,4]` | 209 | 300 | 0.70× |
| mapEq | `{"a":1,"b":2} == {"b":2,"a":1}` | 398 | 305 | 1.31× |
| mapNe | `{"a":1} != {"a":2}` | 222 | 254 | 0.87× |
| nullEq | `null == null` | 45 | 212 | 0.21× |
| stringEq | `a == b` | 64 | 263 | 0.24× |
| stringEqConst | `"a" == "a"` | 48 | 212 | 0.22× |
| stringGe | `a >= b` | 81 | 248 | 0.33× |
| stringGeConst | `"b" >= "b"` | 61 | 194 | 0.31× |
| stringGt | `a > b` | 77 | 247 | 0.31× |
| stringGtConst | `"b" > "a"` | 59 | 194 | 0.30× |
| stringLe | `a <= b` | 80 | 249 | 0.32× |
| stringLeConst | `"a" <= "a"` | 61 | 194 | 0.32× |
| stringLt | `a < b` | 79 | 246 | 0.32× |
| stringLtConst | `"a" < "b"` | 62 | 194 | 0.32× |
| stringNe | `a != b` | 65 | 262 | 0.25× |
| stringNeConst | `"a" != "b"` | 47 | 214 | 0.22× |
| tsEq | `timestamp("2024-01-01T00:00:00Z") == timestamp("2024-01-01T…` | 292 | 900 | 0.32× |
| tsGe | `timestamp("2024-01-01T00:00:00Z") >= timestamp("2024-01-01T…` | 313 | 882 | 0.36× |
| tsGt | `timestamp("2024-01-02T00:00:00Z") > timestamp("2024-01-01T0…` | 318 | 898 | 0.35× |
| tsLe | `timestamp("2024-01-01T00:00:00Z") <= timestamp("2024-01-01T…` | 313 | 904 | 0.35× |
| tsLt | `timestamp("2024-01-01T00:00:00Z") < timestamp("2024-01-02T0…` | 321 | 901 | 0.36× |
| tsNe | `timestamp("2024-01-01T00:00:00Z") != timestamp("2024-01-02T…` | 302 | 917 | 0.33× |
| uintEq | `a == b` | 62 | 238 | 0.26× |
| uintEqConst | `42u == 42u` | 46 | 214 | 0.21× |
| uintGe | `a >= b` | 72 | 220 | 0.33× |
| uintGeConst | `3u >= 3u` | 53 | 195 | 0.27× |
| uintGt | `a > b` | 72 | 221 | 0.33× |
| uintGtConst | `3u > 2u` | 56 | 192 | 0.29× |
| uintLe | `a <= b` | 72 | 222 | 0.32× |
| uintLeConst | `2u <= 2u` | 54 | 194 | 0.28× |
| uintLt | `a < b` | 73 | 221 | 0.33× |
| uintLtConst | `1u < 2u` | 54 | 194 | 0.28× |
| uintNe | `a != b` | 62 | 238 | 0.26× |
| uintNeConst | `42u != 43u` | 46 | 212 | 0.22× |

### comprehensions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| all10 | `[1,2,3,4,5,6,7,8,9,10].all(x, x > 0)` | 808 | 2,542 | 0.32× |
| all20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].all(x,…` | 1,464 | 4,896 | 0.30× |
| all100 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 6,709 | 23.6 µs | 0.28× |
| all1000 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 65.4 µs | 235.0 µs | 0.28× |
| exists20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,251 | 5,289 | 0.24× |
| existsMapKey | `{1:"a",2:"b",3:"c"}.exists(k, k == 2)` | 350 | 1,274 | 0.27× |
| existsOne20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,177 | 5,607 | 0.21× |
| filter20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].f…` | 3,349 | 6,172 | 0.54× |
| map20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].m…` | 5,440 | 3,672 | 1.48× |
| mapLookupLoop64 | `[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22…` | 161.7 µs | 25.0 µs | 6.48× |
| mapLookupLoop256 | `[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22…` | 2.83 ms | 99.1 µs | 28.58× |

### conversions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesFromString | `bytes(s)` | 71 | 222 | 0.32× |
| bytesFromStringConst | `bytes("abc")` | 62 | 198 | 0.31× |
| doubleFromInt | `double(i)` | 56 | 209 | 0.27× |
| doubleFromIntConst | `double(42)` | 49 | 193 | 0.25× |
| doubleFromString | `double(s)` | 80 | 235 | 0.34× |
| doubleFromStringConst | `double("42.5")` | 70 | 212 | 0.33× |
| doubleFromUint | `double(u)` | 57 | 210 | 0.27× |
| doubleFromUintConst | `double(42u)` | 49 | 193 | 0.25× |
| durationRoundTrip | `string(duration(s))` | 123 | 392 | 0.31× |
| intFromDouble | `int(d)` | 62 | 212 | 0.29× |
| intFromDoubleConst | `int(42.9)` | 53 | 196 | 0.27× |
| intFromString | `int(s)` | 65 | 220 | 0.30× |
| intFromStringConst | `int("42")` | 56 | 194 | 0.29× |
| intFromStringNested | `int(string(123))` | 84 | 296 | 0.28× |
| intFromTimestamp | `int(timestamp("2024-01-01T00:00:00Z"))` | 169 | 537 | 0.32× |
| intFromUint | `int(u)` | 61 | 210 | 0.29× |
| intFromUintConst | `int(42u)` | 53 | 193 | 0.27× |
| stringFromBool | `string(x)` | 62 | 221 | 0.28× |
| stringFromBoolConst | `string(true)` | 54 | 205 | 0.26× |
| stringFromBytes | `string(x)` | 69 | 224 | 0.31× |
| stringFromBytesConst | `string(b"abc")` | 60 | 196 | 0.30× |
| stringFromDouble | `string(d)` | 102 | 377 | 0.27× |
| stringFromDoubleConst | `string(42.5)` | 93 | 365 | 0.25× |
| stringFromDuration | `string(duration("90s"))` | 113 | 372 | 0.30× |
| stringFromInt | `string(i)` | 67 | 228 | 0.29× |
| stringFromIntConst | `string(42)` | 58 | 213 | 0.27× |
| stringFromTimestamp | `string(timestamp("2024-01-01T00:00:00Z"))` | 333 | 872 | 0.38× |
| stringFromUint | `string(u)` | 68 | 227 | 0.30× |
| stringFromUintConst | `string(42u)` | 59 | 212 | 0.28× |
| timestampRoundTrip | `string(timestamp(s))` | 354 | 919 | 0.38× |
| typeOfInt | `type(i) == int` | 83 | 318 | 0.26× |
| typeOfString | `type(s) == string` | 84 | 329 | 0.25× |
| uintFromDouble | `uint(d)` | 61 | 217 | 0.28× |
| uintFromDoubleConst | `uint(42.9)` | 52 | 197 | 0.27× |
| uintFromInt | `uint(i)` | 60 | 212 | 0.28× |
| uintFromIntConst | `uint(42)` | 52 | 195 | 0.27× |
| uintFromString | `uint(s)` | 64 | 227 | 0.28× |
| uintFromStringConst | `uint("42")` | 54 | 196 | 0.28× |

### index

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| listInt | `[10,20,30,40,50][i]` | 149 | 227 | 0.66× |
| listIntConst | `[10,20,30,40,50][4]` | 126 | 210 | 0.60× |
| mapBool | `{true:1,false:0}[k]` | 151 | 230 | 0.66× |
| mapBoolConst | `{true:1,false:0}[true]` | 136 | 212 | 0.64× |
| mapInt | `{1:10,2:20,3:30}[k]` | 170 | 290 | 0.58× |
| mapIntConst | `{1:10,2:20,3:30}[3]` | 160 | 274 | 0.58× |
| mapIntN8 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70}[k]` | 384 | 262 | 1.47× |
| mapIntN64 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 2,705 | 262 | 10.34× |
| mapIntN256 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 11.5 µs | 262 | 43.95× |
| mapStrN8 | `{"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40,"k00…` | 374 | 269 | 1.39× |
| mapStrN64 | `{"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40,"k00…` | 2,743 | 266 | 10.32× |
| mapStrN256 | `{"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40,"k00…` | 12.7 µs | 270 | 47.14× |
| mapString | `{"a":1,"b":2,"c":3}[k]` | 195 | 260 | 0.75× |
| mapStringConst | `{"a":1,"b":2,"c":3}["c"]` | 183 | 241 | 0.76× |
| mapUint | `{1u:10,2u:20,3u:30}[k]` | 170 | 288 | 0.59× |
| mapUintConst | `{1u:10,2u:20,3u:30}[3u]` | 169 | 277 | 0.61× |

### lists

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| 5 | `a in ["123","augustine","jess","bob","alice"]` | 159 | 249 | 0.64× |
| 5_lit | `"alice" in ["123","augustine","jess","bob","alice"]` | 151 | 225 | 0.67× |
| 20 | `a in ["alice","bob","carol","dave","eve","frank","grace","h…` | 382 | 260 | 1.47× |
| 20_lit | `"tom" in ["alice","bob","carol","dave","eve","frank","grace…` | 354 | 237 | 1.50× |
| 100 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 1,435 | 356 | 4.03× |
| 100_lit | `"tom4" in ["alice0","bob0","carol0","dave0","eve0","frank0"…` | 1,346 | 306 | 4.40× |
| 100_lit_first | `"alice0" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 959 | 227 | 4.23× |
| 100_lit_miss | `"nobody" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 1,410 | 300 | 4.70× |
| 1000 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 13.5 µs | 1,301 | 10.38× |
| 1000_lit | `"tom49" in ["alice0","bob0","carol0","dave0","eve0","frank0…` | 13.5 µs | 1,330 | 10.18× |
| bool20 | `x in [false,false,false,false,false,false,false,false,false…` | 338 | 244 | 1.38× |
| bound100 | `x in xs` | 423 | 510 | 0.83× |
| bound1000 | `x in xs` | 3,666 | 2,740 | 1.34× |
| bound10000 | `x in xs` | 34.2 µs | 26.2 µs | 1.31× |
| bound100000 | `x in xs` | 336.5 µs | 260.9 µs | 1.29× |
| bound1000000 | `x in xs` | 3.48 ms | 2.63 ms | 1.32× |
| bound1000000_first | `x in xs` | 76 | 991.2 µs | 0.00× |
| bound1000000_miss | `x in xs` | 3.47 ms | 2.63 ms | 1.32× |
| concat | `size([1,2,3] + [4,5])` | 288 | 310 | 0.93× |
| double20 | `x in [1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,10.0,11.0,12.0,13…` | 351 | 246 | 1.43× |
| iam100 | `perm in perms` | 622 | 769 | 0.81× |
| iam1000 | `perm in perms` | 5,992 | 5,472 | 1.10× |
| iam1000_first | `perm in perms` | 76 | 1,266 | 0.06× |
| iam1000_miss | `perm in perms` | 5,613 | 5,076 | 1.11× |
| int20 | `x in [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]` | 354 | 245 | 1.44× |
| uint20 | `x in [1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,16…` | 348 | 245 | 1.42× |

### literals

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bool | `true` | 34 | 108 | 0.32× |
| double | `3.14` | 33 | 107 | 0.31× |
| int | `42` | 33 | 109 | 0.30× |
| null | `null` | 33 | 107 | 0.31× |
| string | `"hello"` | 33 | 116 | 0.28× |

### logic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| and2 | `a && b` | 60 | 225 | 0.27× |
| and2Const | `true && true` | 44 | 195 | 0.22× |
| and10Terms | `a && b && c && d && e && f && g && h && i && j` | 265 | 964 | 0.28× |
| andNoShortCircuit | `a && s.contains("yyy")` | 93 | 327 | 0.29× |
| andShortCircuit | `a && s.contains("yyy")` | 48 | 321 | 0.15× |
| not1 | `!a` | 42 | 223 | 0.19× |
| not1Const | `!false` | 36 | 213 | 0.17× |
| not3 | `!!!a` | 42 | 223 | 0.19× |
| not3Const | `!!!false` | 35 | 212 | 0.17× |
| or2 | `a \|\| b` | 61 | 222 | 0.28× |
| or2Const | `false \|\| true` | 44 | 192 | 0.23× |
| or10Terms | `a \|\| b \|\| c \|\| d \|\| e \|\| f \|\| g \|\| h \|\| i \|\| j` | 256 | 961 | 0.27× |
| orNoShortCircuit | `a \|\| s.contains("yyy")` | 93 | 328 | 0.28× |
| orShortCircuit | `a \|\| s.contains("yyy")` | 51 | 319 | 0.16× |

### long_strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| containsLong_N10 | `a.contains("yyy")` | 67 | 224 | 0.30× |
| containsLong_N100 | `a.contains("yyy")` | 67 | 240 | 0.28× |
| containsLong_N1000 | `a.contains("yyy")` | 83 | 364 | 0.23× |
| containsLong_N10000 | `a.contains("yyy")` | 243 | 1,556 | 0.16× |
| eqLong_N10_match | `a == "xxxxxxxxxx"` | 57 | 235 | 0.24× |
| eqLong_N10_mismatch | `a == "xxxxxxxxxx"` | 57 | 236 | 0.24× |
| eqLong_N100_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 60 | 245 | 0.25× |
| eqLong_N100_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 60 | 240 | 0.25× |
| eqLong_N100_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 58 | 234 | 0.25× |
| eqLong_N1000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 74 | 312 | 0.24× |
| eqLong_N1000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 74 | 312 | 0.24× |
| eqLong_N1000_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 59 | 243 | 0.24× |
| eqLong_N1000_mismatch_len | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 58 | 243 | 0.24× |
| eqLong_N10000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 241 | 947 | 0.25× |
| eqLong_N10000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 241 | 946 | 0.25× |

### maps

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| dotField | `{"k":1}.k` | 110 | n/a | n/a |
| eqIntN64 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 6,978 | 4,676 | 1.49× |
| eqIntN256 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 37.9 µs | 17.6 µs | 2.15× |
| hasKey | `has({"a":1}.a)` | 100 | n/a | n/a |
| inBool | `k in {true:1,false:0}` | 155 | 245 | 0.63× |
| inBoolConst | `true in {true:1,false:0}` | 140 | 224 | 0.63× |
| inInt | `k in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 410 | 276 | 1.49× |
| inIntConst | `10 in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 359 | 261 | 1.37× |
| inIntN8 | `k in {0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70}` | 334 | 276 | 1.21× |
| inIntN64 | `k in {0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:1…` | 3,227 | 277 | 11.66× |
| inIntN256 | `k in {0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:1…` | 20.2 µs | 276 | 73.09× |
| inStrN8 | `k in {"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40…` | 375 | 279 | 1.35× |
| inStrN64 | `k in {"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40…` | 2,835 | 281 | 10.11× |
| inStrN256 | `k in {"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40…` | 17.8 µs | 281 | 63.28× |
| inString | `k in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i":9…` | 466 | 274 | 1.70× |
| inStringConst | `"j" in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i"…` | 427 | 253 | 1.68× |
| inUint | `k in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 364 | 276 | 1.32× |
| inUintConst | `10u in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 353 | 262 | 1.35× |

### policies

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| arena_map_gate | `c.age >= {"us": 21, "de": 18}["us"]` | 232 | 342 | 0.68× |
| authz_basic | `(c.is_premium && c.age >= 18 && c.name in ["Ada", "Grace", …` | 282 | 751 | 0.38× |
| authz_deep | `(m.inner.b && m.inner.i64 >= 18 && m.inner.inner.s in ["Ada…` | 452 | 888 | 0.51× |
| authz_deep8 | `(m.inner.inner.inner.inner.inner.inner.inner.b && m.inner.i…` | 1,041 | 1,365 | 0.76× |
| mega100 | `(m.i64 + m.str_to_i32["q1"] + m.rep_i32[1]) == -1 ? "deny" …` | 9,710 | 19.9 µs | 0.49× |
| premium_gate | `c.is_premium ? c.age : 0` | 118 | 258 | 0.46× |
| quota_check | `m.str_to_i32["used"] + m.str_to_i32["pending"] < m.str_to_i…` | 328 | 923 | 0.36× |
| risk_score | `c.credit_score >= 700.0 && c.balance_cents > 1000u` | 167 | 424 | 0.39× |
| str_in_list | `m.s in ["alpha", "beta", "gamma", "delta"]` | 183 | 370 | 0.50× |
| ternary2 | `c.age > 30 ? (c.is_premium ? "gold" : "silver") : "basic"` | 150 | 429 | 0.35× |
| ternary5 | `c.age > 60 ? "a" : c.age > 50 ? "b" : c.age > 40 ? "c" : c.…` | 300 | 863 | 0.35× |
| tier_route | `c.balance_cents >= 100000u ? "platinum" : (c.is_premium ? "…` | 250 | 904 | 0.28× |

### proto

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| construct_name | `celwasm.testdata.Customer{name: "Ada"}.name` | 161 | 351 | 0.46× |
| cust_age | `c.age` | 65 | 153 | 0.43× |
| cust_is_premium | `c.is_premium` | 65 | 152 | 0.43× |
| cust_name | `c.name` | 76 | 264 | 0.29× |
| map_i64_str | `m.i64_to_str[2]` | 104 | 453 | 0.23× |
| map_str_i32 | `m.str_to_i32["b"]` | 104 | 349 | 0.30× |
| map_str_msg_i64 | `m.str_to_msg["k"].i64` | 147 | 382 | 0.38× |
| metadata_b | `c.metadata["b"]` | 111 | 458 | 0.24× |
| pair_list_arena | `[10, 20, 30, 40, 50][2]` | 126 | 210 | 0.60× |
| pair_map_arena | `{"a": 1, "b": 2, "c": 3}["b"]` | 185 | 223 | 0.83× |
| read_b | `m.b` | 66 | 151 | 0.43× |
| read_f64 | `m.f64` | 67 | 151 | 0.44× |
| read_s | `m.s` | 79 | 265 | 0.30× |
| read_u64 | `m.u64` | 66 | 151 | 0.44× |
| reads5 | `m.i32 + m.i64 + m.si32 + m.si64 + m.sfx32` | 330 | 564 | 0.59× |
| reads10 | `m.i32 + m.i64 + … + m.si32 + m.si64 (10 terms)` | 666 | 1,086 | 0.61× |
| reads100 | `m.i32 + m.i64 + … + m.si32 + m.si64 (100 terms)` | 6,473 | 10.5 µs | 0.62× |
| rep_i32_at0 | `m.rep_i32[0]` | 100 | 291 | 0.34× |
| rep_i32_at9 | `m.rep_i32[9]` | 100 | 293 | 0.34× |
| rep_msg_at1_s | `m.rep_msg[1].s` | 154 | 468 | 0.33× |
| select_depth1 | `m.i64` | 69 | 152 | 0.45× |
| select_depth2 | `m.inner.i64` | 102 | 198 | 0.51× |
| select_depth4 | `m.inner.inner.inner.i64` | 181 | 275 | 0.66× |
| select_depth8 | `m.inner.inner.inner.inner.inner.inner.inner.i64` | 318 | 442 | 0.72× |
| select_depth16 | `m.inner.inner.inner.inner.inner.inner.inner.inner.inner.inn…` | 591 | 741 | 0.80× |
| tags_at2 | `c.tags[2]` | 106 | 405 | 0.26× |

### size

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytes | `size(x)` | 57 | 220 | 0.26× |
| bytesConst | `size(b"0123456789abcdef")` | 53 | 199 | 0.27× |
| list10 | `size([1,2,3,4,5,6,7,8,9,10])` | 205 | 210 | 0.98× |
| list100 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 1,031 | 213 | 4.85× |
| list1000 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 8,889 | 210 | 42.41× |
| map10 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10})` | 395 | 209 | 1.89× |
| map100 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10,11:11,12:12…` | 2,906 | 209 | 13.89× |
| string | `size(s)` | 62 | 227 | 0.27× |
| stringConst | `size("abcdefghijklmnopqrstuvwxyz")` | 60 | 201 | 0.30× |

### strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesConcat2 | `a + b` | 91 | 266 | 0.34× |
| bytesConcat2Const | `b"ab" + b"cd"` | 72 | 219 | 0.33× |
| concat2 | `a + b` | 91 | 269 | 0.34× |
| concat2Const | `"hello " + "world"` | 73 | 222 | 0.33× |
| concatChain10Terms | `a + b + c + d + e + f + g + h + i + j` | 544 | 1,314 | 0.41× |
| concatChain100Terms | `a + b + … + i + j (100 terms)` | 6,422 | 11.4 µs | 0.56× |
| concatChain1000Terms | `a + b + … + i + j (1000 terms)` | 128.1 µs | 130.1 µs | 0.98× |
| contains | `a.contains("aug")` | 68 | 225 | 0.30× |
| containsConst | `"augustine".contains("aug")` | 57 | 200 | 0.28× |
| endsWith | `a.endsWith("ine")` | 65 | 221 | 0.29× |
| endsWithConst | `"augustine".endsWith("ine")` | 55 | 195 | 0.28× |
| eqConst | `"hello" == "world"` | 46 | 213 | 0.22× |
| eqVar | `a == "augustine"` | 55 | 235 | 0.23× |
| matchesCheap | `a.matches("^aug")` | 1,535 | 251 | 6.11× |
| matchesCheapConst | `"augustine".matches("^aug")` | 1,536 | 228 | 6.75× |
| matchesComplex | `a.matches("^[a-z]+-[0-9]{2,4}@[a-z]+\\.(com\|org)$")` | 9,147 | 322 | 28.39× |
| matchesComplexConst | `"user-1234@example.com".matches("^[a-z]+-[0-9]{2,4}@[a-z]+\…` | 9,003 | 300 | 30.01× |
| startsWith | `a.startsWith("aug")` | 64 | 222 | 0.29× |
| startsWithConst | `"augustine".startsWith("aug")` | 55 | 195 | 0.28× |

### ternary

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| intComputedCond | `a > b ? x : y` | 99 | 321 | 0.31× |
| intConst | `1 > 2 ? 10 : 20` | 70 | 268 | 0.26× |
| intVarCond | `c ? x : y` | 64 | n/a | n/a |
| nested3 | `a > b ? "gt" : (a == b ? "eq" : "lt")` | 127 | 482 | 0.26× |
| stringComputedCond | `a > b ? s : t` | 98 | 345 | 0.28× |
| stringConst | `3 > 2 ? "yes" : "no"` | 70 | 274 | 0.26× |

### time

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| durAddDur | `(duration("90s") + duration("30s")).getSeconds()` | 150 | 583 | 0.26× |
| durAddTs | `int(duration("1h") + timestamp("2024-01-01T00:00:00Z"))` | 233 | 790 | 0.29× |
| durGetHours | `duration("3723s").getHours()` | 85 | 332 | 0.26× |
| durGetSeconds | `duration("3723s").getSeconds()` | 85 | 331 | 0.26× |
| durSubDur | `(duration("90s") - duration("30s")).getSeconds()` | 148 | 582 | 0.25× |
| tsAddDur | `int(timestamp("2024-01-01T00:00:00Z") + duration("1h"))` | 239 | 791 | 0.30× |
| tsGetDayOfWeekUtc | `timestamp("2024-06-15T10:30:45Z").getDayOfWeek()` | 200 | 558 | 0.36× |
| tsGetFullYearTz | `timestamp("2024-06-15T10:30:45Z").getFullYear("America/New_…` | 222 | 624 | 0.36× |
| tsGetFullYearUtc | `timestamp("2024-06-15T10:30:45Z").getFullYear()` | 195 | 559 | 0.35× |
| tsGetFullYearUtcMax | `timestamp("9999-12-31T23:59:59Z").getFullYear()` | 223 | 584 | 0.38× |
| tsGetHoursTz | `timestamp("2024-06-15T10:30:45Z").getHours("America/New_Yor…` | 225 | 608 | 0.37× |
| tsGetHoursUtc | `timestamp("2024-06-15T10:30:45Z").getHours()` | 196 | 562 | 0.35× |
| tsGetSecondsTz | `timestamp("2024-06-15T10:30:45Z").getSeconds("America/New_Y…` | 224 | 617 | 0.36× |
| tsGetSecondsUtc | `timestamp("2024-06-15T10:30:45Z").getSeconds()` | 197 | 560 | 0.35× |
| tsSubDur | `int(timestamp("2024-01-01T01:00:00Z") - duration("1h"))` | 232 | 787 | 0.29× |
| tsSubTs | `(timestamp("2024-01-01T01:00:00Z") - timestamp("2024-01-01T…` | 325 | 1,014 | 0.32× |

### Per-operator headline — T(N) = setup + N·per_op

Linear regression over each length-sweep family; slope is the steady-state cost of one more operation, crossover is the expression length where the comparator overtakes cel-cpp.

| surface | operator family | points | cel-cpp slope | cel-cpp setup | celwasm-dynamic slope | celwasm-dynamic setup | celwasm-dynamic crossover vs cel-cpp |
|---|---|---|---|---|---|---|---|
| arithmetic | doubleAdd | 5 | 30.8 | 19 | 76.1 | 145 | never wins |
| arithmetic | intAdd | 5 | 32.0 | -4 | 77.9 | 32 | never wins |
| arithmetic | intMul | 5 | 31.0 | -44 | 80.5 | 64 | never wins |
| arithmetic | intSub | 5 | 31.8 | -65 | 77.4 | 117 | never wins |
| comprehensions | all | 4 | 65.3 | 165 | 234.8 | 164 | never wins |
| index | mapIntN | 3 | 45.1 | -64 | 0.0 | 262 | N ≈ 7 |
| index | mapStrN | 3 | 50.3 | -223 | 0.0 | 267 | N ≈ 10 |
| lists | bound | 5 | 3.5 | -2,835 | 2.6 | -484 | N ≈ 2,763 |
| long_strings | containsLong_N | 4 | 0.0 | 66 | 0.1 | 227 | never wins |
| maps | inIntN | 3 | 82.1 | -1,067 | -0.0 | 276 | N ≈ 16 |
| maps | inStrN | 3 | 72.1 | -890 | 0.0 | 280 | N ≈ 16 |
| proto | reads | 3 | 64.6 | 13 | 104.7 | 39 | never wins |
| proto | select_depth | 5 | 34.8 | 36 | 39.2 | 118 | never wins |
| size | list | 3 | 8.8 | 136 | -0.0 | 211 | N ≈ 9 |
| strings | concatChain | 3 | 131.4 | -3,601 | 130.8 | -793 | N ≈ 4,779 |

