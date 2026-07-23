## Eval benchmark results — 2026-07-22, Mac (dynamic)

Eval steady-state, median real time ns/call (lower is better); `×cel-cpp` > 1.0 means that comparator is faster than cel-cpp.  `n/a` = cell does not run on that comparator (see skip tags in the corpus YAML).

### arithmetic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| abcAbcShapeLit | `1 + 2 + 3 + 1 + 2 + 3` | 155 | 498 | 0.31× |
| abcAbcShapeVars | `a + b + c + a + b + c` | 211 | 541 | 0.39× |
| doubleAdd2 | `a + b` | 71 | 219 | 0.32× |
| doubleAdd2Const | `1.0 + 1.0` | 54 | 189 | 0.28× |
| doubleAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 319 | 930 | 0.34× |
| doubleAdd10TermsConst | `1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0 + 1.0` | 225 | 794 | 0.28× |
| doubleAdd50Terms | `a + b + … + i + j (50 terms)` | 1,696 | 3,889 | 0.44× |
| doubleAdd50TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (50 terms)` | 1,034 | 3,790 | 0.27× |
| doubleAdd250Terms | `a + b + … + i + j (250 terms)` | 7,996 | 19.2 µs | 0.42× |
| doubleAdd250TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (250 terms)` | 5,202 | 19.0 µs | 0.27× |
| doubleAdd1000Terms | `a + b + … + i + j (1000 terms)` | 31.2 µs | 76.2 µs | 0.41× |
| doubleAdd1000TermsConst | `1.0 + 1.0 + … + 1.0 + 1.0 (1000 terms)` | 21.8 µs | 77.0 µs | 0.28× |
| doubleDiv_simple | `a / b` | 74 | 219 | 0.34× |
| doubleDiv_simpleConst | `3.0 / 2.0` | 54 | 188 | 0.29× |
| doubleMul_simple | `a * b` | 74 | 220 | 0.34× |
| doubleMul_simpleConst | `3.14 * 2.0` | 55 | 189 | 0.29× |
| doubleNeg | `-a` | 55 | 210 | 0.26× |
| doubleNegConst | `-3.14` | 34 | 107 | 0.32× |
| doubleSub_simple | `a - b` | 75 | 225 | 0.33× |
| doubleSub_simpleConst | `3.5 - 1.25` | 55 | 203 | 0.27× |
| intAdd2 | `a + b` | 72 | 223 | 0.32× |
| intAdd2Const | `1 + 1` | 54 | 190 | 0.29× |
| intAdd10Terms | `a + b + c + d + e + f + g + h + i + j` | 329 | 922 | 0.36× |
| intAdd10TermsConst | `1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1` | 236 | 802 | 0.29× |
| intAdd50Terms | `a + b + … + i + j (50 terms)` | 1,561 | 3,943 | 0.40× |
| intAdd50TermsConst | `1 + 1 + … + 1 + 1 (50 terms)` | 1,094 | 3,834 | 0.29× |
| intAdd250Terms | `a + b + … + i + j (250 terms)` | 7,805 | 19.2 µs | 0.41× |
| intAdd250TermsConst | `1 + 1 + … + 1 + 1 (250 terms)` | 5,599 | 19.2 µs | 0.29× |
| intAdd1000Terms | `a + b + … + i + j (1000 terms)` | 32.5 µs | 77.1 µs | 0.42× |
| intAdd1000TermsConst | `1 + 1 + … + 1 + 1 (1000 terms)` | 25.8 µs | 77.4 µs | 0.33× |
| intAddDeepTree | `((a + b) + (c + d)) + ((e + f) + (g + h))` | 301 | 766 | 0.39× |
| intDiv_simple | `a / b` | 81 | 218 | 0.37× |
| intDiv_simpleConst | `84 / 2` | 58 | 190 | 0.30× |
| intMixedOps3 | `(a + b) * c - d` | 148 | 409 | 0.36× |
| intMod_simple | `a % b` | 76 | 222 | 0.34× |
| intMod_simpleConst | `100 % 7` | 55 | 191 | 0.29× |
| intMul2 | `a * b` | 78 | 225 | 0.35× |
| intMul2Const | `1 * 1` | 56 | 192 | 0.29× |
| intMul10Terms | `a * b * c * d * e * f * g * h * i * j` | 334 | 949 | 0.35× |
| intMul10TermsConst | `1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1 * 1` | 239 | 807 | 0.30× |
| intMul50Terms | `a * b * … * i * j (50 terms)` | 1,535 | 4,319 | 0.36× |
| intMul50TermsConst | `1 * 1 * … * 1 * 1 (50 terms)` | 1,000 | 3,947 | 0.25× |
| intMul250Terms | `a * b * … * i * j (250 terms)` | 8,739 | 19.7 µs | 0.44× |
| intMul250TermsConst | `1 * 1 * … * 1 * 1 (250 terms)` | 4,757 | 19.6 µs | 0.24× |
| intMul1000Terms | `a * b * … * i * j (1000 terms)` | 30.9 µs | 78.6 µs | 0.39× |
| intMul1000TermsConst | `1 * 1 * … * 1 * 1 (1000 terms)` | 20.4 µs | 78.6 µs | 0.26× |
| intNeg | `-a` | 59 | 211 | 0.28× |
| intNegConst | `-42` | 34 | 112 | 0.30× |
| intSub2 | `a - b` | 73 | 223 | 0.33× |
| intSub2Const | `1 - 1` | 55 | 190 | 0.29× |
| intSub10Terms | `a - b - c - d - e - f - g - h - i - j` | 326 | 929 | 0.35× |
| intSub10TermsConst | `1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1 - 1` | 228 | 802 | 0.28× |
| intSub50Terms | `a - b - … - i - j (50 terms)` | 1,537 | 4,025 | 0.38× |
| intSub50TermsConst | `1 - 1 - … - 1 - 1 (50 terms)` | 1,035 | 3,841 | 0.27× |
| intSub250Terms | `a - b - … - i - j (250 terms)` | 7,584 | 19.2 µs | 0.40× |
| intSub250TermsConst | `1 - 1 - … - 1 - 1 (250 terms)` | 5,030 | 19.0 µs | 0.26× |
| intSub1000Terms | `a - b - … - i - j (1000 terms)` | 31.3 µs | 77.1 µs | 0.41× |
| intSub1000TermsConst | `1 - 1 - … - 1 - 1 (1000 terms)` | 21.4 µs | 75.7 µs | 0.28× |
| polyMix1000Terms | `a*d + b*a + … + i*j + j*g (1000 terms)` | 64.6 µs | 156.5 µs | 0.41× |
| uintAdd_simple | `a + b` | 73 | 225 | 0.32× |
| uintAdd_simpleConst | `1u + 2u` | 55 | 189 | 0.29× |
| uintDiv_simple | `a / b` | 77 | 219 | 0.35× |
| uintDiv_simpleConst | `6u / 2u` | 57 | 190 | 0.30× |
| uintMod_simple | `a % b` | 77 | 220 | 0.35× |
| uintMod_simpleConst | `7u % 3u` | 56 | 189 | 0.30× |
| uintMul_simple | `a * b` | 77 | 223 | 0.34× |
| uintMul_simpleConst | `6u * 7u` | 55 | 210 | 0.26× |
| uintSub_simple | `a - b` | 77 | 231 | 0.33× |
| uintSub_simpleConst | `3u - 1u` | 56 | 189 | 0.30× |

### comparisons

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| boolEq | `a == b` | 63 | 238 | 0.27× |
| boolEqConst | `true == true` | 46 | 210 | 0.22× |
| boolNe | `a != b` | 64 | 240 | 0.27× |
| boolNeConst | `true != false` | 46 | 212 | 0.21× |
| bytesEq | `a == b` | 65 | 259 | 0.25× |
| bytesEqConst | `b"a" == b"a"` | 48 | 212 | 0.23× |
| bytesGe | `a >= b` | 80 | 247 | 0.32× |
| bytesGeConst | `b"b" >= b"b"` | 61 | 190 | 0.32× |
| bytesGt | `a > b` | 79 | 240 | 0.33× |
| bytesGtConst | `b"b" > b"a"` | 60 | 190 | 0.32× |
| bytesLe | `a <= b` | 79 | 247 | 0.32× |
| bytesLeConst | `b"a" <= b"a"` | 60 | 196 | 0.31× |
| bytesLt | `a < b` | 79 | 247 | 0.32× |
| bytesLtConst | `b"a" < b"b"` | 60 | 191 | 0.31× |
| bytesNe | `a != b` | 67 | 264 | 0.25× |
| bytesNeConst | `b"a" != b"b"` | 48 | 212 | 0.23× |
| doubleEq | `a == b` | 63 | 244 | 0.26× |
| doubleEqConst | `1.5 == 1.5` | 46 | 212 | 0.22× |
| doubleGe | `a >= b` | 74 | 218 | 0.34× |
| doubleGeConst | `3.5 >= 3.5` | 56 | 191 | 0.29× |
| doubleGt | `a > b` | 78 | 219 | 0.36× |
| doubleGtConst | `3.5 > 2.5` | 56 | 190 | 0.29× |
| doubleLe | `a <= b` | 74 | 221 | 0.33× |
| doubleLeConst | `2.5 <= 2.5` | 56 | 190 | 0.29× |
| doubleLt | `a < b` | 74 | 229 | 0.32× |
| doubleLtConst | `1.5 < 2.5` | 55 | 191 | 0.29× |
| doubleNe | `a != b` | 63 | 241 | 0.26× |
| doubleNeConst | `1.5 != 2.5` | 46 | 214 | 0.21× |
| durEq | `duration("60s") == duration("1m")` | 117 | 510 | 0.23× |
| durGe | `duration("1m") >= duration("60s")` | 127 | 498 | 0.26× |
| durGt | `duration("2m") > duration("1m")` | 140 | 486 | 0.29× |
| durLe | `duration("1m") <= duration("60s")` | 129 | 495 | 0.26× |
| durLt | `duration("1m") < duration("2m")` | 137 | 486 | 0.28× |
| durNe | `duration("60s") != duration("2m")` | 118 | 511 | 0.23× |
| intEq | `a == b` | 66 | 240 | 0.28× |
| intEqConst | `42 == 42` | 46 | 212 | 0.22× |
| intGe | `a >= b` | 72 | 223 | 0.32× |
| intGeConst | `3 >= 3` | 54 | 192 | 0.28× |
| intGt | `a > b` | 72 | 236 | 0.31× |
| intGtConst | `3 > 2` | 54 | 191 | 0.28× |
| intLe | `a <= b` | 72 | 223 | 0.33× |
| intLeConst | `2 <= 2` | 54 | 190 | 0.28× |
| intLt | `a < b` | 73 | 217 | 0.33× |
| intLtChain20 | `a<b && b<c && … && s<t && t<u (20 terms)` | 1,152 | 3,318 | 0.35× |
| intLtChain20Const | `1<2 && 2<3 && … && 19<20 && 20<21 (20 terms)` | 799 | 3,110 | 0.26× |
| intLtConst | `1 < 2` | 54 | 190 | 0.29× |
| intLtDouble | `a < b` | 75 | n/a | n/a |
| intNe | `a != b` | 64 | 242 | 0.26× |
| intNeConst | `42 != 43` | 48 | 219 | 0.22× |
| listEq | `[1,2,3] == [1,2,3]` | 388 | 298 | 1.30× |
| listNe | `[1,2,3] != [1,2,4]` | 499 | 298 | 1.67× |
| mapEq | `{"a":1,"b":2} == {"b":2,"a":1}` | 566 | 320 | 1.77× |
| mapNe | `{"a":1} != {"a":2}` | 248 | 254 | 0.98× |
| nullEq | `null == null` | 48 | 213 | 0.23× |
| stringEq | `a == b` | 69 | 276 | 0.25× |
| stringEqConst | `"a" == "a"` | 52 | 211 | 0.24× |
| stringGe | `a >= b` | 89 | 248 | 0.36× |
| stringGeConst | `"b" >= "b"` | 71 | 191 | 0.37× |
| stringGt | `a > b` | 93 | 245 | 0.38× |
| stringGtConst | `"b" > "a"` | 73 | 191 | 0.38× |
| stringLe | `a <= b` | 81 | 247 | 0.33× |
| stringLeConst | `"a" <= "a"` | 76 | 191 | 0.40× |
| stringLt | `a < b` | 94 | 246 | 0.38× |
| stringLtConst | `"a" < "b"` | 62 | 190 | 0.33× |
| stringNe | `a != b` | 66 | 263 | 0.25× |
| stringNeConst | `"a" != "b"` | 47 | 214 | 0.22× |
| tsEq | `timestamp("2024-01-01T00:00:00Z") == timestamp("2024-01-01T…` | 296 | 975 | 0.30× |
| tsGe | `timestamp("2024-01-01T00:00:00Z") >= timestamp("2024-01-01T…` | 309 | 913 | 0.34× |
| tsGt | `timestamp("2024-01-02T00:00:00Z") > timestamp("2024-01-01T0…` | 321 | 899 | 0.36× |
| tsLe | `timestamp("2024-01-01T00:00:00Z") <= timestamp("2024-01-01T…` | 308 | 894 | 0.34× |
| tsLt | `timestamp("2024-01-01T00:00:00Z") < timestamp("2024-01-02T0…` | 320 | 918 | 0.35× |
| tsNe | `timestamp("2024-01-01T00:00:00Z") != timestamp("2024-01-02T…` | 304 | 953 | 0.32× |
| uintEq | `a == b` | 64 | 238 | 0.27× |
| uintEqConst | `42u == 42u` | 46 | 212 | 0.22× |
| uintGe | `a >= b` | 72 | 217 | 0.33× |
| uintGeConst | `3u >= 3u` | 54 | 189 | 0.28× |
| uintGt | `a > b` | 74 | 218 | 0.34× |
| uintGtConst | `3u > 2u` | 54 | 190 | 0.28× |
| uintLe | `a <= b` | 72 | 219 | 0.33× |
| uintLeConst | `2u <= 2u` | 54 | 202 | 0.27× |
| uintLt | `a < b` | 72 | 220 | 0.33× |
| uintLtConst | `1u < 2u` | 53 | 190 | 0.28× |
| uintNe | `a != b` | 64 | 241 | 0.26× |
| uintNeConst | `42u != 43u` | 46 | 213 | 0.21× |

### comprehensions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| all10 | `[1,2,3,4,5,6,7,8,9,10].all(x, x > 0)` | 830 | 2,530 | 0.33× |
| all20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].all(x,…` | 1,460 | 4,830 | 0.30× |
| all100 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 6,579 | 23.5 µs | 0.28× |
| all1000 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,2…` | 65.5 µs | 248.1 µs | 0.26× |
| exists20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,240 | 5,252 | 0.24× |
| existsMapKey | `{1:"a",2:"b",3:"c"}.exists(k, k == 2)` | 332 | 1,261 | 0.26× |
| existsOne20 | `[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].exists…` | 1,154 | 5,551 | 0.21× |
| filter20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].f…` | 3,323 | 6,106 | 0.54× |
| map20 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20].m…` | 5,279 | 3,594 | 1.47× |
| mapLookupLoop64 | `[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22…` | 162.3 µs | 24.8 µs | 6.54× |
| mapLookupLoop256 | `[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22…` | 2.69 ms | 97.5 µs | 27.56× |

### conversions

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesFromString | `bytes(s)` | 70 | 220 | 0.32× |
| bytesFromStringConst | `bytes("abc")` | 62 | 192 | 0.32× |
| doubleFromInt | `double(i)` | 56 | 207 | 0.27× |
| doubleFromIntConst | `double(42)` | 48 | 190 | 0.25× |
| doubleFromString | `double(s)` | 79 | 235 | 0.33× |
| doubleFromStringConst | `double("42.5")` | 70 | 211 | 0.33× |
| doubleFromUint | `double(u)` | 55 | 210 | 0.26× |
| doubleFromUintConst | `double(42u)` | 49 | 190 | 0.26× |
| durationRoundTrip | `string(duration(s))` | 126 | 397 | 0.32× |
| intFromDouble | `int(d)` | 64 | 210 | 0.30× |
| intFromDoubleConst | `int(42.9)` | 92 | 189 | 0.48× |
| intFromString | `int(s)` | 137 | 222 | 0.62× |
| intFromStringConst | `int("42")` | 139 | 194 | 0.72× |
| intFromStringNested | `int(string(123))` | 152 | 297 | 0.51× |
| intFromTimestamp | `int(timestamp("2024-01-01T00:00:00Z"))` | 197 | 542 | 0.36× |
| intFromUint | `int(u)` | 67 | 211 | 0.32× |
| intFromUintConst | `int(42u)` | 56 | 183 | 0.31× |
| stringFromBool | `string(x)` | 65 | 220 | 0.29× |
| stringFromBoolConst | `string(true)` | 57 | 201 | 0.28× |
| stringFromBytes | `string(x)` | 72 | 221 | 0.33× |
| stringFromBytesConst | `string(b"abc")` | 68 | 195 | 0.35× |
| stringFromDouble | `string(d)` | 121 | 373 | 0.32× |
| stringFromDoubleConst | `string(42.5)` | 115 | 361 | 0.32× |
| stringFromDuration | `string(duration("90s"))` | 132 | 375 | 0.35× |
| stringFromInt | `string(i)` | 69 | 229 | 0.30× |
| stringFromIntConst | `string(42)` | 62 | 211 | 0.29× |
| stringFromTimestamp | `string(timestamp("2024-01-01T00:00:00Z"))` | 338 | 892 | 0.38× |
| stringFromUint | `string(u)` | 72 | 227 | 0.32× |
| stringFromUintConst | `string(42u)` | 59 | 211 | 0.28× |
| timestampRoundTrip | `string(timestamp(s))` | 360 | 932 | 0.39× |
| typeOfInt | `type(i) == int` | 82 | 317 | 0.26× |
| typeOfString | `type(s) == string` | 82 | 331 | 0.25× |
| uintFromDouble | `uint(d)` | 62 | 211 | 0.30× |
| uintFromDoubleConst | `uint(42.9)` | 52 | 191 | 0.27× |
| uintFromInt | `uint(i)` | 61 | 210 | 0.29× |
| uintFromIntConst | `uint(42)` | 51 | 188 | 0.27× |
| uintFromString | `uint(s)` | 64 | 221 | 0.29× |
| uintFromStringConst | `uint("42")` | 54 | 192 | 0.28× |

### index

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| listInt | `[10,20,30,40,50][i]` | 145 | 223 | 0.65× |
| listIntConst | `[10,20,30,40,50][4]` | 125 | 208 | 0.60× |
| mapBool | `{true:1,false:0}[k]` | 152 | 225 | 0.67× |
| mapBoolConst | `{true:1,false:0}[true]` | 144 | 210 | 0.69× |
| mapInt | `{1:10,2:20,3:30}[k]` | 171 | 283 | 0.60× |
| mapIntConst | `{1:10,2:20,3:30}[3]` | 163 | 269 | 0.60× |
| mapIntN8 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70}[k]` | 393 | 259 | 1.52× |
| mapIntN64 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 2,722 | 260 | 10.48× |
| mapIntN256 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 11.2 µs | 260 | 43.27× |
| mapStrN8 | `{"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40,"k00…` | 396 | 266 | 1.49× |
| mapStrN64 | `{"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40,"k00…` | 2,996 | 267 | 11.22× |
| mapStrN256 | `{"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40,"k00…` | 12.6 µs | 265 | 47.68× |
| mapString | `{"a":1,"b":2,"c":3}[k]` | 197 | 256 | 0.77× |
| mapStringConst | `{"a":1,"b":2,"c":3}["c"]` | 198 | 235 | 0.84× |
| mapUint | `{1u:10,2u:20,3u:30}[k]` | 170 | 283 | 0.60× |
| mapUintConst | `{1u:10,2u:20,3u:30}[3u]` | 163 | 269 | 0.61× |

### lists

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| 5 | `a in ["123","augustine","jess","bob","alice"]` | 219 | 248 | 0.88× |
| 5_lit | `"alice" in ["123","augustine","jess","bob","alice"]` | 175 | 223 | 0.78× |
| 20 | `a in ["alice","bob","carol","dave","eve","frank","grace","h…` | 369 | 258 | 1.43× |
| 20_lit | `"tom" in ["alice","bob","carol","dave","eve","frank","grace…` | 349 | 238 | 1.47× |
| 100 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 1,295 | 342 | 3.79× |
| 100_lit | `"tom4" in ["alice0","bob0","carol0","dave0","eve0","frank0"…` | 1,368 | 314 | 4.36× |
| 100_lit_first | `"alice0" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 894 | 222 | 4.02× |
| 100_lit_miss | `"nobody" in ["alice0","bob0","carol0","dave0","eve0","frank…` | 1,506 | 333 | 4.53× |
| 1000 | `a in ["alice0","bob0","carol0","dave0","eve0","frank0","gra…` | 13.1 µs | 1,067 | 12.29× |
| 1000_lit | `"tom49" in ["alice0","bob0","carol0","dave0","eve0","frank0…` | 13.2 µs | 1,160 | 11.37× |
| bool20 | `x in [false,false,false,false,false,false,false,false,false…` | 362 | 243 | 1.49× |
| bound100 | `x in xs` | 455 | 511 | 0.89× |
| bound1000 | `x in xs` | 3,533 | 2,754 | 1.28× |
| bound10000 | `x in xs` | 36.0 µs | 26.1 µs | 1.38× |
| bound100000 | `x in xs` | 338.5 µs | 261.6 µs | 1.29× |
| bound1000000 | `x in xs` | 3.52 ms | 2.60 ms | 1.35× |
| bound1000000_first | `x in xs` | 78 | 993.9 µs | 0.00× |
| bound1000000_miss | `x in xs` | 3.54 ms | 2.64 ms | 1.34× |
| concat | `size([1,2,3] + [4,5])` | 291 | 311 | 0.93× |
| double20 | `x in [1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,10.0,11.0,12.0,13…` | 364 | 243 | 1.50× |
| iam100 | `perm in perms` | 656 | 771 | 0.85× |
| iam1000 | `perm in perms` | 5,984 | 5,350 | 1.12× |
| iam1000_first | `perm in perms` | 79 | 1,266 | 0.06× |
| iam1000_miss | `perm in perms` | 5,574 | 5,248 | 1.06× |
| int20 | `x in [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]` | 396 | 244 | 1.62× |
| intConst1000 | `k in [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,…` | 15.3 µs | 774 | 19.79× |
| intConst1000_first | `k in [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,…` | 10.8 µs | 234 | 45.94× |
| uint20 | `x in [1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,16…` | 408 | 243 | 1.68× |

### literals

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bool | `true` | 42 | 108 | 0.39× |
| double | `3.14` | 45 | 107 | 0.42× |
| int | `42` | 43 | 108 | 0.40× |
| null | `null` | 42 | 106 | 0.40× |
| string | `"hello"` | 43 | 113 | 0.38× |

### logic

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| and2 | `a && b` | 76 | 227 | 0.33× |
| and2Const | `true && true` | 59 | 192 | 0.31× |
| and10Terms | `a && b && c && d && e && f && g && h && i && j` | 352 | 949 | 0.37× |
| andNoShortCircuit | `a && s.contains("yyy")` | 113 | 313 | 0.36× |
| andShortCircuit | `a && s.contains("yyy")` | 60 | 310 | 0.19× |
| not1 | `!a` | 58 | 221 | 0.26× |
| not1Const | `!false` | 46 | 206 | 0.22× |
| not3 | `!!!a` | 55 | 222 | 0.25× |
| not3Const | `!!!false` | 48 | 206 | 0.23× |
| or2 | `a \|\| b` | 81 | 223 | 0.36× |
| or2Const | `false \|\| true` | 61 | 190 | 0.32× |
| or10Terms | `a \|\| b \|\| c \|\| d \|\| e \|\| f \|\| g \|\| h \|\| i \|\| j` | 324 | 948 | 0.34× |
| orNoShortCircuit | `a \|\| s.contains("yyy")` | 118 | 311 | 0.38× |
| orShortCircuit | `a \|\| s.contains("yyy")` | 65 | 318 | 0.20× |

### long_strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| containsLong_N10 | `a.contains("yyy")` | 84 | 221 | 0.38× |
| containsLong_N100 | `a.contains("yyy")` | 89 | 224 | 0.40× |
| containsLong_N1000 | `a.contains("yyy")` | 102 | 264 | 0.38× |
| containsLong_N10000 | `a.contains("yyy")` | 312 | 666 | 0.47× |
| eqLong_N10_match | `a == "xxxxxxxxxx"` | 70 | 236 | 0.30× |
| eqLong_N10_mismatch | `a == "xxxxxxxxxx"` | 77 | 235 | 0.33× |
| eqLong_N100_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 77 | 240 | 0.32× |
| eqLong_N100_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 79 | 240 | 0.33× |
| eqLong_N100_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 76 | 235 | 0.32× |
| eqLong_N1000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 100 | 310 | 0.32× |
| eqLong_N1000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 90 | 311 | 0.29× |
| eqLong_N1000_mismatch_first | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 77 | 243 | 0.32× |
| eqLong_N1000_mismatch_len | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 73 | 243 | 0.30× |
| eqLong_N10000_match | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 295 | 950 | 0.31× |
| eqLong_N10000_mismatch | `a == "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx…` | 298 | 945 | 0.32× |

### maps

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| dotField | `{"k":1}.k` | 126 | n/a | n/a |
| eqIntN64 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 8,507 | 4,592 | 1.85× |
| eqIntN256 | `{0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:100,11…` | 36.4 µs | 17.5 µs | 2.08× |
| hasKey | `has({"a":1}.a)` | 131 | n/a | n/a |
| inBool | `k in {true:1,false:0}` | 193 | 236 | 0.82× |
| inBoolConst | `true in {true:1,false:0}` | 186 | 221 | 0.84× |
| inInt | `k in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 503 | 275 | 1.83× |
| inIntConst | `10 in {1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10}` | 461 | 260 | 1.77× |
| inIntN8 | `k in {0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70}` | 412 | 275 | 1.50× |
| inIntN64 | `k in {0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:1…` | 2,688 | 275 | 9.77× |
| inIntN256 | `k in {0:0,1:10,2:20,3:30,4:40,5:50,6:60,7:70,8:80,9:90,10:1…` | 17.2 µs | 274 | 62.60× |
| inStrN8 | `k in {"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40…` | 482 | 290 | 1.66× |
| inStrN64 | `k in {"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40…` | 2,989 | 280 | 10.68× |
| inStrN256 | `k in {"k0000":0,"k0001":10,"k0002":20,"k0003":30,"k0004":40…` | 12.6 µs | 280 | 45.04× |
| inString | `k in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i":9…` | 554 | 276 | 2.00× |
| inStringConst | `"j" in {"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i"…` | 528 | 251 | 2.10× |
| inUint | `k in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 459 | 278 | 1.65× |
| inUintConst | `10u in {1u:1,2u:2,3u:3,4u:4,5u:5,6u:6,7u:7,8u:8,9u:9,10u:10}` | 452 | 261 | 1.73× |

### policies

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| arena_map_gate | `c.age >= {"us": 21, "de": 18}["us"]` | 296 | 343 | 0.86× |
| authz_basic | `(c.is_premium && c.age >= 18 && c.name in ["Ada", "Grace", …` | 376 | 742 | 0.51× |
| authz_deep | `(m.inner.b && m.inner.i64 >= 18 && m.inner.inner.s in ["Ada…` | 557 | 874 | 0.64× |
| authz_deep8 | `(m.inner.inner.inner.inner.inner.inner.inner.b && m.inner.i…` | 1,319 | 1,346 | 0.98× |
| mega100 | `(m.i64 + m.str_to_i32["q1"] + m.rep_i32[1]) == -1 ? "deny" …` | 11.9 µs | 19.8 µs | 0.60× |
| premium_gate | `c.is_premium ? c.age : 0` | 155 | 261 | 0.60× |
| quota_check | `m.str_to_i32["used"] + m.str_to_i32["pending"] < m.str_to_i…` | 431 | 922 | 0.47× |
| risk_score | `c.credit_score >= 700.0 && c.balance_cents > 1000u` | 221 | 420 | 0.53× |
| str_in_list | `m.s in ["alpha", "beta", "gamma", "delta"]` | 229 | 367 | 0.62× |
| ternary2 | `c.age > 30 ? (c.is_premium ? "gold" : "silver") : "basic"` | 205 | 430 | 0.48× |
| ternary5 | `c.age > 60 ? "a" : c.age > 50 ? "b" : c.age > 40 ? "c" : c.…` | 379 | 868 | 0.44× |
| tier_route | `c.balance_cents >= 100000u ? "platinum" : (c.is_premium ? "…` | 306 | 894 | 0.34× |

### proto

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| construct_name | `celwasm.testdata.Customer{name: "Ada"}.name` | 208 | 355 | 0.59× |
| cust_age | `c.age` | 85 | 152 | 0.56× |
| cust_is_premium | `c.is_premium` | 85 | 152 | 0.56× |
| cust_name | `c.name` | 94 | 264 | 0.36× |
| map_i64_str | `m.i64_to_str[2]` | 132 | 451 | 0.29× |
| map_str_i32 | `m.str_to_i32["b"]` | 133 | 348 | 0.38× |
| map_str_msg_i64 | `m.str_to_msg["k"].i64` | 192 | 377 | 0.51× |
| metadata_b | `c.metadata["b"]` | 141 | 455 | 0.31× |
| pair_list_arena | `[10, 20, 30, 40, 50][2]` | 160 | 207 | 0.78× |
| pair_map_arena | `{"a": 1, "b": 2, "c": 3}["b"]` | 233 | 223 | 1.04× |
| read_b | `m.b` | 83 | 151 | 0.55× |
| read_f64 | `m.f64` | 83 | 151 | 0.55× |
| read_s | `m.s` | 101 | 265 | 0.38× |
| read_u64 | `m.u64` | 84 | 152 | 0.55× |
| reads5 | `m.i32 + m.i64 + m.si32 + m.si64 + m.sfx32` | 431 | 557 | 0.77× |
| reads10 | `m.i32 + m.i64 + … + m.si32 + m.si64 (10 terms)` | 895 | 1,063 | 0.84× |
| reads100 | `m.i32 + m.i64 + … + m.si32 + m.si64 (100 terms)` | 8,069 | 10.3 µs | 0.78× |
| rep_i32_at0 | `m.rep_i32[0]` | 125 | 291 | 0.43× |
| rep_i32_at9 | `m.rep_i32[9]` | 131 | 292 | 0.45× |
| rep_msg_at1_s | `m.rep_msg[1].s` | 196 | 467 | 0.42× |
| select_depth1 | `m.i64` | 92 | 151 | 0.61× |
| select_depth2 | `m.inner.i64` | 135 | 199 | 0.68× |
| select_depth4 | `m.inner.inner.inner.i64` | 220 | 274 | 0.80× |
| select_depth8 | `m.inner.inner.inner.inner.inner.inner.inner.i64` | 399 | 435 | 0.92× |
| select_depth16 | `m.inner.inner.inner.inner.inner.inner.inner.inner.inner.inn…` | 744 | 745 | 1.00× |
| tags_at2 | `c.tags[2]` | 134 | 404 | 0.33× |

### size

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytes | `size(x)` | 75 | 218 | 0.34× |
| bytesConst | `size(b"0123456789abcdef")` | 77 | 188 | 0.41× |
| list10 | `size([1,2,3,4,5,6,7,8,9,10])` | 266 | 207 | 1.28× |
| list100 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 1,343 | 207 | 6.48× |
| list1000 | `size([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21…` | 11.1 µs | 208 | 53.61× |
| map10 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10})` | 492 | 207 | 2.38× |
| map100 | `size({1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10,11:11,12:12…` | 3,959 | 207 | 19.10× |
| string | `size(s)` | 81 | 218 | 0.37× |
| stringConst | `size("abcdefghijklmnopqrstuvwxyz")` | 79 | 194 | 0.41× |

### strings

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| bytesConcat2 | `a + b` | 124 | 267 | 0.46× |
| bytesConcat2Const | `b"ab" + b"cd"` | 94 | 215 | 0.44× |
| concat2 | `a + b` | 122 | 272 | 0.45× |
| concat2Const | `"hello " + "world"` | 98 | 220 | 0.44× |
| concatChain10Terms | `a + b + c + d + e + f + g + h + i + j` | 705 | 1,306 | 0.54× |
| concatChain100Terms | `a + b + … + i + j (100 terms)` | 8,355 | 11.3 µs | 0.74× |
| concatChain1000Terms | `a + b + … + i + j (1000 terms)` | 163.4 µs | 129.9 µs | 1.26× |
| contains | `a.contains("aug")` | 91 | 223 | 0.41× |
| containsConst | `"augustine".contains("aug")` | 78 | 194 | 0.40× |
| endsWith | `a.endsWith("ine")` | 87 | 221 | 0.40× |
| endsWithConst | `"augustine".endsWith("ine")` | 72 | 191 | 0.38× |
| eqConst | `"hello" == "world"` | 66 | 210 | 0.32× |
| eqVar | `a == "augustine"` | 78 | 235 | 0.33× |
| matchesCheap | `a.matches("^aug")` | 1,883 | 248 | 7.58× |
| matchesCheapConst | `"augustine".matches("^aug")` | 1,892 | 226 | 8.38× |
| matchesComplex | `a.matches("^[a-z]+-[0-9]{2,4}@[a-z]+\\.(com\|org)$")` | 12.4 µs | 322 | 38.61× |
| matchesComplexConst | `"user-1234@example.com".matches("^[a-z]+-[0-9]{2,4}@[a-z]+\…` | 11.0 µs | 298 | 37.02× |
| startsWith | `a.startsWith("aug")` | 90 | 220 | 0.41× |
| startsWithConst | `"augustine".startsWith("aug")` | 71 | 191 | 0.37× |

### ternary

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| intComputedCond | `a > b ? x : y` | 127 | 314 | 0.41× |
| intConst | `1 > 2 ? 10 : 20` | 91 | 263 | 0.34× |
| intVarCond | `c ? x : y` | 83 | n/a | n/a |
| nested3 | `a > b ? "gt" : (a == b ? "eq" : "lt")` | 169 | 479 | 0.35× |
| stringComputedCond | `a > b ? s : t` | 127 | 337 | 0.38× |
| stringConst | `3 > 2 ? "yes" : "no"` | 97 | 267 | 0.36× |

### time

| id | expression | cel-cpp (ns) | celwasm-dynamic (ns) | celwasm-dynamic ×cel-cpp |
|---|---|---|---|---|
| durAddDur | `(duration("90s") + duration("30s")).getSeconds()` | 197 | 582 | 0.34× |
| durAddTs | `int(duration("1h") + timestamp("2024-01-01T00:00:00Z"))` | 304 | 794 | 0.38× |
| durGetHours | `duration("3723s").getHours()` | 112 | 333 | 0.34× |
| durGetSeconds | `duration("3723s").getSeconds()` | 109 | 340 | 0.32× |
| durSubDur | `(duration("90s") - duration("30s")).getSeconds()` | 194 | 604 | 0.32× |
| tsAddDur | `int(timestamp("2024-01-01T00:00:00Z") + duration("1h"))` | 304 | 796 | 0.38× |
| tsGetDayOfWeekUtc | `timestamp("2024-06-15T10:30:45Z").getDayOfWeek()` | 253 | 564 | 0.45× |
| tsGetFullYearTz | `timestamp("2024-06-15T10:30:45Z").getFullYear("America/New_…` | 294 | 628 | 0.47× |
| tsGetFullYearUtc | `timestamp("2024-06-15T10:30:45Z").getFullYear()` | 263 | 565 | 0.47× |
| tsGetFullYearUtcMax | `timestamp("9999-12-31T23:59:59Z").getFullYear()` | 300 | 585 | 0.51× |
| tsGetHoursTz | `timestamp("2024-06-15T10:30:45Z").getHours("America/New_Yor…` | 301 | 613 | 0.49× |
| tsGetHoursUtc | `timestamp("2024-06-15T10:30:45Z").getHours()` | 258 | 566 | 0.46× |
| tsGetSecondsTz | `timestamp("2024-06-15T10:30:45Z").getSeconds("America/New_Y…` | 292 | 617 | 0.47× |
| tsGetSecondsUtc | `timestamp("2024-06-15T10:30:45Z").getSeconds()` | 251 | 565 | 0.44× |
| tsSubDur | `int(timestamp("2024-01-01T01:00:00Z") - duration("1h"))` | 308 | 797 | 0.39× |
| tsSubTs | `(timestamp("2024-01-01T01:00:00Z") - timestamp("2024-01-01T…` | 442 | 989 | 0.45× |

### Per-operator headline — T(N) = setup + N·per_op

Linear regression over each length-sweep family; slope is the steady-state cost of one more operation, crossover is the expression length where the comparator overtakes cel-cpp.

| surface | operator family | points | cel-cpp slope | cel-cpp setup | celwasm-dynamic slope | celwasm-dynamic setup | celwasm-dynamic crossover vs cel-cpp |
|---|---|---|---|---|---|---|---|
| arithmetic | doubleAdd | 5 | 31.2 | 80 | 76.1 | 117 | never wins |
| arithmetic | intAdd | 5 | 32.5 | -77 | 77.0 | 76 | never wins |
| arithmetic | intMul | 5 | 30.9 | 216 | 78.4 | 175 | never wins |
| arithmetic | intSub | 5 | 31.3 | -47 | 77.0 | 94 | never wins |
| comprehensions | all | 4 | 65.4 | 127 | 248.5 | -454 | never wins |
| index | mapIntN | 3 | 43.9 | -14 | 0.0 | 259 | N ≈ 6 |
| index | mapStrN | 3 | 49.6 | -79 | -0.0 | 266 | N ≈ 7 |
| lists | bound | 5 | 3.5 | -2,917 | 2.6 | 440 | N ≈ 3,654 |
| long_strings | containsLong_N | 4 | 0.0 | 83 | 0.0 | 220 | never wins |
| maps | inIntN | 3 | 69.6 | -852 | -0.0 | 275 | N ≈ 16 |
| maps | inStrN | 3 | 49.2 | -19 | -0.0 | 287 | N ≈ 6 |
| proto | reads | 3 | 80.1 | 62 | 102.5 | 41 | never wins |
| proto | select_depth | 5 | 43.5 | 48 | 39.4 | 116 | N ≈ 17 |
| size | list | 3 | 11.0 | 200 | 0.0 | 207 | N ≈ 1 |
| strings | concatChain | 3 | 167.5 | -4,507 | 130.7 | -841 | N ≈ 99 |

