상태: 진행 중 — BC-03b 완료, 기준 3bee0b20, 커밋 없이 단계별 기록 (2026-10-29)

# BreakConstraint 진행 기록

| 단계 | 커밋 | 검증 결과 | 리뷰 |
| --- | --- | --- | --- |
| BC-02 | 미커밋: 전체 빌드 차단으로 완료 보류 | CPU 32/32 PASS (6.60초); Debug Jolt 외부 junction 캐시 쓰기 Permission denied/MSB3073; Release·실행 미수행; master 병합 미수행 | 자기 리뷰, 유효 지적 0건; diff --check PASS |

| BC-02 재개 | 다음 단계 커밋 때 기입 | CPU 32/32 PASS(3.59초), Debug/Release 빌드 PASS(종료 코드 0), viewer 미실행(화면 경로 변경 없음) | codex exec 홈 탐색 실패로 자기 리뷰, 유효 지적 0건 |

| BC-02 커밋 차단 | 미커밋 | git add/commit 모두 worktree 외부 Git 관리 경로 index.lock Permission denied; master 병합·후속 단계 미수행; 최종 보고 BreakConstraintReport.md 저장 | 자기 리뷰, 유효 지적 0건 |

| Git 허용 후 재시도 | 미커밋, HEAD d044de76 | add/commit index.lock·merge ORIG_HEAD.lock 모두 Permission denied; ACL에 Deny Write 확인; 코드 불변으로 CPU/빌드 재실행 없음, 기존 PASS 유지 | 자기 리뷰 0건 유지, 보고서 갱신 |

## 3bee0b20 이후 단계별 변경 기록

사용자 지시로 이 실행에서는 git commit/merge를 시도하지 않는다. 단계별 파일과 검증 결과를 기록하여 나중에 나눠 커밋한다.

| 단계 | 바뀐 파일 목록 | 테스트·빌드·실행 결과 | 리뷰 지적 수 |
| --- | --- | --- | --- |
| BC-02·master 병합 인계 | 사용자 커밋 619022e6·3bee0b20 | 사용자 Debug 빌드 성공 인계; 본 세션 SkeletonPose 32/32, SceneMath 495 checks, Physics Debug 9/9 targets PASS(CTest 1.34초) | BC-02 이전 자기 리뷰 0건; 병합 충돌은 사용자 해결 |
| Static 전제 대조 | Docs/Physics/BreakConstraintImplementation.md | PhysicsWorld·PhysicsSystem·편집/삭제 경로 대조; owner 잠금·삭제 정리 연결 지점 보완 | 코드 변경 없음 |
| BC-03a | MiniEngine/Model/Model.h, Model.cpp; MiniEngine/ModelViewer/Scene/tests/XbotArmChecks.h; 계약·README·CodeMap·Progress | CPU 회귀 32/32·495 checks·9/9; Debug 10.94초/Release14.75초 PASS; Debug Xbot 실행6.32초, BC-03a·BC-00 PASS; synthetic 중복2개 확인 | 별도 새 Codex 읽기 전용 리뷰 0건 |

| BC-03b | MiniEngine/ModelViewer/Scene/Scene.h, Scene.cpp, tests/MeshComponentChecks.h; 계약·README·Scene·CodeMap·Progress | CPU32/32·495 PASS; Debug136.80초+최종로그빌드 PASS/Release177.00초 PASS; physics-sim0 컴포넌트 Debug4.67초·Release4.66초 PASS. 물리기본1로 잘못 실행한 초기 검사 실패는 계약에 기록 | 별도 새 Codex 0건 |
| BC-03b 리뷰 수정 | MiniEngine/ModelViewer/Scene/tests/MeshComponentChecks.h, Scene.cpp, Scene.h; Docs/Current/Scene.md; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md | CPU SkeletonPose32/32 PASS 1.06초·SceneMath495 PASS 1.03초(컴파일+실행); Debug 빌드 PASS 최초39.40초·최종18.26초, Release PASS 최초162.27초·최종5.14초; 컴포넌트 Debug PASS 11.30초·Release PASS 6.19초; Debug Xbot BC-03a·BC-00 PASS 8.13초; 모든 최종 종료 코드0 | 지적 3건 처리: 4개 dt의 wrapper/분리 경로 전체 node·position/normal 비교 및 GPU 준비 전후 보존, FinalizePoses 두 줄 주석, Scene 소유권 본문 동기화 |

### BC-03b 리뷰 수정 검증 조건 (2026-10-29)

CPU 명령은 SkeletonPoseTests.cpp와 SceneMathTests.cpp 머리말을 따랐으며 산출물과 검증 로그는 `$env:TEMP\BreakConstraintTests`에 저장했다. worktree 루트에서 `msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows`와 Configuration=Release를 실행했다(/restore 없음). 빌드의 기존 C4819 경고와 VS 환경 초기화의 vswhere 메시지는 남아 있으나 최종 빌드는 모두 오류0이다.

MiniEngine/ModelViewer에서 Debug·Release 각각 `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`, Debug에서 `-debug 1 -xbot-arm-checks 1`로 실행했다. 두 컴포넌트 로그 모두 3개 owner × 4개 dt의 wrapper 동등성12건·PrepareMeshes 보존12건과 최종 PASS GPU preparation을 확인했다. DirectX debug layer를 활성화했고 GPU 기반 검증 옵션은 사용하지 않았다. 드라이버 레지스트리의 NVIDIA GeForce MX450 버전은 32.0.15.9186이며 실제 선택 adapter는 별도 계측하지 않았다(WMI 조회는 접근 거부).

수정 도중 첫 Debug 컴포넌트 실행은 41.75초 후 종료 코드 -1073741819로 실패했다. 새 진단의 임시 Scene이 미제출 GPU 명령의 버퍼를 먼저 해제한 문제를 `context.Flush(true)`로 수정하고 위 최종 빌드·실행을 통과했다. BC-03a/03b 변경 diff에서 같은 줄의 두 문장 주석은 FinalizePoses 선언·정의 두 곳뿐임을 확인하고 모두 나눴다. BC-04 이후, MiniEngine/Physics 코드, .mini, BreakConstraintReport.md는 수정하지 않았으며 커밋·병합·stash·checkout을 수행하지 않았다.
