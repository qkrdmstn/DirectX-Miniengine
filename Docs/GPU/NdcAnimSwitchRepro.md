상태: 완료 — 기준 `049c3579` 위 녹화용 재현 구현 (본 문서를 포함한 커밋)

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
