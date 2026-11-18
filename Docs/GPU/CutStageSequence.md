상태: 완료 — CS-1·CS-2·리뷰 수정 커밋(base `0112e7ba`, 2026-11-18): Release 빌드, Xbot 시퀀스 실행에서 단계 1→4 진입·Applied·단계 캡처 4장 PNG 확인, 옵션 없는 기존 시연 요청 frame 60·Applied 유지. Debug 빌드·CesiumMan·실제 화면 녹화·텍스트 표시 캡처는 미확인

# 절단 단계 시퀀스 시연 (녹화용)

## 1. 목표
- 기존 스켈레탈 절단 시연(`-skeletal-break-demo 1`, Xbot)의 한 번 절단을 4단계로 나눠 일정 간격으로 차례로 보여 준다.
  1. 평면 입력: 절단 평면만 표시, 절단 요청 전
  2. 패치 복사: 복사된 패치를 원본 옆으로 띄워 강조색으로 표시, 원본은 그대로
  3. 패치 절단: 띄운 복사 패치를 평면으로 자른 결과(cap 포함)를 강조색으로 표시
  4. 패치 부위 마스킹: 조각을 제자리에 두고 원본 패치 부위를 숨긴 기존 최종 게시, 이후 기존처럼 break 물리 진행
- 기본 off. 새 옵션이 없으면 기존 동작·출력·캡처가 한 줄도 바뀌지 않는다.
- 창은 자동 종료하지 않고 4단계에서 계속 남는다(게시 실패 시의 기존 오류 종료는 유지).

## 2. 결정과 이유
- **멈춤 지점은 SkeletalPatchCopy 내부 두 gate**(조사 메모 B의 정지 ②·③).
  - gate A: 복사본 staging 준비 완료 후, `cutting.Request` 제출 전.
  - gate B: body 압축 완료 후(`m_ReadyCut` 보관), `FinishCutResult` 진입 전. break 제출 전에 멈춰야 기존 게시 순서(마스크·조각·break)가 그대로 유지된다.
  - 1단계는 ModelViewer에서 `Request` 호출 자체를 늦춘다(정지 ①).
  - gate 대기 중에도 `IsPending()`은 true여야 한다. 그래야 기존 "게시 신호 없음 → 실패 종료" 판정이 오작동하지 않는다.
- **1~3단계 동안 애니메이션·물리를 정지**한다. 복사·절단이 같은 pose에서 일어나야 하고, 정지 화면이 녹화에 맞다. 4단계 진입(gate B 해제) 때 재개해야 `ProcessQueuedBreaks`가 돌아 Applied가 온다.
- **강조 표시는 "옆으로 띄운 복사본 + 강조색"**. 원본과 같은 위치에 겹치면 z-fighting·cap 가림이 생기고, 원본 Solid+패치 Wireframe 분리는 sorter 변경이 커서 제외한다.
  - 띄우기는 staging GameObject의 표시용 transform만 바꾼다. 절단 제출 직전에는 원래 transform으로 되돌려 절단 결과가 원본 좌표 그대로 나오게 하고, gate B 대기 중 다시 띄운다. `FinishCutResult` 전에는 반드시 원위치.
  - 방향은 현재 카메라 오른쪽 벡터, 거리는 `-cut-stage-offset <m>`(기본 0.45 m, 0이면 제자리).
  - 강조색은 `MeshComponent::StageHighlight` 색 번호(0 끔, 1 주황, 2 보라)를 기존 `OverlayColor`로 전달한다. `DefaultPS::main`은 시선·법선 명암을 남기며 Attachment는 바꾸지 않는다.
  - 2단계 복사본과 3단계 A는 주황 `(1.0, 0.55, 0.1)`, 3단계 B는 보라 `(0.55, 0.35, 1.0)`로 그린다. A/B 사이 간격은 두지 않는다.
  - `ReleaseStageGate`, `OnCutResult`, `FinishCutResult`는 staging owner·결과 컴포넌트 누락 시 `ValidateStaging`에서 준비물을 폐기하고 오류를 기록한다.
  - `FocusBreakCamera`는 시퀀스 시연에서만 offset 방향 y를 0.9로 높여 평면을 위에서 비스듬히 본다. 기존 시연은 0.2를 유지한다.
- **평면 표시**: 데모 평면 `CutPlaneData`를 `SceneTransform::TryFromAffine(Inverse(WorldToPlane))`와 크기로 `CutPlaneSystem`에 등록하고 기존 `RenderCutPlanes`로 그린다. 기존 기본 평면은 시퀀스 모드에서 제거한다. 1~3단계 표시, 4단계에서도 유지(녹화에서 절단 위치가 보이도록).
- **시간은 실제 경과 시간**(프레임 deltaT 누적). 데모 물리는 고정 1/60 step이라 프레임 수로 세면 녹화 시간과 어긋난다.
- **단계 이름 텍스트**는 `RenderUI`의 기존 `TextContext`로 왼쪽 위에 ASCII 영어로 그린다(폰트의 한글 glyph 미확인): `1/4 Cut plane`, `2/4 Patch copy`, `3/4 Patch cut`, `4/4 Mask original`.
- **캡처**: 시퀀스 모드에서 `-capture <prefix>`가 있으면 단계마다 진입 후 표시가 준비된 뒤 한 장(`<prefix>-stage-1`~`-4`)을 저장하고, 기존 데모 8장 캡처는 쓰지 않는다. 4단계는 Applied 게시 후 20프레임에 찍는다. 캡처는 UI 전 버퍼라 텍스트는 들어가지 않는다.
  - 시퀀스 모드의 prefix 디렉터리가 없으면 `Startup`이 `invalid_argument`로 거절한다. 캡처 비트 확정 순서는 유지한다.

## 3. 옵션
| 옵션 | 의미 |
| --- | --- |
| `-cut-stage-sequence 1` | 시퀀스 모드. `-skeletal-break-demo`가 없으면 시작 시 오류 |
| `-cut-stage-sec <초>` | 단계 간격, 기본 2.5, 0.5 미만은 0.5로 |
| `-cut-stage-offset <m>` | 2·3단계 복사본 표시 간격, 기본 0.45 |

## 4. 인터페이스 골격
```cpp
// SkeletalPatchCopy
enum class StageGate { None, CopyReady, CutReady };   // 현재 대기 중인 gate
void SetStageHold(bool hold);          // true면 gate A·B에서 대기
StageGate GetStageGate() const;        // 대기 중인 gate (없으면 None)
void ReleaseStageGate(Scene& scene);   // staging 검증 후 현재 gate 하나 통과
void SetStageDisplay(Scene& scene, const Math::Vector3& offset, bool highlight); // staging 표시 transform·강조
// ModelViewer
enum class CutStage { Off, Plane, Copy, Cut, Mask };
CutStage m_CutStage; float m_CutStageTimer, m_CutStageSeconds = 2.5f, m_CutStageOffset = 0.45f;
```
이름은 구현 시 주변 코드에 맞춰도 된다. 물리 정지는 기존 `PhysicsSystem`의 정지/재개 API를 쓰고, 없으면 ModelViewer의 `AdvanceSimulation` 호출을 건너뛴다(애니메이션 평가 포함).

## 5. 단계 (각 약 300줄 이하, 단계마다 새 Codex 실행 + 다른 Codex 리뷰)
- **CS-1 단계 상태·gate·평면·텍스트**: 옵션 파싱·검증, 60프레임에 평면 등록·정지·1단계 시작, 타이머로 `Request`, SkeletalPatchCopy gate A·B 대기/해제, 4단계 진입 시 물리 재개, `RenderUI` 단계명. 복사본 표시는 아직 기존대로(숨김).
- **CS-2 복사본 표시**: gate A·B 대기 중 staging 표시(오프셋 transform, 강조색, 3단계 A/B 표시), 해제 직전 원위치·숨김 복원, 단계별 캡처 파일.
- **CS-3 검증·문서**: Release 빌드, 실행·캡처 4장 확인, 상태 줄·README 행 갱신.

## 6. 완료 조건
- 옵션 없이 실행하면 기존 `-skeletal-break-demo 1` 동작(요청 프레임 60, 캡처 8장, 로그)이 그대로다.
- 시퀀스 모드에서 단계 1→2→3→4가 `-cut-stage-sec` 간격으로 차례로 보이고, 각 단계 진입이 로그 한 줄(`Cut stage n name t=...`)로 남는다.
- 2단계에 원본은 그대로, 옆에 주황 복사 패치가 보인다. 3단계에 같은 위치에 절단된 패치(cap 포함)가 보인다. 4단계는 기존 최종 게시(원본 마스크, 조각 제자리, break 물리)와 같다.
- 1~3단계 동안 pose가 변하지 않고, 4단계 결과의 조각이 원본과 어긋나지 않는다.
- gate 대기 중 실패 종료 판정이 일어나지 않고, 창이 4단계에서 계속 유지된다.
- `-capture` 지정 시 `<prefix>-stage-1..4-*.raw` 4세트가 저장된다.
- 새·수정 함수 위 한글 1~2줄 주석, 함수 안 단락별 한글 한 줄 주석.

## 7. 하지 않는 것
- CesiumMan(`-skeletal-break-demo 2`)과 `-skeletal-break-demo-bone` 조합의 시각 확인(동작은 막지 않음).
- A/B 조각 사이 간격 표시, 원본 위 와이어프레임 겹침, 한글 텍스트, 동영상 저장.
