상태: 완료 — Release GPU 검증(2026-11-18, base `0112e7ba`): Spine2→Spine1 두 번째 절단 ON 2204/alreadyHidden 1590, OFF 614/0, 옵션 없는 기본 실행은 재복사 로그 0줄

# 인접 본 순차 절단 재복사 재현

## 원인과 근거

기존 Xbot LeftUpLeg(DFS 60)·LeftLeg(DFS 61)의 패치는 2118개·1556개, 교집합은 **0개**다.
순서를 뒤집어도 재복사할 삼각형이 없으므로 ON/OFF 추출량이 같다.
GLB와 실제 `Xbot.mini`를 각각 읽어 확인했다. MINI 집계는 `PatchExtractCS::Selected`와
동일하게 세 정점 중 선택 joint의 packed uint16 weight가 하나라도 양수인 삼각형을 센다.

- `ModelViewer::Update`는 `BuildSkeletalBonePlane`으로 평면을 만들고 Parent 모드로 요청한다.
- `SkeletalPatchCopy::Request`는 skin 컴포넌트를 선택한다. 생성 패치는 numJoints=0으로
  후보에서 제외된다. 기존 로그의 두 게시 모두 owner=4이며 원본을 교체하는 경로가 아니다.
- `SkeletalPatchCopy::Update`는 `Poll` 결과의 원본 triangle ID를 기존 Pending과 합쳐
  m_Removal을 만들고, break Applied 후 원래 m_SourceMesh에 `SetTriangleMask`를 호출한다.
- `MeshComponent::SetTriangleMask`는 Pending과 Revision을 갱신한다.
  `UpdateTriangleMask`는 Pending을 GPU State에 업로드하고 숨김 삼각형의 치환 IB를 만든다.
  게시 직후 `Scene::PrepareMeshes`가 이를 적용한다. `Reset`은 원본 마스크를 지우지 않는다.
- `SkeletalPatchJob::Submit`은 같은 컴포넌트의 Pending을 m_State로 복사하고 HasState를 설정한다.
  같은 queue에서 `UpdateTriangleMask` 후 GPU State를 바인딩하므로 CPU/GPU 상태 입력이 일치한다.
- `SkeletalPatchJob::Poll`의 alreadyHidden은 실제 추출 출처를 제출 당시 m_State와 비교한다.
  계수 기준은 올바르며 수정하지 않았다. 0이라는 값만으로 HasState=false를 뜻하지 않는다.

## 수정한 재현 경로

Xbot **Spine2(DFS 6) → Spine1(DFS 5)**를 사용한다. 직접 부모·자식인 두 본의
원본 패치는 2018개·2204개, 교집합은 **1590개**다(GLB·MINI 집계 일치).
먼저 상위 몸통을 분리한 뒤 그 부모 쪽을 자르는 순서이며 동일 원본 MeshComponent를 사용한다.

`skeletal-break-demo-next-bone` 사용 시 첫 owner/component ID를 두 Request에 전달해
후보를 고정한다. 두 번째 요청은 첫 원본의 숨김 Pending과 AppliedRevision을 확인하며,
마스크가 없거나 아직 적용되지 않았으면 예외로 실패한다. 기본 단일 시연·사용자 C 동작은 유지한다.
첫 게시 후 3초 대기, 고정 카메라, 검은 배경과 절단별 8회 캡처는 기존 옵션을 재사용한다.

`-patch-recopy-bug 1`은 Mode 0의 제거 삼각형 제외만 우회한다. 생략/0은 정상 동작이다.
Mode 5의 제거된 이웃 집계는 유지한다. `Patch recopy source`는 mesh·hasState·revision·
hiddenTotal·bug를 기록하고, `Patch recopy NodeId`는 추출량과 재복사량을 기록한다.
owner/component 고정 로그는 콘솔의 `Skeletal demo request`에서도 확인할 수 있다.

## Claude 녹화·검증 명령

Release 빌드 후 PowerShell에서 실행한다. 각 프로세스 종료 후 다음 명령을 실행한다.
`-capture`는 파일명 접두사이므로 아래처럼 출력 폴더 안의 on/off를 지정한다.
로그는 `%TEMP%/SP6-SkeletalPatchChecks.log`에 추가되므로 실행별 마지막 두 요청을 비교한다.

```powershell
Set-Location C:\Users\Pes\Desktop\DirectX-Miniengine-multicut-repro\MiniEngine\ModelViewer
$exe = '..\Build\x64\Release\Output\ModelViewer\ModelViewer.exe'
$out = '..\..\multicut_capture\patch-recopy-spine'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $exe -debug 1 -skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:Spine2 -skeletal-break-demo-next-bone mixamorig:Spine1 -view 1 -patch-recopy-bug 1 -physics-query-exit 600 -capture "$out/on"
Copy-Item "$env:TEMP/SP6-SkeletalPatchChecks.log" "$out/on.log"
& $exe -debug 1 -skeletal-break-demo 1 -skeletal-break-demo-bone mixamorig:Spine2 -skeletal-break-demo-next-bone mixamorig:Spine1 -view 1 -patch-recopy-bug 0 -physics-query-exit 600 -capture "$out/off"
Copy-Item "$env:TEMP/SP6-SkeletalPatchChecks.log" "$out/off.log"
```

| 요청 | ON triangles / alreadyHidden | OFF triangles / alreadyHidden |
| --- | --- | --- |
| 첫 Spine2, NodeId=6 | 2018 / 0 | 2018 / 0 |
| 두 번째 Spine1, NodeId=5 | **2204 / 1590** | **614 / 0** |

이는 MINI 원본 집계에 근거한 **예상값이며 수정 후 GPU 실측값은 아니다**.
두 번째 source 로그는 첫 요청과 같은 mesh, hasState=1, hiddenTotal=2018이어야 한다.
ON의 두 번째 triangles가 OFF보다 1590개 많고 alreadyHidden>0이면 재복사 재현 성공이다.
RAW는 `on-cut2-demo-000-post.raw`부터 `007`까지이며 첫 절단은 cut1이다.
`raw2png.ps1 -Dir $out -Pattern '*-post.raw' -Scale 1`로 변환하고 두 번째 절단 영상을 비교한다.

이번 검증: 원본 GLB·MINI 양수 weight 패치 집계와 `git diff --check` 확인.
사용자 지시에 따라 빌드·실행·커밋하지 않았다. 이전 Claude Release 실행의 다리 쌍 결과는
2118/0·1556/0으로, 상태 누락이 아닌 교집합 0인 잘못된 재현 fixture와 일치한다.
