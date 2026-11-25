상태: 완료 — 기준 `049c3579` 위 녹화용 재현 구현; 기준 `1ec89f9f` 위 CesiumMan 사선 절단 옵션·열린 GPU 패치 원인 조사 완료(미커밋, 2026-11-25); 45도 캡 문제는 미해결

# NDC 로컬 패치의 애니메이션 전환 문제 재현

Xbot의 오른쪽 다리가 뒤로 간 순간 허벅지를 절단하고 walk에서 idle로 전환한다.
`-patch-anim-follow 0`에서는 몸통 쪽 패치가 절단 당시 각도로 굳어 새 애니메이션을 따르지 않는다.
이는 이 엔진의 NDC 로컬 패치 해석을 보여 주는 재현이며 원본 발표 구현 자체의 검증은 아니다.

| 옵션 | 의미 |
| --- | --- |
| `-skeletal-break-demo-delay <초>` | 기본 0. 첫 프레임부터 wall clock 대기. 장면을 표시하되 시작 클립 시간 0을 유지하고 절단하지 않는다. |
| `-skeletal-break-demo-anim <이름 또는 번호>` | 시작 클립. 기본 0. Xbot은 agree/headShake/idle/run/sad_pose/sneak_pose/walk = 0~6. |
| `-skeletal-break-demo-cut-time <초 또는 auto>` | 실험 시작부터 누적 재생 시간에 처음 도달한 프레임에 요청한다. 생략하면 시작 후 60프레임째. |
| `-skeletal-break-demo-next <이름 또는 번호>` | 게시 후 전환할 반복 클립. 생략하면 전환하지 않는다. |
| `-skeletal-break-demo-switch <초>` | Applied 이후 wall clock 대기. 기본 1.0. next가 없으면 효과 없음. |
| `-skeletal-break-demo-tilt <도>` | 기본 0. 유한한 양·음수 각도. 평면 중심과 크기를 유지하고 모델 +X의 평면 투영축 주위로 회전한다. |

새 옵션은 `-skeletal-break-demo`와 함께만 유효하다. 초는 유한한 0 이상 숫자여야 한다.
next가 있고 `-patch-anim-follow`를 생략하면 0이며, 명시한 값이 우선한다.
기본 데모는 클립 0, 프레임 60 절단, 추종 1, 전환 없음으로 유지한다.

`ConfigureDemoAnimation`은 임시 ModelInstance에서 시작 클립 한 주기의 [0, duration)을 60회 평가한다.
모델 +Z를 전방으로 삼아 `dot(절단 본 첫 자식 - Hips, +Z)`가 최소인 시각을 고른다.
본 생략 시 auto는 mixamorig:RightUpLeg와 첫 자식 RightLeg를 사용한다.
이는 Xbot용 방향 근사이며 다른 모델의 전방이나 본 이름을 자동 추론하지 않는다.

작업 디렉터리는 `MiniEngine/ModelViewer`이다. 녹화 권장 명령:

```text
../Build/x64/Release/Output/ModelViewer/ModelViewer.exe -skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:RightUpLeg -skeletal-break-demo-anim walk -skeletal-break-demo-cut-time auto -skeletal-break-demo-next idle -skeletal-break-demo-switch 1.0 -skeletal-break-demo-delay 5
```

디버그 출력 예상 순서:

```text
Skeletal demo auto cut-time=... clip=6 child=... forward=+Z samples=60
Skeletal demo start frame=... delay=5.00
Skeletal demo bone=mixamorig:RightUpLeg node=... child=...
Skeletal demo request frame=... model=1
Skeletal demo Applied frame=... model=1
Skeletal demo switch frame=... clip=idle
```

auto 선택 로그는 로딩 중, start 로그는 지연 종료 후 나온다.
애니메이션과 물리는 기존 데모처럼 프레임당 고정 1/60초이며 wall clock과 재생 속도가 다를 수 있다.
`ModelInstance::GetAnimationTime`의 주기 내 시간을 누적해 여러 주기 뒤의 절단도 지원한다.
전환은 `GameObject::PauseAnimation`과 PlayAnimation을 사용한다.
기존 ResetAnimation/StopAnimation의 역전된 `>=` 검사 버그는 범위 밖이므로 그대로 둔다.

## 완전 따르기 한계 씬

CesiumMan의 허벅지를 사선으로 자른 뒤에도 몸통 쪽 잔여 패치가 원래 걷기의 허벅지 회전을 그대로 따라 흔들리는 한계를 녹화한다.
`-skeletal-break-demo-next`와 `-patch-anim-follow`를 생략하므로 추종 기본값 1을 유지하며 클립을 전환하지 않는다. 절단 시 카메라는 이동하지 않는다.
기본 전체 장면 카메라에서는 오른쪽 끝 CesiumMan이 화면 밖일 수 있으므로, 녹화 전 5초 지연 중 오른쪽 마우스와 WASD/QE로 CesiumMan 측면을 잡는다.

```text
../Build/x64/Release/Output/ModelViewer/ModelViewer.exe -skeletal-break-demo 2 -skeletal-break-demo-bone leg_joint_R_1 -skeletal-break-demo-tilt 30 -skeletal-break-demo-cut-time 0.5 -skeletal-break-demo-delay 5
```

원본 `Models/CesiumMan.glb`에서 오른쪽 허벅지는 `leg_joint_R_1`, 직접 자식은 `leg_joint_R_2`로 확인했다. 기본 클립 0의 주기는 2초이며 절단 시각은 누적 재생 0.5초를 사용한다.
`BuildSkeletalBonePlane`의 본 중점·법선을 기준으로, 객체 회전을 적용한 모델 +X를 평면에 투영하고 정규화한 축을 쓴다.
이는 모델 좌우 방향에 가장 가까운 평면 내 축이므로 앞뒤로 높이가 달라지는 사선이 측면에서 드러난다. +X와 법선이 평행한 퇴화 상황에서는 기존 평면 X축을 쓴다.
`TryBuildCutPlaneData`로 회전된 세 기저와 기존 중심·가로·세로 크기를 재구성한다. 기본 0도는 원래 평면을 그대로 사용한다.

CesiumMan에는 `mixamorig:Hips`가 없어 현재 `ConfigureDemoAnimation`의 `cut-time auto`가 거절된다.
이 자동 선택은 Xbot의 +Z 전방 가정도 사용하므로 CesiumMan에서는 숫자 시각을 지정한다. 모델별 자동 선택은 추가하지 않는다.
`Skeletal demo tilt=45.0 normal=(...) boneNormal=(...)`로 기울어진 법선과 원래 본 방향을 함께 확인할 수 있다.

사선 절단 검증(2026-11-25): Release·Debug/Windows(x64) 빌드 성공(exit 0, Debug 152.6초). 기존 컴파일 경고와 빌드 후 `pwsh.exe` 미발견 메시지는 남았다. 위 명령의 delay를 2로 바꾸고 `-debug 1` 및 기존 캡처 옵션으로 25초 실행한 뒤 시작한 PID만 종료했다. GPU 로그는 NVIDIA GeForce MX450을 확인했다.

```text
Skeletal demo start frame=114 delay=2.00
Skeletal demo bone=leg_joint_R_1 node=17 child=18
Skeletal demo tilt=45.0 normal=(-0.0227,-0.2351,-0.9717) boneNormal=(-0.0321,-0.8533,-0.5205)
Skeletal demo request frame=143 model=2
Skeletal demo Applied frame=147 model=2
```

두 법선의 내적은 0.707109(약 45도)이며 절단 결과 `error=0`, D3D12 ERROR/CORRUPTION은 0건이었다. 기존 DXGI #80, D3D12 #974/#1008 경고와 드라이버 로그는 남아 있어 전체 로그가 무오류인 것은 아니다.
당시 열린 패치 절단의 `caps=0 uncapped=61 badBoundaryMask=1`도 기록되었다. 아래 추가 조사에서 크기 가설을 배제하고 실제 GPU 패치의 열린 경계 교차를 확인했다.
Applied·+20·+40프레임의 카메라는 모두 `eye=(10,7,15)`로 유지되었다. 캡처 3장과 +20프레임 PNG를 `scratchpad/tilt/`에 저장했으나 기본 카메라에서는 CesiumMan이 화면 밖이어서 잔여 패치의 흔들림을 시각 검증하지 못했다.

## 45도 열린 단면 원인 조사 (2026-11-25)

결론: **유한 평면 크기 부족 가설은 기각**했다. 크기 확대는 적용하지 않았으며 절단 셰이더와 기존 tilt 구현을 보존했다. 45도 캡 문제는 아직 해결하지 않았다.

- `BuildSkeletalBonePlane`의 초기 크기는 0.3×0.3(half-extents 0.15, 0.15)이다. 45도에서 세로만 `1/cos(45°)`로 확대해 half-height 0.212132로 실행해도 패치는 352개, 결과는 `caps=0 uncapped=61 badBoundaryMask=1 error=0`으로 동일했다.
- `SkeletalPatchCopy::Poll`은 실제 cutter 제출 전에 패치 경계 구 전체를 덮도록 평면을 다시 만든다. 원래 크기와 확대 실험 모두 실제 절단 크기는 2.699255×2.588571, worldMargin=0.056139였다.
- `PatchExtractCS::Selected`와 Mode 0은 선택 본 집합에 양의 weight가 있는 정점 및 그 인접 삼각형을 추출한다. Parent 모드의 추출 삼각형은 초기 평면 세로 확대만으로 추가되지 않는다.
- 실제 GPU packed 정점 1,056개(352개 삼각형)를 읽어 절단 평면과 교차시킨 결과 61개 선분이 나왔다. 위치를 1e-5 단위로 용접한 패치에서 **열린 간선 4개가 평면과 교차**했다. 패치 로컬 삼각형 번호는 26·32·35·40이다.
- `CapSegments`의 실제 GPU 선분에도 동일한 네 삼각형에서 상대가 없는 끝점이 나타났다. 가장 가까운 다른 선분 끝점 거리는 각각 0.0258644·0.0640677·0.0903040·0.0255201이다. 연결 허용오차 1e-5보다 훨씬 크므로 작은 끝점 오차나 ear clipping 실패가 아니다.
- 따라서 실제 추출 패치의 열린 경계를 사선이 가로질러 절단선이 끊긴다. `LinkBoundaryCS::Match`가 연결 상대를 찾지 못해 bad bit 1을 만들고, `FinalizeCutCS`가 해당 경계의 캡을 생략한다. 해결에는 본 영향 패치의 추출 범위/폐곡선 보장 정책 검토가 필요하다. 단순 평면 확대나 cap 건너뛰기 조건 변경으로 해결하지 않았다.
- 참고: CPU 스키닝 snapshot 기반의 간선 부호 진단은 교차 0을 냈지만 실제 GPU 정점과 일치하지 않았다. 위 결론은 실제 GPU 정점·GPU 선분 대조에 근거한다. CPU/GPU 포즈 차이의 세부 원인은 이번 범위에서 수정하지 않았다.

Release/Windows(x64) 빌드 exit 0, 97.34초(임시 OutDir). 확대 실험 빌드는 exit 0, 89.57초였다. 기존 컴파일 경고와 `pwsh.exe` 미발견 후 Windows PowerShell fallback 메시지가 있었다. 임시 진단 코드는 조사 후 제거했다. 임시 실행 출력 디렉터리 `probe-bin/`, `final-bin/` 삭제는 자동 승인 검토에서 blocked by policy로 거절되어 남아 있다. Geometry/ 및 셰이더 변경은 없다.

실행은 `MiniEngine/ModelViewer`에서 각 25초, `-debug 1 -skeletal-break-demo 2 -skeletal-break-demo-bone leg_joint_R_1 -skeletal-break-demo-tilt <각도> -skeletal-break-demo-cut-time 0.5 -skeletal-break-demo-delay 2`에 캡처 옵션을 더했다. 각 실행에서 생성한 PID만 종료했다. GPU는 로그상 NVIDIA GeForce MX450이며 드라이버 32.0.15.9186은 위 기존 검증 기록의 값이다(이번 세션의 드라이버 재조회는 접근 거부).

| tilt | caps | uncapped | badBoundaryMask | error |
| --- | ---: | ---: | ---: | ---: |
| 0 | 1 | 0 | 0 | 0 |
| 30 | 1 | 0 | 0 | 0 |
| 45 | 0 | 61 | 1 | 0 |
| 45, 세로 √2배 실험 | 0 | 61 | 1 | 0 |

세 각도 모두 CesiumMan NodeId=17, child=18, JointId=14, Dynamic=4를 유지했다. 45도 측면 캡처에서 반대쪽 다리는 연결된 상태로 보인다. D3D12 ERROR/CORRUPTION은 각 실행 PID에서 0건이며 기존 WARNING은 남았다. GPU error=0은 캡 성공을 뜻하지 않는다.

검증 자료 절대 경로의 공통 디렉터리:
`C:/Users/Pes/AppData/Local/Temp/claude/80629d7db37982f2/518c1cc2-650b-4503-b9cf-db7aa22eec9c/scratchpad/tilt/`

- `final0/`, `final30/`, `final45/`: 실행 PID, 디버그 출력, 패치 로그, 카메라 로그, RAW 캡처.
- `probe45/`: 크기 확대 실험 로그.
- `vertices45/`, `vertices45-analysis.txt`: 실제 추출 GPU 정점과 열린 간선 4개.
- `segments45/`, `segments45-analysis.txt`: GPU 선분의 연결 상대 부재와 거리.
- `diagnostic-only.diff`: 제거한 임시 진단 코드 기록(제품 변경 아님).
- `final-build.log`, `probe-build.log`: Release 빌드 근거.

기존 `ReviewCameraSide.cs`의 프로세스 카메라 메모리 override를 재사용했다. `eye=(9.5,-1,3)`, `at=(12.5,-0.5,0)`에서 고정한 뒤 Applied+80·+120을 캡처했다. `final45/debug.txt`에 각각 `Demo cap ... i=4 +80`, `i=6 +120`이 기록되어 있다. 단면과 반대쪽 다리를 포함한 측면에서 잔여 패치의 각도 변화를 확인했다.

- `final-side-tilt45-plus80.png`: Applied+80 전체 측면 PNG.
- `final-side-tilt45-plus120.png`: Applied+120 전체 측면 PNG.
- `final-stump-comparison.png`: 두 프레임의 절단부 확대 비교 PNG.

## 기존 클립 전환 검증

검증: Release/Windows(프로젝트 x64), NVIDIA GeForce MX450, 드라이버 32.0.15.9186.
권장 명령의 delay를 2로 바꾸고 `-debug 1`로 25초 실행 후 종료했다.
auto는 0.273889초(clip 6, child 66)를 선택했다. 실제 로그:

```text
Skeletal demo start frame=108 delay=2.00
Skeletal demo request frame=124 model=1
Skeletal demo Applied frame=128 model=1
Skeletal demo switch frame=190 clip=idle
```

무옵션 데모도 25초 실행: start frame=1, request frame=60, Applied frame=64, 전환 없음.
숫자 클립 6→2, cut-time 1.2, switch 0, 명시적 추종 1 조합도 요청 73·게시/전환 77프레임으로 실행했다.
두 실행 모두 DXGI 출력 장치 불일치 #80과 D3D12 경고 #974/#1008이 있었다.
이번 검증은 실행 로그 기준이며 녹화 영상의 육안 비교는 포함하지 않았다.
