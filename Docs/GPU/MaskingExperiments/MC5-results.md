> 주의: 이 파일은 생성 당시 한글 인코딩이 일부 손상되어 '?'나 '�'로 보이는 글자가 있다. 숫자·표 값은 원본과 같다.

# MC5 ?�시 ?�험 결과

조건: MX450, Release, -debug 0, TAA off, vsync off, ???�션(조합??1???�행), masked skeletal mesh�?비교(Xbot-bg 조합�?배경 ?�함). 모드 6�?None-orig, None-R32, Degenerate, Compact, VC-clip, VC-discard-any) x 3?�이?? ?�환 ??30?�레???�외, 모드??창별 200?�본(모드??600?�본 ?�균). GPU ??= shadow+depth+opaque+transparent timestamp ???�체 ?�레???�님). 범위 = 3?�이??�??�균??min~max, Compact 범위? 겹치�?noise.

모드 ?�석 주의:
- None-orig???�본 R16 IB, ?�머� IB ?�기 모드(None-R32, Degenerate, Compact, VC-clip, VC-discard-any)??모두 R32 IB�??�는?? VC 모드???�거 ?�는 R32 ?�체 ?�본(None-R32? 같�? 구조)?�로 ?�체 IB�?그린??
- Degenerate??IA/VS ?�력??줄�? ?�는???�본 ?�기 IB, 같�? draw ??.
- VC-clip? ?�각?�으�??�르???�점�?clip, 경계 보간). VC-discard-any??깊이/그림???�스?�도 PS(discard)� ?�요?�다.
- ?�험 모델 모든 section ibFormat=R16, 모든 draw baseVertex=0(ib-info.txt). baseVertex� 0???�닌 모델? ?�험?��? ?�았?��?�?VS??vertexId + MaskBaseVertex ?�중 �??�?�성? �증되� ?�았??
- 갱신 비용? indicative scopes(??�적 범위)?? ?�벤?�는 Finish() ?�출 ?�까�?�며 CPU 값�? 구간별로 ?�의� ?�르?? VC??Mode 7 ?�보 초기?��? 게시 AND�??�함?�고 Mode 0 추출 비용? ?�외?�다.

## ?�스�?GPU ms? ?�계 (Compact ?�?

| 조합 | 모드 | shadow | depth | opaque | transparent | ??| ??Compact | ?�이??범위 | ?�정 |
|---|---|---:|---:|---:|---:|---:|---:|---|---|
| Xbot-c1-i1 | None-orig | 0.3478 | 0.0965 | 0.1395 | 0.0190 | 0.6028 | -0.0320 (-5.0%) | 0.5822~0.6300 | noise |
| Xbot-c1-i1 | None-R32 | 0.3615 | 0.1035 | 0.1465 | 0.0200 | 0.6314 | -0.0034 (-0.5%) | 0.5947~0.6727 | noise |
| Xbot-c1-i1 | Degenerate | 0.3559 | 0.1018 | 0.1432 | 0.0192 | 0.6201 | -0.0147 (-2.3%) | 0.6098~0.6279 | noise |
| Xbot-c1-i1 | Compact | 0.3760 | 0.0994 | 0.1403 | 0.0190 | 0.6348 | 0.0000 (0.0%) | 0.6165~0.6527 | - |
| Xbot-c1-i1 | VC-clip | 0.3718 | 0.1032 | 0.1428 | 0.0187 | 0.6366 | 0.0018 (0.3%) | 0.6214~0.6566 | noise |
| Xbot-c1-i1 | VC-discard-any | 0.3594 | 0.1034 | 0.1450 | 0.0201 | 0.6279 | -0.0068 (-1.1%) | 0.5926~0.6560 | noise |
| Xbot-c1-i10 | None-orig | 0.7926 | 0.8129 | 0.8361 | 0.0166 | 2.4582 | 0.0060 (0.2%) | 2.3805~2.5208 | noise |
| Xbot-c1-i10 | None-R32 | 0.8424 | 0.8571 | 0.8963 | 0.0155 | 2.6113 | 0.1592 (6.5%) | 2.5339~2.7231 | beyond range |
| Xbot-c1-i10 | Degenerate | 0.8421 | 0.8747 | 0.9319 | 0.0150 | 2.6637 | 0.2116 (8.6%) | 2.5872~2.7198 | beyond range |
| Xbot-c1-i10 | Compact | 0.8234 | 0.7456 | 0.8621 | 0.0210 | 2.4521 | 0.0000 (0.0%) | 2.4164~2.4737 | - |
| Xbot-c1-i10 | VC-clip | 0.8586 | 0.8000 | 0.8803 | 0.0172 | 2.5562 | 0.1041 (4.2%) | 2.5056~2.6528 | beyond range |
| Xbot-c1-i10 | VC-discard-any | 0.8496 | 0.8435 | 0.8739 | 0.0166 | 2.5836 | 0.1315 (5.4%) | 2.5503~2.6196 | beyond range |
| CesiumMan-c1-i1 | None-orig | 0.1373 | 0.0479 | 0.1103 | 0.0211 | 0.3167 | -0.0281 (-8.1%) | 0.2936~0.3443 | noise |
| CesiumMan-c1-i1 | None-R32 | 0.1297 | 0.0476 | 0.1345 | 0.0203 | 0.3320 | -0.0127 (-3.7%) | 0.3205~0.3388 | noise |
| CesiumMan-c1-i1 | Degenerate | 0.1789 | 0.0452 | 0.1215 | 0.0212 | 0.3668 | 0.0221 (6.4%) | 0.3433~0.3932 | noise |
| CesiumMan-c1-i1 | Compact | 0.1369 | 0.0431 | 0.1427 | 0.0220 | 0.3447 | 0.0000 (0.0%) | 0.2555~0.4138 | - |
| CesiumMan-c1-i1 | VC-clip | 0.1564 | 0.0418 | 0.1623 | 0.0271 | 0.3876 | 0.0429 (12.4%) | 0.3388~0.4123 | noise |
| CesiumMan-c1-i1 | VC-discard-any | 0.1394 | 0.0462 | 0.1298 | 0.0223 | 0.3378 | -0.0070 (-2.0%) | 0.2739~0.3825 | noise |
| CesiumMan-c1-i10 | None-orig | 0.1658 | 0.0854 | 0.1521 | 0.0174 | 0.4206 | -0.0033 (-0.8%) | 0.3968~0.4674 | noise |
| CesiumMan-c1-i10 | None-R32 | 0.1539 | 0.0985 | 0.1625 | 0.0190 | 0.4339 | 0.0101 (2.4%) | 0.4119~0.4486 | noise |
| CesiumMan-c1-i10 | Degenerate | 0.1886 | 0.0979 | 0.1666 | 0.0161 | 0.4692 | 0.0453 (10.7%) | 0.4207~0.5297 | noise |
| CesiumMan-c1-i10 | Compact | 0.1649 | 0.0953 | 0.1458 | 0.0179 | 0.4239 | 0.0000 (0.0%) | 0.4098~0.4479 | - |
| CesiumMan-c1-i10 | VC-clip | 0.2142 | 0.0993 | 0.1400 | 0.0157 | 0.4691 | 0.0452 (10.7%) | 0.4287~0.5147 | noise |
| CesiumMan-c1-i10 | VC-discard-any | 0.1973 | 0.0974 | 0.1447 | 0.0159 | 0.4553 | 0.0314 (7.4%) | 0.4271~0.5030 | noise |
| Xbot-c2-i1 | None-orig | 0.1525 | 0.1108 | 0.1806 | 0.0190 | 0.4629 | -0.0214 (-4.4%) | 0.4582~0.4685 | noise |
| Xbot-c2-i1 | None-R32 | 0.1483 | 0.1176 | 0.1910 | 0.0191 | 0.4759 | -0.0084 (-1.7%) | 0.4651~0.4832 | noise |
| Xbot-c2-i1 | Degenerate | 0.1633 | 0.1122 | 0.1770 | 0.0209 | 0.4734 | -0.0110 (-2.3%) | 0.4572~0.4943 | noise |
| Xbot-c2-i1 | Compact | 0.2005 | 0.1074 | 0.1598 | 0.0167 | 0.4843 | 0.0000 (0.0%) | 0.4277~0.5440 | - |
| Xbot-c2-i1 | VC-clip | 0.3107 | 0.1117 | 0.1582 | 0.0190 | 0.5996 | 0.1153 (23.8%) | 0.5958~0.6035 | beyond range |
| Xbot-c2-i1 | VC-discard-any | 0.1964 | 0.1151 | 0.1737 | 0.0190 | 0.5042 | 0.0199 (4.1%) | 0.4638~0.5291 | noise |
| Xbot-c2-i10 | None-orig | 0.8575 | 0.6891 | 0.7922 | 0.0199 | 2.3587 | 0.0254 (1.1%) | 2.3360~2.3884 | noise |
| Xbot-c2-i10 | None-R32 | 0.8435 | 0.7467 | 0.8626 | 0.0172 | 2.4700 | 0.1367 (5.9%) | 2.4268~2.5049 | beyond range |
| Xbot-c2-i10 | Degenerate | 0.8357 | 0.7353 | 0.8577 | 0.0167 | 2.4455 | 0.1122 (4.8%) | 2.4295~2.4741 | beyond range |
| Xbot-c2-i10 | Compact | 0.8849 | 0.6842 | 0.7454 | 0.0187 | 2.3333 | 0.0000 (0.0%) | 2.2674~2.3894 | - |
| Xbot-c2-i10 | VC-clip | 0.8703 | 0.7199 | 0.7902 | 0.0174 | 2.3978 | 0.0646 (2.8%) | 2.3784~2.4259 | noise |
| Xbot-c2-i10 | VC-discard-any | 0.8716 | 0.7089 | 0.8546 | 0.0178 | 2.4529 | 0.1197 (5.1%) | 2.4313~2.4834 | beyond range |
| Xbot-bg-c1-i1 | None-orig | 2.1111 | 1.8127 | 2.1601 | 0.0179 | 6.1019 | 0.0517 (0.9%) | 5.8896~6.2157 | noise |
| Xbot-bg-c1-i1 | None-R32 | 2.1384 | 1.7537 | 2.1730 | 0.0171 | 6.0822 | 0.0320 (0.5%) | 5.8428~6.2571 | noise |
| Xbot-bg-c1-i1 | Degenerate | 2.1550 | 1.8392 | 2.1123 | 0.0172 | 6.1237 | 0.0736 (1.2%) | 6.0715~6.1580 | noise |
| Xbot-bg-c1-i1 | Compact | 2.0933 | 1.7837 | 2.1559 | 0.0173 | 6.0501 | 0.0000 (0.0%) | 5.9924~6.0887 | - |
| Xbot-bg-c1-i1 | VC-clip | 2.1336 | 1.8697 | 2.1064 | 0.0183 | 6.1280 | 0.0779 (1.3%) | 6.0812~6.1769 | noise |
| Xbot-bg-c1-i1 | VC-discard-any | 2.1749 | 1.8730 | 2.1003 | 0.0229 | 6.1710 | 0.1208 (2.0%) | 6.0754~6.2321 | noise |

## ?�스 ?�계 카운??(shadow+depth+opaque+transparent ?? ?�레?�당 ?�균)

| 조합 | 모드 | IA prim | VS inv | C inv | C prim | PS inv |
|---|---|---:|---:|---:|---:|---:|
| Xbot-c1-i1 | None-orig | 154368 | 149178 | 154368 | 154368 | 99083 |
| Xbot-c1-i1 | None-R32 | 154368 | 149178 | 154368 | 154368 | 99083 |
| Xbot-c1-i1 | Degenerate | 154368 | 147294 | 154368 | 154368 | 91827 |
| Xbot-c1-i1 | Compact | 148014 | 143586 | 148014 | 148014 | 91827 |
| Xbot-c1-i1 | VC-clip | 154368 | 149178 | 154368 | 148260 | 91827 |
| Xbot-c1-i1 | VC-discard-any | 154368 | 149178 | 154368 | 154368 | 230342 |
| Xbot-c1-i10 | None-orig | 1480392 | 1301911 | 1480392 | 1480392 | 58876 |
| Xbot-c1-i10 | None-R32 | 1480392 | 1301912 | 1480392 | 1480392 | 58876 |
| Xbot-c1-i10 | Degenerate | 1480392 | 1283072 | 1480392 | 1480392 | 53889 |
| Xbot-c1-i10 | Compact | 1416852 | 1245998 | 1416852 | 1416852 | 53889 |
| Xbot-c1-i10 | VC-clip | 1480392 | 1301914 | 1480392 | 1419312 | 53889 |
| Xbot-c1-i10 | VC-discard-any | 1480392 | 1301913 | 1480392 | 1480392 | 169988 |
| CesiumMan-c1-i1 | None-orig | 15009 | 16689 | 15009 | 15009 | 168388 |
| CesiumMan-c1-i1 | None-R32 | 15009 | 16689 | 15009 | 15009 | 168388 |
| CesiumMan-c1-i1 | Degenerate | 15009 | 16494 | 15009 | 15009 | 141166 |
| CesiumMan-c1-i1 | Compact | 14196 | 15954 | 14196 | 14196 | 141166 |
| CesiumMan-c1-i1 | VC-clip | 15009 | 16689 | 15009 | 14388 | 141204 |
| CesiumMan-c1-i1 | VC-discard-any | 15009 | 16689 | 15009 | 15009 | 314102 |
| CesiumMan-c1-i10 | None-orig | 141153 | 140079 | 141153 | 137226 | 88916 |
| CesiumMan-c1-i10 | None-R32 | 141153 | 140079 | 141153 | 137226 | 88916 |
| CesiumMan-c1-i10 | Degenerate | 141153 | 138129 | 141153 | 137226 | 81289 |
| CesiumMan-c1-i10 | Compact | 133023 | 132729 | 133023 | 129367 | 81289 |
| CesiumMan-c1-i10 | VC-clip | 141153 | 140079 | 141153 | 131223 | 81580 |
| CesiumMan-c1-i10 | VC-discard-any | 141153 | 140079 | 141153 | 137226 | 205985 |
| Xbot-c2-i1 | None-orig | 159372 | 164190 | 159372 | 159372 | 109625 |
| Xbot-c2-i1 | None-R32 | 159372 | 164190 | 159372 | 159372 | 109625 |
| Xbot-c2-i1 | Degenerate | 159372 | 160860 | 159372 | 159372 | 91515 |
| Xbot-c2-i1 | Compact | 148350 | 154626 | 148350 | 148350 | 91515 |
| Xbot-c2-i1 | VC-clip | 159372 | 164191 | 159372 | 148596 | 91515 |
| Xbot-c2-i1 | VC-discard-any | 159372 | 164190 | 159372 | 159372 | 236803 |
| Xbot-c2-i10 | None-orig | 1485396 | 1316925 | 1485396 | 1485396 | 60167 |
| Xbot-c2-i10 | None-R32 | 1485396 | 1316928 | 1485396 | 1485396 | 60167 |
| Xbot-c2-i10 | Degenerate | 1485396 | 1283631 | 1485396 | 1485396 | 49425 |
| Xbot-c2-i10 | Compact | 1375176 | 1221285 | 1375176 | 1375176 | 49425 |
| Xbot-c2-i10 | VC-clip | 1485396 | 1316927 | 1485396 | 1377636 | 49425 |
| Xbot-c2-i10 | VC-discard-any | 1485396 | 1316928 | 1485396 | 1485396 | 166090 |
| Xbot-bg-c1-i1 | None-orig | 2813000 | 2534889 | 2813000 | 1129998 | 1102046 |
| Xbot-bg-c1-i1 | None-R32 | 2813000 | 2534885 | 2813000 | 1129998 | 1102046 |
| Xbot-bg-c1-i1 | Degenerate | 2813000 | 2533002 | 2813000 | 1129998 | 1100057 |
| Xbot-bg-c1-i1 | Compact | 2806646 | 2529292 | 2806646 | 1123644 | 1100057 |
| Xbot-bg-c1-i1 | VC-clip | 2813000 | 2534887 | 2813000 | 1123890 | 1100057 |
| Xbot-bg-c1-i1 | VC-discard-any | 2813000 | 2534886 | 2813000 | 1129998 | 1228897 |

## 갱신 비용 (게시???�계, cpu_ms / gpu_ms, indicative scopes)

Compact = CPU 개수 계산+기록 / GPU ?�축 dispatch. Degenerate, None-R32 copy = 기록 / GPU ?�자�??�기 dispatch. VC = Mode 7 초기??+ 게시 AND(Mode 0 ?�외). ?�벤???�는 K10 추�? ?�스?�스 ?�함.

| 조합 | Compact ?�벤????| Compact | Degenerate | None-R32 copy | VC ?�벤??init/apply) | VC |
|---|---:|---|---|---|---|---|
| Xbot-c1-i1 | 1 | 0.913 / 0.043 | 0.379 / 0.029 | 0.290 / 0.031 | 1/1 | 4.143 / 0.014 |
| Xbot-c1-i10 | 10 | 7.393 / 0.474 | 2.883 / 0.310 | 2.666 / 0.318 | 1/1 | 4.780 / 0.016 |
| CesiumMan-c1-i1 | 1 | 0.842 / 0.013 | 0.360 / 0.006 | 0.357 / 0.006 | 1/1 | 5.471 / 0.015 |
| CesiumMan-c1-i10 | 10 | 7.925 / 0.169 | 2.870 / 0.077 | 3.164 / 0.071 | 1/1 | 5.956 / 0.014 |
| Xbot-c2-i1 | 2 | 1.511 / 0.091 | 0.585 / 0.060 | 0.600 / 0.060 | 2/2 | 4.177 / 0.031 |
| Xbot-c2-i10 | 11 | 12.052 / 0.583 | 4.041 / 0.380 | 4.051 / 0.367 | 2/2 | 4.078 / 0.033 |
| Xbot-bg-c1-i1 | 1 | 1.426 / 0.047 | 1.538 / 0.032 | 1.184 / 0.033 | 1/1 | 8.495 / 0.016 |

## 메모�?(bytes, ?�리 buffer Width, ?�스?�스 ??

Compact IB? Degenerate IB???�로 ?�기?�다. None-R32/VC??R32 ?�체 ?�본? Degenerate IB? 같�? ?�기??별도 버퍼??

| 조합 | Compact IB | Degenerate IB | Triangle State | 1-bit ?�점 마스??| ?�거 ?�각??|
|---|---:|---:|---:|---:|---:|
| Xbot-c1-i1 | 563928 | 589344 | 196448 | 3548 | 2118 |
| Xbot-c1-i10 | 5639280 | 5893440 | 1964480 | 35480 | 21180 |
| CesiumMan-c1-i1 | 52812 | 56064 | 18688 | 412 | 271 |
| CesiumMan-c1-i10 | 528120 | 560640 | 186880 | 4120 | 2710 |
| Xbot-c2-i1 | 545256 | 589344 | 196448 | 3548 | 3674 |
| Xbot-c2-i10 | 5452560 | 5893440 | 1964480 | 35480 | 36740 |
| Xbot-bg-c1-i1 | 563928 | 589344 | 196448 | 3548 | 2118 |

## HDR ?�체 ?�레??비교 (고정 pose, ?�출 ??R11G11B10 비트 비교, 1920x1080 ?�체, Compact ?�??�른 ?��? ??

| 모델 | None-orig | None-R32 | Degenerate | VC-clip | VC-discard-any | Compact 반복 |
|---|---:|---:|---:|---:|---:|---:|
| Xbot | 3825 | 3825 | 0 | 0 | 0 | 0 |
| CesiumMan | 6966 | 6966 | 0 | 1143 | 0 | 0 |
| Xbot2 | 7926 | 7926 | 0 | 0 | 0 | 0 |

## ?�스�??�세 ?�균 (모드??600?�본)

| 조합 | 모드 | pass | IA prim | VS inv | C inv | C prim | PS inv | GPU ms | draws |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| Xbot-c1-i1 | None-orig | shadow | 51456 | 49726 | 51456 | 51456 | 0 | 0.3478 | 6 |
| Xbot-c1-i1 | None-orig | depth | 51456 | 49726 | 51456 | 51456 | 0 | 0.0965 | 6 |
| Xbot-c1-i1 | None-orig | opaque | 51456 | 49726 | 51456 | 51456 | 99083 | 0.1395 | 6 |
| Xbot-c1-i1 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0190 | 0 |
| Xbot-c1-i1 | None-R32 | shadow | 51456 | 49726 | 51456 | 51456 | 0 | 0.3615 | 6 |
| Xbot-c1-i1 | None-R32 | depth | 51456 | 49726 | 51456 | 51456 | 0 | 0.1035 | 6 |
| Xbot-c1-i1 | None-R32 | opaque | 51456 | 49726 | 51456 | 51456 | 99083 | 0.1465 | 6 |
| Xbot-c1-i1 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0200 | 0 |
| Xbot-c1-i1 | Degenerate | shadow | 51456 | 49098 | 51456 | 51456 | 0 | 0.3559 | 6 |
| Xbot-c1-i1 | Degenerate | depth | 51456 | 49098 | 51456 | 51456 | 0 | 0.1018 | 6 |
| Xbot-c1-i1 | Degenerate | opaque | 51456 | 49098 | 51456 | 51456 | 91827 | 0.1432 | 6 |
| Xbot-c1-i1 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0192 | 0 |
| Xbot-c1-i1 | Compact | shadow | 49338 | 47862 | 49338 | 49338 | 0 | 0.3760 | 6 |
| Xbot-c1-i1 | Compact | depth | 49338 | 47862 | 49338 | 49338 | 0 | 0.0994 | 6 |
| Xbot-c1-i1 | Compact | opaque | 49338 | 47862 | 49338 | 49338 | 91827 | 0.1403 | 6 |
| Xbot-c1-i1 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0190 | 0 |
| Xbot-c1-i1 | VC-clip | shadow | 51456 | 49726 | 51456 | 49420 | 0 | 0.3718 | 6 |
| Xbot-c1-i1 | VC-clip | depth | 51456 | 49726 | 51456 | 49420 | 0 | 0.1032 | 6 |
| Xbot-c1-i1 | VC-clip | opaque | 51456 | 49726 | 51456 | 49420 | 91827 | 0.1428 | 6 |
| Xbot-c1-i1 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0187 | 0 |
| Xbot-c1-i1 | VC-discard-any | shadow | 51456 | 49726 | 51456 | 51456 | 3285 | 0.3594 | 6 |
| Xbot-c1-i1 | VC-discard-any | depth | 51456 | 49726 | 51456 | 51456 | 133286 | 0.1034 | 6 |
| Xbot-c1-i1 | VC-discard-any | opaque | 51456 | 49726 | 51456 | 51456 | 93771 | 0.1450 | 6 |
| Xbot-c1-i1 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0201 | 0 |
| Xbot-c1-i10 | None-orig | shadow | 493464 | 433971 | 493464 | 493464 | 0 | 0.7926 | 24 |
| Xbot-c1-i10 | None-orig | depth | 493464 | 433970 | 493464 | 493464 | 0 | 0.8129 | 24 |
| Xbot-c1-i10 | None-orig | opaque | 493464 | 433970 | 493464 | 493464 | 58876 | 0.8361 | 24 |
| Xbot-c1-i10 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0166 | 0 |
| Xbot-c1-i10 | None-R32 | shadow | 493464 | 433971 | 493464 | 493464 | 0 | 0.8424 | 24 |
| Xbot-c1-i10 | None-R32 | depth | 493464 | 433970 | 493464 | 493464 | 0 | 0.8571 | 24 |
| Xbot-c1-i10 | None-R32 | opaque | 493464 | 433972 | 493464 | 493464 | 58876 | 0.8963 | 24 |
| Xbot-c1-i10 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0155 | 0 |
| Xbot-c1-i10 | Degenerate | shadow | 493464 | 427692 | 493464 | 493464 | 0 | 0.8421 | 24 |
| Xbot-c1-i10 | Degenerate | depth | 493464 | 427690 | 493464 | 493464 | 0 | 0.8747 | 24 |
| Xbot-c1-i10 | Degenerate | opaque | 493464 | 427691 | 493464 | 493464 | 53889 | 0.9319 | 24 |
| Xbot-c1-i10 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0150 | 0 |
| Xbot-c1-i10 | Compact | shadow | 472284 | 415332 | 472284 | 472284 | 0 | 0.8234 | 24 |
| Xbot-c1-i10 | Compact | depth | 472284 | 415333 | 472284 | 472284 | 0 | 0.7456 | 24 |
| Xbot-c1-i10 | Compact | opaque | 472284 | 415333 | 472284 | 472284 | 53889 | 0.8621 | 24 |
| Xbot-c1-i10 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0210 | 0 |
| Xbot-c1-i10 | VC-clip | shadow | 493464 | 433972 | 493464 | 473104 | 0 | 0.8586 | 24 |
| Xbot-c1-i10 | VC-clip | depth | 493464 | 433971 | 493464 | 473104 | 0 | 0.8000 | 24 |
| Xbot-c1-i10 | VC-clip | opaque | 493464 | 433972 | 493464 | 473104 | 53889 | 0.8803 | 24 |
| Xbot-c1-i10 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0172 | 0 |
| Xbot-c1-i10 | VC-discard-any | shadow | 493464 | 433972 | 493464 | 493464 | 29884 | 0.8496 | 24 |
| Xbot-c1-i10 | VC-discard-any | depth | 493464 | 433971 | 493464 | 493464 | 86172 | 0.8435 | 24 |
| Xbot-c1-i10 | VC-discard-any | opaque | 493464 | 433971 | 493464 | 493464 | 53932 | 0.8739 | 24 |
| Xbot-c1-i10 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0166 | 0 |
| CesiumMan-c1-i1 | None-orig | shadow | 5003 | 5563 | 5003 | 5003 | 0 | 0.1373 | 5 |
| CesiumMan-c1-i1 | None-orig | depth | 5003 | 5563 | 5003 | 5003 | 0 | 0.0479 | 5 |
| CesiumMan-c1-i1 | None-orig | opaque | 5003 | 5563 | 5003 | 5003 | 168388 | 0.1103 | 5 |
| CesiumMan-c1-i1 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0211 | 0 |
| CesiumMan-c1-i1 | None-R32 | shadow | 5003 | 5563 | 5003 | 5003 | 0 | 0.1297 | 5 |
| CesiumMan-c1-i1 | None-R32 | depth | 5003 | 5563 | 5003 | 5003 | 0 | 0.0476 | 5 |
| CesiumMan-c1-i1 | None-R32 | opaque | 5003 | 5563 | 5003 | 5003 | 168388 | 0.1345 | 5 |
| CesiumMan-c1-i1 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0203 | 0 |
| CesiumMan-c1-i1 | Degenerate | shadow | 5003 | 5498 | 5003 | 5003 | 0 | 0.1789 | 5 |
| CesiumMan-c1-i1 | Degenerate | depth | 5003 | 5498 | 5003 | 5003 | 0 | 0.0452 | 5 |
| CesiumMan-c1-i1 | Degenerate | opaque | 5003 | 5498 | 5003 | 5003 | 141166 | 0.1215 | 5 |
| CesiumMan-c1-i1 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0212 | 0 |
| CesiumMan-c1-i1 | Compact | shadow | 4732 | 5318 | 4732 | 4732 | 0 | 0.1369 | 5 |
| CesiumMan-c1-i1 | Compact | depth | 4732 | 5318 | 4732 | 4732 | 0 | 0.0431 | 5 |
| CesiumMan-c1-i1 | Compact | opaque | 4732 | 5318 | 4732 | 4732 | 141166 | 0.1427 | 5 |
| CesiumMan-c1-i1 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0220 | 0 |
| CesiumMan-c1-i1 | VC-clip | shadow | 5003 | 5563 | 5003 | 4796 | 0 | 0.1564 | 5 |
| CesiumMan-c1-i1 | VC-clip | depth | 5003 | 5563 | 5003 | 4796 | 0 | 0.0418 | 5 |
| CesiumMan-c1-i1 | VC-clip | opaque | 5003 | 5563 | 5003 | 4796 | 141204 | 0.1623 | 5 |
| CesiumMan-c1-i1 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0271 | 0 |
| CesiumMan-c1-i1 | VC-discard-any | shadow | 5003 | 5563 | 5003 | 5003 | 3770 | 0.1394 | 5 |
| CesiumMan-c1-i1 | VC-discard-any | depth | 5003 | 5563 | 5003 | 5003 | 166089 | 0.0462 | 5 |
| CesiumMan-c1-i1 | VC-discard-any | opaque | 5003 | 5563 | 5003 | 5003 | 144243 | 0.1298 | 5 |
| CesiumMan-c1-i1 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0223 | 0 |
| CesiumMan-c1-i10 | None-orig | shadow | 47051 | 46693 | 47051 | 43124 | 0 | 0.1658 | 14 |
| CesiumMan-c1-i10 | None-orig | depth | 47051 | 46693 | 47051 | 47051 | 0 | 0.0854 | 14 |
| CesiumMan-c1-i10 | None-orig | opaque | 47051 | 46693 | 47051 | 47051 | 88916 | 0.1521 | 14 |
| CesiumMan-c1-i10 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0174 | 0 |
| CesiumMan-c1-i10 | None-R32 | shadow | 47051 | 46693 | 47051 | 43124 | 0 | 0.1539 | 14 |
| CesiumMan-c1-i10 | None-R32 | depth | 47051 | 46693 | 47051 | 47051 | 0 | 0.0985 | 14 |
| CesiumMan-c1-i10 | None-R32 | opaque | 47051 | 46693 | 47051 | 47051 | 88916 | 0.1625 | 14 |
| CesiumMan-c1-i10 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0190 | 0 |
| CesiumMan-c1-i10 | Degenerate | shadow | 47051 | 46043 | 47051 | 43124 | 0 | 0.1886 | 14 |
| CesiumMan-c1-i10 | Degenerate | depth | 47051 | 46043 | 47051 | 47051 | 0 | 0.0979 | 14 |
| CesiumMan-c1-i10 | Degenerate | opaque | 47051 | 46043 | 47051 | 47051 | 81289 | 0.1666 | 14 |
| CesiumMan-c1-i10 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0161 | 0 |
| CesiumMan-c1-i10 | Compact | shadow | 44341 | 44243 | 44341 | 40685 | 0 | 0.1649 | 14 |
| CesiumMan-c1-i10 | Compact | depth | 44341 | 44243 | 44341 | 44341 | 0 | 0.0953 | 14 |
| CesiumMan-c1-i10 | Compact | opaque | 44341 | 44243 | 44341 | 44341 | 81289 | 0.1458 | 14 |
| CesiumMan-c1-i10 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0179 | 0 |
| CesiumMan-c1-i10 | VC-clip | shadow | 47051 | 46693 | 47051 | 41261 | 0 | 0.2142 | 14 |
| CesiumMan-c1-i10 | VC-clip | depth | 47051 | 46693 | 47051 | 44981 | 0 | 0.0993 | 14 |
| CesiumMan-c1-i10 | VC-clip | opaque | 47051 | 46693 | 47051 | 44981 | 81580 | 0.1400 | 14 |
| CesiumMan-c1-i10 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0157 | 0 |
| CesiumMan-c1-i10 | VC-discard-any | shadow | 47051 | 46693 | 47051 | 43124 | 31780 | 0.1973 | 14 |
| CesiumMan-c1-i10 | VC-discard-any | depth | 47051 | 46693 | 47051 | 47051 | 92775 | 0.0974 | 14 |
| CesiumMan-c1-i10 | VC-discard-any | opaque | 47051 | 46693 | 47051 | 47051 | 81430 | 0.1447 | 14 |
| CesiumMan-c1-i10 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0159 | 0 |
| Xbot-c2-i1 | None-orig | shadow | 53124 | 54730 | 53124 | 53124 | 0 | 0.1525 | 10 |
| Xbot-c2-i1 | None-orig | depth | 53124 | 54730 | 53124 | 53124 | 0 | 0.1108 | 10 |
| Xbot-c2-i1 | None-orig | opaque | 53124 | 54730 | 53124 | 53124 | 109625 | 0.1806 | 10 |
| Xbot-c2-i1 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0190 | 0 |
| Xbot-c2-i1 | None-R32 | shadow | 53124 | 54730 | 53124 | 53124 | 0 | 0.1483 | 10 |
| Xbot-c2-i1 | None-R32 | depth | 53124 | 54730 | 53124 | 53124 | 0 | 0.1176 | 10 |
| Xbot-c2-i1 | None-R32 | opaque | 53124 | 54730 | 53124 | 53124 | 109625 | 0.1910 | 10 |
| Xbot-c2-i1 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0191 | 0 |
| Xbot-c2-i1 | Degenerate | shadow | 53124 | 53620 | 53124 | 53124 | 0 | 0.1633 | 10 |
| Xbot-c2-i1 | Degenerate | depth | 53124 | 53620 | 53124 | 53124 | 0 | 0.1122 | 10 |
| Xbot-c2-i1 | Degenerate | opaque | 53124 | 53620 | 53124 | 53124 | 91515 | 0.1770 | 10 |
| Xbot-c2-i1 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0209 | 0 |
| Xbot-c2-i1 | Compact | shadow | 49450 | 51542 | 49450 | 49450 | 0 | 0.2005 | 10 |
| Xbot-c2-i1 | Compact | depth | 49450 | 51542 | 49450 | 49450 | 0 | 0.1074 | 10 |
| Xbot-c2-i1 | Compact | opaque | 49450 | 51542 | 49450 | 49450 | 91515 | 0.1598 | 10 |
| Xbot-c2-i1 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0167 | 0 |
| Xbot-c2-i1 | VC-clip | shadow | 53124 | 54731 | 53124 | 49532 | 0 | 0.3107 | 10 |
| Xbot-c2-i1 | VC-clip | depth | 53124 | 54730 | 53124 | 49532 | 0 | 0.1117 | 10 |
| Xbot-c2-i1 | VC-clip | opaque | 53124 | 54730 | 53124 | 49532 | 91515 | 0.1582 | 10 |
| Xbot-c2-i1 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0190 | 0 |
| Xbot-c2-i1 | VC-discard-any | shadow | 53124 | 54730 | 53124 | 53124 | 3157 | 0.1964 | 10 |
| Xbot-c2-i1 | VC-discard-any | depth | 53124 | 54730 | 53124 | 53124 | 135145 | 0.1151 | 10 |
| Xbot-c2-i1 | VC-discard-any | opaque | 53124 | 54730 | 53124 | 53124 | 98501 | 0.1737 | 10 |
| Xbot-c2-i1 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0190 | 0 |
| Xbot-c2-i10 | None-orig | shadow | 495132 | 438974 | 495132 | 495132 | 0 | 0.8575 | 28 |
| Xbot-c2-i10 | None-orig | depth | 495132 | 438975 | 495132 | 495132 | 0 | 0.6891 | 28 |
| Xbot-c2-i10 | None-orig | opaque | 495132 | 438975 | 495132 | 495132 | 60167 | 0.7922 | 28 |
| Xbot-c2-i10 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0199 | 0 |
| Xbot-c2-i10 | None-R32 | shadow | 495132 | 438976 | 495132 | 495132 | 0 | 0.8435 | 28 |
| Xbot-c2-i10 | None-R32 | depth | 495132 | 438975 | 495132 | 495132 | 0 | 0.7467 | 28 |
| Xbot-c2-i10 | None-R32 | opaque | 495132 | 438977 | 495132 | 495132 | 60167 | 0.8626 | 28 |
| Xbot-c2-i10 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0172 | 0 |
| Xbot-c2-i10 | Degenerate | shadow | 495132 | 427877 | 495132 | 495132 | 0 | 0.8357 | 28 |
| Xbot-c2-i10 | Degenerate | depth | 495132 | 427877 | 495132 | 495132 | 0 | 0.7353 | 28 |
| Xbot-c2-i10 | Degenerate | opaque | 495132 | 427877 | 495132 | 495132 | 49425 | 0.8577 | 28 |
| Xbot-c2-i10 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0167 | 0 |
| Xbot-c2-i10 | Compact | shadow | 458392 | 407094 | 458392 | 458392 | 0 | 0.8849 | 28 |
| Xbot-c2-i10 | Compact | depth | 458392 | 407096 | 458392 | 458392 | 0 | 0.6842 | 28 |
| Xbot-c2-i10 | Compact | opaque | 458392 | 407096 | 458392 | 458392 | 49425 | 0.7454 | 28 |
| Xbot-c2-i10 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0187 | 0 |
| Xbot-c2-i10 | VC-clip | shadow | 495132 | 438976 | 495132 | 459212 | 0 | 0.8703 | 28 |
| Xbot-c2-i10 | VC-clip | depth | 495132 | 438976 | 495132 | 459212 | 0 | 0.7199 | 28 |
| Xbot-c2-i10 | VC-clip | opaque | 495132 | 438976 | 495132 | 459212 | 49425 | 0.7902 | 28 |
| Xbot-c2-i10 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0174 | 0 |
| Xbot-c2-i10 | VC-discard-any | shadow | 495132 | 438975 | 495132 | 495132 | 29859 | 0.8716 | 28 |
| Xbot-c2-i10 | VC-discard-any | depth | 495132 | 438975 | 495132 | 495132 | 86415 | 0.7089 | 28 |
| Xbot-c2-i10 | VC-discard-any | opaque | 495132 | 438978 | 495132 | 495132 | 49816 | 0.8546 | 28 |
| Xbot-c2-i10 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0178 | 0 |
| Xbot-bg-c1-i1 | None-orig | shadow | 957892 | 905643 | 957892 | 804542 | 0 | 2.1111 | 18 |
| Xbot-bg-c1-i1 | None-orig | depth | 927554 | 814621 | 927554 | 162728 | 0 | 1.8127 | 17 |
| Xbot-bg-c1-i1 | None-orig | opaque | 927554 | 814625 | 927554 | 162728 | 1102046 | 2.1601 | 17 |
| Xbot-bg-c1-i1 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0179 | 0 |
| Xbot-bg-c1-i1 | None-R32 | shadow | 957892 | 905641 | 957892 | 804542 | 0 | 2.1384 | 18 |
| Xbot-bg-c1-i1 | None-R32 | depth | 927554 | 814621 | 927554 | 162728 | 0 | 1.7537 | 17 |
| Xbot-bg-c1-i1 | None-R32 | opaque | 927554 | 814623 | 927554 | 162728 | 1102046 | 2.1730 | 17 |
| Xbot-bg-c1-i1 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0171 | 0 |
| Xbot-bg-c1-i1 | Degenerate | shadow | 957892 | 905016 | 957892 | 804542 | 0 | 2.1550 | 18 |
| Xbot-bg-c1-i1 | Degenerate | depth | 927554 | 813990 | 927554 | 162728 | 0 | 1.8392 | 17 |
| Xbot-bg-c1-i1 | Degenerate | opaque | 927554 | 813996 | 927554 | 162728 | 1100057 | 2.1123 | 17 |
| Xbot-bg-c1-i1 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0172 | 0 |
| Xbot-bg-c1-i1 | Compact | shadow | 955774 | 903777 | 955774 | 802424 | 0 | 2.0933 | 18 |
| Xbot-bg-c1-i1 | Compact | depth | 925436 | 812755 | 925436 | 160610 | 0 | 1.7837 | 17 |
| Xbot-bg-c1-i1 | Compact | opaque | 925436 | 812759 | 925436 | 160610 | 1100057 | 2.1559 | 17 |
| Xbot-bg-c1-i1 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0173 | 0 |
| Xbot-bg-c1-i1 | VC-clip | shadow | 957892 | 905645 | 957892 | 802506 | 0 | 2.1336 | 18 |
| Xbot-bg-c1-i1 | VC-clip | depth | 927554 | 814619 | 927554 | 160692 | 0 | 1.8697 | 17 |
| Xbot-bg-c1-i1 | VC-clip | opaque | 927554 | 814623 | 927554 | 160692 | 1100057 | 2.1064 | 17 |
| Xbot-bg-c1-i1 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0183 | 0 |
| Xbot-bg-c1-i1 | VC-discard-any | shadow | 957892 | 905645 | 957892 | 804542 | 2981 | 2.1749 | 18 |
| Xbot-bg-c1-i1 | VC-discard-any | depth | 927554 | 814619 | 927554 | 162728 | 123915 | 1.8730 | 17 |
| Xbot-bg-c1-i1 | VC-discard-any | opaque | 927554 | 814622 | 927554 | 162728 | 1102001 | 2.1003 | 17 |
| Xbot-bg-c1-i1 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0229 | 0 |
