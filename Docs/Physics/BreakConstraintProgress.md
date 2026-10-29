상태: 진행 중 — BC-04a `8593a0be`, BC-04b `d31863a7`, BC-05 구현·검증 완료(미커밋), BC-06 이후 미착수 (2026-10-29)

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
| BC-04a（커밋 8593a0be） | MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.h, SkeletalPhysicsAsset.cpp; MiniEngine/Physics/tests/SkeletalAssetTests.cpp; MiniEngine/Physics/CMakeLists.txt, Physics.vcxproj, Physics.vcxproj.filters; Tools/BuildJolt.ps1; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/README.md | CPU 42/42·/O2 /DNDEBUG 42/42 PASS, 종료0; Debug/Release 최초22.51/7.44초·최종6.89/7.41초 PASS, 경고0·오류0; CTest·viewer 미실행（호출 경로 변경 없음） | codex exec 홈 탐색 실패로 자기 리뷰; 매크로 한 줄 여러 문장·weld 누락 2건 수정 후 재검증, 잔여0건 |

BC-04a 산출물·로그는 `$env:TEMP\BreakConstraintTests`에 두었다(`SkeletalAssetTests.exe`, `SkeletalAssetTestsRelease.exe`, `BC04a-Debug-build-final.log`, `BC04a-Release-build-final.log`, `BC04a-review.log`). 새 C++ 파일은 총 275줄이며 실제 fixture·Xbot 생성 API는 추가하지 않았다. 기존 .mini 3개와 BreakConstraintReport.md는 수정하지 않았고 파일 삭제·junction 수동 변경·git commit/merge/stash/reset/checkout은 수행하지 않았다.

### BC-03b 리뷰 수정 검증 조건 (2026-10-29)

CPU 명령은 SkeletonPoseTests.cpp와 SceneMathTests.cpp 머리말을 따랐으며 산출물과 검증 로그는 `$env:TEMP\BreakConstraintTests`에 저장했다. worktree 루트에서 `msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows`와 Configuration=Release를 실행했다(/restore 없음). 빌드의 기존 C4819 경고와 VS 환경 초기화의 vswhere 메시지는 남아 있으나 최종 빌드는 모두 오류0이다.

MiniEngine/ModelViewer에서 Debug·Release 각각 `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`, Debug에서 `-debug 1 -xbot-arm-checks 1`로 실행했다. 두 컴포넌트 로그 모두 3개 owner × 4개 dt의 wrapper 동등성12건·PrepareMeshes 보존12건과 최종 PASS GPU preparation을 확인했다. DirectX debug layer를 활성화했고 GPU 기반 검증 옵션은 사용하지 않았다. 드라이버 레지스트리의 NVIDIA GeForce MX450 버전은 32.0.15.9186이며 실제 선택 adapter는 별도 계측하지 않았다(WMI 조회는 접근 거부).

수정 도중 첫 Debug 컴포넌트 실행은 41.75초 후 종료 코드 -1073741819로 실패했다. 새 진단의 임시 Scene이 미제출 GPU 명령의 버퍼를 먼저 해제한 문제를 `context.Flush(true)`로 수정하고 위 최종 빌드·실행을 통과했다. BC-03a/03b 변경 diff에서 같은 줄의 두 문장 주석은 FinalizePoses 선언·정의 두 곳뿐임을 확인하고 모두 나눴다. BC-04 이후, MiniEngine/Physics 코드, .mini, BreakConstraintReport.md는 수정하지 않았으며 커밋·병합·stash·checkout을 수행하지 않았다.

### BC-04b 완료 (2026-10-29)

| 단계 | 커밋 | 바뀐 파일 목록 | CPU 테스트·전체 빌드·실행 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-04b | `d31863a7` | MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.h, SkeletalPhysicsAsset.cpp; MiniEngine/Physics/tests/SkeletalAssetTests.cpp; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/README.md | 직접 cl CPU 82/82 PASS 1.19초, /O2 /DNDEBUG 82/82 PASS 3.37초, 경고 없음; ModelViewer Debug 105.21초·Release 119.91초 PASS, 종료0, /restore 없음; 기존 C4819·vswhere/pwsh 탐색 메시지 있음; CTest·viewer 미실행(새 API의 런타임 호출 없음) | 자기 리뷰, anchor 시험 부족 1건 보강 후 재검증, 잔여0건; diff --check PASS |

fixture 3 body·2 joints·body 없는 tip, Xbot 고정 NodeId 13→14→15·명시적 m/kg/rad 값을 추가했다. GLB 정적 translation을 읽어 +X 방향을 확인하고 BC-00 배율에서 반올림 anchor 오차≤0.1mm를 검사했다. 손목 swing/twist 제한은 BC-00 측정 범위를 포함한다. 손 크기·폭·두께·질량·관절 한계는 M1 proxy 값이며 계약 §6·§8에 근거와 한계를 적었다. 새 파일이 없어 기존 vcxproj/.filters·CMake·BuildJolt.ps1 시험 target 등록을 그대로 사용한다.

CPU 산출물과 빌드 로그 및 자기 리뷰 메모는 `$env:TEMP/BreakConstraintTests`에 있다(`SkeletalAssetTests.exe`, `SkeletalAssetTestsRelease.exe`, `BC04b-Debug-build.log`, `BC04b-Release-build.log`, `BC04b-self-review.txt`). BC-04a 커밋 칸을 `8593a0be`로 채웠다. BC-05 이후는 시작하지 않았고 git 쓰기 명령·파일 삭제·junction 수동 변경·기존 .mini 3개 및 BreakConstraintReport.md 편집은 하지 않았다.
### BC-05 완료·미커밋 (2026-10-29)

| 단계 | 커밋 | 바뀐 파일 목록 | CPU 테스트·전체 빌드·실행 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-05 | 미커밋, 상위 관리자 담당 | MiniEngine/Physics/PhysicsWorld.h, PhysicsWorldBodies.cpp, tests/PhysicsBodyTests.cpp; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/README.md | CPU Debug 362/362 PASS 7.63초·Release 362/362 PASS 10.26초, 경고0; 전체 Debug 40.82초·Release 136.18초 PASS, 오류0, 기존 경고18/20; 물리OFF component-checks Debug 6.32초·Release 7.31초 PASS GPU preparation·종료0; CTest 전체 미실행 | 자기 리뷰, 왕복 전환/거절 뒤 속도 보존 시험 부족1건 보강, 재검증 후 잔여0건; diff --check PASS |

BC-04b 커밋 칸을 `d31863a7`로 채웠다. 기존 handle·COM 속도 조회·Moving 계층을 재사용하고 Kinematic 생성·MoveKinematic·Dynamic 왕복 전환·속도 설정·월드 작용점 impulse를 추가했다. 새 파일이 없어 기존 PhysicsBodyTests의 CMake·BuildJolt.ps1 testTargets·vcxproj/.filters 등록을 재사용한다. API와 시험 C++ 변경은 약 250줄이다.

검증 산출물과 로그는 `$env:TEMP/BreakConstraintTests`의 `PhysicsBodyTestsDebug.exe`, `PhysicsBodyTestsRelease.exe`, `BC05-CPU-*-final.log`, `BC05-*-build-final.log`, `BC05-*-component-checks.log`, `BC05-self-review.txt`다. 실행 명령은 ModelViewer 폴더에서 각 구성 exe에 `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`이다. DirectX debug layer 사용, GPU/driver 재측정 없음이다. 첫 CPU/Debug 컴파일의 벡터 선언 오류·시험의 Dynamic 전용 질량 조회 오류·재검증 스크립트의 LNK1181은 수정 후 최종 재검증했다(계약 §12). 초기 실패를 성공으로 계산하지 않았다.

BC-05만 끝내고 중단한다. BC-06 이후 미착수이며 git commit/merge/stash/reset/checkout·파일 삭제·junction 수동 변경·.mini 수동 편집·BreakConstraintReport.md 편집은 하지 않았다. 기존 .mini 부산물은 그대로 커밋 대상에서 제외한다.