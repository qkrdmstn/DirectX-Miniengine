# MC6 임시 실험 결과

조건: MX450, Release, -debug 0, TAA off, vsync off, 한 세션(조합당 1회 실행), masked skeletal mesh만 비교(Xbot-bg 조합만 배경 포함). 모드 6개(None-orig, None-R32, Degenerate, Compact, VC-clip, VC-discard-any) x 3사이클, 전환 후 30프레임 제외, 모드당 창별 200표본(모드당 600표본 평균). GPU 합 = shadow+depth+opaque+transparent timestamp 합(전체 프레임 아님). 범위 = 3사이클 창 평균의 min~max, Compact 범위와 겹치면 noise.

모드 해석 주의:
- None-orig는 원본 R16 IB, 나머지 IB 읽기 모드(None-R32, Degenerate, Compact, VC-clip, VC-discard-any)는 모두 R32 IB를 읽는다. VC 모드는 제거 없는 R32 전체 사본(None-R32와 같은 구조)으로 전체 IB를 그린다.
- Degenerate는 IA/VS 입력이 줄지 않는다(원본 크기 IB, 같은 draw 수).
- VC-clip은 시각적으로 다르다(정점별 clip, 경계 보간). VC-discard-any는 깊이/그림자 패스에도 PS(discard)가 필요하다.
- 시험 모델 모든 section ibFormat=R16, 모든 draw baseVertex=0(ib-info.txt). baseVertex가 0이 아닌 모델은 시험하지 않았으므로 VS의 vertexId + MaskBaseVertex 이중 가산 가능성은 검증되지 않았다.
- 갱신 비용은 indicative scopes(대략적 범위)다: 이벤트는 Finish() 제출 전까지이며 CPU 값은 구간별로 정의가 다르다. VC는 Mode 7 후보 초기화와 게시 AND만 포함하고 Mode 0 추출 비용은 제외한다.

## 패스별 GPU ms와 합계 (Compact 대비)

| 조합 | 모드 | shadow | depth | opaque | transparent | 합 | 합-Compact | 사이클 범위 | 판정 |
|---|---|---:|---:|---:|---:|---:|---:|---|---|
| Xbot-c1-i1 | None-orig | 0.1101 | 0.1000 | 0.1665 | 0.0155 | 0.3921 | -0.0063 (-1.6%) | 0.3876~0.3946 | beyond range |
| Xbot-c1-i1 | None-R32 | 0.1204 | 0.1059 | 0.1649 | 0.0153 | 0.4064 | 0.0080 (2.0%) | 0.3928~0.4134 | noise |
| Xbot-c1-i1 | Degenerate | 0.1207 | 0.1026 | 0.1633 | 0.0231 | 0.4097 | 0.0113 (2.8%) | 0.3997~0.4285 | noise |
| Xbot-c1-i1 | Compact | 0.1142 | 0.1023 | 0.1641 | 0.0178 | 0.3984 | 0.0000 (0.0%) | 0.3956~0.4033 | - |
| Xbot-c1-i1 | VC-clip | 0.1146 | 0.1059 | 0.1669 | 0.0158 | 0.4032 | 0.0048 (1.2%) | 0.3980~0.4128 | noise |
| Xbot-c1-i1 | VC-discard-any | 0.1194 | 0.1053 | 0.1720 | 0.0157 | 0.4123 | 0.0139 (3.5%) | 0.4113~0.4142 | beyond range |
| Xbot-c1-i10 | None-orig | 1.2927 | 1.0541 | 0.7003 | 0.0152 | 3.0622 | -0.0904 (-2.9%) | 3.0259~3.0857 | beyond range |
| Xbot-c1-i10 | None-R32 | 1.3620 | 1.1225 | 0.7688 | 0.0282 | 3.2815 | 0.1290 (4.1%) | 3.2330~3.3173 | beyond range |
| Xbot-c1-i10 | Degenerate | 1.3509 | 1.1312 | 0.7460 | 0.0264 | 3.2546 | 0.1021 (3.2%) | 3.2376~3.2836 | beyond range |
| Xbot-c1-i10 | Compact | 1.3482 | 1.0711 | 0.7168 | 0.0164 | 3.1525 | 0.0000 (0.0%) | 3.1332~3.1839 | - |
| Xbot-c1-i10 | VC-clip | 1.3739 | 1.1494 | 0.7291 | 0.0242 | 3.2767 | 0.1241 (3.9%) | 3.2518~3.3014 | beyond range |
| Xbot-c1-i10 | VC-discard-any | 1.3921 | 1.1151 | 0.7880 | 0.0349 | 3.3302 | 0.1777 (5.6%) | 3.2991~3.3707 | beyond range |
| CesiumMan-c1-i1 | None-orig | 0.0664 | 0.0478 | 0.1168 | 0.0156 | 0.2466 | 0.0155 (6.7%) | 0.2455~0.2474 | beyond range |
| CesiumMan-c1-i1 | None-R32 | 0.0663 | 0.0488 | 0.1177 | 0.0159 | 0.2487 | 0.0175 (7.6%) | 0.2475~0.2494 | beyond range |
| CesiumMan-c1-i1 | Degenerate | 0.0667 | 0.0463 | 0.1042 | 0.0158 | 0.2331 | 0.0020 (0.8%) | 0.2318~0.2338 | beyond range |
| CesiumMan-c1-i1 | Compact | 0.0657 | 0.0460 | 0.1037 | 0.0158 | 0.2312 | 0.0000 (0.0%) | 0.2309~0.2316 | - |
| CesiumMan-c1-i1 | VC-clip | 0.0655 | 0.0475 | 0.1041 | 0.0158 | 0.2330 | 0.0018 (0.8%) | 0.2323~0.2334 | beyond range |
| CesiumMan-c1-i1 | VC-discard-any | 0.0666 | 0.0463 | 0.1026 | 0.0159 | 0.2314 | 0.0002 (0.1%) | 0.2287~0.2329 | noise |
| CesiumMan-c1-i10 | None-orig | 0.1156 | 0.0861 | 0.4890 | 0.0497 | 0.7404 | 0.0078 (1.1%) | 0.7171~0.7536 | noise |
| CesiumMan-c1-i10 | None-R32 | 0.1281 | 0.0973 | 0.5267 | 0.0425 | 0.7944 | 0.0618 (8.4%) | 0.7427~0.8294 | noise |
| CesiumMan-c1-i10 | Degenerate | 0.1261 | 0.0964 | 0.4825 | 0.0372 | 0.7423 | 0.0096 (1.3%) | 0.7239~0.7564 | noise |
| CesiumMan-c1-i10 | Compact | 0.1244 | 0.0948 | 0.4665 | 0.0470 | 0.7326 | 0.0000 (0.0%) | 0.6650~0.7982 | - |
| CesiumMan-c1-i10 | VC-clip | 0.1303 | 0.1037 | 0.5316 | 0.0567 | 0.8223 | 0.0897 (12.2%) | 0.7906~0.8494 | noise |
| CesiumMan-c1-i10 | VC-discard-any | 0.1299 | 0.0973 | 0.4718 | 0.0386 | 0.7375 | 0.0049 (0.7%) | 0.6837~0.7961 | noise |
| Xbot-c2-i1 | None-orig | 0.1159 | 0.1095 | 0.2106 | 0.0381 | 0.4741 | 0.0269 (6.0%) | 0.4537~0.5070 | noise |
| Xbot-c2-i1 | None-R32 | 0.1221 | 0.1140 | 0.2243 | 0.0438 | 0.5042 | 0.0569 (12.7%) | 0.4861~0.5313 | beyond range |
| Xbot-c2-i1 | Degenerate | 0.1166 | 0.1090 | 0.1994 | 0.0385 | 0.4636 | 0.0163 (3.6%) | 0.4409~0.4843 | noise |
| Xbot-c2-i1 | Compact | 0.1134 | 0.1066 | 0.1993 | 0.0279 | 0.4473 | 0.0000 (0.0%) | 0.4218~0.4622 | - |
| Xbot-c2-i1 | VC-clip | 0.1145 | 0.1120 | 0.2103 | 0.0416 | 0.4785 | 0.0312 (7.0%) | 0.4690~0.4907 | beyond range |
| Xbot-c2-i1 | VC-discard-any | 0.1215 | 0.1115 | 0.2127 | 0.0547 | 0.5004 | 0.0531 (11.9%) | 0.4513~0.5465 | noise |
| Xbot-c2-i10 | None-orig | 1.3515 | 1.0921 | 0.7020 | 0.0157 | 3.1613 | -0.0316 (-1.0%) | 3.1087~3.1911 | noise |
| Xbot-c2-i10 | None-R32 | 1.4088 | 1.1598 | 0.8776 | 0.0374 | 3.4836 | 0.2908 (9.1%) | 3.4130~3.5561 | beyond range |
| Xbot-c2-i10 | Degenerate | 1.3328 | 1.1523 | 0.8179 | 0.0404 | 3.3435 | 0.1507 (4.7%) | 3.2738~3.3823 | beyond range |
| Xbot-c2-i10 | Compact | 1.3639 | 1.1018 | 0.7074 | 0.0198 | 3.1929 | 0.0000 (0.0%) | 3.1257~3.2718 | - |
| Xbot-c2-i10 | VC-clip | 1.3338 | 1.1712 | 0.7964 | 0.0546 | 3.3561 | 0.1633 (5.1%) | 3.2644~3.4273 | noise |
| Xbot-c2-i10 | VC-discard-any | 1.3539 | 1.1944 | 0.8773 | 0.0410 | 3.4667 | 0.2738 (8.6%) | 3.4004~3.5045 | beyond range |
| Xbot-bg-c1-i1 | None-orig | 2.8800 | 2.0834 | 2.6398 | 0.0161 | 7.6192 | 0.0357 (0.5%) | 7.5086~7.6954 | noise |
| Xbot-bg-c1-i1 | None-R32 | 2.8408 | 2.0692 | 2.6097 | 0.0163 | 7.5360 | -0.0476 (-0.6%) | 7.4673~7.6198 | noise |
| Xbot-bg-c1-i1 | Degenerate | 2.8484 | 2.0738 | 2.6220 | 0.0152 | 7.5593 | -0.0242 (-0.3%) | 7.5245~7.5901 | noise |
| Xbot-bg-c1-i1 | Compact | 2.8623 | 2.0813 | 2.6214 | 0.0186 | 7.5836 | 0.0000 (0.0%) | 7.4376~7.6662 | - |
| Xbot-bg-c1-i1 | VC-clip | 2.8704 | 2.0937 | 2.6040 | 0.0164 | 7.5845 | 0.0009 (0.0%) | 7.4836~7.6911 | noise |
| Xbot-bg-c1-i1 | VC-discard-any | 2.8853 | 2.0415 | 2.6381 | 0.0189 | 7.5839 | 0.0003 (0.0%) | 7.5406~7.6565 | noise |

## 패스 합계 카운터 (shadow+depth+opaque+transparent 합, 프레임당 평균)

| 조합 | 모드 | IA prim | VS inv | C inv | C prim | PS inv |
|---|---|---:|---:|---:|---:|---:|
| Xbot-c1-i1 | None-orig | 154368 | 149178 | 154368 | 154368 | 99108 |
| Xbot-c1-i1 | None-R32 | 154368 | 149178 | 154368 | 154368 | 99108 |
| Xbot-c1-i1 | Degenerate | 154368 | 147294 | 154368 | 154368 | 91811 |
| Xbot-c1-i1 | Compact | 148014 | 143586 | 148014 | 148014 | 91811 |
| Xbot-c1-i1 | VC-clip | 154368 | 149178 | 154368 | 148260 | 91811 |
| Xbot-c1-i1 | VC-discard-any | 154368 | 149178 | 154368 | 154368 | 230249 |
| Xbot-c1-i10 | None-orig | 1480392 | 1301908 | 1480392 | 1480392 | 58876 |
| Xbot-c1-i10 | None-R32 | 1480392 | 1301911 | 1480392 | 1480392 | 58876 |
| Xbot-c1-i10 | Degenerate | 1480392 | 1283068 | 1480392 | 1480392 | 53889 |
| Xbot-c1-i10 | Compact | 1416852 | 1246000 | 1416852 | 1416852 | 53889 |
| Xbot-c1-i10 | VC-clip | 1480392 | 1301909 | 1480392 | 1419312 | 53889 |
| Xbot-c1-i10 | VC-discard-any | 1480392 | 1301910 | 1480392 | 1480392 | 170002 |
| CesiumMan-c1-i1 | None-orig | 15009 | 16689 | 15009 | 15009 | 168388 |
| CesiumMan-c1-i1 | None-R32 | 15009 | 16689 | 15009 | 15009 | 168388 |
| CesiumMan-c1-i1 | Degenerate | 15009 | 16494 | 15009 | 15009 | 141166 |
| CesiumMan-c1-i1 | Compact | 14196 | 15954 | 14196 | 14196 | 141166 |
| CesiumMan-c1-i1 | VC-clip | 15009 | 16689 | 15009 | 14388 | 141204 |
| CesiumMan-c1-i1 | VC-discard-any | 15009 | 16689 | 15009 | 15009 | 314102 |
| CesiumMan-c1-i10 | None-orig | 141153 | 140079 | 141153 | 137228 | 89557 |
| CesiumMan-c1-i10 | None-R32 | 141153 | 140078 | 141153 | 137228 | 89557 |
| CesiumMan-c1-i10 | Degenerate | 141153 | 138129 | 141153 | 137228 | 81676 |
| CesiumMan-c1-i10 | Compact | 133023 | 132730 | 133023 | 129369 | 81676 |
| CesiumMan-c1-i10 | VC-clip | 141153 | 140078 | 141153 | 131225 | 81959 |
| CesiumMan-c1-i10 | VC-discard-any | 141153 | 140078 | 141153 | 137228 | 206720 |
| Xbot-c2-i1 | None-orig | 159372 | 164190 | 159372 | 159372 | 109625 |
| Xbot-c2-i1 | None-R32 | 159372 | 164190 | 159372 | 159372 | 109625 |
| Xbot-c2-i1 | Degenerate | 159372 | 160860 | 159372 | 159372 | 91515 |
| Xbot-c2-i1 | Compact | 148350 | 154626 | 148350 | 148350 | 91515 |
| Xbot-c2-i1 | VC-clip | 159372 | 164190 | 159372 | 148596 | 91515 |
| Xbot-c2-i1 | VC-discard-any | 159372 | 164190 | 159372 | 159372 | 236784 |
| Xbot-c2-i10 | None-orig | 1485396 | 1316918 | 1485396 | 1485396 | 60167 |
| Xbot-c2-i10 | None-R32 | 1485396 | 1316922 | 1485396 | 1485396 | 60167 |
| Xbot-c2-i10 | Degenerate | 1485396 | 1283639 | 1485396 | 1485396 | 49425 |
| Xbot-c2-i10 | Compact | 1375176 | 1221282 | 1375176 | 1375176 | 49425 |
| Xbot-c2-i10 | VC-clip | 1485396 | 1316920 | 1485396 | 1377636 | 49425 |
| Xbot-c2-i10 | VC-discard-any | 1485396 | 1316921 | 1485396 | 1485396 | 166132 |
| Xbot-bg-c1-i1 | None-orig | 2813000 | 2534888 | 2813000 | 1129998 | 1102046 |
| Xbot-bg-c1-i1 | None-R32 | 2813000 | 2534886 | 2813000 | 1129998 | 1102046 |
| Xbot-bg-c1-i1 | Degenerate | 2813000 | 2533006 | 2813000 | 1129998 | 1100057 |
| Xbot-bg-c1-i1 | Compact | 2806646 | 2529300 | 2806646 | 1123644 | 1100057 |
| Xbot-bg-c1-i1 | VC-clip | 2813000 | 2534890 | 2813000 | 1123890 | 1100057 |
| Xbot-bg-c1-i1 | VC-discard-any | 2813000 | 2534890 | 2813000 | 1129998 | 1228865 |

## 갱신 비용 (게시당 합계, cpu_ms / gpu_ms, indicative scopes)

Compact = CPU 개수 계산+기록 / GPU 압축 dispatch. Degenerate, None-R32 copy = 기록 / GPU 제자리 쓰기 dispatch. VC = Mode 7 초기화 + 게시 AND(Mode 0 제외). 이벤트 수는 K10 추가 인스턴스 포함.

| 조합 | Compact 이벤트 수 | Compact | Degenerate | None-R32 copy | VC 이벤트(init/apply) | VC |
|---|---:|---|---|---|---|---|
| Xbot-c1-i1 | 1 | 0.764 / 0.046 | 0.317 / 0.032 | 0.242 / 0.031 | 1/1 | 1.488 / 0.020 |
| Xbot-c1-i10 | 10 | 10.237 / 0.548 | 3.579 / 0.319 | 3.719 / 0.319 | 1/1 | 0.715 / 0.015 |
| CesiumMan-c1-i1 | 1 | 0.603 / 0.016 | 0.375 / 0.006 | 0.302 / 0.006 | 1/1 | 1.092 / 0.017 |
| CesiumMan-c1-i10 | 10 | 9.573 / 0.205 | 4.402 / 0.092 | 5.225 / 0.088 | 1/1 | 0.875 / 0.015 |
| Xbot-c2-i1 | 2 | 1.987 / 0.090 | 0.754 / 0.061 | 0.631 / 0.065 | 2/2 | 1.732 / 0.033 |
| Xbot-c2-i10 | 11 | 10.705 / 0.605 | 4.168 / 0.354 | 4.429 / 0.355 | 2/2 | 1.039 / 0.033 |
| Xbot-bg-c1-i1 | 1 | 1.199 / 0.046 | 0.453 / 0.030 | 0.395 / 0.029 | 1/1 | 1.544 / 0.019 |

## 메모리 (bytes, 논리 buffer Width, 인스턴스 합)

Compact IB와 Degenerate IB는 따로 표기한다. None-R32/VC용 R32 전체 사본은 Degenerate IB와 같은 크기의 별도 버퍼다.

| 조합 | Compact IB | Degenerate IB | Triangle State | 1-bit 정점 마스크 | 제거 삼각형 |
|---|---:|---:|---:|---:|---:|
| Xbot-c1-i1 | 563928 | 589344 | 196448 | 3548 | 2118 |
| Xbot-c1-i10 | 5639280 | 5893440 | 1964480 | 35480 | 21180 |
| CesiumMan-c1-i1 | 52812 | 56064 | 18688 | 412 | 271 |
| CesiumMan-c1-i10 | 528120 | 560640 | 186880 | 4120 | 2710 |
| Xbot-c2-i1 | 545256 | 589344 | 196448 | 3548 | 3674 |
| Xbot-c2-i10 | 5452560 | 5893440 | 1964480 | 35480 | 36740 |
| Xbot-bg-c1-i1 | 563928 | 589344 | 196448 | 3548 | 2118 |

## HDR 전체 프레임 비교 (고정 pose, 노출 전 R11G11B10 비트 비교, 1920x1080 전체, Compact 대비 다른 픽셀 수)

| 모델 | None-orig | None-R32 | Degenerate | VC-clip | VC-discard-any | Compact 반복 |
|---|---:|---:|---:|---:|---:|---:|
| Xbot | 3825 | 3825 | 0 | 0 | 0 | 0 |
| CesiumMan | 6966 | 6966 | 0 | 1143 | 0 | 0 |
| Xbot2 | 7870 | 7870 | 0 | 0 | 0 | 0 |

## 패스별 상세 평균 (모드당 600표본)

| 조합 | 모드 | pass | IA prim | VS inv | C inv | C prim | PS inv | GPU ms | draws |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| Xbot-c1-i1 | None-orig | shadow | 51456 | 49726 | 51456 | 51456 | 0 | 0.1101 | 6 |
| Xbot-c1-i1 | None-orig | depth | 51456 | 49726 | 51456 | 51456 | 0 | 0.1000 | 6 |
| Xbot-c1-i1 | None-orig | opaque | 51456 | 49726 | 51456 | 51456 | 99108 | 0.1665 | 6 |
| Xbot-c1-i1 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0155 | 0 |
| Xbot-c1-i1 | None-R32 | shadow | 51456 | 49726 | 51456 | 51456 | 0 | 0.1204 | 6 |
| Xbot-c1-i1 | None-R32 | depth | 51456 | 49726 | 51456 | 51456 | 0 | 0.1059 | 6 |
| Xbot-c1-i1 | None-R32 | opaque | 51456 | 49726 | 51456 | 51456 | 99108 | 0.1649 | 6 |
| Xbot-c1-i1 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0153 | 0 |
| Xbot-c1-i1 | Degenerate | shadow | 51456 | 49098 | 51456 | 51456 | 0 | 0.1207 | 6 |
| Xbot-c1-i1 | Degenerate | depth | 51456 | 49098 | 51456 | 51456 | 0 | 0.1026 | 6 |
| Xbot-c1-i1 | Degenerate | opaque | 51456 | 49098 | 51456 | 51456 | 91811 | 0.1633 | 6 |
| Xbot-c1-i1 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0231 | 0 |
| Xbot-c1-i1 | Compact | shadow | 49338 | 47862 | 49338 | 49338 | 0 | 0.1142 | 6 |
| Xbot-c1-i1 | Compact | depth | 49338 | 47862 | 49338 | 49338 | 0 | 0.1023 | 6 |
| Xbot-c1-i1 | Compact | opaque | 49338 | 47862 | 49338 | 49338 | 91811 | 0.1641 | 6 |
| Xbot-c1-i1 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0178 | 0 |
| Xbot-c1-i1 | VC-clip | shadow | 51456 | 49726 | 51456 | 49420 | 0 | 0.1146 | 6 |
| Xbot-c1-i1 | VC-clip | depth | 51456 | 49726 | 51456 | 49420 | 0 | 0.1059 | 6 |
| Xbot-c1-i1 | VC-clip | opaque | 51456 | 49726 | 51456 | 49420 | 91811 | 0.1669 | 6 |
| Xbot-c1-i1 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0158 | 0 |
| Xbot-c1-i1 | VC-discard-any | shadow | 51456 | 49726 | 51456 | 51456 | 3267 | 0.1194 | 6 |
| Xbot-c1-i1 | VC-discard-any | depth | 51456 | 49726 | 51456 | 51456 | 133235 | 0.1053 | 6 |
| Xbot-c1-i1 | VC-discard-any | opaque | 51456 | 49726 | 51456 | 51456 | 93746 | 0.1720 | 6 |
| Xbot-c1-i1 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0157 | 0 |
| Xbot-c1-i10 | None-orig | shadow | 493464 | 433970 | 493464 | 493464 | 0 | 1.2927 | 24 |
| Xbot-c1-i10 | None-orig | depth | 493464 | 433966 | 493464 | 493464 | 0 | 1.0541 | 24 |
| Xbot-c1-i10 | None-orig | opaque | 493464 | 433971 | 493464 | 493464 | 58876 | 0.7003 | 24 |
| Xbot-c1-i10 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0152 | 0 |
| Xbot-c1-i10 | None-R32 | shadow | 493464 | 433971 | 493464 | 493464 | 0 | 1.3620 | 24 |
| Xbot-c1-i10 | None-R32 | depth | 493464 | 433968 | 493464 | 493464 | 0 | 1.1225 | 24 |
| Xbot-c1-i10 | None-R32 | opaque | 493464 | 433972 | 493464 | 493464 | 58876 | 0.7688 | 24 |
| Xbot-c1-i10 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0282 | 0 |
| Xbot-c1-i10 | Degenerate | shadow | 493464 | 427689 | 493464 | 493464 | 0 | 1.3509 | 24 |
| Xbot-c1-i10 | Degenerate | depth | 493464 | 427687 | 493464 | 493464 | 0 | 1.1312 | 24 |
| Xbot-c1-i10 | Degenerate | opaque | 493464 | 427692 | 493464 | 493464 | 53889 | 0.7460 | 24 |
| Xbot-c1-i10 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0264 | 0 |
| Xbot-c1-i10 | Compact | shadow | 472284 | 415332 | 472284 | 472284 | 0 | 1.3482 | 24 |
| Xbot-c1-i10 | Compact | depth | 472284 | 415336 | 472284 | 472284 | 0 | 1.0711 | 24 |
| Xbot-c1-i10 | Compact | opaque | 472284 | 415332 | 472284 | 472284 | 53889 | 0.7168 | 24 |
| Xbot-c1-i10 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0164 | 0 |
| Xbot-c1-i10 | VC-clip | shadow | 493464 | 433971 | 493464 | 473104 | 0 | 1.3739 | 24 |
| Xbot-c1-i10 | VC-clip | depth | 493464 | 433967 | 493464 | 473104 | 0 | 1.1494 | 24 |
| Xbot-c1-i10 | VC-clip | opaque | 493464 | 433972 | 493464 | 473104 | 53889 | 0.7291 | 24 |
| Xbot-c1-i10 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0242 | 0 |
| Xbot-c1-i10 | VC-discard-any | shadow | 493464 | 433971 | 493464 | 493464 | 29898 | 1.3921 | 24 |
| Xbot-c1-i10 | VC-discard-any | depth | 493464 | 433967 | 493464 | 493464 | 86172 | 1.1151 | 24 |
| Xbot-c1-i10 | VC-discard-any | opaque | 493464 | 433972 | 493464 | 493464 | 53932 | 0.7880 | 24 |
| Xbot-c1-i10 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0349 | 0 |
| CesiumMan-c1-i1 | None-orig | shadow | 5003 | 5563 | 5003 | 5003 | 0 | 0.0664 | 5 |
| CesiumMan-c1-i1 | None-orig | depth | 5003 | 5563 | 5003 | 5003 | 0 | 0.0478 | 5 |
| CesiumMan-c1-i1 | None-orig | opaque | 5003 | 5563 | 5003 | 5003 | 168388 | 0.1168 | 5 |
| CesiumMan-c1-i1 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0156 | 0 |
| CesiumMan-c1-i1 | None-R32 | shadow | 5003 | 5563 | 5003 | 5003 | 0 | 0.0663 | 5 |
| CesiumMan-c1-i1 | None-R32 | depth | 5003 | 5563 | 5003 | 5003 | 0 | 0.0488 | 5 |
| CesiumMan-c1-i1 | None-R32 | opaque | 5003 | 5563 | 5003 | 5003 | 168388 | 0.1177 | 5 |
| CesiumMan-c1-i1 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0159 | 0 |
| CesiumMan-c1-i1 | Degenerate | shadow | 5003 | 5498 | 5003 | 5003 | 0 | 0.0667 | 5 |
| CesiumMan-c1-i1 | Degenerate | depth | 5003 | 5498 | 5003 | 5003 | 0 | 0.0463 | 5 |
| CesiumMan-c1-i1 | Degenerate | opaque | 5003 | 5498 | 5003 | 5003 | 141166 | 0.1042 | 5 |
| CesiumMan-c1-i1 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0158 | 0 |
| CesiumMan-c1-i1 | Compact | shadow | 4732 | 5318 | 4732 | 4732 | 0 | 0.0657 | 5 |
| CesiumMan-c1-i1 | Compact | depth | 4732 | 5318 | 4732 | 4732 | 0 | 0.0460 | 5 |
| CesiumMan-c1-i1 | Compact | opaque | 4732 | 5318 | 4732 | 4732 | 141166 | 0.1037 | 5 |
| CesiumMan-c1-i1 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0158 | 0 |
| CesiumMan-c1-i1 | VC-clip | shadow | 5003 | 5563 | 5003 | 4796 | 0 | 0.0655 | 5 |
| CesiumMan-c1-i1 | VC-clip | depth | 5003 | 5563 | 5003 | 4796 | 0 | 0.0475 | 5 |
| CesiumMan-c1-i1 | VC-clip | opaque | 5003 | 5563 | 5003 | 4796 | 141204 | 0.1041 | 5 |
| CesiumMan-c1-i1 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0158 | 0 |
| CesiumMan-c1-i1 | VC-discard-any | shadow | 5003 | 5563 | 5003 | 5003 | 3770 | 0.0666 | 5 |
| CesiumMan-c1-i1 | VC-discard-any | depth | 5003 | 5563 | 5003 | 5003 | 166089 | 0.0463 | 5 |
| CesiumMan-c1-i1 | VC-discard-any | opaque | 5003 | 5563 | 5003 | 5003 | 144243 | 0.1026 | 5 |
| CesiumMan-c1-i1 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0159 | 0 |
| CesiumMan-c1-i10 | None-orig | shadow | 47051 | 46693 | 47051 | 43126 | 0 | 0.1156 | 14 |
| CesiumMan-c1-i10 | None-orig | depth | 47051 | 46693 | 47051 | 47051 | 0 | 0.0861 | 14 |
| CesiumMan-c1-i10 | None-orig | opaque | 47051 | 46693 | 47051 | 47051 | 89557 | 0.4890 | 14 |
| CesiumMan-c1-i10 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0497 | 0 |
| CesiumMan-c1-i10 | None-R32 | shadow | 47051 | 46693 | 47051 | 43126 | 0 | 0.1281 | 14 |
| CesiumMan-c1-i10 | None-R32 | depth | 47051 | 46693 | 47051 | 47051 | 0 | 0.0973 | 14 |
| CesiumMan-c1-i10 | None-R32 | opaque | 47051 | 46692 | 47051 | 47051 | 89557 | 0.5267 | 14 |
| CesiumMan-c1-i10 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0425 | 0 |
| CesiumMan-c1-i10 | Degenerate | shadow | 47051 | 46043 | 47051 | 43126 | 0 | 0.1261 | 14 |
| CesiumMan-c1-i10 | Degenerate | depth | 47051 | 46043 | 47051 | 47051 | 0 | 0.0964 | 14 |
| CesiumMan-c1-i10 | Degenerate | opaque | 47051 | 46043 | 47051 | 47051 | 81676 | 0.4825 | 14 |
| CesiumMan-c1-i10 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0372 | 0 |
| CesiumMan-c1-i10 | Compact | shadow | 44341 | 44243 | 44341 | 40687 | 0 | 0.1244 | 14 |
| CesiumMan-c1-i10 | Compact | depth | 44341 | 44243 | 44341 | 44341 | 0 | 0.0948 | 14 |
| CesiumMan-c1-i10 | Compact | opaque | 44341 | 44244 | 44341 | 44341 | 81676 | 0.4665 | 14 |
| CesiumMan-c1-i10 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0470 | 0 |
| CesiumMan-c1-i10 | VC-clip | shadow | 47051 | 46693 | 47051 | 41263 | 0 | 0.1303 | 14 |
| CesiumMan-c1-i10 | VC-clip | depth | 47051 | 46693 | 47051 | 44981 | 0 | 0.1037 | 14 |
| CesiumMan-c1-i10 | VC-clip | opaque | 47051 | 46692 | 47051 | 44981 | 81959 | 0.5316 | 14 |
| CesiumMan-c1-i10 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0567 | 0 |
| CesiumMan-c1-i10 | VC-discard-any | shadow | 47051 | 46693 | 47051 | 43126 | 31618 | 0.1299 | 14 |
| CesiumMan-c1-i10 | VC-discard-any | depth | 47051 | 46693 | 47051 | 47051 | 93224 | 0.0973 | 14 |
| CesiumMan-c1-i10 | VC-discard-any | opaque | 47051 | 46692 | 47051 | 47051 | 81878 | 0.4718 | 14 |
| CesiumMan-c1-i10 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0386 | 0 |
| Xbot-c2-i1 | None-orig | shadow | 53124 | 54730 | 53124 | 53124 | 0 | 0.1159 | 10 |
| Xbot-c2-i1 | None-orig | depth | 53124 | 54730 | 53124 | 53124 | 0 | 0.1095 | 10 |
| Xbot-c2-i1 | None-orig | opaque | 53124 | 54730 | 53124 | 53124 | 109625 | 0.2106 | 10 |
| Xbot-c2-i1 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0381 | 0 |
| Xbot-c2-i1 | None-R32 | shadow | 53124 | 54730 | 53124 | 53124 | 0 | 0.1221 | 10 |
| Xbot-c2-i1 | None-R32 | depth | 53124 | 54730 | 53124 | 53124 | 0 | 0.1140 | 10 |
| Xbot-c2-i1 | None-R32 | opaque | 53124 | 54730 | 53124 | 53124 | 109625 | 0.2243 | 10 |
| Xbot-c2-i1 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0438 | 0 |
| Xbot-c2-i1 | Degenerate | shadow | 53124 | 53620 | 53124 | 53124 | 0 | 0.1166 | 10 |
| Xbot-c2-i1 | Degenerate | depth | 53124 | 53620 | 53124 | 53124 | 0 | 0.1090 | 10 |
| Xbot-c2-i1 | Degenerate | opaque | 53124 | 53620 | 53124 | 53124 | 91515 | 0.1994 | 10 |
| Xbot-c2-i1 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0385 | 0 |
| Xbot-c2-i1 | Compact | shadow | 49450 | 51542 | 49450 | 49450 | 0 | 0.1134 | 10 |
| Xbot-c2-i1 | Compact | depth | 49450 | 51542 | 49450 | 49450 | 0 | 0.1066 | 10 |
| Xbot-c2-i1 | Compact | opaque | 49450 | 51542 | 49450 | 49450 | 91515 | 0.1993 | 10 |
| Xbot-c2-i1 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0279 | 0 |
| Xbot-c2-i1 | VC-clip | shadow | 53124 | 54730 | 53124 | 49532 | 0 | 0.1145 | 10 |
| Xbot-c2-i1 | VC-clip | depth | 53124 | 54730 | 53124 | 49532 | 0 | 0.1120 | 10 |
| Xbot-c2-i1 | VC-clip | opaque | 53124 | 54730 | 53124 | 49532 | 91515 | 0.2103 | 10 |
| Xbot-c2-i1 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0416 | 0 |
| Xbot-c2-i1 | VC-discard-any | shadow | 53124 | 54730 | 53124 | 53124 | 3138 | 0.1215 | 10 |
| Xbot-c2-i1 | VC-discard-any | depth | 53124 | 54730 | 53124 | 53124 | 135145 | 0.1115 | 10 |
| Xbot-c2-i1 | VC-discard-any | opaque | 53124 | 54730 | 53124 | 53124 | 98501 | 0.2127 | 10 |
| Xbot-c2-i1 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0547 | 0 |
| Xbot-c2-i10 | None-orig | shadow | 495132 | 438972 | 495132 | 495132 | 0 | 1.3515 | 28 |
| Xbot-c2-i10 | None-orig | depth | 495132 | 438970 | 495132 | 495132 | 0 | 1.0921 | 28 |
| Xbot-c2-i10 | None-orig | opaque | 495132 | 438976 | 495132 | 495132 | 60167 | 0.7020 | 28 |
| Xbot-c2-i10 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0157 | 0 |
| Xbot-c2-i10 | None-R32 | shadow | 495132 | 438976 | 495132 | 495132 | 0 | 1.4088 | 28 |
| Xbot-c2-i10 | None-R32 | depth | 495132 | 438970 | 495132 | 495132 | 0 | 1.1598 | 28 |
| Xbot-c2-i10 | None-R32 | opaque | 495132 | 438976 | 495132 | 495132 | 60167 | 0.8776 | 28 |
| Xbot-c2-i10 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0374 | 0 |
| Xbot-c2-i10 | Degenerate | shadow | 495132 | 427881 | 495132 | 495132 | 0 | 1.3328 | 28 |
| Xbot-c2-i10 | Degenerate | depth | 495132 | 427881 | 495132 | 495132 | 0 | 1.1523 | 28 |
| Xbot-c2-i10 | Degenerate | opaque | 495132 | 427877 | 495132 | 495132 | 49425 | 0.8179 | 28 |
| Xbot-c2-i10 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0404 | 0 |
| Xbot-c2-i10 | Compact | shadow | 458392 | 407093 | 458392 | 458392 | 0 | 1.3639 | 28 |
| Xbot-c2-i10 | Compact | depth | 458392 | 407094 | 458392 | 458392 | 0 | 1.1018 | 28 |
| Xbot-c2-i10 | Compact | opaque | 458392 | 407096 | 458392 | 458392 | 49425 | 0.7074 | 28 |
| Xbot-c2-i10 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0198 | 0 |
| Xbot-c2-i10 | VC-clip | shadow | 495132 | 438974 | 495132 | 459212 | 0 | 1.3338 | 28 |
| Xbot-c2-i10 | VC-clip | depth | 495132 | 438971 | 495132 | 459212 | 0 | 1.1712 | 28 |
| Xbot-c2-i10 | VC-clip | opaque | 495132 | 438975 | 495132 | 459212 | 49425 | 0.7964 | 28 |
| Xbot-c2-i10 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0546 | 0 |
| Xbot-c2-i10 | VC-discard-any | shadow | 495132 | 438975 | 495132 | 495132 | 29901 | 1.3539 | 28 |
| Xbot-c2-i10 | VC-discard-any | depth | 495132 | 438970 | 495132 | 495132 | 86415 | 1.1944 | 28 |
| Xbot-c2-i10 | VC-discard-any | opaque | 495132 | 438976 | 495132 | 495132 | 49816 | 0.8773 | 28 |
| Xbot-c2-i10 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0410 | 0 |
| Xbot-bg-c1-i1 | None-orig | shadow | 957892 | 905636 | 957892 | 804542 | 0 | 2.8800 | 18 |
| Xbot-bg-c1-i1 | None-orig | depth | 927554 | 814640 | 927554 | 162728 | 0 | 2.0834 | 17 |
| Xbot-bg-c1-i1 | None-orig | opaque | 927554 | 814613 | 927554 | 162728 | 1102046 | 2.6398 | 17 |
| Xbot-bg-c1-i1 | None-orig | transparent | 0 | 0 | 0 | 0 | 0 | 0.0161 | 0 |
| Xbot-bg-c1-i1 | None-R32 | shadow | 957892 | 905636 | 957892 | 804542 | 0 | 2.8408 | 18 |
| Xbot-bg-c1-i1 | None-R32 | depth | 927554 | 814635 | 927554 | 162728 | 0 | 2.0692 | 17 |
| Xbot-bg-c1-i1 | None-R32 | opaque | 927554 | 814615 | 927554 | 162728 | 1102046 | 2.6097 | 17 |
| Xbot-bg-c1-i1 | None-R32 | transparent | 0 | 0 | 0 | 0 | 0 | 0.0163 | 0 |
| Xbot-bg-c1-i1 | Degenerate | shadow | 957892 | 905010 | 957892 | 804542 | 0 | 2.8484 | 18 |
| Xbot-bg-c1-i1 | Degenerate | depth | 927554 | 814011 | 927554 | 162728 | 0 | 2.0738 | 17 |
| Xbot-bg-c1-i1 | Degenerate | opaque | 927554 | 813984 | 927554 | 162728 | 1100057 | 2.6220 | 17 |
| Xbot-bg-c1-i1 | Degenerate | transparent | 0 | 0 | 0 | 0 | 0 | 0.0152 | 0 |
| Xbot-bg-c1-i1 | Compact | shadow | 955774 | 903775 | 955774 | 802424 | 0 | 2.8623 | 18 |
| Xbot-bg-c1-i1 | Compact | depth | 925436 | 812777 | 925436 | 160610 | 0 | 2.0813 | 17 |
| Xbot-bg-c1-i1 | Compact | opaque | 925436 | 812748 | 925436 | 160610 | 1100057 | 2.6214 | 17 |
| Xbot-bg-c1-i1 | Compact | transparent | 0 | 0 | 0 | 0 | 0 | 0.0186 | 0 |
| Xbot-bg-c1-i1 | VC-clip | shadow | 957892 | 905640 | 957892 | 802506 | 0 | 2.8704 | 18 |
| Xbot-bg-c1-i1 | VC-clip | depth | 927554 | 814635 | 927554 | 160692 | 0 | 2.0937 | 17 |
| Xbot-bg-c1-i1 | VC-clip | opaque | 927554 | 814616 | 927554 | 160692 | 1100057 | 2.6040 | 17 |
| Xbot-bg-c1-i1 | VC-clip | transparent | 0 | 0 | 0 | 0 | 0 | 0.0164 | 0 |
| Xbot-bg-c1-i1 | VC-discard-any | shadow | 957892 | 905638 | 957892 | 804542 | 2948 | 2.8853 | 18 |
| Xbot-bg-c1-i1 | VC-discard-any | depth | 927554 | 814640 | 927554 | 162728 | 123915 | 2.0415 | 17 |
| Xbot-bg-c1-i1 | VC-discard-any | opaque | 927554 | 814611 | 927554 | 162728 | 1102001 | 2.6381 | 17 |
| Xbot-bg-c1-i1 | VC-discard-any | transparent | 0 | 0 | 0 | 0 | 0 | 0.0189 | 0 |

# MC5 전/MC6 후 비교

## VC 갱신 비용 (게시 이벤트 합계, cpu_ms / gpu_ms) 전 -> 후, 참고로 Compact 전체 합과 이벤트당 평균

VC-init = Mode 7 후보 초기화, VC-apply = 게시 AND(별도 컨텍스트 Begin~Finish 포함, MC6). 합계는 조합 한 번의 모든 이벤트(cut 수만큼)이며 인스턴스 수와 무관하다. VC-copy = 추가 인스턴스 마스크 복사(MC6 신규, 인스턴스당 1회).

| 조합 | VC-init 전 | VC-init 후 | VC-apply 전 | VC-apply 후 | VC 합 전 | VC 합 후 | VC-copy 후 (개수, 합) | Compact 이벤트 수 | Compact 합 (MC6) | Compact 이벤트당 평균 |
|---|---|---|---|---|---|---|---|---:|---|---|
| Xbot-c1-i1 | 1.292 / 0.0074 | 1.344 / 0.0121 | 2.851 / 0.0065 | 0.144 / 0.0082 | 4.143 / 0.0139 | 1.488 / 0.0203 | 0, 0.000 / 0.0000 | 1 | 0.764 / 0.0465 | 0.764 / 0.0465 |
| Xbot-c1-i10 | 1.075 / 0.0068 | 0.580 / 0.0082 | 3.704 / 0.0092 | 0.135 / 0.0066 | 4.780 / 0.0160 | 0.715 / 0.0148 | 9, 0.828 / 0.0483 | 10 | 10.237 / 0.5476 | 1.024 / 0.0548 |
| CesiumMan-c1-i1 | 1.258 / 0.0076 | 0.965 / 0.0119 | 4.213 / 0.0072 | 0.126 / 0.0050 | 5.471 / 0.0148 | 1.092 / 0.0169 | 0, 0.000 / 0.0000 | 1 | 0.603 / 0.0156 | 0.603 / 0.0156 |
| CesiumMan-c1-i10 | 1.397 / 0.0081 | 0.612 / 0.0084 | 4.559 / 0.0055 | 0.263 / 0.0061 | 5.956 / 0.0136 | 0.875 / 0.0146 | 9, 1.070 / 0.0394 | 10 | 9.573 / 0.2053 | 0.957 / 0.0205 |
| Xbot-c2-i1 | 1.535 / 0.0165 | 1.463 / 0.0198 | 2.642 / 0.0141 | 0.269 / 0.0135 | 4.177 / 0.0306 | 1.732 / 0.0333 | 0, 0.000 / 0.0000 | 2 | 1.987 / 0.0901 | 0.994 / 0.0451 |
| Xbot-c2-i10 | 1.219 / 0.0165 | 0.739 / 0.0186 | 2.859 / 0.0164 | 0.300 / 0.0147 | 4.078 / 0.0329 | 1.039 / 0.0333 | 9, 0.815 / 0.0474 | 11 | 10.705 / 0.6049 | 0.973 / 0.0550 |
| Xbot-bg-c1-i1 | 1.442 / 0.0077 | 1.348 / 0.0102 | 7.053 / 0.0083 | 0.197 / 0.0083 | 8.495 / 0.0160 | 1.544 / 0.0185 | 0, 0.000 / 0.0000 | 1 | 1.199 / 0.0460 | 1.199 / 0.0460 |

## 렌더 GPU ms 합 (shadow+depth+opaque+transparent, 모드 평균) MC5 -> MC6

| 조합 | None-orig | None-R32 | Degenerate | Compact | VC-clip | VC-discard-any |
|---|---:|---:|---:|---:|---:|---:|
| Xbot-c1-i1 | 0.6028 -> 0.3921 | 0.6314 -> 0.4064 | 0.6201 -> 0.4097 | 0.6348 -> 0.3984 | 0.6366 -> 0.4032 | 0.6279 -> 0.4123 |
| Xbot-c1-i10 | 2.4582 -> 3.0622 | 2.6113 -> 3.2815 | 2.6637 -> 3.2546 | 2.4521 -> 3.1525 | 2.5562 -> 3.2767 | 2.5836 -> 3.3302 |
| CesiumMan-c1-i1 | 0.3167 -> 0.2466 | 0.3320 -> 0.2487 | 0.3668 -> 0.2331 | 0.3447 -> 0.2312 | 0.3876 -> 0.2330 | 0.3378 -> 0.2314 |
| CesiumMan-c1-i10 | 0.4206 -> 0.7404 | 0.4339 -> 0.7944 | 0.4692 -> 0.7423 | 0.4239 -> 0.7326 | 0.4691 -> 0.8223 | 0.4553 -> 0.7375 |
| Xbot-c2-i1 | 0.4629 -> 0.4741 | 0.4759 -> 0.5042 | 0.4734 -> 0.4636 | 0.4843 -> 0.4473 | 0.5996 -> 0.4785 | 0.5042 -> 0.5004 |
| Xbot-c2-i10 | 2.3587 -> 3.1613 | 2.4700 -> 3.4836 | 2.4455 -> 3.3435 | 2.3333 -> 3.1929 | 2.3978 -> 3.3561 | 2.4529 -> 3.4667 |
| Xbot-bg-c1-i1 | 6.1019 -> 7.6192 | 6.0822 -> 7.5360 | 6.1237 -> 7.5593 | 6.0501 -> 7.5836 | 6.1280 -> 7.5845 | 6.1710 -> 7.5839 |

## 일회성 준비 비용 (vc-setup.txt, 측정 구간 밖)

```
VC-PSO-create 1.1379 ms
VC-candidate-alloc 0.1947 ms
VC-mask-alloc 0.4295 ms
VC-PSO-create 1.2022 ms
VC-candidate-alloc 0.2630 ms
VC-mask-alloc 0.4501 ms
VC-mask-alloc 1.2798 ms
VC-mask-alloc 0.7605 ms
VC-mask-alloc 1.5132 ms
VC-mask-alloc 0.7762 ms
VC-mask-alloc 0.5098 ms
VC-mask-alloc 0.5191 ms
VC-mask-alloc 0.7235 ms
VC-mask-alloc 0.5209 ms
VC-mask-alloc 0.5193 ms
VC-PSO-create 1.4716 ms
VC-candidate-alloc 0.1532 ms
VC-mask-alloc 0.2290 ms
VC-PSO-create 2.6176 ms
VC-candidate-alloc 0.2997 ms
VC-mask-alloc 0.8483 ms
VC-mask-alloc 0.4115 ms
VC-mask-alloc 0.4014 ms
VC-mask-alloc 0.4957 ms
VC-mask-alloc 0.3914 ms
VC-mask-alloc 0.2962 ms
VC-mask-alloc 0.4248 ms
VC-mask-alloc 0.5889 ms
VC-mask-alloc 0.3969 ms
VC-mask-alloc 0.2872 ms
VC-PSO-create 1.9230 ms
VC-candidate-alloc 0.2194 ms
VC-mask-alloc 0.6001 ms
VC-PSO-create 1.8405 ms
VC-candidate-alloc 0.2799 ms
VC-mask-alloc 0.5247 ms
VC-mask-alloc 1.0985 ms
VC-mask-alloc 0.9768 ms
VC-mask-alloc 0.5722 ms
VC-mask-alloc 0.5784 ms
VC-mask-alloc 0.6635 ms
VC-mask-alloc 0.8106 ms
VC-mask-alloc 0.4292 ms
VC-mask-alloc 0.4003 ms
VC-mask-alloc 0.5226 ms
VC-PSO-create 2.2730 ms
VC-candidate-alloc 0.7245 ms
VC-mask-alloc 0.6840 ms
```
