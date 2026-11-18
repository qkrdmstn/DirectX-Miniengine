상태: 진행 중 — 미커밋(base `607dc038`, 2026-11-18): Solid 재복사 소유 ID 보존·카메라 입력 고정, 빌드·GPU 검증은 Claude 담당. 첫 패치 소실의 근본 원인은 미확정.
# 양쪽 허벅지 순차 절단 재복사 녹화

## CesiumMan 양쪽 허벅지 녹화

`ModelViewer::Startup`은 demo 1/2별 허벅지 이름 쌍을 판별하고, 지정한 두 본의
절단 중심 중간으로 카메라를 한 번 고정한다. 반대 순서도 같은 구도다.
`FocusBreakCamera`는 Xbot에서 중심보다 0.35 m 아래를 보고 정면 약 3 m 거리를 유지한다.
CesiumMan은 중심보다 0.15 m 위를 보고 약 2.4 m까지 당긴다(아래 원인 참조).
CesiumMan MINI 반지름은 0.7725211, Xbot은 0.01127856으로 원본 크기가 다르지만,
Startup이 모두 반지름 2 m로 정규화한다. CesiumMan 배율은 약 2.58893,
정규화한 rest 경계 높이는 약 3.90035 m다. 기존 월드 거리·오프셋을 유지하며
걷는 pose와 절단 후 다리까지 담기는지는 Claude가 캡처로 확인한다.
`PlayAnimation(0)`은 기본 loop=true이며 시연은 정지하지 않는다.
`ModelViewer::Update`는 애니메이션·물리를 렌더 프레임마다 고정 1/60초 진행한다.
10초 시작 지연과 첫 Applied 후 3초 간격은 실제 경과 시간이므로 요청 pose는 FPS에 따라 달라진다.

### CesiumMan 조각 안 보임: 원인과 확인 (Release 실측)

원인은 `FocusBreakCamera`의 양 다리 구도였다. CesiumMan의 `leg_joint_L_1`/`R_1` 절단은 정강이 부근이라
조각이 짧은데, 기존 구도(중심 +1.0 m, 5 m 거리)는 화면의 약 40 px에 불과해 와이어프레임에서 조각과
주황 영역이 묻혔다. 지연 시간(1초/10초)·검은 배경·옵션 조합은 원인이 아니었다(지연 1초에서도 동일).
수정은 `FocusBreakCamera`의 CesiumMan 분기(목표 +0.15 m, 거리 2.4 m)뿐이며 카메라는 시작 때 한 번만 잡는다.
Xbot과 옵션 없는 기본 구도는 그대로다. 로그는 그대로: 첫 절단 364, 두 번째 ON 352/alreadyHidden 190, OFF 162/0.
Xbot ON 2118 후 2096/90, OFF 2006/0도 그대로다.

### 재복사 색 규칙 (`-show-masked-region 1`, 와이어프레임 원본만)

비마스크 원본은 흰색, 마스크된 원본은 주황, 이미 복사됐는데 또 복사된(두 번 마스크된) 원본은 노랑이다.
`SkeletalPatchCopy`가 원본 상태에 표시 전용 비트 `0x40000000`을 새 숨김 시 이미 숨겨진 삼각형에만 더한다.
`SetTriangleMask`는 이 비트를 허용하고 `TriangleMaskCS`의 진단 선택 2가 이 면만 `RecopiedIB`에 보존한다.
`MeshComponent::Render`가 뒤쪽 노랑 상수 절반으로 한 번 더 그린다. `-patch-recopy-bug 0`은 재복사가 없어 노랑이 없다.
패치(초록) 쪽 노랑은 구현하지 않았다. 확인: CesiumMan ON은 190개 면이 노랑, Xbot ON은 고관절 90개가 노랑, OFF는 노랑 없음.
### Solid 최종 결과 (진행 중 — 미커밋)

`-view`와 `-show-masked-region`을 모두 생략한다. 원본 제거 마스크와 실제 부착 패치를 그린다.
완료 조건은 cut2 `007`에서 **몸통 쪽 양쪽 허벅지 패치가 모두 남고**, ON에서는 재복사된
고관절 면이 새 패치에 포함되어 기존 패치와 겹치거나 늘어나는 것이다. OFF는 두 패치가 정상 부착된다.
CesiumMan은 같은 재질로 겹쳐 차이가 작을 수 있다. 원본 전체를 다시 그려 구멍을 가리지 않는다.

조사 결과와 수정 범위:
- `SkeletalPatchCopy::Update`는 숨긴 원본 삼각형의 하위 16비트 소유 ID를 새 ID로 덮어썼다.
  이번에는 **명시적 `-patch-recopy-bug 1`에서만** 기존 ID에 재복사 비트 `0x40000000`을 더한다.
  새 삼각형에는 새 ID를 배정하며 추출/절단/새 A/B 추가는 유지한다. 생략/0은 기존 코드와 같다.
- 다만 ID 덮어쓰기가 첫 패치 소실을 유발한다는 연결은 확인되지 않았다.
  `SkeletalPatchJob::Submit` Mode 5와 `Poll`의 인접 집계, `ConsumeMultiBreak`의 `patchIds`는 로그용이다.
  `Update`의 SP7e는 `GameObject::CommitMeshComponent`로 추가하며 이전 패치를 retire/replace하지 않는다.
  `MeshComponent::ValidatePatchSkinSource`도 마스크 ID 대신 source revision/binding을 검사한다.
- ON 게시 후 `Patch recopy retained` 로그를 추가했다. 첫 게시의 두 mesh/revision이 두 번째 게시에도
  같은 값으로 남고 `visible=1 skin=1 ready=1`인지 확인한다. 이 로그는 픽셀 가시성 판정이 아니다.
  소실의 근본 원인과 수정 효과는 아직 GPU 실행으로 확정하지 않았다.
- `ModelViewer::Startup`은 이미 양다리 구도를 한 번 설정한다. ON/OFF 카메라 분기는 없다.
  우클릭+WASD 이동은 녹화 중 구도 조정을 위해 그대로 허용한다(자동 이동만 없음).
  시작 eye/forward와 캡처 eye/target을 소수 6자리로 기록한다. 절단 시 재초점/텔레포트는 없다.
  관측된 ON/OFF 구도 차이는 캡처 실행 중 입력이 아니면 원인 미확정이다.

Claude 검증 명령 (이 worktree의 Developer PowerShell):

```powershell
Set-Location C:\Users\Pes\Desktop\DirectX-Miniengine-multicut-repro
msbuild MiniEngine/ModelViewer/ModelViewer.slnx /restore /m /p:Configuration=Debug /p:Platform=Windows
msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Release /p:Platform=Windows
Set-Location MiniEngine/ModelViewer
$out = '..\..\multicut_capture\solid-retained'
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($config in 'Debug', 'Release') {
    $exe = "../Build/x64/$config/Output/ModelViewer/ModelViewer.exe"
    foreach ($model in 1, 2) {
        $left = if ($model -eq 1) { 'mixamorig:LeftUpLeg' } else { 'leg_joint_L_1' }
        $right = if ($model -eq 1) { 'mixamorig:RightUpLeg' } else { 'leg_joint_R_1' }
        $common = @('-debug', '1', '-skeletal-break-demo', "$model",
            '-skeletal-break-demo-bone', $left, '-skeletal-break-demo-next-bone', $right,
            '-skeletal-break-demo-delay-sec', '10', '-skeletal-break-demo-next-sec', '3')
        foreach ($bug in 0, 1) {
            $prefix = "$out/$config-model$model-bug$bug"
            & $exe @common -patch-recopy-bug $bug -physics-query-exit 1800 -capture $prefix *> "$prefix-console.log"
            Copy-Item "$env:TEMP/SP6-SkeletalPatchChecks.log" "$prefix.log"
        }
    }
}
```

`-capture`는 디렉터리 아래 **파일 prefix**다. cut1/cut2 `000`~`007`의 eye/target이
ON/OFF 및 절단 전후 동일해야 한다. `*-cut2-demo-007-post.raw`에서 위 완료 조건을 확인한다.
Xbot 두 번째 추출은 ON 2096/90, OFF 2006/0, CesiumMan은 ON 352/190, OFF 162/0을 비교한다.
애니메이션은 프레임 단위, 요청은 실제 시간 단위라 요청 pose까지 같다는 보장은 없다.
GPU·드라이버·구성, 프로세스 종료 코드와 D3D12 오류 여부를 기록한다.
Codex는 빌드·실행·커밋하지 않았으며 정적 경로 조사와 diff 검사만 수행했다.

### 데이터 확인 (GPU 실측 아님)

`Models/CesiumMan.glb` accessor offset/stride와 skin palette, 실제 `CesiumMan.mini`의
section VB/IB·R16 인덱스·draw startIndex/baseVertex·정점 끝 16바이트의 uint16 joint/weight를 읽었다.
`PatchExtractCS::Selected`처럼 세 정점 중 하나라도 해당 본 weight>0이면 포함했다.
MINI palette의 19개 NodeId를 GLB scene DFS 이름에 대응시켰다.

| 본 | DFS NodeId | 실제 첫 자식 | GLB 삼각형 | MINI 삼각형 |
| --- | --- | --- | --- | --- |
| leg_joint_L_1 | 13 | leg_joint_L_2 (14) | 364 | 364 |
| leg_joint_R_1 | 17 | leg_joint_R_2 (18) | 352 | 352 |

**왼쪽 ∩ 오른쪽 = 190개**로 GLB/MINI 모두 일치한다. 허벅지 쌍을 그대로 쓴다.
같은 파서로 Xbot도 2118/2096/90을 재확인했다. 자손 본의 영향은 합산하지 않는다.
CesiumMan MINI는 mesh/section 1개, draw 1개, stride 40, joint 19개다.
`GameObject::GameObject`는 원본 MeshComponent 하나를 만든다.
`ModelViewer::Update`는 첫 요청에서 owner와 front component ID를 저장하고 두 번째에도
같은 ID로 `SkeletalPatchCopy::Request`를 호출한다. Request의 후보 필터가 생성 조각을 제외한다.
두 번째 요청 전 원본 Pending의 숨김 비트와 AppliedRevision==Revision도 검사한다.

### VS 녹화 인수 (작업 디렉터리 MiniEngine/ModelViewer)

```text
-debug 1 -skeletal-break-demo 2 -skeletal-break-demo-bone leg_joint_L_1 -skeletal-break-demo-next-bone leg_joint_R_1 -skeletal-break-demo-delay-sec 10 -skeletal-break-demo-next-sec 3 -view 1 -show-masked-region 1 -patch-recopy-bug 1
```

`-patch-recopy-bug 0`으로 OFF를 녹화한다. `-show-masked-region 2`는 초록 본 부착 패치도 표시한다.
1/2 모두 숨겨진 원본을 주황색으로 표시한다. `-skeletal-break-demo-unmask`는 생략한다.

### Claude 검증 명령 (빌드 후)

```powershell
Set-Location C:\Users\Pes\Desktop\DirectX-Miniengine-multicut-repro\MiniEngine\ModelViewer
$exe = '..\Build\x64\Release\Output\ModelViewer\ModelViewer.exe'
$common = @('-debug', '1', '-skeletal-break-demo', '2', '-skeletal-break-demo-bone', 'leg_joint_L_1',
    '-skeletal-break-demo-next-bone', 'leg_joint_R_1', '-skeletal-break-demo-delay-sec', '10',
    '-skeletal-break-demo-next-sec', '3', '-view', '1')
$out = '..\..\multicut_capture\cesium-both-legs'
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($mode in 1, 2) {
    foreach ($bug in 0, 1) {
        $prefix = "$out/mode$mode-bug$bug"
        & $exe @common -show-masked-region $mode -patch-recopy-bug $bug -physics-query-exit 1800 -capture $prefix
        Copy-Item "$env:TEMP/SP6-SkeletalPatchChecks.log" "$prefix.log"
    }
}
```

`-capture`는 디렉터리 자체가 아니라 기존 디렉터리 아래 파일 prefix다.
예상 첫 요청: `Patch recopy NodeId=13 triangles=364 alreadyHidden=0`.
예상 두 번째: ON `Patch recopy NodeId=17 triangles=352 alreadyHidden=190`,
OFF `Patch recopy NodeId=17 triangles=162 alreadyHidden=0`.
두 번째 source 로그는 두 실행 모두 같은 원본 mesh, `hasState=1 hiddenTotal=364`여야 한다.
콘솔의 두 `Skeletal demo request` owner/mesh도 같아야 한다.
이는 데이터 기반 예상값이며 이번 작업에서 빌드·실행하지 않았다.
`*-cut2-demo-000-post.raw`~`007`에서 주황색 원본 늘어짐, 골반·양다리 구도,
cut1/cut2 카메라 점프 없음, D3D12 오류 없음을 확인한다. GPU·드라이버·구성을 실행 기록에 남긴다.

## 숨겨진 원본 영역 표시 (새 옵션, 검증 대기)

`-show-masked-region 0|1|2`는 기본 0(생략과 동일)이며 `-view 1`과 함께 사용한다.
- 1: 비마스크 원본은 기존 흰색, 마스크 원본은 주황색 `(1, 0.25, 0.05)` 선으로 표시한다.
  본 부착 패치는 시뮬레이션·게시를 유지하며 렌더·그림자만 숨긴다.
- 2: 같은 원본 표시와 기존 초록색 본 부착 패치를 함께 그린다.
- 주황색은 제거 비트가 설정된 **원본 삼각형**이다. 현재 원본 PG 또는 legacy VS 팔레트를
  공유하므로 분리된 몸체 사이에서 늘어난 원본이 보인다. 카메라·예약·종료 로직은 바꾸지 않는다.
- 주황색 추가는 메인 Wireframe에만 적용한다. Solid/Vertices에는 추가하지 않는다.
  값 1의 패치 숨김은 모든 표시 모드에 적용한다. 기존 깊이 검사·가림은 유지한다.

기존 `-skeletal-break-demo-unmask 1`은 `SkeletalPatchCopy::Update`의 SP7e 게시에서
`SetTriangleMask(m_Removal)` 자체를 생략한다. 원본 전체와 초록 패치가 함께 보이지만
마스크 면 색상 구분이 없으며 `IsUnmaskDemoPublished`가 후속 C 요청도 거절한다.
새 옵션은 실제 마스크를 정상 게시하여 양쪽 다리와 recopy ON/OFF를 유지한다.
unmask는 생략하거나 0을 지정한다. 새 옵션과 unmask 1의 혼용은 시작 오류로 거절한다.

`TriangleMask::Pending`은 제거 비트와 마지막 패치 ID만 저장한다. 새 패치 ID는 덮어쓰며,
정상 recopy OFF는 이미 숨겨진 90개를 두 번째 추출에서 제외한다. ON의 재선택만 세면
ON/OFF 색 의미도 달라진다. 같은 의미의 교집합 표시는 제외 전 선택이나 별도 이력이 필요하므로
이번 변경에서는 세 번째 색을 생략한다. 주황색은 마스크 합집합이며 90개를 중복 draw하지 않는다.

`MeshComponent::UpdateTriangleMask`는 `TriangleMaskCS::Replacement`를
`DrawInput::Reserved1` 선택 반전으로 한 번 더 실행해 마스크 전용 IB를 만든다.
`MeshComponent::Update`는 기존 `MeshConstants::OverlayColor`만 바꾼 상수를 준비하고,
`MeshComponent::Render`는 동일 VB·pose·재질·컬링으로 추가 등록한다.
기본 옵션에는 추가 GPU 버퍼·dispatch·draw가 없다. 새 셰이더·프로젝트 항목도 없다.

### 새 옵션 녹화 명령 (자동 종료 없음)

Claude가 Debug/Release 빌드 후 실행한다. 각 창을 닫은 뒤 다음 명령을 실행한다.
```powershell
Set-Location C:\Users\Pes\Desktop\DirectX-Miniengine-multicut-repro\MiniEngine\ModelViewer
$exe = '..\Build\x64\Release\Output\ModelViewer\ModelViewer.exe'
$common = @('-debug', '1', '-skeletal-break-demo', '1', '-skeletal-break-demo-bone', 'mixamorig:LeftUpLeg',
    '-skeletal-break-demo-next-bone', 'mixamorig:RightUpLeg', '-skeletal-break-demo-delay-sec', '10',
    '-skeletal-break-demo-next-sec', '3', '-view', '1')
& $exe @common -show-masked-region 1 -patch-recopy-bug 1
& $exe @common -show-masked-region 1 -patch-recopy-bug 0
& $exe @common -show-masked-region 2 -patch-recopy-bug 1
& $exe @common -show-masked-region 2 -patch-recopy-bug 0
```

### Claude 검증 명령과 예상 캡처

위 `$common`을 재사용한다. `-capture`는 파일 prefix이므로 부모 폴더를 먼저 만든다.
```powershell
$out = '..\..\multicut_capture\masked-original'
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($mode in 0, 1, 2) {
    foreach ($bug in 0, 1) {
        $prefix = "$out/mode$mode-bug$bug"
        & $exe @common -show-masked-region $mode -patch-recopy-bug $bug -physics-query-exit 1800 -capture $prefix
        Copy-Item "$env:TEMP/SP6-SkeletalPatchChecks.log" "$prefix.log"
    }
}
```

- mode0: 기존 흰 원본·초록 패치. 옵션 생략과 같은 결과여야 한다.
- mode1: cut1부터 마스크 원본이 주황색이며 초록 패치는 없다. cut2의
  `*-cut2-demo-000-post.raw`~`007`에서 다리·몸통 사이의 늘어난 주황 선과 골반 겹침을 확인한다.
- mode2: 같은 주황 원본에 초록 패치가 추가된다. 앞쪽 면에 가려진 선은 보이지 않을 수 있다.
- 두 번째 로그는 기존 ON `2096/90`, OFF `2006/0`을 유지해야 한다. 카메라 점프와 D3D12 오류도 확인한다.
  원본 마스크 합집합은 ON/OFF가 같을 수 있으므로 주황 선 차이만으로 recopy 여부를 판단하지 않는다.

이번 변경은 빌드·실행·캡처하지 않았다. 아래 기존 Release 실측과 구분한다.

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
