상태: 진행 중 — BC-07 `8dc58b08` 완료, BC-08a-1 구현·검증 완료(미커밋), BC-08a-2 이후 미착수 (2026-10-29)

# BreakConstraint 결과 보고

## Codex 인계 후 이번 작업

Claude 호출 없이 기존 break-constraint worktree에서 BC-08a-1을 완료했다. BC-08a 전체는 약 300줄의 단계 범위를 넘으므로 순수 frame 변환(08a-1)과 rig 적용(08a-2)으로 분리했다. 이번 코드·시험·target 등록 변경은 267줄 추가·1줄 삭제다.

`SkeletalPhysicsFrames.h`는 `B = R(E * Gnode) * BoneFromBody`와 `Gnode = inverse(E) * B * inverse(BoneFromBody) * S(s)`를 구현한다. 본 배율을 분리하여 물리 offset은 월드 m 단위로 한 번만 적용한다. 유한 affine·양수 uniform·직교성을 검사하며 실패 시 출력은 보존한다.

- CPU Debug/Release 각각 706/706 PASS, 컴파일 경고 0, 실행 종료 코드 0. 컴파일·실행 0.89/1.27초.
- ModelViewer 전체 Debug/Release 최종 빌드 PASS, 경고·오류 0, 종료 코드 0. 벽시계 측정 4.88/3.70초. 최초 빌드는 12.61/38.95초였고 Release의 기존 C4819 경고 19개가 있었다. `/restore`는 사용하지 않았다.
- 구현과 분리한 읽기 전용 Codex 리뷰에서 P3 1건을 수정했다. 큰/작은 배율에서 float 길이 제곱의 overflow/underflow를 double 길이·정규화 계산으로 해결했고 재리뷰 잔여 지적은 0건이다.
- 추가 30종 E 변환 시험에서 발견한 affine 역행렬 반올림 거절은 마지막 행 허용오차 1e-6으로 수정했다. Xbot 배율·회전 offset·행렬 전체 기대값과 왕복·오류 입력·출력 보존을 검사한다.
- 런타임 호출 연결이 없어 viewer 실행·화면 확인·GPU/driver 재측정·CTest는 수행하지 않았다. Geometry 변경은 없다.

로그와 실행 파일은 `$env:TEMP/BreakConstraintTests`의 `BC08a1-{Debug,Release}-{CPU,build}.log`와 `SkeletalFrameTests{Debug,Release}.exe`다. 재현 컴파일 명령은 `Scene/tests/SkeletalFrameTests.cpp` 머리말에 있다. Implementation·Progress·README·CodeMap을 동기화했고 `git diff --check`를 통과했다. 기존 `.mini` 3개는 보존했으며 커밋·병합·junction 변경은 하지 않았다.

다음 단계 BC-08a-2는 rig의 배율 저장과 변경 거절, Kinematic target·COM 속도, Dynamic 상태 보존이다. section E 동일성·Nmesh 영향 검사는 Scene 연결 단계에서 처리한다. 실제 break·Xbot 낙하 데모·M1은 아직 완료되지 않았다. 저장소의 단계별 검토 지침에 따라 이번 단계에서 중단한다.

## 이전 실행 기록

사용자 지시에 따라 이 실행은 git commit/merge를 시도하지 않는다. 기존 BC-02는 619022e6, master 병합은 3bee0b20이다. 병합 충돌은 사용자가 ModelViewer.cpp의 양쪽 include·IsDone 조건·멤버를 보존하여 해결했다. 본 실행의 변경은 미커밋이며 단계별 파일 목록은 BreakConstraintProgress.md에 기록한다.

| 단계 | 테스트·빌드·실행 | 리뷰 |
| --- | --- | --- |
| 병합 후 회귀 | SkeletonPose32/32, SceneMath495 checks, Physics Debug9/9 PASS | Static API·편집·삭제 전제 대조 |
| BC-03a | Debug/Release 최종10.94/14.75초 PASS. Debug -xbot-arm-checks1 6.32초 종료0, BC-03a·BC-00 PASS. Xbot nodes70/palette67·중복0, 별도 synthetic 중복2개와 서로 다른 IBM 검증 | 새 Codex 읽기 전용 리뷰0건 |
| BC-03b | Debug/Release 빌드 PASS(첫136.80/177.00초, 진단 추가 최종Debug도 PASS). physics-sim0 컴포넌트 Debug4.67/Release4.66초, 종료0·PASS GPU preparation. CPU32/32·495 재실행PASS | 새 Codex 읽기 전용 리뷰0건 |

BC-03a는 ModelInstance의 애니메이션 평가와 최종 팔레트를 분리하고 기존 Update wrapper를 유지했다. BC-03b는 Scene의 CPU 평가·최종 확정·GPU 준비를 분리했다. 비물리 호출은 기존 wrapper를 사용한다. 무효 pose의 readiness 해제와 삭제 ID 거절을 검사했다.

BC-03b 초기 component-checks는 physics-sim0을 빠뜨려 master 기본값1로 실행됐고 바닥이 추가되어 Two components per owner 조건에 실패했다. 진단 로그로 확인한 뒤 명시적으로 물리를 끈 Debug/Release 재실행이 통과했다. 기본값1은 변경하지 않고 계약에 기록했다. 최초 Release 로그 파일 경합은 실행 실패로 분리했고 고유 로그 경로의 재실행만 성공으로 계산했다.

DirectX debug layer(-debug1)를 사용했다. GPU 기반 검증(-gpu_debug1)은 미사용이다. 레지스트리 GPU 정보는 Intel Iris Xe, 드라이버27.20.100.9415이며 실제 선택 adapter를 별도로 계측하지 않았다. 기존 미수정 파일들의 C4819 경고와 pwsh fallback 메시지는 있었으나 최종 빌드는 성공했다. GUI 검사에는 computer-use 스킬로 오류 창 유무를 확인했으며 앱 설정은 바꾸지 않았다.

진행 중이며 M1 break 데모·스크린샷·BC-13a 수명 회귀는 아직 미완료다. 메인 체크아웃의 일반 파일·junction·*.empty·기존 .mini 변경을 수정·커밋하지 않는다.

직접 현재 검사를 실행하는 명령:

```powershell
Set-Location MiniEngine/ModelViewer
../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6
../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -physics-sim 0 -xbot-arm-checks 1
```
