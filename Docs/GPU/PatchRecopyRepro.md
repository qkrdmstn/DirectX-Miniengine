상태: 완료 — Release GPU 검증(2026-11-18)
# 양쪽 허벅지 순차 절단 재복사 녹화

## 데이터 근거

`Models/Xbot.glb`와 실제 `Models/Xbot.mini`를 각각 읽어 본별 패치 집합을 집계했다.
MINI는 `PatchExtractCS::Selected`와 동일하게 세 정점 중 하나라도 선택 joint의
packed uint16 weight가 양수인 삼각형을 포함한다. GLB accessor의 offset/stride/형식과
MINI의 section별 VB/IB·R16 인덱스·draw.startIndex·baseVertex·정점 마지막 16바이트를 해석했다.
MINI joint palette→DFS 매핑도 GLB scene DFS와 67개 모두 일치했다.
변환 시 인덱스 순서가 바뀌므로 GLB/MINI triangle ID 자체를 직접 비교하지 않는다.

| 본 | DFS NodeId | GLB 패치 | MINI 패치 |
| --- | --- | --- | --- |
| mixamorig:LeftUpLeg | 60 | 2118 | 2118 |
| mixamorig:RightUpLeg | 65 | 2096 | 2096 |
| mixamorig:Hips | 3 | 1981 | 1981 |

**LeftUpLeg ∩ RightUpLeg = 90개**(GLB·MINI 모두). Hips와의 교집합은
LeftUpLeg 90개, RightUpLeg 157개다. 양쪽 허벅지의 골반 접합부가 겹치므로
다른 본으로 대체하지 않는다. 왼쪽→오른쪽 순서로 같은 원본 MeshComponent를 자른다.
평면은 `BuildSkeletalBonePlane`으로 UpLeg와 첫 자식 Leg 사이에 만든다.

기존 LeftUpLeg→LeftLeg는 2118/1556개, 교집합 **0개**여서 재복사를 재현하지 못했다.
Spine2→Spine1은 2018/2204개, 교집합 **1590개**다. HEAD `98183ff0`의 Release GPU
실측은 두 번째 ON 2204/1590, OFF 614/0이었다. 아래 양다리 Release 실측값과 구분한다.

## 요청·시간·화면

- `ModelViewer::Startup`은 `-skeletal-break-demo-delay-sec <sec>`를 읽는다(기본 0).
  데모에서만 사용하며 Startup 진입부터 `steady_clock`으로 잰 실제 경과 시간이다.
  10이면 초기화를 포함해 최소 10초 후, 기존 60프레임 준비 조건도 만족한 뒤 첫 요청을 한다.
  생략하면 기존 첫 요청 조건(60프레임)을 유지한다.
- `-skeletal-break-demo-next-sec <sec>`는 첫 Applied 게시 뒤 간격(기본 3)이다.
  첫 게시 시각을 `steady_clock`으로 저장하고 실제 경과 초를 비교한다.
  두 시간 옵션은 유한한 0 이상 값만 허용한다.
- 데모 물리·애니메이션은 **렌더 프레임마다 고정 1/60초** 진행한다.
  `-animation-step-ms`는 데모에서 이를 덮어쓰지 않는다. 실제 10초를 애니메이션
  600프레임과 동일시하면 안 된다. VSync 시 `deltaT`가 1/60초로 고정되어도
  두 번째 요청은 실제 3초 경과 후 가능하며 프레임 수에 의존하지 않는다.
- `-view 1`은 `Renderer::RenderMode::Wireframe`이다(0 Solid, 2 Vertices).
  양쪽 UpLeg를 지정하면 Startup의 pose 초기화 후 두 절단 중심의 중간을 기준으로
  시선을 0.35 m 낮추고 정면 약 3 m에 고정한다. 첫 절단에서도 다시 움직이지 않는다.
  골반·양다리를 함께 담는 구도이며 검은 배경을 유지한다. 캡처 확인은 Claude 담당이다.

`skeletal-break-demo-next-bone`은 첫 owner/component를 두 Request에 고정한다.
두 번째 요청 전 첫 숨김 Pending과 AppliedRevision을 확인한다.
`SkeletalPatchJob::Submit`은 원본 Pending을 State로 전달하고,
`-patch-recopy-bug 1`만 Mode 0의 제거 삼각형 제외를 우회한다(생략/0 정상).
Mode 5의 제거 이웃 집계는 유지한다. `Poll`의 alreadyHidden은 실제 추출 출처와
제출 시 State를 비교한다. `Patch recopy ...` 로그는 next-bone을 줄 때만 기록한다.

## 녹화 명령 — 자동 종료 없음

Claude가 Release 빌드 후 PowerShell에서 실행한다. ON 창을 닫은 뒤 OFF를 실행한다.
```powershell
Set-Location C:\Users\Pes\Desktop\DirectX-Miniengine-multicut-repro\MiniEngine\ModelViewer
$exe = '..\Build\x64\Release\Output\ModelViewer\ModelViewer.exe'
& $exe -debug 1 -skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:LeftUpLeg -skeletal-break-demo-next-bone mixamorig:RightUpLeg -skeletal-break-demo-delay-sec 10 -skeletal-break-demo-next-sec 3 -view 1 -patch-recopy-bug 1
& $exe -debug 1 -skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:LeftUpLeg -skeletal-break-demo-next-bone mixamorig:RightUpLeg -skeletal-break-demo-delay-sec 10 -skeletal-break-demo-next-sec 3 -view 1 -patch-recopy-bug 0
```

## Release GPU 검증 명령·실측 로그

같은 작업 디렉터리와 `$exe`를 사용한다. `-capture`는 파일명 접두사이므로 부모 폴더를 만든다.
2026-11-18 Release 검증은 `-skeletal-break-demo-delay-sec 10`과
`-physics-query-exit 1800`으로 실행했으며 실행 시간은 33초였다.
첫 게시 후 기본 3초 간격과 두 번째 게시 후 140프레임 캡처를 포함한다.
높은 FPS로 먼저 1800프레임에 도달해도 기존 `IsDone`의
demoReady 조건이 두 번째 게시와 8장 캡처까지 종료를 보류하므로 대기를 건너뛰지 않는다.
```powershell
$out = '..\..\multicut_capture\patch-recopy-both-legs'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $exe -debug 1 -skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:LeftUpLeg -skeletal-break-demo-next-bone mixamorig:RightUpLeg -skeletal-break-demo-delay-sec 10 -skeletal-break-demo-next-sec 3 -view 1 -patch-recopy-bug 1 -physics-query-exit 1800 -capture "$out/on"
Copy-Item "$env:TEMP/SP6-SkeletalPatchChecks.log" "$out/on.log"
& $exe -debug 1 -skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:LeftUpLeg -skeletal-break-demo-next-bone mixamorig:RightUpLeg -skeletal-break-demo-delay-sec 10 -skeletal-break-demo-next-sec 3 -view 1 -patch-recopy-bug 0 -physics-query-exit 1800 -capture "$out/off"
Copy-Item "$env:TEMP/SP6-SkeletalPatchChecks.log" "$out/off.log"
```

| 요청 | ON triangles / alreadyHidden | OFF triangles / alreadyHidden |
| --- | --- | --- |
| 첫 LeftUpLeg, NodeId=60 | 2118 / 0 | 2118 / 0 |
| 두 번째 RightUpLeg, NodeId=65 | **2096 / 90** | **2006 / 0** |

위 표는 **LeftUpLeg→RightUpLeg 양다리 Release GPU 실측값**이다.
첫 절단은 2118개이며 두 번째 `Patch recopy NodeId=65`는
ON triangles=2096, alreadyHidden=90 / OFF triangles=2006, alreadyHidden=0이다.
ON triangles−OFF triangles=90으로 원본 재복사를 재현했다.
두 번째 source는 첫 요청과 같은 mesh, hasState=1, hiddenTotal=2118을 확인한다.
로그는 `%TEMP%/SP6-SkeletalPatchChecks.log`에 추가되므로 실행별 마지막 두 요청을 비교한다.
콘솔의 `Skeletal demo request` owner/mesh 및 Applied 두 번도 확인한다.
캡처는 `on-cut2-demo-000-post.raw`부터 `007`까지며 첫 절단은 cut1이다.
`raw2png.ps1 -Dir $out -Pattern '*-post.raw' -Scale 1`로 변환해 골반·양다리와 선 표시를 확인한다.

스위치 OFF에서도 떨어진 다리에 길게 늘어난 패치 선이 보인다.
이는 재복사와 구분되는 기존 현상으로 남아 있다.

Release GPU 실측 결과는 사용자 제공 검증 기록을 반영했다.
이번 리뷰 수정은 소스·문서와 `git diff --check`만 확인하며 빌드·GPU 재실행·커밋은 하지 않는다.
